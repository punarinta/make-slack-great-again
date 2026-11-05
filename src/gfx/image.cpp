// Image decoding: stb_image (PNG, JPEG, GIF) and optionally libwebp, into
// premultiplied ARGB32. The only file that sees decoder types, so the
// Windows/macOS builds can swap it for WIC/ImageIO.
#include "gfx/internal.h"

#include "base/mime.h"

#include "stb/stb_config.h"
#include "stb/stb_image.h"

#ifdef MSGA_GFX_WEBP
#include <webp/decode.h>
#include <webp/demux.h>
#endif

#include <algorithm>
#include <cstdlib>

namespace gfx {

namespace {

// Straight RGBA bytes → premultiplied 0xAARRGGBB.
void premultiplyRgba(const uint8_t *src, uint32_t *dst, size_t n) {
    for (size_t i = 0; i < n; ++i, src += 4) {
        const uint32_t a = src[3];
        if (a == 255)
            dst[i] = 0xff000000u | (uint32_t(src[0]) << 16) | (uint32_t(src[1]) << 8) | src[2];
        else if (a == 0)
            dst[i] = 0;
        else
            dst[i] = (a << 24) | (div255(src[0] * a) << 16) | (div255(src[1] * a) << 8) |
                     div255(src[2] * a);
    }
}

bool tooBig(int w, int h) {
    return w <= 0 || h <= 0 || int64_t(w) * h > kMaxImagePixels;
}

bool isGif(std::string_view b) {
    return b.size() >= 6 && b.substr(0, 4) == "GIF8";
}

#ifdef MSGA_GFX_WEBP
bool decodeWebp(std::string_view bytes, Bitmap *out) {
    int         w = 0, h = 0;
    const auto *d = reinterpret_cast<const uint8_t *>(bytes.data());
    if (!WebPGetInfo(d, bytes.size(), &w, &h) || tooBig(w, h))
        return false;
    uint8_t *rgba = WebPDecodeRGBA(d, bytes.size(), &w, &h);
    if (!rgba)
        return false;
    *out = Bitmap(w, h);
    premultiplyRgba(rgba, out->pixels(), size_t(w) * size_t(h));
    WebPFree(rgba);
    return true;
}

bool isWebp(std::string_view b) {
    return b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP";
}

// Cheap header-only peek
// rejects an oversized or malformed file before the full demux
// same reasoning as decodeWebp's own WebPGetInfo-before-allocate
bool webpFeatures(std::string_view bytes, WebPBitstreamFeatures *f) {
    return WebPGetFeatures(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), f) ==
           VP8_STATUS_OK;
}

// GIPHY can serve a picked GIF as animated WebP, not GIF
// decodeWebp only handles a still frame -- without this it silently
// shows nothing once posted. The caller has checked the canvas size.
bool decodeAnimatedWebp(std::string_view bytes, std::vector<AnimFrame> *out, const AnimOptions &o) {
    WebPData               data{reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()};
    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts))
        return false;
    opts.color_mode            = MODE_RGBA;
    WebPAnimDecoder *const dec = WebPAnimDecoderNew(&data, &opts);
    if (!dec)
        return false;
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info)) {
        WebPAnimDecoderDelete(dec);
        return false;
    }
    const int    cw = int(info.canvas_width), ch = int(info.canvas_height);
    const size_t px       = size_t(cw) * size_t(ch);
    // Shrunk frames go through one canvas-sized scratch, not one each.
    const bool   shrink   = o.width > 0 && o.height > 0 && (cw != o.width || ch != o.height);
    Bitmap       canvas   = shrink ? Bitmap(cw, ch) : Bitmap();
    int64_t      kept     = 0;
    int          prevTsMs = 0;
    bool         over     = false;
    while (WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t *buf     = nullptr;
        int      endTsMs = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &endTsMs))
            break;
        AnimFrame f{shrink ? Bitmap() : Bitmap(cw, ch), 100};
        premultiplyRgba(buf, shrink ? canvas.pixels() : f.frame.pixels(), px);
        if (shrink)
            f.frame = coverResize(canvas.view(), o.width, o.height);
        // endTsMs is cumulative -- the gap from prevTsMs is this frame's delay
        // same 0/10ms-means-unspecified floor as the GIF path
        const int delay = endTsMs - prevTsMs;
        f.delayMs       = delay <= 10 ? 100 : delay;
        prevTsMs        = endTsMs;
        kept += int64_t(f.frame.width()) * f.frame.height();
        if (kept > o.maxPixels && !out->empty()) {
            over = true; // past the budget: the first frame alone, still
            break;
        }
        out->push_back(std::move(f));
    }
    WebPAnimDecoderDelete(dec);
    // Over budget: show frame 0 still, not a partial replay
    // same rule the GIF path below uses
    if (over) {
        out->resize(1);
        out->shrink_to_fit();
    }
    return !out->empty();
}
#endif

} // namespace

bool canDecodeImage(std::string_view head) {
    const std::string_view m = mime::sniff(head);
    if (m == "image/png" || m == "image/jpeg" || m == "image/gif")
        return true;
#ifdef MSGA_GFX_WEBP
    // Still WebP only: an animated one decodes through decodeAnimation
    // alone, and a long recording's frames would cost too much memory for
    // a preview, so it stays a file card.
    WebPBitstreamFeatures f;
    if (m == "image/webp")
        return webpFeatures(head, &f) && !f.has_animation;
#endif
    return false;
}

bool decodeImage(std::string_view bytes, Bitmap *out) {
    if (bytes.empty() || bytes.size() > 0x7fffffff)
        return false;
#ifdef MSGA_GFX_WEBP
    if (isWebp(bytes))
        return decodeWebp(bytes, out);
#endif
    const auto *d = reinterpret_cast<const stbi_uc *>(bytes.data());
    const int   n = int(bytes.size());
    int         w = 0, h = 0, comp = 0;
    // Check the header first so a decompression bomb never gets allocated.
    if (!stbi_info_from_memory(d, n, &w, &h, &comp) || tooBig(w, h))
        return false;
    stbi_uc *rgba = stbi_load_from_memory(d, n, &w, &h, &comp, 4);
    if (!rgba)
        return false;
    *out = Bitmap(w, h);
    premultiplyRgba(rgba, out->pixels(), size_t(w) * size_t(h));
    stbi_image_free(rgba);
    return true;
}

bool decodeAnimation(std::string_view bytes, std::vector<AnimFrame> *out, const AnimOptions &o) {
    out->clear();
    const bool fit = o.width > 0 && o.height > 0;
#ifdef MSGA_GFX_WEBP
    // Same decompression-bomb size cap as the paths below
    // checked before the demux, not after
    if (isWebp(bytes) && bytes.size() <= 0x7fffffff) {
        WebPBitstreamFeatures f;
        if (webpFeatures(bytes, &f) && f.has_animation)
            return !tooBig(f.width, f.height) && decodeAnimatedWebp(bytes, out, o);
    }
#endif
    if (!isGif(bytes) || bytes.size() > 0x7fffffff) {
        Bitmap b;
        if (!decodeImage(bytes, &b))
            return false;
        if (fit && (b.width() != o.width || b.height() != o.height))
            b = coverResize(b.view(), o.width, o.height);
        out->push_back({std::move(b)});
        return true;
    }
    const auto *d = reinterpret_cast<const stbi_uc *>(bytes.data());
    int         w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(d, int(bytes.size()), &w, &h, &comp) || tooBig(w, h))
        return false;
    // stb composites every frame onto the canvas, honouring the disposal
    // methods (none / background / previous); each is premultiplied (and
    // shrunk) as it comes, so only the kept frames add up.
    struct Sink {
        std::vector<AnimFrame> *out;
        const AnimOptions      &o;
        bool                    fit, over = false;
        int64_t                 kept = 0;
    } sink{out, o, fit};
    const auto each = [](void *ctx, const unsigned char *rgba, int fw, int fh, int dm) -> int {
        Sink        &s  = *static_cast<Sink *>(ctx);
        const size_t px = size_t(fw) * size_t(fh);
        AnimFrame    f{Bitmap(fw, fh), 100};
        premultiplyRgba(rgba, f.frame.pixels(), px);
        if (s.fit && (fw != s.o.width || fh != s.o.height))
            f.frame = coverResize(f.frame.view(), s.o.width, s.o.height);
        // Browsers treat 0 and 10 ms as "unspecified" and play them at
        // 100 ms; anything else is kept, so the effective minimum is 20 ms.
        f.delayMs = dm <= 10 ? 100 : dm;
        s.kept += int64_t(f.frame.width()) * f.frame.height();
        if (s.kept > s.o.maxPixels && !s.out->empty()) {
            s.over = true; // past the budget: the first frame alone, still
            return 0;
        }
        s.out->push_back(std::move(f));
        return 1;
    };
    msga_gif_frames(d, int(bytes.size()), each, &sink);
    if (sink.over) {
        out->resize(1);
        out->shrink_to_fit();
    }
    return !out->empty();
}

} // namespace gfx
