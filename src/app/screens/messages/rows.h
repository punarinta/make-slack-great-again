// The rows of a MessageList (internal to screens/messages). A row is rebuilt
// from the Store on every bind — but for its body (text, blocks, files,
// attachments), kept when only the message's reactions, replies, pin or
// saved state changed; ~20 are on screen and a few dozen more kept built off
// screen for scrolling back (VirtualList::setKeep), the rest recycled per
// kind. A row holds views and a fingerprint of what its body was built from,
// never a second copy of a message's text.
#pragma once

#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"

#include <string>
#include <vector>

namespace screens {

enum RowKind : int { kRowFull, kRowGrouped, kRowDay, kRowSystem, kRowDivider };

// Children laid out left to right, wrapping onto new lines (reaction pills).
class FlowRow : public ui::View {
public:
    explicit FlowRow(float gap) : _gap(gap) {}
    ui::SizeF measureContent(float availW, float availH) override;
    void      layout() override;

private:
    ui::SizeF place(float width, bool apply);
    float     _gap;
};

// The plain file chip (the message list's, the forward preview's).
ui::Clickable *addFileChip(ui::View *parent, const model::File &f, MessageList *list, Ts ts);
// A file card's frame: the file chip fill under a 1-px border (file chips,
// canvas and audio cards, message previews).
void           paintCardFrame(
    gfx::Painter &p, ui::RectF r, float radius = 8, ui::C border = ui::C::FileChipBorder
);

class SelectableText;

// A huddle row's sentence ("Mira and Jonas were in
// the huddle for 1h 5m.").
std::string huddleSummaryText(const Store &st, const model::Huddle &h);
// A call card's two lines:
// - under its name: "Started 5 minutes ago" or "Ended · lasted 12m"
// - by its Join: who is in it ("Waiting for people to join"), or once
//   ended who was ("" for nobody)
std::string callStartedText(const model::Call &c, int64_t now);
std::string callStatusText(const Store &st, const model::Call &c);
// Its people's avatars' alpha: faded once it has ended, as Slack greys them
// (msga's inactive look is a fade; it has no greyscale filter).
float       callAvatarAlpha(const model::Call &c);

// The texts a message's selection runs over (MessageRow::selectionLabels'),
// for rows not on screen: its body, or its text blocks.
std::vector<std::string> selectableTexts(Context &ctx, const model::Message &m);

// A canvas card: its title as shown, and the start of the document as text — headings larger, list
// markers, inline formats, member mentions as chips with names, emoji (custom
// ones as inline boxes, box id i is (*images)[i - 1]); no pictures.
std::string          canvasTitle(const Context &ctx, const model::File &f);
text::AttributedText canvasPreviewText(
    Context &ctx, const std::string &html, const model::File &f, std::vector<std::string> *images
);

class MessageRow : public ui::View {
public:
    MessageRow(MessageList &list, int kind);
    ~MessageRow() override;

    void                                 bind(const MessageList::Item &item);
    // Shown again as built (the list kept it, its message unchanged): false
    // when its text went stale with the clock (a reminder due, the day).
    bool                                 reuse();
    Ts                                   ts() const { return _ts; }
    // The message's text labels (and tables) a selection runs over, in order.
    const std::vector<SelectableText *> &selectionLabels() const { return _sel; }
    // An attachment card under the pointer (index), for the "×" in the
    // gutter beside it (the dismiss button on link previews).
    void                                 attachHovered(int index, ui::View *card, bool on);

    void        paint(gfx::Painter &p) override;
    void        paintOver(gfx::Painter &p) override;
    bool        hasLiveOverlay() const override { return _attach >= 0; }
    void        styleChanged() override;
    bool        onEvent(ui::Event &e) override;
    std::string tooltip() const override;
    ui::RectF   tooltipAnchor() const override;
    bool        tooltipImmediate() const override { return true; }

private:
    void      buildMessage(const model::Message &m, bool grouped);
    // The body as built still shows `m` (see bind): only the rest is rebuilt.
    bool      bodyHolds(const model::Message &m) const;
    void      rememberBody(const model::Message &m);
    // The banners' texts and the row's padding (they take room above it).
    void      applyBanners(const model::Message &m, bool grouped);
    // What follows the body: reactions, the reply bar, an inline thread.
    void      buildTail(ui::View *col, const model::Message &m);
    // The author's avatar (the profile card on hover) or, for a bot, its
    // picture over its name's letter.
    ui::View *addAvatar(ui::View *parent, const model::Message &m);
    void      buildSystem(const model::Message &m);
    void      buildHeader(ui::View *col, const model::Message &m, bool tight);
    // The message's content under the header: body or blocks, buttons,
    // files, attachments (index ≥ 0 cards get the dismiss "×"), reactions.
    void      buildContent(ui::View *col, const model::Message &m, bool root);
    void      buildBlocks(
        ui::View                      *col,
        const model::Block            *blocks,
        size_t                         count,
        Ts                             ts,
        int                            attachment,
        bool                           edited,
        std::vector<SelectableText *> *labels
    );
    void      buildReactions(ui::View *col, const model::Message &m);
    void      buildThreadSummary(ui::View *col, const model::Message &m);
    void      buildInlineThread(ui::View *col, const model::Message &root);
    void      buildFile(ui::View *col, const model::File &f, Ts ts);
    void      buildGallery(ui::View *col, const std::vector<const model::File *> &files, Ts ts);
    // Pictures first (gallery or single), then the rest via buildFile.
    // Shared by the message body and a forwarded message's own files.
    void      buildFilesSection(ui::View *col, const std::vector<model::File> &files, Ts ts);
    void      buildAttachment(ui::View *col, const model::Message &m, size_t index, bool root);
    // The buttons of attachment `owner` (1-based; 0 = the message's own) as a row.
    void      buildButtons(ui::View *col, const model::Message &m, int32_t owner);
    void      buildUnfurl(ui::View *col, const model::Message &m, size_t index);
    // A call block's card (MessageExtras::call), its Join inside.
    void      buildCallCard(ui::View *col, const model::Message &m);
    ui::RectF dismissRect() const; // local; empty while no card is hovered
    // The gutter strip from the "×" to the card, as tall as the card: the
    // pointer on its way to the "×" (the 4-px gap, a diagonal) keeps it shown.
    ui::RectF dismissReach() const;
    bool      dismissable(int index) const;
    // `file`: an uploaded image (a placeholder box and "Loading image…"
    // while it loads); else a link preview's picture (nothing until then).
    ui::View *addThumb(
        ui::View          *col,
        const std::string &path,
        int                w,
        int                h,
        int                maxW,
        int                maxH,
        float              gapAbove = 6,
        bool               file     = false
    );

    MessageList                  &_list;
    std::string                   _hoverTime; // grouped rows: the time shown in the gutter on hover
    std::unique_ptr<text::Layout> _hoverLayout;
    // The mini-banners over the message: "Pinned by …" and the saved /
    // reminder strip (text; laid out when painted).
    std::string                   _pinText, _savedText;
    std::unique_ptr<text::Layout> _pinLayout, _savedLayout;
    std::unique_ptr<text::Layout> _dismissLayout; // the attachment "×"
    gfx::Color                    _dismissColor = 0;
    float                         _dismissScale = 0;
    std::vector<SelectableText *> _sel;
    ui::View                     *_attachCard = nullptr; // the hovered attachment card
    int                           _attach     = -1;      // … its index
    Ts                            _ts         = 0;
    int64_t                       _dueAt = 0; // an upcoming reminder, a call's next minute (secs)
    int64_t                       _day   = 0; // local day when built
    bool                          _reminded = false;
    int                           _kind;
    bool                          _pending = false, _overDismiss = false;

    // The body's column and where its tail starts (null: not a message's).
    // What the body was built from: the text's hash (with author, edited,
    // pending), the extras, and the list's epoch then.
    ui::View                             *_col    = nullptr;
    size_t                                _tailAt = 0;
    std::unique_ptr<model::MessageExtras> _bodyExtras;
    uint64_t                              _bodyHash  = 0;
    uint32_t                              _bodyEpoch = 0;

    friend class MessageList; // links every row of the list (MessageList::_rows)
    MessageRow *_prevRow = nullptr, *_nextRow = nullptr;
};

// "Today" pill on a hairline.
class DayRow : public ui::View {
public:
    DayRow();
    void setText(std::string s) { _label->setText(std::move(s)); }
    void paint(gfx::Painter &p) override;

private:
    ui::Label *_label;
};

// Thread mode: "3 replies ———".
class DividerRow : public ui::View {
public:
    DividerRow();
    void setText(std::string s) { _label->setText(std::move(s)); }

private:
    ui::Label *_label;
};

} // namespace screens
