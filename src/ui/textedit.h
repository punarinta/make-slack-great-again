// TextEdit: the composer. Multi-line rich text (bold/italic/strike/code and
// links as spans), selection with mouse and keyboard, undo/redo, clipboard
// with text/html, IME preedit, caret blink, grows with its content up to
// maxLines and scrolls inside after that.
#pragma once

#include "ui/view.h"
#include "ui/widgets.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

class TextEdit : public View {
public:
    // Character formats (bits). Links are separate: a per-run URL.
    enum Format : uint8_t { Bold = 1, Italic = 2, Strike = 4, Code = 8 };

    // A maximal run of equally formatted text (for serialising to mrkdwn).
    struct Run {
        uint32_t         start = 0, end = 0; // byte offsets into text()
        uint8_t          format = 0;
        std::string_view link; // empty = not a link
    };

    TextEdit();
    ~TextEdit() override;

    // ── Content ─────────────────────────────────────────────────────────────
    const std::string &text() const { return _text; } // plain UTF-8, '\n' between lines
    bool               empty() const { return _text.empty(); }
    void               setText(std::string_view plain); // clears formats and undo history
    void               clear() { setText({}); }
    std::vector<Run>   runs() const;
    // Replace the selection (typing formats apply) / insert parsed HTML.
    void               insertText(std::string_view utf8);
    // Replace a byte range with what the owner rewrote it into — a smiley
    // finished into its emoji — as one undo step of its own, in the formatting
    // being typed. The caret keeps its place in the text around the range.
    void               replaceTyped(uint32_t from, uint32_t to, std::string_view utf8);
    void               insertHtml(std::string_view html);
    // The selection (or everything when there is none) as an HTML fragment.
    std::string        html() const;

    // ── Selection ───────────────────────────────────────────────────────────
    uint32_t    caret() const { return _caret; }
    uint32_t    anchor() const { return _anchor; }
    bool        hasSelection() const { return _caret != _anchor; }
    void        setSelection(uint32_t anchor, uint32_t caret);
    void        selectAll();
    std::string selectedText() const;

    // ── Formatting ──────────────────────────────────────────────────────────
    // With a selection: toggles the format over it (undoable). Without one:
    // toggles the format of what is typed next.
    void toggleFormat(Format f);
    bool formatActive(Format f) const; // for toolbar button state
    void setLink(std::string url);     // on the selection; empty removes links

    // ── Behaviour ───────────────────────────────────────────────────────────
    void setPlaceholder(std::string s);
    void setMinLines(int n);
    void setMaxLines(int n); // 0 = grow without limit
    void setFont(Font f);
    // Links drawn as pills on this background (C::None: plain link colour).
    void setLinkBackground(C c);
    // Paste only text/plain, never rich text (the composer).
    void setPlainPaste(bool on) { _plainPaste = on; }
    // Password-style display: every byte shows as a bullet, no IME, no copy.
    void setMasked(bool on);
    bool masked() const { return _masked; }
    // At most n code points (0: no limit): typing and pastes past it are
    // cut to what fits, as one edit (undo steps over it as usual).
    void setMaxLength(int n);

    // Enter without Shift: return true to consume (send), false inserts a newline.
    std::function<bool()>              onSubmit;
    // Every KeyDown first: return true to consume (mention popup navigation,
    // Up in an empty composer to edit the last message, …).
    std::function<bool(const Event &)> onKey;
    std::function<void()>              onChange;          // text or formats changed
    std::function<void()>              onSelectionChange; // caret/selection moved
    std::function<void(bool focused)>  onFocusChange;
    // Before a paste inserts text: the clipboard's MIME types (plat's
    // normalised names) and which clipboard (Primary: a middle click on
    // Linux). Return true when the owner takes the paste (files, a picture:
    // the composer attaches them); it then reads what it needs itself
    // from that selection, and may still call pasteText() when that turns
    // out to be text.
    std::function<bool(const std::vector<std::string> &mimes, plat::Selection sel)> onPasteMedia;

    // ── Squiggles (the composer's misspelled words) ─────────────────────────
    // Wavy underlines under byte ranges of text(). They follow edits; one an
    // edit touches (or runs up against) is dropped until the owner sets new
    // ones. While focused, the one the caret is typing in (the last caret
    // change was an edit, the caret inside or at the end of it) is not drawn:
    // a word being typed isn't wrong yet. Moving the caret onto one shows it.
    struct Range {
        uint32_t from = 0, to = 0;
        bool     operator==(const Range &o) const { return from == o.from && to == o.to; }
    };
    void                      setSquiggles(std::vector<Range> ranges);
    const std::vector<Range> &squiggles() const { return _squiggles; }
    // The squiggle at `offset` (inside it or at either end), else null.
    const Range              *squiggleAt(uint32_t offset) const;
    // Drawn now (not the one the caret is typing in).
    bool                      squiggleShown(const Range &r) const;

    // ── Context menu ────────────────────────────────────────────────────────
    // Right-click at text offset `offset`: return true when the owner shows a
    // menu of its own (the composer's spelling suggestions, above the
    // standard items, which standardMenuItems / runStandardItem provide).
    std::function<bool(uint32_t offset, PointF windowPos)> onContextMenu;
    std::vector<MenuItem>                                  standardMenuItems() const;
    void                                                   runStandardItem(int id);
    // Ids the standard items use: an owner's own start here.
    static constexpr int                                   kFirstOwnerMenuId = 100;

    bool undo();
    bool redo();
    bool canUndo() const { return !_undo.empty(); }
    void copy();
    void cut();
    void paste(bool plainText = false, plat::Selection sel = plat::Selection::Clipboard);
    // paste() without onPasteMedia
    void pasteText(bool plainText = false, plat::Selection sel = plat::Selection::Clipboard);

    const std::string &preedit() const { return _preedit; }
    RectF              caretRect() const; // local coordinates
    // The caret is on the first (top) / last visual line: Up / Down can't
    // move it to another line (the composer's prompt history steps from there).
    bool               caretOnEdgeLine(bool top) const;

    SizeF       measureContent(float availW, float availH) override;
    void        layout() override;
    void        paint(gfx::Painter &p) override;
    bool        onEvent(Event &e) override;
    uint8_t     cursorAt(PointF) const override { return uint8_t(plat::Cursor::IBeam); }
    void        styleChanged() override;
    void        windowChanged() override;
    std::string accessibleName() const override { return _placeholder; }

private:
    enum class EditKind : uint8_t { Other, Typing, Backspace, DeleteForward, Format };
    struct Edit {
        uint32_t              pos = 0;
        std::string           removed, inserted;
        std::vector<uint16_t> removedFmt, insertedFmt;
        uint32_t              anchorBefore = 0, caretBefore = 0, anchorAfter = 0, caretAfter = 0;
        double                time = 0;
        EditKind              kind = EditKind::Other;
    };

    void replace(
        uint32_t         from,
        uint32_t         to,
        std::string_view ins,
        const uint16_t  *fmt,
        uint16_t         fillFmt,
        EditKind         kind
    );
    void     apply(const Edit &e, bool reverse);
    void     deleteSelection(EditKind kind = EditKind::Other);
    void     moveTo(uint32_t caret, bool extend);
    void     contentChanged();
    void     selectionChanged();
    void     ensureCaretVisible();
    void     updateIme();
    void     startBlink();
    void     stopBlink();
    void     setPrimarySelection();
    uint16_t typingFormat() const;

    // The text as paragraphs (split at '\n'), each laid out on its own, so
    // an edit reshapes only the paragraphs it touches.
    struct Para {
        uint32_t                      start = 0, len = 0; // model bytes; len without the break
        uint8_t                       brk = 0; // break bytes after it: 0 (last), 1 "\n", 2 "\r\n"
        float                         top = 0; // logical y in the document
        std::unique_ptr<text::Layout> layout;  // null: to build
    };
    void splitParas(); // all of them anew from _text
    void scanParas(uint32_t from, uint32_t end, bool last, std::vector<Para> &out) const;
    void editParas(uint32_t pos, size_t removed, size_t inserted);
    void dropParaAt(uint32_t modelPos);
    void dropLayouts();
    std::unique_ptr<text::Layout> buildPara(const Para &p, float w) const;
    bool                          holdsPreedit(const Para &p) const;
    uint32_t                      paraStart(size_t i) const; // display offsets
    uint32_t                      paraLen(size_t i) const;
    size_t                        paraAt(uint32_t display) const;
    size_t                        paraAtY(float y) const;
    // The document over the paragraphs, in display offsets and content
    // coordinates: what one Layout of the whole text answers.
    float                         docHeight() const;
    RectF                         docCaretRect(uint32_t d) const;
    uint32_t                      docHitTest(PointF p) const;
    uint32_t                      docMoveCaret(uint32_t d, int dx, int dy) const;
    uint32_t                      docWordStart(uint32_t d) const;
    uint32_t                      docWordEnd(uint32_t d) const;
    // Only paragraphs that reach between top and bottom (document y).
    std::vector<RectF>
    docSelectionRects(uint32_t from, uint32_t to, float top = -1e30f, float bottom = 1e30f) const;

    void        layoutFor(float contentWidth);
    void        currentLayout();
    float       contentWidth() const;
    float       lineHeight() const;
    text::Style baseStyle(C c) const;
    float       clampedHeight() const;
    uint32_t    toDisplay(uint32_t m) const;
    uint32_t    toModel(uint32_t d) const;
    uint32_t    hitOffset(PointF local);
    uint32_t    prevChar(uint32_t o) const; // code point boundaries
    uint32_t    nextChar(uint32_t o) const;
    uint32_t    wordLeft(uint32_t o);
    uint32_t    wordRight(uint32_t o);
    uint32_t    lineEdge(uint32_t o, bool end);
    void        showContextMenu(PointF local);
    void        shiftSquiggles(uint32_t pos, size_t removed, size_t inserted);
    void        placeWaves();
    void        paintSquiggles(gfx::Painter &p);

    // A squiggle's run along one line (document coordinates).
    struct Wave {
        float    x, y, w;
        uint32_t squiggle; // index into _squiggles
    };

    std::string                   _text;
    std::vector<uint16_t>         _fmt;   // per byte: format bits | link index << 8
    std::vector<std::string>      _links; // link index - 1
    std::vector<Range>            _squiggles;
    std::vector<Wave>             _waves; // placeWaves(), while _wavesValid
    std::string                   _preedit, _placeholder;
    std::vector<Edit>             _undo, _redo;
    std::vector<Para>             _paras;
    std::unique_ptr<text::Layout> _placeholderLayout;
    std::shared_ptr<char>         _alive; // guards async clipboard callbacks
    float                         _layoutW = -1, _scrollY = 0;
    uint32_t                      _caret = 0, _anchor = 0, _preeditPos = 0;
    uint32_t                      _selOriginA = 0, _selOriginB = 0;
    uint32_t                      _maxLength     = 0; // code points; 0 = none
    int                           _preeditCursor = -1;
    plat::TimerId                 _blinkTimer    = 0;
    uint16_t                      _typing        = 0;
    uint16_t                      _blinkFlips    = 0; // since startBlink()
    uint8_t                       _minLines = 1, _maxLines = 8;
    uint8_t                       _selMode   = 0; // 0 char, 1 word, 2 line (drag granularity)
    Font                          _font      = Font::Body;
    bool                          _typingSet = false, _caretOn = true, _dragging = false;
    bool                          _caretTyped = false; // the caret last moved by an edit
    bool                          _masked = false, _plainPaste = false;
    bool                          _parasDirty = true; // a layout to build or tops to place
    bool                          _wavesValid = false;
    C                             _linkBg     = C::None;
};

// HTML fragments ↔ formatted text (the clipboard's text/html; also handy for
// the app's mrkdwn bridge). fmt/link encoding as in TextEdit: one uint16 per
// byte, format bits | (link index + 1) << 8 into `links`.
namespace rich {
std::string
toHtml(std::string_view text, const uint16_t *fmt, const std::vector<std::string> &links);
void fromHtml(
    std::string_view          html,
    std::string              *text,
    std::vector<uint16_t>    *fmt,
    std::vector<std::string> *links
);

// The forgiving tag scanner fromHtml reads with (the app's canvas reader
// too). Not an HTML parser: a tag's name and attribute text, nothing more.
struct Tag {
    std::string_view name;
    // The name and its attributes (after a closing '/'); empty: a comment,
    // <!…>, <?…>, <> or </>.
    std::string_view text;
    bool             closing = false;
};
// Reads the tag at html[at] (a '<') into *tag; returns the offset past it,
// or npos when it never closes (an unclosed comment runs to the end).
size_t           readTag(std::string_view html, size_t at, Tag *tag);
// True when the tag carries nothing to act on: a comment or declaration, or
// script/style content (with `documentParts`, head and title too): its tags,
// and every tag inside one (*depth counts them; the text between is skipped
// while it is above 0).
bool             skippedTag(const Tag &tag, int *depth, bool documentParts);
// An attribute's raw value (entities not decoded) in a tag's text, "" if
// absent. Forgiving: any case, spaces around '=', unquoted values, an
// unclosed quote to the end. `strict` wants exactly name="…" or name='…',
// closed.
std::string_view tagAttr(std::string_view tag, std::string_view name, bool strict = false);
} // namespace rich

} // namespace ui
