// The inline data table (with the list's "Open full table" pill): a Block Kit table in a message,
// at most kMaxInlineTableRows rows, row 0 the header in bold. No vertical lines: a rounded 1-px
// frame, a tinted header with a hairline under it, rules between rows. Columns take their natural
// width and shrink in proportion (wrapping) when the message column is narrower. When rows were cut
// (the last one shown dimmed) or columns squeezed, hovering the table shows the "Open full table"
// pill, which opens the table viewer.
#pragma once

#include "app/screens/messages/context_fwd.h"
#include "app/screens/messages/rich.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace screens {

class EmojiFrameTimer;

constexpr int kMaxInlineTableRows = 10;

// What a selection copies from an inline table (the rows it shows): the
// cells' text '\t' apart, rows '\n' apart — a grid when pasted into a
// spreadsheet. TableView's selectable text.
std::string tableText(Context &ctx, const std::vector<std::vector<std::string>> &cells);

// The table itself, for TableView and the table viewer (message_dialogs.h):
// cells shaped once at their natural width, columns shrunk in proportion
// (wrapping; only the cells that no longer fit are shaped again) when the
// width is short of them, rows as tall as their tallest cell; drawn with the
// header tint, the rules and the frame, only the rows in view.
class TableGrid {
public:
    // The cells' text, row-major (rows may be short), shaped at `scale`.
    void setCells(std::vector<std::vector<text::AttributedText>> cells, float scale);
    // Columns for `width` (logical px): a squeezed one keeps at least
    // minColW; a row without text is emptyRowH high (plus the padding).
    void fit(float width, float minColW, float emptyRowH);
    // From y0 down in view `v`'s coordinates, the rows that meet [top,
    // bottom) only. With a Context, the cells' custom emoji boxes too (box id
    // i is images[i - 1]; `anim` repaints v for their next frame).
    void paint(
        gfx::Painter                   &p,
        ui::View                       &v,
        float                           y0,
        float                           top,
        float                           bottom,
        Context                        *ctx,
        const std::vector<std::string> &images,
        EmojiFrameTimer                *anim
    ) const;
    bool  empty() const { return _texts.empty(); }
    float scale() const { return _scale; }
    float naturalWidth() const { return _naturalW; } // every column at its natural width
    float width() const { return _tableW; }
    float height() const { return _tableH; }
    bool  squeezed() const { return _squeezed; }

    // The cells' text as one string (tableText): a highlighted range of it,
    // and hit tests (after fit; y from the table's top).
    void     setSelection(uint32_t from, uint32_t to);
    uint32_t textSize() const { return _textSize; }
    uint32_t offsetAt(ui::PointF p) const;
    void     wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const;
    void     rowAt(float y, uint32_t *from, uint32_t *to) const;

    // The cells' links (richText's link ids, numbered across the table): the
    // one under a point (0: none; after fit; y from the table's top) with
    // its cell's box, a link's text, and the one painted underlined (the
    // hovered URL link; 0: none).
    uint32_t    linkAt(ui::PointF p, ui::RectF *cellBox) const;
    std::string linkText(uint32_t id) const;
    void        setUnderlinedLink(uint32_t id);

private:
    const text::Layout &cell(size_t r, size_t c) const {
        return _wrapped[r][c] ? *_wrapped[r][c] : *_natural[r][c];
    }
    size_t rowIndexAt(float y) const;
    float  rowTop(size_t r) const;
    // The column under x in row r (past a short row's end: its last cell),
    // and its left edge.
    size_t colAt(size_t r, float x, float *left) const;

    std::vector<std::vector<text::AttributedText>>          _texts;
    std::vector<std::vector<std::unique_ptr<text::Layout>>> _natural, _wrapped;
    std::vector<float>                                      _colW, _natW, _rowH;
    std::vector<std::vector<uint32_t>>                      _base; // each cell's text offset
    float    _scale = 0, _naturalW = 0, _tableW = 0, _tableH = 0;
    uint32_t _textSize = 0, _selFrom = 0, _selTo = 0, _underlined = 0;
    bool     _squeezed = false;
};

class TableView : public ui::View, public SelectableText {
public:
    // cells: mrkdwn, row-major.
    TableView(Context &ctx, std::vector<std::vector<std::string>> cells);
    ~TableView() override;

    // Message text the list selects across (tableText): the I-beam over it.
    void setSelectable(bool on) { _selectable = on; }

    ui::View &textView() override { return *this; }
    uint32_t  textSize() const override { return _textSize; }
    void      selectText(uint32_t from, uint32_t to) override;
    uint32_t  textOffsetAt(ui::PointF local) const override;
    void      wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const override;
    void      lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const override;

    // The pill was pressed (default: the table viewer with every row, the
    // cells rich as here).
    std::function<void()> onOpenFull;

    bool      clipped();        // rows cut or columns squeezed (the pill's condition)
    ui::RectF pillRect() const; // local; empty unless clipped and hovered

    ui::SizeF   measureContent(float availW, float availH) override;
    void        layout() override;
    void        paint(gfx::Painter &p) override;
    void        paintOver(gfx::Painter &p) override;
    bool        onEvent(ui::Event &e) override;
    uint8_t     cursorAt(ui::PointF local) const override;
    void        styleChanged() override;
    // The header cells, " | " between them.
    std::string accessibleName() const override;
    // Link hover: the URL above the pointer, at once (RichLabel's).
    std::string tooltip() const override { return _hover.tip(); }
    ui::RectF   tooltipAnchor() const override { return _hover.anchor(); }
    bool        tooltipImmediate() const override { return true; }

private:
    void     build(float width);
    // The link under a local point (0: none) and its cell (window coordinates).
    uint32_t linkAt(ui::PointF local, ui::RectF *cell = nullptr) const;

    Context                                       &_ctx;
    std::vector<std::vector<std::string>>          _cells;
    TableGrid                                      _grid;
    std::vector<std::string>                       _images;  // emoji box id i: [i - 1]
    std::vector<LinkTarget>                        _targets; // link id i: [i - 1]
    TargetHover                                    _hover;
    uint32_t                                       _pressedLink = 0;
    // The cells as the constructor made them, for the first build().
    std::vector<std::vector<text::AttributedText>> _made;
    std::unique_ptr<EmojiFrameTimer>               _anim;
    std::unique_ptr<text::Layout>                  _pillText;
    float                                          _builtW   = -1;
    uint32_t                                       _textSize = 0, _selFrom = 0, _selTo = 0;
    bool                                           _overPill = false, _selectable = false;
};

} // namespace screens
