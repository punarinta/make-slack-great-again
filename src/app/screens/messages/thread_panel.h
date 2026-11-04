// ThreadPanel — the right-hand thread view: a 48 px header ("Thread", an
// agent link's chip, then mute / download / open-as-session / close; while it
// covers the messages a back arrow on the left stands in for close), the root
// and its replies (a MessageList in thread mode), the shell's typing indicator
// and composer in their slots, and "Also send to channel" under the composer
// for a thread in a channel on a service that can broadcast.
//
//   auto *tp = row->add<screens::ThreadPanel>(ctx);
//   tp->style().width(420);
//   tp->setComposer(makeComposer(ctx));   // the shell's composer widget
//   tp->show(conv, rootTs);               // from Context::openThread
//
// Close asks Context::closeThread (the shell hides the panel).
#pragma once

#include "app/screens/messages/message_list.h"

#include <memory>

namespace screens {

class ThreadPanel : public ui::View {
public:
    explicit ThreadPanel(Context &ctx);

    void    show(ConvRef conv, Ts root);
    ConvRef conversation() const { return _list->conversation(); }
    Ts      root() const { return _list->threadRoot(); }

    // Puts the composer under the replies (replacing any previous one) and
    // returns it; null clears the slot. The typing slot sits above it.
    ui::View    *setComposer(std::unique_ptr<ui::View> composer);
    ui::View    *setTyping(std::unique_ptr<ui::View> typing);
    ui::View    *composer() const { return _composer; }
    MessageList &list() { return *_list; }

    // "Also send to channel": shown for a channel thread where the backend
    // broadcasts replies; the tick is for one reply (the shell clears it
    // after a send) and is unticked and disabled while `blocked` (an edit,
    // attachments).
    ui::CheckBox *broadcast() const { return _broadcast; }
    bool          broadcastShown() const;
    bool          broadcastWanted() const;
    void          setBroadcastWanted(bool on);
    void          setBroadcastBlocked(bool blocked);

    // While the panel covers the messages, the header's back arrow takes the
    // close button's place (one "Close thread" button either way).
    void      setBackShown(bool on);
    ui::View *backButton() const { return _back; }
    ui::View *closeButton() const { return _close; }

    ui::View *muteButton() const { return _mute; }
    void      toggleMuted();
    void      downloadThread();

    std::function<void(ConvRef, Ts)> onOpenAsSession; // the external-link button

    // The chip of an agent's thread that answers a Slack thread (Context::
    // agentLinkSource): where the questions come from; a click opens it.
    ui::Clickable     *linkChip() const { return _linkChip; }
    const std::string &linkChipText() const { return _linkText->text(); }
    void               refreshLink();

private:
    void refreshMute();
    void refreshBroadcast();

    Context       &_ctx;
    ui::Clickable *_mute = nullptr, *_download = nullptr, *_openSession = nullptr;
    ui::Clickable *_back = nullptr, *_close = nullptr;
    ui::Clickable *_linkChip   = nullptr;
    ui::Label     *_linkText   = nullptr;
    MessageList   *_list       = nullptr;
    ui::View      *_typingSlot = nullptr, *_slot = nullptr, *_composer = nullptr;
    ui::View      *_broadcastRow = nullptr;
    ui::CheckBox  *_broadcast    = nullptr;
    bool           _wanted = false, _blocked = false;
};

} // namespace screens
