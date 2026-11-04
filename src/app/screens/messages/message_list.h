// MessageList — a conversation's messages (or one thread's) on a
// ui::VirtualList, fed and kept live by the Store's change notifications.
//
//   auto *list = parent->add<screens::MessageList>(ctx);
//   list->style().flex(1);
//   list->showConversation(conv);          // or showThread(conv, rootTs)
//
// Behaviour:
//  - opens at the newest message and stays pinned there while new ones
//    arrive; scrolled up, the viewport stays on the same message when
//    messages are appended, prepended (older history) or change height;
//  - loads older history when scrolled near the top (Backend::loadHistory
//    while Conversation::hasMoreBefore), marks the conversation read when its
//    newest message is on screen (Backend::markRead);
//  - consecutive messages by the same author within 5 minutes are grouped
//    (avatar, name and time only on the first; hovering a grouped message
//    shows its time in the gutter); date separators between days (Today,
//    Yesterday, weekday, date); system messages (joins) are compact lines;
//  - rows: mrkdwn body (see rich.h), "(edited)", pending sends dimmed,
//    reactions (click toggles, "+" opens the emoji picker), thread summary
//    ("N replies · Last reply …" → Context::openThread), image files
//    (thumbnails, GIFs animated, click → in-window viewer), file chips, link
//    previews and legacy attachments, bot/app names;
//  - a hover toolbar (react, reply in thread, more) whose "…" opens the
//    message menu (so do right click, long press and the Menu key on the
//    row); right click on a link opens the link menu, on an image or a file
//    the file menu;
//  - a typing line under the list ("Mira is typing…");
//  - text selection across messages (drag; double click a word, triple
//    click a line), copied with Ctrl/Cmd+C, cleared by Escape or a click;
//  - opening a conversation lands on its first unread message (a third down
//    the viewport), or where it was left when it was shown before;
//  - Settings → Threads "Inline": a reply bar expands the thread under it;
//  - Block Kit headers, dividers, images and tables (tables: at most ten
//    rows, "Open full table"), shared-message cards, canvas cards, PDF
//    previews; link previews' "×" (Remove preview / Hide preview);
//  - empty-list states: while the first page (a thread's replies, or
//    — setWaiting — the workspace's conversations) loads, the loading ring
//    with "Loading your stuff..." and its sequels after 1 / 5 / 15 s; a
//    loaded conversation with nothing in it says "No messages yet".
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace screens {

class MessageRow;
class MessageList;

// The link menu (Open link, Copy link) at a window point.
void showLinkMenu(Context &ctx, ui::Window &w, ui::PointF at, const std::string &url);

// The file viewer: the picture (an image, a PDF's
// page) on the viewer backdrop under a bar with the name and Download,
// Forward, Open in browser, More actions and Close; Escape or a click on the
// backdrop closes it. `list` gets the actions (null: Close only).
ui::Popup *
showFileViewer(Context &ctx, ui::Window &w, MessageList *list, Ts ts, const model::File &f);

class MessageList : public ui::View {
public:
    explicit MessageList(Context &ctx);
    ~MessageList() override;

    void    showConversation(ConvRef conv);
    // Thread mode (the thread panel): the root, a "N replies" divider, the replies.
    void    showThread(ConvRef conv, Ts root);
    void    clear(); // shows nothing, stops observing
    ConvRef conversation() const { return _conv; }
    Ts      threadRoot() const { return _root; }
    bool    threadMode() const { return _root != 0; }

    // The workspace's first load (no conversations yet) —
    // the loading state even with no conversation shown.
    void setWaiting(bool on);
    bool waiting() const { return _waiting; }

    // Settings → Threads changed (Context::threadsInline): inline threads
    // close; the rows are rebuilt.
    void setThreadsInline(bool on);
    // The thread shown in the side panel (0 = none): its reply bar says
    // "Close thread". The shell keeps it current.
    void setOpenThreadRoot(Ts root);
    // Reading: off while the window is unfocused, hidden or
    // minimized, so the open conversation builds up unreads; back on, its
    // newest message is marked read once on screen. A thread reads itself
    // either way.
    void setReading(bool on);

    // ── Selection ───────────────────────────────────────────────────────────
    // The text selection: a range from one message's text to another's,
    // offsets into selectableTexts() joined with '\n'.
    struct TextPos {
        Ts       ts     = 0; // 0 = none
        uint32_t offset = 0;
        bool     operator==(const TextPos &o) const { return ts == o.ts && offset == o.offset; }
    };
    bool        hasSelection() const;
    std::string selectedText() const; // the messages' texts, '\n' between them
    void        clearSelection();
    void        select(TextPos anchor, TextPos focus);
    // The position under a window point (ts 0: not over message text).
    TextPos     textPosAt(ui::PointF windowPos) const;
    // The code under a window point in message text (SelectableText::codeAt).
    std::string codeAt(ui::PointF windowPos) const;

    // Scroll so the message is visible (search hits, permalinks); `flash`
    // briefly highlights it.
    void scrollToMessage(Ts ts, bool animated = true, bool flash = true);
    // A search result: scrollToMessage, or — while the
    // conversation's first page is still loading — once it is in. A target
    // that page doesn't have is dropped; so is one the loaded list lacks.
    void jumpTo(Ts ts);

    // "Reply in thread" from the toolbar or menu (default: Context::openThread).
    std::function<void(ConvRef, Ts)> onReply;
    // "Edit message" on one's own message; unset = no Edit item. The shell's
    // composer handles the editing itself.
    std::function<void(ConvRef, Ts)> onEdit;

    ui::VirtualList &list() { return *_list; }

    // ── Introspection (tests, the shell's debug overlay) ───────────────────
    enum class ItemKind : uint8_t { Message, Day, System, Divider };
    struct Item {
        Ts       ts      = 0; // Message/System: the message; Day: its first message
        int64_t  day     = 0; // local calendar day (base::localDay)
        ItemKind kind    = ItemKind::Message;
        bool     grouped = false; // continues the previous message's group
    };
    const std::vector<Item> &items() const { return _items; }
    std::string              itemLabel(size_t i) const; // Day: "Today"; Divider: "3 replies"
    // What the empty list shows, and its text (the hint under the ring, or
    // "No messages yet").
    enum class State : uint8_t { None, Loading, Empty };
    State              state() const;
    std::string        stateText() const;
    const std::string &typingText() const;
    // The shell shows typing in its own indicator above the
    // composer: off, the list keeps no typing row.
    void               setTypingRow(bool on);

    // ── Internal (rows) ─────────────────────────────────────────────────────
    Context              &ctx() { return _ctx; }
    const model::Message *message(Ts ts) const;
    void                  rowHovered(MessageRow *row, bool on);
    void                  rowMade(MessageRow *row);
    void                  rowGone(MessageRow *row);
    // Moves on whenever rows are bound again for anything but their own
    // message changing (MessageRow keeps its body only within one epoch).
    uint32_t              bodyEpoch() const { return _bodyEpoch; }
    MessageRow           *toolbarRow() const { return _toolbarRow; }
    // The toolbar's one-click reactions (Context::quickReactions), a button each.
    ui::View             *quickReactionBar() const { return _tbQuick; }
    // Tests: how many times a message row was (re)bound so far.
    int                   rowBinds() const { return _rowBinds; }
    bool                  flashing(Ts ts) const { return ts == _flashTs; }
    void                  openMenu(Ts ts, ui::PointF windowPos);
    // The menus as data, and what their items do (tests drive these). Items,
    // order, wording and icons are in message_list.cpp.
    enum MenuId : int {
        kReply = 1,
        kOpenThread,
        kMuteThread,
        kUnmuteThread,
        kEdit,
        kCopyLink,
        kCopyLinkInText,
        kCopyText,
        kPin,
        kUnpin,
        kSave,
        kUnsave,
        kRemind, // opens the presets (remindItems) in its place
        kRemoveReminder,
        kForward, // shown disabled without Context::forwardMessage
        kMoveToThread,
        kSummarize, // "Summarize down" (summary.h)
        kDelete,
        kOpenLink, // the link menu
        kCopyLinkUrl,
        kCopyImageLink, // the image / file menu
        kCopyFileLink,
        kCopyImage,
        kPreview,    // CSV files; shown disabled (no table viewer yet)
        kDeleteFile, // "Delete image…" / "Delete file…"
        kAllowAsker, // agent thread links (Context::agentLink)
        kUnlinkAgent,
        kRemindPreset = 40, // + 0…4: in 20 minutes … next week
        kRemindCustom = 45,
    };
    std::vector<ui::MenuItem>        menuItems(Ts ts) const;
    std::vector<ui::MenuItem>        fileMenuItems(Ts ts, const std::string &path) const;
    static std::vector<ui::MenuItem> remindItems();
    static std::vector<ui::MenuItem> linkMenuItems();
    void runMenuAction(Ts ts, int id, const std::string &path = {}, ui::PointF windowPos = {});
    // The file action bar ("…" → the file menu), for the hovered file.
    void openFileMenu(Ts ts, const std::string &path, ui::PointF windowPos);
    void fileHovered(ui::View *fileView, Ts ts, const std::string &path, bool on);
    // These run in the background (model::jobs(): the footer's cog lists
    // them) and outlive the list.
    void downloadFile(Ts ts, const std::string &path); // the bar's Download
    void copyImage(const model::File &f);              // "Copy full image"
    void openCsvPreview(const model::File &f);         // "Preview" (the table viewer)
    void openReactionPicker(Ts ts, ui::RectF anchorWindowRect);
    void openImage(const std::string &path, int w, int h);
    // A message's button (MessageExtras::buttons) pressed: Backend::
    // pressButton, then a toast at the button ("Sent to the app", or
    // "Couldn't press the button: …").
    void pressButton(Ts ts, const std::string &buttonId, ui::PointF windowPos);
    void showToast(const std::string &text, int ms, ui::PointF windowPos);
    void reply(Ts ts);

    // ── Internal (rows): state the rows read and the actions they take ─────
    void               applySelection(MessageRow &row) const;
    bool               threadOpen(Ts root) const; // its reply bar says "Close thread"
    bool               inlineOpen(Ts root) const { return has(_inlineThreads, root); }
    void               replyBarClicked(Ts root);
    // Link previews off (Settings), or dismissed with the "×".
    bool               attachmentHidden(const model::Message &m, size_t index) const;
    void               dismissAttachment(Ts ts, size_t index);
    // Own message on a backend with Capabilities::removePreview: the "×"
    // removes the preview for everyone, else it only hides it here.
    bool               removesPreviewServerSide(const model::Message &m) const;
    bool               imageCollapsed(Ts ts, int attachment, int block) const;
    void               toggleImage(Ts ts, int attachment, int block);
    bool               unfurlExpanded(Ts ts, int attachment) const;
    void               toggleUnfurl(Ts ts, int attachment);
    void               openFileViewer(Ts ts, const std::string &path);
    void               openHtmlFile(const model::File &f);
    void               openCanvas(const model::File &f);
    // A canvas card's preview HTML (null while loading, *state -1 if it
    // failed); the cards showing it are bound again when it arrives.
    const std::string *canvasPreview(const std::string &fileId, int *state);

    // Something the toolbar shows changed elsewhere (an agent link came).
    void refreshToolbar() { placeToolbar(); }

    void      layout() override;
    bool      onEvent(ui::Event &e) override;
    ui::SizeF measureContent(float, float) override { return {0, 0}; }

private:
    class Adapter;
    class ListState;
    void rebuild(bool notify);
    void updateState();
    void subscribe();
    void onChange(const model::Change &ch);
    void usersChanged(); // a coalesced Users burst: re-bind what it touched
    // Item i (a message) is grouped as its message, its neighbours' and the
    // grouping rules now say (an Update need not rebuild the items).
    bool groupingHolds(size_t i) const;
    void updateTyping();
    void scheduleEdgeCheck();
    void checkEdges();
    void placeToolbar();
    void syncQuickReactions(bool shown);
    void toggleQuickReaction(const std::string &name);
    void placeFileBar();
    bool applyJump();

public:
    std::vector<Ts> threadRoots(Ts except) const; // for "Move to thread…"

private:
    void        toggleSaved(Ts ts);
    void        hideToolbar();
    int         itemIndex(Ts ts) const;
    void        saveAnchor();
    bool        applyOpenTarget();
    void        rowChanged(Ts ts);
    void        rowsChanged(int index, int n); // itemsChanged, new epoch
    void        resetRows();                   // reset, new epoch
    void        loadInline(Ts root);
    void        copySelection();
    // A drag's end: the nearest message text to a window point (a header,
    // a gap, a picture or past the list's edge too).
    TextPos     dragPosAt(ui::PointF windowPos) const;
    // A drag held at (or past) the list's top or bottom scrolls it.
    void        dragScroll();
    void        stopDragScroll();
    static bool has(const std::vector<Ts> &v, Ts ts) {
        for (Ts x : v)
            if (x == ts)
                return true;
        return false;
    }
    struct Key { // a message's part: (ts, attachment or -1, block or -1)
        Ts  ts;
        int a, b;
    };
    static bool has(const std::vector<Key> &v, Key k);
    static void toggle(std::vector<Key> &v, Key k);
    // Where a conversation was left (by its id, so it
    // survives switching away and back).
    struct SavedAnchor {
        std::string conv;
        bool        atBottom = false;
        Ts          ts       = 0; // the item's message
        ItemKind    kind     = ItemKind::Message;
        float       offset   = 0; // the viewport top, px below the item's top
    };
    struct CanvasPreview {
        std::string id, html;
        int         state = 0; // 0 loading, 1 loaded, -1 failed
    };

    Context                   &_ctx;
    std::unique_ptr<Adapter>   _adapter;
    ui::VirtualList           *_list    = nullptr;
    ui::Label                 *_typing  = nullptr;
    ListState                 *_state   = nullptr;
    float                      _typingH = 22; // 0: no typing row (setTypingRow)
    ui::View                  *_toolbar = nullptr, *_tbQuick = nullptr;
    std::vector<std::string>   _quickNames;  // what _tbQuick's buttons are for
    std::vector<std::string>   _quickImages; // and their pictures (Store::EmojiGlyph::image)
    ui::Clickable             *_tbEmoji = nullptr, *_tbForward = nullptr, *_tbMore = nullptr;
    class ActionButton        *_tbSave = nullptr, *_tbAgent = nullptr;
    ui::View                  *_fileBar = nullptr, *_fileView = nullptr; // the hovered file
    Ts                         _fileTs = 0;
    std::string                _filePath;
    MessageRow                *_toolbarRow = nullptr;
    std::vector<Item>          _items;
    std::shared_ptr<char>      _alive; // guards timers and backend callbacks
    ConvRef                    _conv = model::kNoConv;
    Ts                         _root = 0, _markedTs = 0, _flashTs = 0, _jumpTs = 0;
    uint32_t                   _observer  = 0;
    plat::TimerId              _edgeTimer = 0, _flashTimer = 0, _usersTimer = 0;
    // The Store revisions the rows were last bound against (usersChanged).
    const model::Store        *_seenStore   = nullptr;
    uint64_t                   _seenProfile = 0, _seenText = 0;
    int                        _rowBinds  = 0;
    uint32_t                   _bodyEpoch = 0;
    MessageRow                *_rows = nullptr; // every message row (live, kept, spare), linked
    bool                       _loadingOlder = false, _loadingThread = false, _waiting = false;
    // Selection.
    TextPos                    _selAnchor, _selFocus;
    bool                       _selDragging = false;
    ui::PointF                 _selPointer;
    plat::TimerId              _selScrollTimer = 0;
    // Opening: where to land once the first page is laid out — the saved
    // position, else the first message after this read cursor (0: bottom).
    std::vector<SavedAnchor>   _anchors;
    SavedAnchor                _openAnchor;
    Ts                         _openLastRead   = 0;
    bool                       _openPending    = false;
    bool                       _reading        = true;
    // Inline threads, dismissed previews, folded images, expanded cards.
    bool                       _threadsInline  = false;
    Ts                         _openThreadRoot = 0;
    // Not "_inline": a keyword to MSVC, and a macro (__inline) in mingw's CRT.
    std::vector<Ts>            _inlineThreads;
    std::vector<Key>           _dismissed, _folded, _expanded;
    std::vector<CanvasPreview> _canvasPreviews;
};

} // namespace screens
