// The frame the sidebar's overview pages share (Threads, Saved messages,
// Scheduled messages): a 48 px header with the page's title over a hairline,
// then on the page's grey a scroll view with the status line ("Loading…",
// "… will appear here") and the list of cards. The page follows the open
// workspace's Store while it exists (onChange); a burst of changes rebuilds
// it once (rebuildSoon).
//
// The cards' parts are shared too: the conversation line over a card
// (cardHeader), the white bordered body (cardBody) and the chat-style
// message row inside it (fillMessageRow).
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>

namespace shell {

// Text that reads as a link: the channel name on a card (bold, underlined
// while hovered), "Show N more replies" (link colour, underlined), the
// Saved messages page's "Remove".
class TextLink final : public ui::Clickable {
public:
    TextLink(std::string text, ui::Font f, ui::C c, bool underline);
    bool        onEvent(ui::Event &e) override;
    std::string accessibleName() const override { return _text; }

private:
    void        refreshLook();
    std::string _text;
    ui::Label  *_label = nullptr;
    ui::Font    _font;
    ui::C       _color;
    bool        _underline;
};

class OverviewPage : public ui::View {
public:
    OverviewPage(screens::Context &ctx, Avatars &avatars, std::string title);
    ~OverviewPage() override;

    // (Re)fills the page. Every time it is brought to front.
    virtual void open()  = 0;
    // Drops the cards (workspace switch).
    virtual void clear() = 0;

    const std::string &statusText() const { return _statusText; }

    void paint(gfx::Painter &p) override; // one grey surface; the cards are white

protected:
    // A Store change while the page exists.
    virtual void onChange(const model::Change &ch) = 0;
    // rebuild() on the next loop turn, if the page is still on screen then:
    // a server sync changes many items in one go.
    void         rebuildSoon();
    bool         rebuildQueued() const { return _rebuildQueued; }
    virtual void rebuild() {}
    void         setStatus(std::string text);

    screens::Context    &_ctx;
    Avatars             &_avatars;
    ui::View            *_rule   = nullptr; // the hairline under the title
    ui::ScrollView      *_scroll = nullptr;
    ui::Label           *_status = nullptr;
    ui::View            *_list   = nullptr; // the cards
    std::shared_ptr<int> _alive  = std::make_shared<int>(0);

private:
    std::string              _statusText;
    bool                     _rebuildQueued = false;
    model::Store::ObserverId _observer      = 0;
};

// The conversation line over a card: its glyph and name, a click on the
// name calls onOpen. Added to `parent`; returns the line (for what follows
// the name).
ui::View *cardHeader(
    ui::View *parent, const model::Store &st, model::ConvRef conv, std::function<void()> onOpen
);
// The card's white, bordered body, added to `card`.
ui::View *cardBody(ui::View *card);
// `row` (a plain view, or a clickable one the caller wires) as a chat row:
// the avatar (`picture`, else the initial of `who`), then a column with the
// name, and `time` beside it when not empty. Returns the column, for the body.
ui::View *fillMessageRow(
    ui::View          *row,
    Avatars           &avatars,
    const std::string &who,
    const std::string &picture,
    const std::string &time
);

} // namespace shell
