// Win32 backend, CPU image helpers: picking and scaling icon sizes, HICONs
// and drag-image DIBs from plat::Image, PNG through WIC (Windows' own codec
// library, so plat carries no encoder), and the taskbar badge bitmap.
#include "win32/win32.h"

#include "core/image_util.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plat::win32 {

namespace {

template <class T>
struct Com {
    T *p                        = nullptr;
    Com()                       = default;
    Com(const Com &)            = delete;
    Com &operator=(const Com &) = delete;
    ~Com() {
        if (p)
            p->Release();
    }
    T      **out() { return &p; }
    T       *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The factory, created once and kept for the loop thread (bindWic): COM
// objects must not outlive their apartment, so the App drops it (releaseWic)
// before it uninitialises COM. Any other thread (a paste's PNG encode on a
// worker) gets a fresh one per call.
IWICImagingFactory *g_wic       = nullptr;
DWORD               g_wicThread = 0;

IWICImagingFactory *createWic() {
    IWICImagingFactory *f = nullptr;
    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void **>(&f)
    );
    return f;
}

// A reference the caller releases.
IWICImagingFactory *wic() {
    if (g_wicThread != GetCurrentThreadId())
        return createWic();
    if (!g_wic)
        g_wic = createWic();
    if (g_wic)
        g_wic->AddRef();
    return g_wic;
}

std::string readStream(IStream *s) {
    STATSTG st{};
    if (FAILED(s->Stat(&st, STATFLAG_NONAME)))
        return {};
    LARGE_INTEGER zero{};
    s->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::string out(size_t(st.cbSize.QuadPart), '\0');
    ULONG       got = 0;
    if (FAILED(s->Read(out.data(), ULONG(out.size()), &got)))
        return {};
    out.resize(got);
    return out;
}

// Straight-alpha BGRA rows → PNG.
std::string encodePngBgra(IWICImagingFactory *f, int w, int h, const uint32_t *px) {
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.out())))
        return {};
    Com<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.out())) ||
        FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache)))
        return {};
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2>         props;
    if (FAILED(enc->CreateNewFrame(frame.out(), props.out())) ||
        FAILED(frame->Initialize(props.p)) || FAILED(frame->SetSize(UINT(w), UINT(h))))
        return {};
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA)
        return {};
    const UINT stride = UINT(w) * 4;
    if (FAILED(frame->WritePixels(
            UINT(h), stride, stride * UINT(h), reinterpret_cast<BYTE *>(const_cast<uint32_t *>(px))
        )) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return {};
    return readStream(stream.p);
}

} // namespace

void bindWic() {
    if (!g_wic) // a factory still held belongs to the thread that made it
        g_wicThread = GetCurrentThreadId();
}

void releaseWic() {
    if (g_wic && g_wicThread == GetCurrentThreadId()) {
        g_wic->Release();
        g_wic = nullptr;
    }
}

Image fitImage(const std::vector<Image> &sizes, int size) {
    return fitImage(sizes, size, 0);
}

Image fitImage(const std::vector<Image> &sizes, int size, double scale) {
    // The slot is in physical pixels (the notification area renders at the
    // system DPI), so pixel size decides. Image::scale only breaks ties: two
    // 32 px icons, one drawn as 16 pt @2x and one as 32 pt @1x, are equally
    // sharp, but the one designed for this scale has the right line weights.
    const Image *best = core::pickImage(sizes, size, scale, false);
    if (!best)
        return {};
    if (best->width == size && best->height == size)
        return *best;
    // Keep the aspect ratio, centred in the square slot.
    const double k  = double(size) / std::max(best->width, best->height);
    const int    w  = std::max(1, int(std::lround(best->width * k)));
    const int    h  = std::max(1, int(std::lround(best->height * k)));
    const Image  sc = core::scaleImage(*best, w, h);
    Image        out{size, size, std::vector<uint32_t>(size_t(size) * size, 0)};
    const int    ox = (size - w) / 2, oy = (size - h) / 2;
    for (int y = 0; y < h; ++y)
        std::memcpy(
            &out.pixels[size_t(y + oy) * size + ox], &sc.pixels[size_t(y) * w], size_t(w) * 4
        );
    return out;
}

HBITMAP dibFromImage(const Image &img) {
    if (img.empty() || img.pixels.size() < size_t(img.width) * img.height)
        return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = img.width;
    bi.bmiHeader.biHeight      = -img.height; // top-down, like Image
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void   *bits               = nullptr;
    HBITMAP bmp                = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp)
        std::memcpy(bits, img.pixels.data(), size_t(img.width) * img.height * 4);
    return bmp;
}

HICON iconFromImage(const Image &img) {
    if (img.empty() || img.pixels.size() < size_t(img.width) * img.height)
        return nullptr;
    // HICON colour bitmaps carry straight alpha; Image is premultiplied.
    Image straight = img;
    for (auto &p : straight.pixels)
        p = core::unpremultiply(p);
    HBITMAP              color = dibFromImage(straight);
    // The AND mask is ignored for 32-bpp icons with alpha, but must exist.
    std::vector<uint8_t> zeros(size_t((img.width + 15) / 16 * 2) * img.height, 0);
    HBITMAP              mask = CreateBitmap(img.width, img.height, 1, 1, zeros.data());
    HICON                icon = nullptr;
    if (color && mask) {
        ICONINFO ii{};
        ii.fIcon    = TRUE;
        ii.hbmColor = color;
        ii.hbmMask  = mask;
        icon        = CreateIconIndirect(&ii); // copies both bitmaps
    }
    if (color)
        DeleteObject(color);
    if (mask)
        DeleteObject(mask);
    return icon;
}

std::string encodePng(const Image &img) {
    if (img.empty() || img.pixels.size() < size_t(img.width) * img.height)
        return {};
    Com<IWICImagingFactory> f;
    f.p = wic();
    if (!f)
        return {};
    std::vector<uint32_t> straight(
        img.pixels.begin(), img.pixels.begin() + size_t(img.width) * img.height
    );
    for (auto &p : straight)
        p = core::unpremultiply(p);
    return encodePngBgra(f.p, img.width, img.height, straight.data());
}

std::string bmpFileToPng(std::string_view bmp) {
    if (bmp.size() < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) || bmp.size() > MAXDWORD)
        return {};
    Com<IWICImagingFactory> f;
    f.p = wic();
    if (!f)
        return {};
    // Read in place: a stream over the caller's bytes, no copy.
    Com<IWICStream> in;
    if (FAILED(f->CreateStream(in.out())) ||
        FAILED(in->InitializeFromMemory(
            reinterpret_cast<BYTE *>(const_cast<char *>(bmp.data())), DWORD(bmp.size())
        )))
        return {};
    // WIC's BMP decoder knows every DIB flavour (palettes, bitfields, V5
    // alpha, bottom-up); converting to BGRA gives us plain rows to encode.
    Com<IWICBitmapDecoder>     dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter>   conv;
    UINT                       w = 0, h = 0;
    if (FAILED(
            f->CreateDecoderFromStream(in.p, nullptr, WICDecodeMetadataCacheOnDemand, dec.out())
        ) ||
        FAILED(dec->GetFrame(0, frame.out())) || FAILED(frame->GetSize(&w, &h)) || !w || !h ||
        w > 16384 || h > 16384 || FAILED(f->CreateFormatConverter(conv.out())) ||
        FAILED(conv->Initialize(
            frame.p,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0,
            WICBitmapPaletteTypeCustom
        )))
        return {};
    std::vector<uint32_t> px(size_t(w) * h);
    if (FAILED(conv->CopyPixels(
            nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE *>(px.data())
        )))
        return {};
    return encodePngBgra(f.p, int(w), int(h), px.data());
}

bool writeFile(const std::wstring &path, std::string_view bytes) {
    HANDLE h = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD      written = 0;
    const bool ok      = WriteFile(h, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
                         written == bytes.size();
    CloseHandle(h);
    return ok;
}

// ── badge ───────────────────────────────────────────────────────────────────

namespace {
// 3×5 glyphs, one row per 3-bit value (MSB = left column): 0-9, then '+'.
constexpr uint8_t kGlyphs[11][5] = {
    {7, 5, 5, 5, 7},
    {2, 6, 2, 2, 7},
    {7, 1, 7, 4, 7},
    {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7},
    {7, 1, 1, 1, 1},
    {7, 5, 7, 5, 7},
    {7, 5, 7, 1, 7},
    {0, 2, 7, 2, 0},
};

void drawGlyph(Image &img, int glyph, int x0, int y0, int s, uint32_t argb) {
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 3; ++c)
            if (kGlyphs[glyph][r] & (4 >> c))
                for (int y = y0 + r * s; y < y0 + (r + 1) * s; ++y)
                    for (int x = x0 + c * s; x < x0 + (c + 1) * s; ++x)
                        if (x >= 0 && y >= 0 && x < img.width && y < img.height)
                            img.pixels[size_t(y) * img.width + x] = argb;
}
} // namespace

Image badgeImage(int count, bool dot, int size) {
    Image img{size, size, std::vector<uint32_t>(size_t(size) * size, 0)};
    if ((count <= 0 && !dot) || size <= 0)
        return img;
    // The disc, anti-aliased by 4×4 supersampling; premultiplied red.
    const double r = size / 2.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int in = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const double dx = x + (sx + 0.5) / 4 - r, dy = y + (sy + 0.5) / 4 - r;
                    in += dx * dx + dy * dy <= r * r;
                }
            const uint32_t a = uint32_t(in * 255 / 16);
            // 0xd93025 scaled by coverage.
            img.pixels[size_t(y) * size + x] =
                (a << 24) | ((0xd9 * a / 255) << 16) | ((0x30 * a / 255) << 8) | (0x25 * a / 255);
        }
    const uint32_t white = 0xffffffff;
    if (count <= 0) {
        // The dot: white over the (opaque there) red, a third of the disc
        // across, anti-aliased the same way.
        const double d = r / 3;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                int in = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const double dx = x + (sx + 0.5) / 4 - r, dy = y + (sy + 0.5) / 4 - r;
                        in += dx * dx + dy * dy <= d * d;
                    }
                if (!in)
                    continue;
                uint32_t &px = img.pixels[size_t(y) * size + x];
                auto      ch = [&](int shift) {
                    const uint32_t c = (px >> shift) & 0xff;
                    return (c + (255 - c) * uint32_t(in) / 16) << shift;
                };
                px = (px & 0xff000000U) | ch(16) | ch(8) | ch(0);
            }
        return img;
    }
    const int s     = std::max(1, int(size * 0.625 / 5)); // digit height ≈ 5/8 of the disc
    const int digit = std::min(count, 9);
    if (count <= 9) {
        drawGlyph(img, digit, (size - 3 * s) / 2, (size - 5 * s) / 2, s, white);
    } else {
        // "9+": the plus at half scale, like a superscript, so both fit.
        const int ps = std::max(1, s / 2), w = 3 * s + ps + 3 * ps;
        const int x = (size - w) / 2, y = (size - 5 * s) / 2;
        drawGlyph(img, 9, x, y, s, white);
        drawGlyph(img, 10, x + 3 * s + ps, y, ps, white);
    }
    return img;
}

} // namespace plat::win32
