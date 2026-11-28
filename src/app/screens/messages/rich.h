// mrkdwn → views: message bodies as a column of blocks (paragraphs, list
// items with hanging markers, code blocks, quoted runs with a bar), with
// mentions/channels/@here as pills, links, Unicode emoji as text and custom
// emoji as inline image boxes. Used by the message rows and anything else
// that shows message text (unfurl text, attachment fields).
#pragma once

#include "app/mrkdwn/mrkdwn.h"
#include "app/screens/messages/context_fwd.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {
class Button;
}

namespace screens {

// A piece of message text the message list selects across (RichOptions::
// labels, MessageRow::selectionLabels): a body label, or a data table's cells
// (TableView). Offsets are bytes of its text (selectableTexts' part).
class SelectableText {
public:
    virtual ui::View   &textView()                             = 0; // its geometry
    virtual uint32_t    textSize() const                       = 0;
    // The highlighted range (from == to: none).
    virtual void        selectText(uint32_t from, uint32_t to) = 0;
    // The offset nearest to a point in textView()'s coordinates.
    virtual uint32_t    textOffsetAt(ui::PointF local) const   = 0;
    // Double click: the word around an offset; triple click: the line
    // (a table's row) under a point. [*from, *to).
    virtual void        wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const  = 0;
    virtual void        lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const = 0;
    // What a click at a window point copies: the inline code under it, or a
    // whole code block; "" for none.
    virtual std::string codeAt(ui::PointF) const { return {}; }

protected:
    ~SelectableText() = default;
};

// What a link id in message text points at.
struct LinkTarget {
    mrkdwn::Kind kind;
    std::string  data; // URL, user id, conversation id, …
};

// The link a view's pointer is over (RichLabel's, a table's): its URL
// tooltip, and the profile card after a delay over a mention.
class TargetHover {
public:
    // The pointer moved to `windowPos`, over link id `id` of `targets` (0:
    // none). `shown` gives the link's text, asked when the link changes (no
    // tooltip when it already is the URL); `card` is where a profile card
    // anchors (window coordinates). True when the link changed: the view
    // underlines underlined() and rearms its tooltip.
    bool move(
        Context                            &ctx,
        const std::vector<LinkTarget>      &targets,
        uint32_t                            id,
        ui::PointF                          windowPos,
        ui::RectF                           card,
        const std::function<std::string()> &shown
    );
    // New content: no link hovered (a profile card shown stays).
    void clear() {
        _link = _underlined = 0;
        _tip.clear();
    }
    // The hovered link when it is a URL link (mentions, channels and message
    // links never underline); 0: none.
    uint32_t           underlined() const { return _underlined; }
    const std::string &tip() const { return _tip; }
    ui::PointF         at() const { return _at; }                          // the pointer
    ui::RectF          anchor() const { return {_at.x, _at.y - 2, 1, 4}; } // the tooltip's

private:
    std::string    _tip;
    ui::PointF     _at;
    ui::RectF      _card;
    uint32_t       _link = 0, _underlined = 0;
    model::UserRef _user = model::kNoUser;
};

// A click on a link target: a URL opens (openLink; its toast at `at`), a
// message link jumps to the message, a channel opens, a mention shows the
// profile card at `card` (window coordinates) at once.
void activateTarget(
    Context &ctx, const LinkTarget &tg, ui::Window *w, ui::PointF at, ui::RectF card
);
// A right click on a link target: the link menu (a URL link's, a message
// link's permalink). False: none for this target.
bool showTargetMenu(Context &ctx, const LinkTarget &tg, ui::Window &w, ui::PointF windowPos);

// A Label that knows what its links point at and paints custom emoji boxes.
// Clicks go through the Context (openUrl / openConversation / openProfile).
class RichLabel : public ui::Label, public SelectableText {
public:
    using Target = LinkTarget;
    RichLabel(Context &ctx, ui::View *imageWaiter);
    ~RichLabel() override;

    // Replaces the text; targets[i] is linkId i + 1, images[i] is box id i + 1.
    void setContent(
        text::AttributedText t, std::vector<Target> targets, std::vector<std::string> images
    );
    void          activate(uint32_t linkId);
    // The target under a local point (null: plain text).
    const Target *targetAt(ui::PointF local) const;

    // Message text the list selects across: the I-beam over plain text.
    void setSelectable(bool on) { _selectable = on; }
    bool selectable() const { return _selectable; }
    // The text of a code block (filling the block's box): a click anywhere
    // on it copies all of it.
    void setCodeBlock(bool on) { _codeBlock = on; }

    ui::View   &textView() override { return *this; }
    uint32_t    textSize() const override { return uint32_t(text().size()); }
    void        selectText(uint32_t from, uint32_t to) override { setSelection(from, to); }
    uint32_t    textOffsetAt(ui::PointF local) const override { return offsetAt(local); }
    void        wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const override;
    void        lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const override;
    std::string codeAt(ui::PointF windowPos) const override;

    void        paint(gfx::Painter &p) override;
    // Hover on a mention or name: the profile card; right click on a link:
    // the link menu.
    bool        onEvent(ui::Event &e) override;
    uint8_t     cursorAt(ui::PointF local) const override;
    void        windowChanged() override;
    // Link hover: the URL above the pointer, at once (none when the
    // link's text is the URL itself).
    std::string tooltip() const override { return _hover.tip(); }
    ui::RectF   tooltipAnchor() const override;
    bool        tooltipImmediate() const override { return true; }

private:
    Context                 &_ctx;
    ui::View                *_waiter; // repainted when an emoji image lands
    std::vector<Target>      _targets;
    std::vector<std::string> _images;
    TargetHover              _hover;
    plat::TimerId            _animTimer  = 0; // the next frame of an animated emoji
    bool                     _selectable = false;
    bool                     _codeBlock  = false;
    std::string              _pressedCode; // a press on code (not selectable): copied on release
};

struct RichOptions {
    ui::Font                       font     = ui::Font::Body;
    ui::C                          color    = ui::C::Text;
    bool                           edited   = false; // append " (edited)" to the last text block
    int                            maxLines = 0;     // > 0: one paragraph, ellipsized (previews)
    float                          scale = 1; // the font size times this (Block Kit headers: 1.1)
    // A preview cut over the text the user reads, on budgets shared by
    // several texts (characters, lines): it stops where they run out with
    // "…" (both drop to 0), else what it used is taken off. Null: the whole
    // text.
    int                           *cutChars = nullptr, *cutLines = nullptr;
    // Collects the text labels made, in order (the message list's selection).
    std::vector<SelectableText *> *labels = nullptr;
};

// Appends the blocks of `mrkdwnText` to `column` (a Column view).
// `imageWaiter` is the view to repaint when custom emoji images arrive (the row).
// Returns where the preview cut it (a byte offset of the parsed text), or
// UINT32_MAX when all of it shows.
uint32_t buildBody(
    Context           &ctx,
    ui::View          *column,
    std::string_view   mrkdwnText,
    const RichOptions &o,
    ui::View          *imageWaiter
);

// The whole of a mrkdwn string as one styled run of text (no paragraphs):
// table cells. With `images`, custom emoji are inline boxes whose id i is
// (*images)[i - 1] (appended to what it holds; paintEmojiBoxes). With
// `targets`, links, mentions and channels keep their link ids, id i being
// (*targets)[i - 1] (appended likewise); without, they are plain text.
text::AttributedText richText(
    Context                  &ctx,
    std::string_view          mrkdwnText,
    const RichOptions        &o,
    std::vector<std::string> *images  = nullptr,
    std::vector<LinkTarget>  *targets = nullptr
);

// Draws the custom emoji boxes of a laid-out text at `origin` (box id i is
// images[i - 1]), animated on the shared clock; `waiter` is repainted when
// an image lands. Returns the ms until the next frame changes (-1: none) —
// when to repaint for the animation.
double paintEmojiBoxes(
    Context                        &ctx,
    gfx::Painter                   &p,
    const text::Layout             &l,
    ui::PointF                      origin,
    const std::vector<std::string> &images,
    ui::View                       *waiter
);

// Repaints a view that paints emoji boxes itself (paintEmojiBoxes) when
// its animated emoji change frame — RichLabel's timer.
class EmojiFrameTimer {
public:
    EmojiFrameTimer(Context &ctx, ui::View &view) : _ctx(ctx), _view(view) {}
    ~EmojiFrameTimer();
    void schedule(double ms); // paintEmojiBoxes' answer; < 0: nothing to do

private:
    Context      &_ctx;
    ui::View     &_view;
    plat::TimerId _id = 0;
};

// The texts of the labels buildBody makes for `mrkdwnText` (o.labels gets
// the same ones, same order): what a selection spanning them copies, also
// when their rows are gone.
std::vector<std::string> bodyTexts(Context &ctx, std::string_view mrkdwnText, const RichOptions &o);

// A link from a message: mailto: opens the mail app — none registered, the
// address is copied and a toast says so at `at` — else Context::openUrl.
void openLink(Context &ctx, const std::string &url, ui::Window *w, ui::PointF at);

// A tooltip chip above a click for `ms`.
void showClickToast(Context &ctx, ui::Window &w, const std::string &text, int ms, ui::PointF at);

// A click on code: copies `code` and shows "Copied" above the click.
void copyCode(Context &ctx, const std::string &code, ui::Window *w, ui::PointF at);

// A dialog's Copy button once clicked: "Copied" for 1.4 s, then "Copy"
// again (while `alive` lasts).
void flashCopied(Context &ctx, ui::Button *copy, std::weak_ptr<char> alive);

// A conversation's place label: "#name", a DM peer's name, a group DM's name or
// "group message"; "" for a conversation this workspace can't see.
std::string placeLabel(const Store &st, std::string_view convId);
// What a message permalink's chip reads.
std::string messageLinkLabel(const Store &st, const mrkdwn::MessageRef &ref);

} // namespace screens
