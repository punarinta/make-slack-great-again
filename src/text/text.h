// text — fonts, shaping, glyph rasterisation and rich-text layout.
//
// Contract shared by ui/ and app/. Additive changes are fine; changing a
// signature needs the lead. One implementation per OS behind this header:
// Linux = own font index + HarfBuzz + FreeType (colour emoji via CBDT/COLR);
// Windows = DirectWrite, macOS = CoreText (later — keep the API free of
// FreeType/HarfBuzz types).
//
// Text is UTF-8 everywhere; offsets are byte offsets into the UTF-8 string.
// Sizes and positions are logical pixels; layouts are built for a given
// display scale so glyphs are rasterised crisp at physical size.
#pragma once

#include "gfx/gfx.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace text {

// Must be called once before anything else (discovers fonts, builds or loads
// the font index cache). False (with *error) if no usable font exists.
bool init(std::string *error = nullptr);
// At exit (ui::App's destructor): frees the fonts and shaping state. Nothing
// here may be used afterwards, short of init() again.
void shutdown();

enum class Weight : uint16_t { Regular = 400, Medium = 500, Semibold = 600, Bold = 700 };

struct Style {
    float      size        = 15; // logical px
    Weight     weight      = Weight::Regular;
    bool       italic      = false;
    bool       mono        = false; // the system monospace family
    bool       underline   = false;
    bool       strike      = false;
    gfx::Color color       = gfx::rgb(0x1d1c1d);
    gfx::Color background  = 0; // non-zero: filled behind the glyphs (inline code, mentions)
    uint32_t   linkId      = 0; // non-zero: reported by hitTest (links, mentions, channels)
    // Non-zero: this span is one inline box of the given logical size instead
    // of glyphs (custom emoji images, avatars in text); the app paints it at
    // Layout::boxes(). The span's text is ignored for rendering.
    uint32_t   inlineBoxId = 0;
    float      boxWidth = 0, boxHeight = 0;

    bool operator==(const Style &) const = default;
};

// A UTF-8 string with styled spans. Spans must not overlap and must cover the
// whole string (the builder guarantees it).
struct Span {
    uint32_t start = 0, end = 0; // byte offsets
    Style    style;
};
struct AttributedText {
    std::string       text;
    std::vector<Span> spans;
    // Builder: appends text in a style (merges with the previous span when equal).
    void              append(std::string_view utf8, const Style &s);
};

struct LayoutOptions {
    float maxWidth   = 1e9f;  // wrap width, logical
    float lineHeight = 1.4f;  // multiple of the font size (per line: tallest run wins)
    int   maxLines   = 0;     // 0 = unlimited
    bool  ellipsis   = false; // with maxLines: end the last line with "…"
    enum class Align : uint8_t { Left, Center, Right } align = Align::Left;
};

struct InlineBox {
    uint32_t   id;
    gfx::RectF rect; // logical, relative to the layout origin
};

struct HitResult {
    uint32_t offset = 0;     // nearest caret position (byte offset)
    uint32_t linkId = 0;     // link under the point, 0 if none
    bool     inside = false; // the point is over glyphs, not margin
};

class Layout {
public:
    static std::unique_ptr<Layout>
    build(const AttributedText &t, const LayoutOptions &o, float scale);
    // The same, taking over t's text instead of copying it.
    static std::unique_ptr<Layout> build(AttributedText &&t, const LayoutOptions &o, float scale);
    // The same, borrowing `utf8` instead of holding a copy: the caller keeps
    // those bytes alive and unchanged for the layout's whole life (a label
    // that owns both its text and the layout, dropping the layout before the
    // text changes). Long message text is then held once.
    static std::unique_ptr<Layout> buildBorrowed(
        std::string_view utf8, const std::vector<Span> &spans, const LayoutOptions &o, float scale
    );
    virtual ~Layout() = default;

    virtual float width() const            = 0; // logical, tight
    virtual float height() const           = 0;
    virtual int   lineCount() const        = 0;
    virtual float baseline(int line) const = 0; // logical y of a line's baseline
    virtual bool  truncated() const        = 0; // ellipsis/maxLines cut something

    // Paints at `origin` (logical, top-left of the layout box).
    virtual void       paint(gfx::Painter &p, gfx::PointF origin) const                     = 0;
    // paint() with every glyph, underline and strike in `color` (colour
    // glyphs at its opacity) and no span backgrounds: the text as it shows
    // over a selection highlight, from the same shaping.
    virtual void       paintAs(gfx::Painter &p, gfx::PointF origin, gfx::Color color) const = 0;
    // Every span's text colour becomes `color` (paint time only: nothing is
    // reshaped). For one-colour text whose colour follows state (hover,
    // enabled, focus).
    virtual void       setColor(gfx::Color color)                                           = 0;
    // The spans of this link id paint underlined as well (0: none). Paint
    // time only, like setColor: underline doesn't change the shaping.
    virtual void       setUnderlinedLink(uint32_t linkId)                                   = 0;
    // The ink box of the rasterised glyphs (pixels of at least 1/8 coverage;
    // logical, relative to the origin) as painted at an origin on a whole
    // device pixel; empty when nothing draws. For centring a few glyphs by
    // what shows: side bearings are uneven and hinting moves the cap height.
    virtual gfx::RectF inkBounds() const                                                    = 0;
    // How far right of its own ink box's centre each glyph's ink mass sits,
    // averaged over the glyphs (logical; 0 for colour glyphs). A "1"'s flag
    // widens its box to the left of the stem, so centring the box alone puts
    // the stem right of centre; centring on (box centre + lean) centres what
    // the eye weighs.
    // Per glyph, so a light "+" in "99+" does not drag the whole string.
    virtual float      inkLean() const                                                      = 0;
    // Selection highlight between byte offsets, painted behind the text.
    virtual std::vector<gfx::RectF> selectionRects(uint32_t from, uint32_t to) const        = 0;
    // A line's box (logical, relative to the origin): x and w span its
    // visible text (hanging trailing spaces left out), y and h its height.
    virtual gfx::RectF              lineRect(int line) const                                = 0;

    virtual HitResult  hitTest(gfx::PointF p) const                   = 0; // p relative to origin
    virtual gfx::RectF caretRect(uint32_t offset) const               = 0; // 1-px-wide caret box
    virtual uint32_t moveCaret(uint32_t offset, int dx, int dy) const = 0; // arrows (grapheme/line)
    virtual uint32_t wordStart(uint32_t offset) const                 = 0; // double-click selection
    virtual uint32_t wordEnd(uint32_t offset) const                   = 0;
    virtual const std::vector<InlineBox> &boxes() const               = 0;
};

// One run of text in one style, laid out: what most labels and badges need.
std::unique_ptr<Layout>
layoutPlain(std::string_view utf8, const Style &s, float scale, float maxWidth = 1e9f);

// Single-line width. Builds one layout per call: callers keep the answer
// (per scale and text) rather than asking on every pass.
float measure(std::string_view utf8, const Style &s, float scale);

// How many layouts Layout::build has made in this process (all threads): a
// diagnostic for tests proving that a path does not reshape. Counted in test
// and demo builds only (0 otherwise), as is layoutTextOwned().
size_t layoutBuilds();
// How many text bytes Layout::build has stored in layouts of their own
// (copied or moved in; all threads). Borrowed text is not counted: a
// diagnostic for tests proving that a path holds its text once.
size_t layoutTextOwned();

// Font metrics for layout decisions (line boxes, vertical centering).
struct Metrics {
    float ascent = 0, descent = 0, lineGap = 0; // logical
    float capHeight = 0;                        // logical: flat capitals and digits
};
Metrics metrics(const Style &s, float scale);

} // namespace text
