// The sidebar's "Threads" page (like the official client's Threads
// view): every thread the user follows, newest activity first, as
// cards — the channel name (a click opens it) over the participants and a
// "New" pill, then in a bordered card the root, "Show N more replies", the
// latest replies (a click on one opens the real thread) and an inline reply
// box ("Reply in thread…" turns into a thread composer). Data comes from
// Backend::loadThreadsView, so the page exists only where
// Capabilities::threadsView does. It sits in the content area in the message
// list's place; the feed has no push, so every open() reloads it, and the
// cards follow replies that reach the Store while it is up.
#pragma once

#include "screens/shell/composer.h"
#include "screens/shell/overview_page.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shell {

// "axelvonsydow and you" / "Adam, Patryk and 3 others": the root's author,
// then the reply authors; me as "you", last; a bot root without an author
// shows the bot's name.
std::string threadParticipants(const model::Store &store, const model::Message &root);

class ThreadsPage : public OverviewPage {
public:
    class Card;
    // setupComposer readies a card's reply box (avatars, GIF key) like the
    // shell's own composers.
    ThreadsPage(
        screens::Context               &ctx,
        Avatars                        &avatars,
        DraftStash                     &drafts,
        std::function<void(Composer &)> setupComposer
    );

    // (Re)loads the first page. Every time the page is brought to front.
    void open() override;
    // Drops the cards and any load in flight (workspace switch).
    void clear() override;

    // "Show N more replies" / a message: the thread for real (channel +
    // thread panel). The channel name on a card: the channel.
    std::function<void(model::ConvRef, model::Ts root)> onOpenThread;
    std::function<void(model::ConvRef)>                 onOpenChannel;

    // Tests: what the page shows.
    size_t cardCount() const { return _cards.size(); }
    Card  &card(size_t i) const { return *_cards[i]; }
    void   showMore(); // "Show more threads"

private:
    void loadPage(std::string cursor);
    // Keeps the cards live: replies arriving while the page is up, our own
    // sends and their confirmations.
    void onChange(const model::Change &ch) override;

    DraftStash                     &_drafts;
    std::function<void(Composer &)> _setupComposer;
    ui::Button                     *_more = nullptr;
    std::vector<Card *>             _cards;
    std::string                     _nextCursor;
    bool                            _loading    = false;
    uint32_t                        _generation = 0; // the load an answer belongs to
};

// One followed thread.
class ThreadsPage::Card : public ui::View {
public:
    Card(ThreadsPage &page, model::Backend::FollowedThread item);

    model::ConvRef conv() const { return _item.conv; }
    model::Ts      root() const { return _item.root.ts; }
    // What it shows (tests).
    bool           unread() const;
    std::string    participants() const;
    size_t         replyRows() const { return _item.latestReplies.size(); }
    ui::View      *replyButton() const { return _replyBtn; }
    Composer      *composer() const { return _composer; }
    ui::View      *newPill() const { return _newPill; }

    void showComposer(); // "Reply in thread…"
    void openThread();   // marks it read, then onOpenThread
    // Store changes in this thread (a reply arrived, was confirmed, went).
    void onChange(const model::Change &ch);

private:
    void markRead();
    void rebuildReplies();

    ThreadsPage                   &_page;
    model::Backend::FollowedThread _item;
    model::Ts                      _latest = 0; // newest reply shown
    ui::View                      *_body = nullptr, *_replies = nullptr;
    ui::View                      *_newPill  = nullptr;
    ui::Button                    *_replyBtn = nullptr;
    Composer                      *_composer = nullptr;
};

} // namespace shell
