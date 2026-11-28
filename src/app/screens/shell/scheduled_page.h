// The sidebar's "Scheduled messages" page: every message waiting for the
// service to post it (Slack's "Scheduled"), soonest first, as cards like the
// Saved messages page's — the conversation name (a click opens it) on the
// page's grey, then in a bordered card the message as a chat row under my
// name and a footer with the send time, "Send now" and "Cancel". Data is the
// Store's scheduled list (Store::scheduled); opening the page asks the
// backend to list them again, and the cards follow the list while it is up.
#pragma once

#include "screens/shell/overview_page.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

class ScheduledPage : public OverviewPage {
public:
    ScheduledPage(screens::Context &ctx, Avatars &avatars);

    // (Re)builds the cards and re-lists. Every time the page is brought to front.
    void open() override;
    // Drops the cards (workspace switch).
    void clear() override;

    // The conversation name on a card: the conversation.
    std::function<void(model::ConvRef)> onOpenChannel;

    // Tests: what the page shows.
    size_t      cardCount() const { return _cards.size(); }
    std::string whenText(size_t i) const; // a card's footer ("Sends Mar 15, 2:34 PM")
    bool        canSendNow(size_t i) const;
    void        sendNow(size_t i); // the card's "Send now"
    void        cancel(size_t i);  // the card's "Cancel"

private:
    class Card;
    void        rebuild() override;
    void        reset(); // rebuilt from scratch (a failed action's card clickable again)
    void        onChange(const model::Change &ch) override;
    // The names the cards show (my name and picture, the conversations'):
    // a change there rebuilds them even when the list is the same.
    std::string namesKey() const;

    std::vector<Card *>                      _cards;
    std::vector<model::Store::ScheduledItem> _items; // what the cards show, in order
    std::string                              _names; // namesKey() of the cards shown
};

} // namespace shell
