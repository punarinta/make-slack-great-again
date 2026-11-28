// Win32 backend, data half: the one mapping between MIME types and clipboard
// formats (shared by the clipboard and OLE drag and drop), CF_HTML, CF_HDROP
// and DIB conversions, and the clipboard itself.
//
// Standard types map to what Windows apps actually exchange:
//   text/plain;charset=utf-8  CF_UNICODETEXT (CRLF; Windows synthesises CF_TEXT)
//   text/html                 "HTML Format" (CF_HTML: UTF-8 with an offset header)
//   image/png                 "PNG" (what browsers and Office register)
//   image/bmp                 CF_DIB (reading: CF_DIB/CF_DIBV5 + a file header)
//   text/uri-list             registered "text/uri-list" (exact bytes between
//                             plat apps) + CF_HDROP when every URI is a local
//                             file, so Explorer and every file-taking app
//                             understand it; reading also takes CF_HDROP and
//                             "UniformResourceLocatorW"
// Every other MIME type is a format registered under its own name, which
// round-trips between plat apps and matches Chromium's custom types.
#include "win32/win32.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace plat::win32 {

namespace {

UINT registered(const wchar_t *name) {
    return RegisterClipboardFormatW(name);
}
UINT cfUrlW() {
    static const UINT f = registered(L"UniformResourceLocatorW");
    return f;
}
UINT cfUrlA() {
    static const UINT f = registered(L"UniformResourceLocator");
    return f;
}

std::string lfToCrlf(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            out += '\r';
        out += s[i];
    }
    return out;
}

std::string crlfToLf(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n'))
            out += s[i];
    return out;
}

// NUL-terminated UTF-16 as bytes, the layout of CF_UNICODETEXT.
std::string wideBytes(std::wstring_view w) {
    std::string b(reinterpret_cast<const char *>(w.data()), w.size() * sizeof(wchar_t));
    b.append(sizeof(wchar_t), '\0');
    return b;
}

std::wstring_view wideUntilNul(std::string_view bytes) {
    const auto  *w = reinterpret_cast<const wchar_t *>(bytes.data());
    const size_t n = wcsnlen(w, bytes.size() / sizeof(wchar_t));
    return {w, n};
}

std::string_view untilNul(std::string_view s) {
    const size_t n = s.find('\0');
    return n == std::string_view::npos ? s : s.substr(0, n);
}

long headerValue(std::string_view header, std::string_view key) {
    const size_t at = header.find(key);
    if (at == std::string_view::npos)
        return -1;
    size_t i   = at + key.size();
    bool   neg = false;
    if (i < header.size() && header[i] == '-') {
        neg = true;
        ++i;
    }
    long v   = 0;
    bool any = false;
    for (; i < header.size() && header[i] >= '0' && header[i] <= '9'; ++i) {
        v   = v * 10 + (header[i] - '0');
        any = true;
        if (v > (1L << 30))
            return -1;
    }
    return any && !neg ? v : -1;
}

// Text in the ANSI code page (CF_TEXT, a non-wide CF_HDROP) → UTF-16.
std::wstring acpToWide(std::string_view a) {
    const int    n = MultiByteToWideChar(CP_ACP, 0, a.data(), int(a.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, a.data(), int(a.size()), w.data(), n);
    return w;
}

bool openClipboard(HWND owner) {
    // Another process may hold the clipboard for a moment (clipboard
    // managers, RDP); a few short retries is what every toolkit does.
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(owner))
            return true;
        Sleep(5);
    }
    return false;
}

UINT cfHtml() {
    static const UINT f = registered(L"HTML Format");
    return f;
}
UINT cfPng() {
    static const UINT f = registered(L"PNG");
    return f;
}
UINT cfUriList() { // registered "text/uri-list": exact bytes between plat apps
    static const UINT f = registered(L"text/uri-list");
    return f;
}

// Where a packed DIB's (CF_DIB / CF_DIBV5) pixels start in the .bmp file
// made of it: after BITMAPFILEHEADER, the header, the BI_BITFIELDS masks
// (only outside the header for a plain BITMAPINFOHEADER) and the palette.
// 0 when the header does not describe the data.
size_t bmpPixelOffset(std::string_view dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER))
        return 0;
    BITMAPINFOHEADER h;
    std::memcpy(&h, dib.data(), sizeof(h));
    if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > dib.size())
        return 0;
    size_t extra = 0;
    if (h.biSize == sizeof(BITMAPINFOHEADER) && h.biCompression == BI_BITFIELDS)
        extra = 12;
    else if (h.biSize == sizeof(BITMAPINFOHEADER) && h.biCompression == 6) // BI_ALPHABITFIELDS
        extra = 16;
    size_t colors = h.biClrUsed;
    if (!colors && h.biBitCount <= 8)
        colors = size_t(1) << h.biBitCount;
    const size_t off = sizeof(BITMAPFILEHEADER) + h.biSize + extra + colors * sizeof(RGBQUAD);
    return off > sizeof(BITMAPFILEHEADER) + dib.size() ? 0 : off;
}

// The DIB as a .bmp file (BITMAPFILEHEADER in front); empty when invalid.
std::string dibToBmpFile(std::string dib) {
    const size_t off = bmpPixelOffset(dib);
    if (!off)
        return {};
    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4d42; // "BM"
    fh.bfSize    = DWORD(sizeof(fh) + dib.size());
    fh.bfOffBits = DWORD(off);
    dib.insert(0, reinterpret_cast<const char *>(&fh), sizeof(fh));
    return dib;
}

// The standard MIME name of a native format; nullopt for ones with no
// meaningful MIME (CF_LOCALE, CF_OEMTEXT duplicates, private GDI formats).
std::optional<std::string> mimeForFormat(UINT cf) {
    switch (cf) {
    case CF_UNICODETEXT:
    case CF_TEXT:
    case CF_OEMTEXT:
        return core::kTextMime;
    case CF_DIB:
    case CF_DIBV5:
    case CF_BITMAP:
        return "image/bmp";
    case CF_HDROP:
        return "text/uri-list";
    default:
        break;
    }
    if (cf < 0xC000)
        return std::nullopt; // other predefined formats: GDI handles, locale, …
    if (cf == cfHtml())
        return "text/html";
    if (cf == cfPng())
        return "image/png";
    if (cf == cfUriList() || cf == cfUrlW() || cf == cfUrlA())
        return "text/uri-list";
    wchar_t   name[256];
    const int n = GetClipboardFormatNameW(cf, name, 256);
    if (n <= 0)
        return std::nullopt;
    return toUtf8(std::wstring_view(name, size_t(n)));
}

} // namespace

// ── HGLOBAL ─────────────────────────────────────────────────────────────────

HGLOBAL globalFromBytes(const void *p, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(n, 1));
    if (!g)
        return nullptr;
    if (void *d = GlobalLock(g)) {
        if (n)
            std::memcpy(d, p, n);
        GlobalUnlock(g);
        return g;
    }
    GlobalFree(g);
    return nullptr;
}

std::optional<std::string> bytesFromGlobal(HGLOBAL g) {
    // GlobalSize may round up; the terminator bounds text formats, and
    // binary types get what every other reader sees.
    const size_t size = GlobalSize(g);
    const void  *p    = GlobalLock(g);
    if (!p)
        return std::nullopt;
    std::string bytes(static_cast<const char *>(p), size);
    GlobalUnlock(g);
    return bytes;
}

// ── CF_HTML ─────────────────────────────────────────────────────────────────

std::string cfHtmlEncode(std::string_view fragment) {
    // Offsets are byte positions from the start of the data; zero-padded to a
    // fixed width so the header's own length does not depend on them.
    static constexpr char kPre[]  = "<html>\r\n<body>\r\n<!--StartFragment-->";
    static constexpr char kPost[] = "<!--EndFragment-->\r\n</body>\r\n</html>";
    static constexpr char kFmt[]  = "Version:0.9\r\nStartHTML:%010ld\r\nEndHTML:%010ld\r\n"
                                    "StartFragment:%010ld\r\nEndFragment:%010ld\r\n";
    char                  header[160];
    const int             headerLen = std::snprintf(header, sizeof(header), kFmt, 0L, 0L, 0L, 0L);
    const long            startHtml = headerLen;
    const long            startFrag = startHtml + long(sizeof(kPre) - 1);
    const long            endFrag   = startFrag + long(fragment.size());
    const long            endHtml   = endFrag + long(sizeof(kPost) - 1);
    std::snprintf(header, sizeof(header), kFmt, startHtml, endHtml, startFrag, endFrag);
    std::string out(header, size_t(headerLen));
    out += kPre;
    out += fragment;
    out += kPost;
    out += '\0'; // readers expect a terminated string; the offsets exclude it
    return out;
}

std::string cfHtmlDecode(std::string_view data) {
    data                          = untilNul(data);
    const size_t           first  = data.find('<');
    // The header is everything before the markup; offsets are only trusted
    // when they fall inside the data (some producers write -1 or garbage).
    const std::string_view header = data.substr(0, first == std::string_view::npos ? 0 : first);
    const long             size   = long(data.size());
    const long sf = headerValue(header, "StartFragment:"), ef = headerValue(header, "EndFragment:");
    if (sf >= 0 && ef >= sf && ef <= size)
        return std::string(data.substr(size_t(sf), size_t(ef - sf)));
    const std::string_view kStart = "<!--StartFragment-->", kEnd = "<!--EndFragment-->";
    const size_t           ms = data.find(kStart), me = data.rfind(kEnd);
    if (ms != std::string_view::npos && me != std::string_view::npos && me >= ms + kStart.size())
        return std::string(data.substr(ms + kStart.size(), me - ms - kStart.size()));
    const long sh = headerValue(header, "StartHTML:"), eh = headerValue(header, "EndHTML:");
    if (sh >= 0 && eh >= sh && eh <= size)
        return std::string(data.substr(size_t(sh), size_t(eh - sh)));
    return first == std::string_view::npos ? std::string() : std::string(data.substr(first));
}

// ── CF_HDROP ────────────────────────────────────────────────────────────────

std::string hdropToUriList(const void *p, size_t size) {
    if (size < sizeof(DROPFILES))
        return {};
    const auto *df  = static_cast<const DROPFILES *>(p);
    const char *raw = static_cast<const char *>(p);
    if (df->pFiles >= size)
        return {};
    std::string out;
    if (df->fWide) {
        const auto  *w = reinterpret_cast<const wchar_t *>(raw + df->pFiles);
        const size_t n = (size - df->pFiles) / sizeof(wchar_t);
        for (size_t i = 0; i < n && w[i];) {
            const size_t len = wcsnlen(w + i, n - i);
            out += fileUri(std::wstring_view(w + i, len)) + "\r\n";
            i += len + 1;
        }
    } else {
        const char  *a = raw + df->pFiles;
        const size_t n = size - df->pFiles;
        for (size_t i = 0; i < n && a[i];) {
            const size_t len = strnlen(a + i, n - i);
            out += fileUri(acpToWide(std::string_view(a + i, len))) + "\r\n";
            i += len + 1;
        }
    }
    return out;
}

std::string hdropFromUriList(std::string_view list) {
    std::wstring files;
    for (const auto &u : parseUriList(list)) {
        auto path = pathFromFileUri(u);
        if (!path || path->empty())
            return {}; // one non-file URI: CF_HDROP would silently drop it
        files += *path;
        files += L'\0';
    }
    if (files.empty())
        return {};
    files += L'\0';
    DROPFILES df{};
    df.pFiles = sizeof(DROPFILES);
    df.fWide  = TRUE;
    std::string out(reinterpret_cast<const char *>(&df), sizeof(df));
    out.append(reinterpret_cast<const char *>(files.data()), files.size() * sizeof(wchar_t));
    return out;
}

// ── the mapping ─────────────────────────────────────────────────────────────

std::vector<NativeData> encodeForOs(const std::vector<DataItem> &items) {
    std::vector<NativeData> out;
    auto                    add = [&](UINT cf, std::string bytes) {
        if (!cf)
            return;
        for (const auto &n : out)
            if (n.cf == cf)
                return; // the first item of a type wins
        out.push_back({cf, std::move(bytes)});
    };
    for (const auto &i : items) {
        if (isTextMime(i.mime)) {
            // Windows text is CRLF; lone LFs paste as one line in Notepad & co.
            add(CF_UNICODETEXT, wideBytes(toWide(lfToCrlf(i.data))));
        } else if (i.mime == "text/html") {
            add(cfHtml(), cfHtmlEncode(i.data));
        } else if (i.mime == "image/png") {
            add(cfPng(), i.data);
        } else if (i.mime == "image/bmp") {
            if (i.data.size() > sizeof(BITMAPFILEHEADER) && i.data.compare(0, 2, "BM") == 0)
                add(CF_DIB, i.data.substr(sizeof(BITMAPFILEHEADER)));
        } else if (i.mime == "text/uri-list") {
            add(cfUriList(), i.data);
            if (std::string drop = hdropFromUriList(i.data); !drop.empty())
                add(CF_HDROP, std::move(drop));
            else if (auto uris = parseUriList(i.data); !uris.empty())
                add(cfUrlW(), wideBytes(toWide(uris[0]))); // browsers take one link
        } else {
            add(RegisterClipboardFormatW(toWide(i.mime).c_str()), i.data);
        }
    }
    return out;
}

std::vector<std::string> mimesForFormats(const std::vector<UINT> &cfs) {
    std::vector<std::string> out;
    bool                     bmp = false;
    for (UINT cf : cfs) {
        auto m = mimeForFormat(cf);
        if (!m)
            continue;
        bmp |= *m == "image/bmp";
        if (std::find(out.begin(), out.end(), *m) == out.end())
            out.push_back(std::move(*m));
    }
    // Screenshots (PrtScn, Snipping Tool) put only a DIB on the clipboard;
    // decodeFromOs turns it into PNG through WIC, so offer that too.
    if (bmp && std::find(out.begin(), out.end(), "image/png") == out.end())
        out.push_back("image/png");
    return out;
}

std::optional<OsData>
readFromOs(std::string_view mime, const std::function<std::optional<std::string>(UINT)> &get) {
    // Every format worth trying, best first; the first one present wins. A
    // DIB that does not form a valid bitmap falls through to the next one,
    // as before the split: checked here (cheap), converted in finishDecode.
    auto first = [&](std::initializer_list<UINT> cfs) -> std::optional<OsData> {
        for (UINT cf : cfs)
            if (auto b = get(cf)) {
                if ((cf == CF_DIB || cf == CF_DIBV5) && !bmpPixelOffset(*b))
                    continue;
                return OsData{cf, std::move(*b)};
            }
        return std::nullopt;
    };
    if (isTextMime(mime))
        return first({CF_UNICODETEXT, CF_TEXT});
    if (mime == "text/html")
        return first({cfHtml(), registered(L"text/html")});
    if (mime == "text/uri-list")
        return first({cfUriList(), CF_HDROP, cfUrlW(), cfUrlA()});
    if (mime == "image/png")
        return first({cfPng(), registered(L"image/png"), CF_DIBV5, CF_DIB});
    if (mime == "image/bmp")
        return first({CF_DIB, CF_DIBV5});
    return first({RegisterClipboardFormatW(toWide(mime).c_str())});
}

std::optional<std::string> finishDecode(std::string_view mime, OsData raw) {
    const UINT cf = raw.cf;
    if (cf == CF_UNICODETEXT || (cf == cfUrlW() && mime == "text/uri-list")) {
        std::string s = toUtf8(wideUntilNul(raw.bytes));
        return cf == CF_UNICODETEXT ? crlfToLf(s) : s + "\r\n";
    }
    if (cf == CF_TEXT)
        return crlfToLf(toUtf8(acpToWide(untilNul(raw.bytes))));
    if (cf == cfHtml() && mime == "text/html")
        return cfHtmlDecode(raw.bytes);
    if (cf == CF_HDROP)
        return hdropToUriList(raw.bytes.data(), raw.bytes.size());
    if (cf == cfUrlA() && mime == "text/uri-list")
        return std::string(untilNul(raw.bytes)) + "\r\n";
    if (cf == CF_DIB || cf == CF_DIBV5) {
        std::string bmp = dibToBmpFile(std::move(raw.bytes));
        if (mime != "image/png")
            return bmp;
        std::string png = bmpFileToPng(bmp);
        if (png.empty())
            return std::nullopt;
        return png;
    }
    return std::move(raw.bytes);
}

bool decodeIsSlow(std::string_view mime, const OsData &raw) {
    return (raw.cf == CF_DIB || raw.cf == CF_DIBV5) && mime == "image/png";
}

// ── decoding on a worker ────────────────────────────────────────────────────

// How a worker reaches the App: null once the App is going away.
struct DecodeLink {
    std::mutex m;
    Win32App  *app = nullptr;
};

namespace {

struct DecodeJob {
    std::shared_ptr<DecodeLink> link;
    uint64_t                    id = 0;
    std::string                 mime;
    OsData                      raw;
};

DWORD WINAPI decodeThread(LPVOID p) {
    std::unique_ptr<DecodeJob> job(static_cast<DecodeJob *>(p));
    // WIC is free-threaded; this thread's factory is its own (see wic()).
    const HRESULT              co     = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::optional<std::string> result = finishDecode(job->mime, std::move(job->raw));
    if (SUCCEEDED(co))
        CoUninitialize();
    // Under the lock: the App clears the link before it shuts its loop down.
    std::lock_guard lock(job->link->m);
    if (Win32App *app = job->link->app)
        app->post([app, id = job->id, r = std::move(result)]() mutable {
            app->decodeFinished(id, std::move(r));
        });
    return 0;
}

} // namespace

void Win32App::decodeOffThread(std::string mime, OsData raw, DecodeDone done) {
    if (!_decodeLink) {
        _decodeLink      = std::make_shared<DecodeLink>();
        _decodeLink->app = this;
    }
    const uint64_t id  = _nextDecode++;
    auto          *job = new DecodeJob{_decodeLink, id, std::move(mime), std::move(raw)};
    if (HANDLE t = CreateThread(nullptr, 0, &decodeThread, job, 0, nullptr)) {
        CloseHandle(t); // detached: it reports back through the link
        _decodes.push_back({id, std::move(done)});
        return;
    }
    // No thread: convert here, still answering from the loop.
    std::optional<std::string> result = finishDecode(job->mime, std::move(job->raw));
    delete job;
    post([done = std::move(done), r = std::move(result)]() mutable { done(std::move(r)); });
}

void Win32App::decodeFinished(uint64_t id, std::optional<std::string> result) {
    auto it = std::find_if(_decodes.begin(), _decodes.end(), [id](auto &d) { return d.id == id; });
    if (it == _decodes.end())
        return;
    DecodeDone done = std::move(it->done);
    _decodes.erase(it);
    done(std::move(result));
}

void Win32App::detachDecoders() {
    if (!_decodeLink)
        return;
    std::lock_guard lock(_decodeLink->m);
    _decodeLink->app = nullptr;
}

// ── clipboard ───────────────────────────────────────────────────────────────

void Win32App::setClipboard(std::vector<DataItem> items, Selection sel) {
    if (sel != Selection::Clipboard)
        return; // no primary selection on Windows
    const std::vector<NativeData> native = encodeForOs(items);
    if (!openClipboard(_msgHwnd))
        return;
    EmptyClipboard(); // a new selection replaces every offered type
    for (const auto &n : native) {
        HGLOBAL g = globalFromBytes(n.bytes.data(), n.bytes.size());
        if (g && !SetClipboardData(n.cf, g))
            GlobalFree(g); // else ownership passed to the OS
    }
    CloseClipboard();
}

void Win32App::requestClipboard(
    std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
) {
    // The data is local to the OS, so reading is synchronous; the callback is
    // still posted because the contract promises it never runs re-entrantly.
    // Only the copy happens with the clipboard open: converting waits until
    // other apps can have it back, and a slow conversion (a screenshot's DIB
    // to PNG) runs on a worker.
    std::optional<OsData> raw;
    if (sel == Selection::Clipboard && openClipboard(_msgHwnd)) {
        raw = readFromOs(mime, [](UINT cf) -> std::optional<std::string> {
            if (!cf || !IsClipboardFormatAvailable(cf))
                return std::nullopt;
            HANDLE h = GetClipboardData(cf);
            return h ? bytesFromGlobal(h) : std::nullopt;
        });
        CloseClipboard();
    }
    if (raw && decodeIsSlow(mime, *raw)) {
        decodeOffThread(std::string(mime), std::move(*raw), std::move(cb));
        return;
    }
    std::optional<std::string> result;
    if (raw)
        result = finishDecode(mime, std::move(*raw));
    post([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void Win32App::requestClipboardMimes(
    std::function<void(std::vector<std::string>)> cb, Selection sel
) {
    std::vector<UINT> formats;
    if (sel == Selection::Clipboard && openClipboard(_msgHwnd)) {
        for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f))
            formats.push_back(f);
        CloseClipboard();
    }
    post([cb = std::move(cb), m = mimesForFormats(formats)] { cb(m); });
}

} // namespace plat::win32
