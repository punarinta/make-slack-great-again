// Shared by every screen: the one object a view needs to reach the data and
// the app. Passed by reference into screen constructors; owned by main().
//
// Contract between the shell (app/screens/shell: window, rail, sidebar,
// header, composer, main) and the messages screens (app/screens/messages:
// message list, message rows, thread panel, image cache). Additive changes are
// fine; changing a member needs the lead.
#pragma once

#include "model/backend.h"
#include "model/store.h"
#include "model/store_slot.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace llm {
class Service; // app/llm/service.h
}
namespace media {
class AudioPlayer; // app/media/audio_player.h
}

namespace screens {

// The model types every screen names (Context uses them unqualified).
using model::Backend;
using model::ConvRef;
using model::Store;
using model::Ts;
using model::UserRef;

class ImageCache;   // app/screens/messages/image_cache.h (decoded, scaled, LRU-bounded)
class RemoteImages; // app/screens/common/remote_images.h (downloads + their disk cache)

struct Context {
    ui::App         &app;
    // The open workspace's Store (store() reads it). Observe through the
    // slot itself: a workspace switch moves those observers along.
    model::StoreSlot store;
    Backend         &backend;
    ImageCache      &images;

    // Navigation requests any screen may make; the shell implements them.
    std::function<void(ConvRef)>                   openConversation;
    std::function<void(ConvRef, Ts root)>          openThread; // shows the thread panel
    std::function<void()>                          closeThread;
    std::function<void(UserRef)>                   openProfile; // may be a no-op for now
    std::function<void(const std::string &)>       openUrl;     // links in messages
    // A message link's chip: the conversation (or
    // the thread, for a reply) with the message scrolled to and flashed.
    std::function<void(ConvRef, Ts ts, Ts thread)> openMessage;

    // The hover profile card: the pointer entered (1) or left (0) an
    // avatar, name or mention standing for the user, or clicked it (2);
    // the rect is its window rect.
    std::function<void(UserRef, ui::RectF, int mode)>             profileHover;
    // A profile card's "Message", the member list: the
    // user's DM, opened (or created) and shown — or, for a teammate (an
    // agent's avatar in a Claude Code session, which has no DM), its page.
    std::function<void(UserRef)>                                  messageUser;
    // The Forward dialog for a message, or for one of its files ("Forward
    // this file", from the file bar's Share).
    std::function<void(ConvRef, Ts, const std::string &filePath)> forwardMessage;

    // The main window: parent of file dialogs, host of the in-app file
    // browser (screens/common/file_dialogs.h). Set by the shell.
    ui::Window                                                      *window = nullptr;
    // Files dropped on a message list become attachments of the composer
    // below it (thread = the thread panel's). Set by the shell.
    std::function<void(std::vector<std::string> paths, bool thread)> attachFiles;
    // Settings → Link previews (the shell sets it and rebuilds the rows).
    bool                                                             linkPreviews  = true;
    // Settings → Threads "Inline": a thread's replies open under its message
    // in the list instead of the side panel (the shell rebuilds the rows).
    bool                                                             threadsInline = false;
    // Settings → One-click reactions: the message toolbar's emoji, left to
    // right (none when unset or empty). Set by the shell.
    std::function<std::vector<std::string>()>                        quickReactions;
    // A canvas file shared in a message (the canvas page in a popup over
    // the window). Set by the shell; unset = the file's page.
    std::function<void(ConvRef, const model::File &)>                openCanvas;
    // http(s) pictures (avatars, files, emoji, previews): main's; null in
    // tests that only show local files.
    RemoteImages                                                    *remote = nullptr;
    // The AI providers (Settings → AI assistance): main's; null where no AI
    // feature is wired (tests that don't need one).
    llm::Service                                                    *ai     = nullptr;
    // The inline audio player (one clip at a time, app-wide): main's; null
    // where nothing plays (the cards still show, inert).
    media::AudioPlayer                                              *audio  = nullptr;
    // Settings opened on its AI assistance page (the Summarize notice's
    // "Open settings"). Set by the shell.
    std::function<void()>                                            openAiSettings;

    // Agent thread links (a Slack thread answered by a Claude Code session):
    // what the hover toolbar's robot and the message menu offer for a
    // message, and what their items do. Set by the shell while there are
    // links to make; unset: nothing of it shows.
    struct AgentLink {
        bool offered  = false; // the robot, "Ask agent…" (a Claude Code workspace exists)
        bool linked   = false; // its thread is linked: they open the agent's thread
        bool canAllow = false; // linked; its author can't ask yet: "Allow <name> to ask agent"
        bool root     = false; // linked, and it is the thread's root: "Unlink agent"
    };
    enum class AgentLinkAction : uint8_t { Ask, Allow, Unlink, OpenSource };
    std::function<AgentLink(ConvRef, const model::Message &)> agentLink;
    // OpenSource: (conv, root) is an agent's thread of a link (a branch of
    // the session): the Slack thread it answers opens.
    std::function<void(ConvRef, Ts ts, AgentLinkAction)>      agentLinkAction;
    // An agent session's thread that answers a link: where its questions
    // come from ("#general in Lumen", the thread panel's chip); "" = none.
    std::function<std::string(ConvRef, Ts root)>              agentLinkSource;

    // "Mark unread" (the message menu): a conversation (root 0) or a thread
    // marked unread stays unread while it is on screen — the lists and the
    // shell don't read it — until it is opened again (the shell drops the
    // hold then, and on a workspace switch).
    struct UnreadHold {
        ConvRef conv = model::kNoConv;
        Ts      root = 0;
    };
    std::vector<UnreadHold> unreadHolds;
    bool                    unreadHeld(ConvRef conv, Ts root) const {
        for (const UnreadHold &h : unreadHolds)
            if (h.conv == conv && h.root == root)
                return true;
        return false;
    }
    void holdUnread(ConvRef conv, Ts root) {
        if (!unreadHeld(conv, root))
            unreadHolds.push_back({conv, root});
    }
    void releaseUnread(ConvRef conv, Ts root) {
        std::erase_if(unreadHolds, [&](const UnreadHold &h) {
            return h.conv == conv && h.root == root;
        });
    }
};

} // namespace screens
