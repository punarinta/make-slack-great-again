// Cocoa data transfer: plat's MIME-typed DataItems ↔ NSPasteboard types,
// shared by the clipboard (cocoa_app.mm), the drag source and the drop
// target (cocoa_window.mm), plus Image → CGImage/NSImage for drag images,
// tray icons and notification pictures.
#include "cocoa/cocoa_internal.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>

namespace plat::cocoa {

NSString *nsString(std::string_view s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]
               ?: @"";
}

namespace {

// File-reference URLs (file:///.file/id=…) are not portable paths.
NSURL *portable(NSURL *u) {
    return u.isFileReferenceURL ? u.filePathURL : u;
}

} // namespace

NSPasteboardType pasteboardTypeForMime(std::string_view mime) {
    if (isTextMime(mime))
        return NSPasteboardTypeString;
    if (mime == "text/html")
        return NSPasteboardTypeHTML;
    if (mime == "image/png")
        return NSPasteboardTypePNG;
    if (mime == "image/tiff")
        return NSPasteboardTypeTIFF;
    if (mime == "text/rtf")
        return NSPasteboardTypeRTF;
    NSString *m = nsString(mime);
    if (!m.length)
        return nil;
    // Not a MIME type (no '/'): a pasteboard type handed back from
    // requestClipboardMimes() for a native format without one — use as is.
    if ([m rangeOfString:@"/"].location == NSNotFound)
        return m;
    // A known MIME type maps to its declared UTI; an unknown one to a dynamic
    // UTI whose preferredMIMEType is the string again, so it round-trips
    // through the pasteboard server and other apps can read what it is.
    if (UTType *t = [UTType typeWithMIMEType:m])
        return t.identifier;
    return m;
}

std::string mimeForPasteboardType(NSPasteboardType type) {
    if ([type isEqualToString:NSPasteboardTypeString])
        return core::kTextMime;
    if ([type isEqualToString:NSPasteboardTypeHTML])
        return "text/html";
    if ([type isEqualToString:NSPasteboardTypePNG])
        return "image/png";
    if ([type isEqualToString:NSPasteboardTypeTIFF])
        return "image/tiff";
    if ([type isEqualToString:NSPasteboardTypeRTF])
        return "text/rtf";
    if ([type isEqualToString:NSPasteboardTypeFileURL] ||
        [type isEqualToString:NSPasteboardTypeURL])
        return "text/uri-list";
    if ([type rangeOfString:@"/"].location != NSNotFound)
        return type.UTF8String; // a private type that is a MIME string already
    if (UTType *t = [UTType typeWithIdentifier:type]) {
        // UTF-16 and other plain-text flavours are all "the text" to plat;
        // the pasteboard converts between them on read.
        if ([t conformsToType:UTTypePlainText])
            return core::kTextMime;
        if (NSString *m = t.preferredMIMEType)
            return m.UTF8String;
        if (t.dynamic)
            return {}; // dyn.… without a MIME tag: nothing to call it
    }
    // Pre-UTI legacy names ("NeXT RTFD pasteboard type", "Apple PDF …") and
    // similar have spaces; they are aliases of types listed anyway.
    if ([type rangeOfString:@" "].location != NSNotFound)
        return {};
    return type.UTF8String; // a UTI without a MIME type round-trips by name
}

NSArray<NSPasteboardItem *> *pasteboardItems(const std::vector<DataItem> &items) {
    NSMutableArray<NSPasteboardItem *> *out   = [NSMutableArray array];
    NSPasteboardItem                   *first = [NSPasteboardItem new];
    [out addObject:first];
    std::vector<std::string> uris;
    for (const auto &i : items) {
        if (i.mime == "text/uri-list") {
            auto lines = core::parseUriList(i.data);
            uris.insert(uris.end(), lines.begin(), lines.end());
            continue;
        }
        NSPasteboardType type = pasteboardTypeForMime(i.mime);
        if (!type)
            continue;
        if (type == NSPasteboardTypeString) {
            [first setString:nsString(i.data) forType:type];
        } else {
            [first setData:[NSData dataWithBytes:i.data.data() length:i.data.size()] forType:type];
        }
    }
    for (size_t n = 0; n < uris.size(); ++n) {
        // parseUriList never yields an empty line: "" here is not UTF-8.
        NSString *s = nsString(uris[n]);
        NSURL    *u = s.length ? [NSURL URLWithString:s] : nil;
        if (!u)
            continue;
        NSPasteboardItem *item = n == 0 ? first : [NSPasteboardItem new];
        // Files as public.file-url (what Finder and every drop target look
        // for), anything else as public.url.
        [item setString:u.absoluteString
                forType:u.isFileURL ? NSPasteboardTypeFileURL : NSPasteboardTypeURL];
        if (n > 0)
            [out addObject:item];
    }
    return out;
}

std::vector<std::string> pasteboardMimes(NSPasteboard *pb) {
    std::vector<std::string> out;
    for (NSPasteboardItem *item in pb.pasteboardItems) {
        for (NSPasteboardType t in item.types) {
            std::string m = mimeForPasteboardType(t);
            if (!m.empty() && std::find(out.begin(), out.end(), m) == out.end())
                out.push_back(std::move(m));
        }
    }
    // Many apps copy pictures as TIFF only; readPasteboard converts on
    // request, so offer the standard name too (as Windows does for a DIB).
    if (std::find(out.begin(), out.end(), "image/tiff") != out.end() &&
        std::find(out.begin(), out.end(), "image/png") == out.end())
        out.push_back("image/png");
    return out;
}

std::vector<std::string> pasteboardUris(NSPasteboard *pb) {
    std::vector<std::string> out;
    for (NSPasteboardItem *item in pb.pasteboardItems) {
        NSString *s = [item stringForType:NSPasteboardTypeFileURL]
                          ?: [item stringForType:NSPasteboardTypeURL];
        NSURL    *u = s ? [NSURL URLWithString:s] : nil;
        if (u && (u = portable(u)).absoluteString)
            out.emplace_back(u.absoluteString.UTF8String);
    }
    return out;
}

std::optional<std::string> readPasteboard(NSPasteboard *pb, std::string_view mime) {
    if (mime == "text/uri-list") {
        auto uris = pasteboardUris(pb);
        if (uris.empty())
            return std::nullopt;
        std::string list;
        for (auto &u : uris)
            list += u + "\r\n";
        return list;
    }
    NSPasteboardType type = pasteboardTypeForMime(mime);
    if (!type)
        return std::nullopt;
    if (type == NSPasteboardTypeString) {
        if (NSString *s = [pb stringForType:type])
            return std::string(s.UTF8String);
        return std::nullopt;
    }
    if (NSData *d = [pb dataForType:type])
        return std::string(static_cast<const char *>(d.bytes), d.length);
    if (type == NSPasteboardTypePNG) // a TIFF-only picture, as PNG
        if (NSData *tiff = [pb dataForType:NSPasteboardTypeTIFF])
            return pngFromTiff(tiff);
    return std::nullopt;
}

NSData *tiffOnlyPicture(NSPasteboard *pb, std::string_view mime) {
    if (pasteboardTypeForMime(mime) != NSPasteboardTypePNG ||
        [pb availableTypeFromArray:@[ NSPasteboardTypePNG ]])
        return nil;
    return [pb dataForType:NSPasteboardTypeTIFF];
}

std::optional<std::string> pngFromTiff(NSData *tiff) {
    @autoreleasepool {
        NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:tiff];
        NSData           *png =
            rep ? [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
        if (png.length)
            return std::string(static_cast<const char *>(png.bytes), png.length);
        return std::nullopt;
    }
}

CGImageRef createCGImage(const Image &img) {
    if (img.empty() || img.pixels.size() < size_t(img.width) * img.height)
        return nullptr;
    CFDataRef data = CFDataCreate(
        nullptr,
        reinterpret_cast<const UInt8 *>(img.pixels.data()),
        CFIndex(size_t(img.width) * img.height * 4)
    );
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // 0xAARRGGBB words in native (little) endianness = BGRA bytes, alpha first.
    CGImageRef      cg = CGImageCreate(
        size_t(img.width),
        size_t(img.height),
        8,
        32,
        size_t(img.width) * 4,
        cs,
        CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little,
        provider,
        nullptr,
        false,
        kCGRenderingIntentDefault
    );
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    return cg;
}

NSImage *nsImage(const Image &img, NSSize points) {
    CGImageRef cg = createCGImage(img);
    if (!cg)
        return nil;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
    CGImageRelease(cg);
    rep.size       = points; // pixels per point = the rep's scale
    NSImage *image = [[NSImage alloc] initWithSize:points];
    [image addRepresentation:rep];
    return image;
}

// ── drop actions ────────────────────────────────────────────────────────────

uint32_t dropActionsFromOperation(NSDragOperation op) {
    uint32_t a = 0;
    if (op & NSDragOperationCopy)
        a |= ActCopy;
    // Generic ("whatever the destination thinks best") is what AppKit and
    // Finder use for a plain move; Delete is a move to the trash.
    if (op & (NSDragOperationMove | NSDragOperationGeneric | NSDragOperationDelete))
        a |= ActMove;
    if (op & NSDragOperationLink)
        a |= ActLink;
    return a;
}

NSDragOperation operationFromActions(uint32_t actions) {
    NSDragOperation op = NSDragOperationNone;
    if (actions & ActCopy)
        op |= NSDragOperationCopy;
    if (actions & ActMove)
        op |= NSDragOperationMove | NSDragOperationGeneric;
    if (actions & ActLink)
        op |= NSDragOperationLink;
    return op;
}

DropAction dropActionFromOperation(NSDragOperation op) {
    if (op & NSDragOperationCopy)
        return DropAction::Copy;
    if (op & (NSDragOperationMove | NSDragOperationGeneric | NSDragOperationDelete))
        return DropAction::Move;
    if (op & NSDragOperationLink)
        return DropAction::Link;
    return DropAction::None;
}

NSDragOperation operationFromAction(DropAction a) {
    switch (a) {
    case DropAction::Copy:
        return NSDragOperationCopy;
    case DropAction::Move:
        return NSDragOperationMove;
    case DropAction::Link:
        return NSDragOperationLink;
    default:
        return NSDragOperationNone;
    }
}

} // namespace plat::cocoa
