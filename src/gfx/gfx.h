// gfx — the CPU painter every pixel of the app goes through, plus images.
//
// Contract shared by text/ (draws glyphs through blitMask/blitColor), ui/ and
// app/. Additive changes are fine; changing a signature needs the lead.
//
// Pixels: premultiplied ARGB32 in native endianness (0xAARRGGBB as uint32_t),
// the same format as plat::Canvas. Colors in the API are *straight* (not
// premultiplied) 0xAARRGGBB — the painter premultiplies.
//
// Units: the Painter works in logical pixels and applies `scale` (plat's
// physical/logical ratio) itself, so callers never think about DPI. Glyphs and
// other pre-rasterised masks are placed in physical pixels (blitMask/blitColor)
// because they were rasterised at physical size.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gfx {

using Color = uint32_t; // straight 0xAARRGGBB

constexpr Color rgb(uint32_t rgb) {
    return 0xff000000u | rgb;
}
constexpr Color rgba(uint32_t rgb, uint8_t a) {
    return (uint32_t(a) << 24) | (rgb & 0xffffff);
}
Color withAlpha(Color c, float opacity); // multiplies the alpha
// "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa" (the '#' optional, either case;
// a short form doubles each digit). False, *out untouched, for anything else.
bool  parseHexColor(std::string_view s, Color *out);

// "#rrggbb", lower case; the alpha dropped.
std::string hexColor(Color c);

struct PointF {
    float x = 0, y = 0;
};
struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool  contains(PointF p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
};

// A view onto pixels owned elsewhere (a plat::Canvas, a Bitmap, a cache page).
struct BitmapView {
    uint32_t *pixels = nullptr; // premultiplied ARGB32
    int       width = 0, height = 0;
    int       stride = 0; // in pixels
};

class Bitmap {
public:
    Bitmap() = default;
    Bitmap(int w, int h); // cleared to transparent
    int             width() const { return _w; }
    int             height() const { return _h; }
    bool            empty() const { return _w <= 0 || _h <= 0; }
    uint32_t       *pixels() { return _px.data(); }
    const uint32_t *pixels() const { return _px.data(); }
    BitmapView      view() { return {_px.data(), _w, _h, _w}; }
    BitmapView      view() const { return {const_cast<uint32_t *>(_px.data()), _w, _h, _w}; }

private:
    int                   _w = 0, _h = 0;
    std::vector<uint32_t> _px;
};

// 8-bit coverage (a glyph, an anti-aliased shape).
struct Mask8 {
    const uint8_t *data  = nullptr;
    int            width = 0, height = 0, stride = 0; // stride in bytes
};

// Vector path in logical units (icons, custom shapes). Non-zero fill rule.
class Path {
public:
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void close();
    // A full circle as four cubics (clockwise in y-down coordinates).
    void addCircle(float cx, float cy, float r);
    void clear(); // empty again, capacity kept
    bool empty() const { return _cmds.empty(); }

    // Flattened representation the rasterizer consumes.
    enum Cmd : uint8_t { Move, Line, Cubic, Close };
    const std::vector<uint8_t> &cmds() const { return _cmds; }
    const std::vector<float>   &pts() const { return _pts; }

private:
    std::vector<uint8_t> _cmds;
    std::vector<float>   _pts;
};

enum class Sampling : uint8_t { Nearest, Bilinear, Smooth /* area-average when shrinking */ };

enum class FillRule : uint8_t { NonZero, EvenOdd };

// A 2D affine transform in SVG order: x' = a·x + c·y + e, y' = b·x + d·y + f.
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// Linear or radial gradient paint. Colours are interpolated premultiplied
// (like browsers); outside [0, 1] the spread method applies.
struct GradientStop {
    float offset = 0; // 0..1, ascending
    Color color  = 0; // straight
};
struct Gradient {
    enum class Kind : uint8_t { Linear, Radial };
    enum class Spread : uint8_t { Pad, Reflect, Repeat };
    Kind                      kind   = Kind::Linear;
    Spread                    spread = Spread::Pad;
    PointF                    a, b; // Linear: t = 0 at a, 1 at b. Radial: centre a (no focal point)
    float                     radius = 0; // Radial: t = 1 at this distance from a
    // Maps the painter's current (translated, logical) coordinates into the
    // space a/b/radius are given in; identity = the same space.
    Affine                    toGradient;
    std::vector<GradientStop> stops;
};

// Stroke geometry for strokePath(Path, Stroke, …). Dash lengths are in the
// same units as the path.
struct Stroke {
    enum class Cap : uint8_t { Butt, Round, Square };
    enum class Join : uint8_t { Miter, Round, Bevel };
    float        width      = 1;
    Cap          cap        = Cap::Round;
    Join         join       = Join::Round;
    float        miterLimit = 4;
    const float *dashes     = nullptr; // on/off lengths; null or count 0 = solid
    int          dashCount  = 0;
    float        dashOffset = 0;
};

// Working memory for painters made one after another (a window paints a new
// Painter every frame): scratch rows, rasterizer tables and icon geometry
// keep their capacity instead of being reallocated per painter and per fill.
// Serves one painter at a time, on one thread.
class PaintScratch {
public:
    PaintScratch() = default;
    ~PaintScratch();
    PaintScratch(const PaintScratch &)            = delete;
    PaintScratch &operator=(const PaintScratch &) = delete;

    struct Data; // gfx-internal (internal.h)
    Data &data();

private:
    Data *_d = nullptr; // made on first use
};

// Painters are cheap (make one per frame). dropShadow and drawIcon keep
// small process-wide caches (blurred masks, icon coverage), so those two are
// for the UI thread only; everything else may run on any thread with its own
// Painter.
class Painter {
public:
    // Paints into `target` (physical pixels); logical units are multiplied by scale.
    // `scratch` (optional, outlives the painter) lends reusable working memory.
    Painter(BitmapView target, float scale, PaintScratch *scratch = nullptr);

    float scale() const { return _scale; }

    // State stack: transform (translation only) + clip + opacity.
    void  save();
    void  restore();
    void  translate(float dx, float dy);
    void  setOpacity(float o); // multiplies everything drawn until restore()
    // Intersects the current clip. Rounded clips are anti-aliased (avatars,
    // image cards).
    void  clipRect(RectF r);
    void  clipRoundRect(RectF r, float radius);
    RectF clipBounds() const; // logical, in current coordinates

    // Rects whose edges land on whole physical pixels are filled crisp;
    // fractional edges get exact anti-aliased coverage.
    void fillRect(RectF r, Color c);
    void fillRoundRect(RectF r, float radius, Color c);
    // Strokes lie *inside* the shape's outline, like a CSS border: a 1 px
    // stroke of `r` never paints outside `r`, and strokeCircle's outer edge is
    // `radius`.
    void strokeRoundRect(RectF r, float radius, float width, Color c);
    void fillCircle(PointF center, float radius, Color c);
    void strokeCircle(PointF center, float radius, float width, Color c);
    // Butt caps; horizontal/vertical lines go through fillRect (crisp when aligned).
    void drawLine(PointF a, PointF b, float width, Color c);
    void fillPath(const Path &p, Color c);
    void fillPath(const Path &p, Color c, FillRule rule);
    void fillPath(const Path &p, const Gradient &g, FillRule rule = FillRule::NonZero);
    // Stroke centred on the path, round caps and joins (icons, custom shapes).
    void strokePath(const Path &p, float width, Color c);
    void strokePath(const Path &p, const Stroke &s, Color c);
    void strokePath(const Path &p, const Stroke &s, const Gradient &g);
    // Linear gradient from `c0` at `a` to `c1` at `b` (used sparingly: shadows, fades).
    void fillRectGradient(RectF r, PointF a, Color c0, PointF b, Color c1);
    // Soft drop shadow of the rounded box `r` (popups, menus); `blur` is the
    // CSS blur radius, `offset` moves the shadow (not the box). Like CSS
    // box-shadow it is not painted under the box itself — draw the box on top.
    void dropShadow(RectF r, float radius, float blur, Color c);
    void dropShadow(RectF r, float radius, float blur, PointF offset, Color c);

    // Scaled image into `dst` (logical), snapped to whole physical pixels.
    // Smooth shrinking area-averages the whole source on every call: for an
    // image drawn every frame (avatars), draw a cached resize() result.
    void drawBitmap(
        const BitmapView &src, RectF dst, Sampling s = Sampling::Smooth, float opacity = 1.0f
    );
    // The whole physical pixels drawBitmap snaps `dst` to here (w×h): a
    // bitmap resized to exactly that is drawn 1:1, with the pixels a Smooth
    // shrink of the original would give.
    void snappedSize(RectF dst, int *w, int *h) const;

    // Pre-rasterised content at *physical* integer positions relative to the
    // current (translated) origin converted to physical: text uses these.
    void   blitMask(const Mask8 &m, int physX, int physY, Color c);
    void   blitColor(const BitmapView &src, int physX, int physY, float opacity = 1.0f);
    // Current logical translation → physical pixel origin (for text placement).
    PointF toPhysical(PointF logical) const;

private:
    friend struct PainterImpl; // the raster/blend code in the .cpp files
    struct State {
        float tx = 0, ty = 0;
        int   clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0; // physical, rect part
        float opacity   = 1.0f;
        int   roundClip = -1; // index into _roundClips, -1 = none
    };
    BitmapView         _target;
    int                _ox = 0, _oy = 0; // physical position of the target's first pixel
    float              _scale;
    State              _s;
    std::vector<State> _stack;
    struct RoundClip {
        float x, y, w, h, r; // physical
        int   parent;
    };
    std::vector<RoundClip> _roundClips;
    // Scratch rows and tables: the lent PaintScratch, else the painter's own
    // (so painters on different threads never share buffers).
    PaintScratch          *_lent = nullptr;
    PaintScratch           _own;
};

// ── Images ──────────────────────────────────────────────────────────────────
// Decode PNG / JPEG / GIF (first frame) / WebP (when compiled in) into a
// premultiplied Bitmap. False on unsupported or corrupt data.
bool decodeImage(std::string_view bytes, Bitmap *out);
// Whether decodeImage takes a file that starts with `head` (its first bytes,
// 64 are plenty): the format is one this build decodes. Callers show anything
// else as a file, not as a picture that never appears — WebP without libwebp,
// an animated WebP, a BMP.
bool canDecodeImage(std::string_view head);

struct AnimFrame {
    Bitmap frame; // full canvas-sized frame, already composited
    int    delayMs = 100;
};
// How decodeAnimation sizes and bounds what it keeps.
struct AnimOptions {
    // Each frame cover-resized to width×height as it is decoded (as
    // coverResize would after); 0,0 keeps the canvas size.
    int     width = 0, height = 0;
    // The pixels all kept frames may take together (after the resize); an
    // animation past it keeps its first frame only (shown still).
    int64_t maxPixels = 32ll << 20; // 128 MB of ARGB32
};
// Animated GIF or WebP (when compiled in) → composited frames
// decoded one at a time, never the whole animation at canvas size
// a still image yields one frame
bool decodeAnimation(std::string_view bytes, std::vector<AnimFrame> *out, const AnimOptions &o);
inline bool decodeAnimation(std::string_view bytes, std::vector<AnimFrame> *out) {
    return decodeAnimation(bytes, out, AnimOptions{});
}

// High-quality resize (area-average down, bilinear up) — thumbnails, avatars.
Bitmap resize(const BitmapView &src, int width, int height);

// ── Fitting into shapes (cover.cpp) ────────────────────────────────────────
// src covering exactly w×h: its centred part with the target's aspect, then
// one resize (none when that part already is w×h). Empty for empty input.
Bitmap coverResize(const BitmapView &src, int w, int h);
// An SVG rendered at the scale that covers w×h (at least that size; its own
// size when both are <= 0), ready for coverResize. False when not an SVG.
bool   renderSvgCover(std::string_view svg, int w, int h, Bitmap *out);
// Scales premultiplied pixels by the coverage of a rounded rect with corner
// `radius` (a circle when it is half the short side or more), anti-aliased
// over one pixel.
void   maskRoundedRect(Bitmap &b, float radius);
// Clears a disc out of b (rim anti-aliased): the pixels inside become transparent.
void   clearDisc(Bitmap &b, float cx, float cy, float r);

// Decoders refuse images larger than this many pixels (decompression bombs).
constexpr int64_t kMaxImagePixels = 64ll << 20;

// ── SVG ─────────────────────────────────────────────────────────────────────
// Runtime SVG (custom workspace icons, SVG attachments), rendered by gfx's
// own basic renderer on every OS (identical pixels everywhere): shapes, paths, strokes
// (caps, joins, dashes), transforms, opacity, linear/radial gradients,
// clipPath, use. Text, filters, masks, patterns, markers and CSS <style>
// sheets are skipped (the rest still renders). Input is capped at 4 MB.
//
// Intrinsic size in CSS px from width/height, else the viewBox. False when
// the data is not an SVG or declares neither.
bool svgSize(std::string_view svg, float *w, float *h);
// Renders into a width×height premultiplied bitmap, transparent background,
// the viewBox fitted per preserveAspectRatio (default xMidYMid meet).
bool renderSvg(std::string_view svg, int width, int height, Bitmap *out);

// ── Icons ───────────────────────────────────────────────────────────────────
// Icons are compiled from ../gfx/**/*.svg by tools/icons.py into compact path
// data (no SVG renderer in the binary). Draw one into `r` (logical), tinted.
// The viewBox is fitted into `r` (centred, aspect kept) with its origin
// snapped to a whole physical pixel so strokes stay crisp. Parts drawn in
// currentColor take `tint`; the few fixed-colour parts (the Slack mark, the
// grey spinner arrows) keep their colour and only take tint's alpha.
enum class Icon : uint16_t; // generated: gfx/icons_generated.h (also kIconCount)
void drawIcon(Painter &p, Icon icon, RectF r, Color tint);
// The same turned clockwise by `degrees` about r's centre (a spinner).
void drawIconRotated(Painter &p, Icon icon, RectF r, Color tint, float degrees);

} // namespace gfx
