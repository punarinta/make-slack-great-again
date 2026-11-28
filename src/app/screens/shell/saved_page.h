// The sidebar's "Saved messages" page: every saved
// message (Slack's "Later": reminders soonest due first, then the plain "Save
// for later" bookmarks newest first) as cards — the conversation name (a
// click opens it) on the page's grey, then in a bordered card the message as
// a chat row (avatar, name, time, its text on one line; a click jumps to
// it) and a footer with the due time or "Saved for later" and "Remove".
// Data is the Store's saved list (Store::savedList), so the page opens at
// once and follows changes while it is up; a card whose message isn't loaded
// fetches it for the preview (Backend::loadMessage).
#pragma once

#include "screens/shell/overview_page.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

class SavedPage : public OverviewPage {
public:
    SavedPage(screens::Context &ctx, Avatars &avatars);

    // (Re)builds the cards. Every time the page is brought to front.
    void open() override;
    // Drops the cards (workspace switch).
    void clear() override;

    // A message row: jump to the message (in its thread, for a reply). The
    // conversation name on a card: the conversation.
    std::function<void(model::ConvRef, model::Ts ts, model::Ts thread)> onOpenMessage;
    std::function<void(model::ConvRef)>                                 onOpenChannel;

    // Tests: what the page shows.
    size_t      cardCount() const { return _cards.size(); }
    ui::View   *card(size_t i) const;
    // A card's footer line ("Saved for later", "Reminder set for …").
    std::string dueText(size_t i) const;
    void        remove(size_t i);   // the card's "Remove"
    void        activate(size_t i); // a click on the card's message

private:
    class Card;
    void rebuild() override;
    void resolvePreviews();
    void onChange(const model::Change &ch) override;

    std::vector<Card *>                               _cards;
    std::vector<model::Store::SavedItem>              _items; // what the cards show, in order
    std::vector<std::pair<model::ConvRef, model::Ts>> _tried; // previews asked for
};

} // namespace shell
