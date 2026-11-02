// The composer (channel and thread share it): in a
// rounded box the formatting toolbar, the "Editing message" banner, the
// attachment chips, the editor and the bottom bar (attach, emoji, GIF,
// mention … voice, send + schedule).
//
// The editor is plain text: the toolbar and the format keys
// type Slack's markers ("*bold*", "> quote", "```"), and a link is typed as
// <url|label>. Mentions, channels and GIFs picked from the popups are pills
// that show "@Name" / "#name" / "GIF · title" and send their raw token.
//
// Draft invariant (the composer draft stash): whatever is typed — text and
// attached files — is stashed per (conversation, thread) on EVERY path that
// leaves it — switching conversation, switching or closing the thread,
// destroying the composer — and restored when that target is shown again.
// Sending clears it.
//
// Attachments: the paperclip opens the file chooser (screens/common/
// file_dialogs.h); files and pictures pasted from the clipboard, and files
// dropped on the composer (or, through
// Context::attachFiles, on the message list above it) are added too. Each
// shows as a 160×92 chip (image cover, text preview, or plain card; name and
// size on plates; remove); sending hands them to Backend::sendWithFiles.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/composer_popups.h"
#include "screens/shell/shortcuts.h"
#include "ui/ui.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shell {

class GlyphButton;
class VoiceStrip;

// TextEdit runs → mrkdwn. Marks hug the words (leading/trailing spaces move
// outside, as Slack requires), never cross a line break, nest without
// "*a**b*" collisions across adjacent runs; links become <url|label>;
// & < > are escaped as the API expects. Code spans carry no other marks.
// A link whose URL is a raw token ("<@U…>", "<#C…|name>", "<!here>") or
// "@U…" / "#C…" is a pill and goes out as that token.
std::string toMrkdwn(std::string_view text, const std::vector<ui::TextEdit::Run> &runs);
// The ":" completion: the ~20 common emoji first, then the
// built-in table, then the workspace's custom emoji, each tier by match — a
// prefix, then at a _ - + boundary (":bcd" finds ":abc_bcd:"), then anywhere.
// At most 8.
struct EmojiCompletion {
    std::string name;
    bool        custom = false; // inserted as :name: (no Unicode form)
};
std::vector<EmojiCompletion> emojiCompletions(const model::Store &store, std::string_view query);
// The other way, for editing a sent message: the
// mrkdwn stays literal text, except <@U…>, <#C…|name>, <!here> and GIPHY
// links, which become pills; &amp; &lt; &gt; are shown decoded.
void        loadMrkdwn(ui::TextEdit &edit, const model::Store &store, std::string_view mrkdwn);
// The formatting the editors are typed in (Settings → Appearance): Slack's
// (*bold*, _italic_), or Markdown's (**bold**, *italic*). Either way what
// leaves an editor is mrkdwn: Composer::mrkdwn() rewrites Markdown's *x* to
// _x_ (mrkdwn::markdownItalics), and loadMrkdwn writes Slack's *x* as **x**
// (mrkdwn::markdownBold).
bool        markdownFormatting();
void        setMarkdownFormatting(bool on);
// An editor's text and runs as mrkdwn, its typing in Markdown's formatting
// or not (what Composer::mrkdwn() gives for the current one).
std::string editorMrkdwn(const ui::TextEdit &edit, bool markdown);
// A draft's html typed in one formatting, written in the other (only when a
// '*' in it reads differently; else as it is).
std::string redialectDraft(const model::Store &store, const std::string &html, bool toMarkdown);

// The composer's unsent message lives in the workspace's Store
// (model::Store::setDraft), which is what the workspace cache persists. What
// is left here is which workspace's Store a key belongs to.
class DraftStash {
public:
    struct Key {
        model::ConvRef conv   = model::kNoConv;
        model::Ts      thread = 0;
        bool operator==(const Key &o) const { return conv == o.conv && thread == o.thread; }
    };
    // The Store of a workspace key, for a draft touched while it is not the
    // open one. The shell's: nullptr for a workspace that is gone.
    void                     setStoreOf(std::function<model::Store *(const std::string &scope)> fn);
    // The open workspace's key.
    void                     setScope(std::string scope) { _scope = std::move(scope); }
    const std::string       &scope() const { return _scope; }
    // Stores `html` and attached files for key (both empty erases the draft).
    void                     stash(Key k, std::string html, std::vector<std::string> files = {});
    // The stashed html ("" when none); the draft stays stashed.
    std::string              get(Key k) const;
    // The stashed attachments (absolute paths).
    std::vector<std::string> files(Key k) const;
    bool                     has(Key k) const;
    void                     erase(Key k);
    // Adds plain `text` at the end of the draft of (scope, k), after a space
    // (a dictation that finished after its composer moved on).
    void                     appendText(const std::string &scope, Key k, std::string_view text);
    // Puts `html` in front of the draft of (scope, k), what it held on the
    // next line, and adds `files` to its own (a message that never went out,
    // Composer::giveBack).
    void
    putBack(const std::string &scope, Key k, std::string_view html, std::vector<std::string> files);

private:
    model::Store *storeOf(const std::string &scope) const;

    std::function<model::Store *(const std::string &scope)> _storeOf;
    std::string                                             _scope;
};

class Composer : public ui::View {
public:
    Composer(screens::Context &ctx, DraftStash &drafts);
    ~Composer() override; // stashes: destruction is a leave path too

    // Shows the draft of (conv, thread) after stashing the current one.
    void           setTarget(model::ConvRef conv, model::Ts thread);
    // Stashes the current input without changing target (window close, …).
    void           stashNow();
    model::ConvRef conv() const { return _key.conv; }
    model::Ts      thread() const { return _key.thread; }
    ui::TextEdit  &edit() { return *_edit; }
    // The current input as mrkdwn.
    std::string    mrkdwn() const;
    // The Markdown formatting option changed: what is typed (and an edit's
    // set-aside draft) is rewritten from the other formatting, call it
    // before setMarkdownFormatting(toMarkdown).
    void           redialect(bool toMarkdown);
    // Sends the input (if any) and clears it (and its draft).
    bool           send();
    // "Edit message": the input becomes my message ts (the draft is set
    // aside); Enter saves through Backend::edit, Escape or the banner's
    // cross cancels. Leaving the conversation cancels too.
    void           beginEdit(model::Ts ts);
    void           endEdit();
    model::Ts      editing() const { return _editTs; }
    bool           editBannerShown() const;

    // Thread mode (the thread panel's composer): "@channel" & co. are
    // listed but inserted as plain text, "Reply in thread…".
    void               setThreadMode(bool on) { _threadMode = on; }
    // The schedule-send chevron beside Send (Capabilities::scheduledSend
    // on the channel composer; the thread composer never hides it).
    void               setScheduleVisible(bool on);
    bool               scheduleVisible() const;
    // A fixed placeholder, whatever the target (the forward dialog's); ""
    // goes back to the target's ("Message #design", "Reply in thread…").
    void               setPlaceholder(std::string text);
    // Claude Code's suggested reply, shown in place of the placeholder while
    // the editor is empty ("…  →"); → or Tab takes it. "" drops it.
    void               setSuggestion(std::string text);
    const std::string &suggestion() const { return _suggestion; }
    bool               acceptSuggestion();
    // A conversation nobody can write to from
    // here (Conversation::readOnly) locks the composer, its reason the
    // placeholder; "" unlocks it ("Message #design" again).
    void               setLockReason(std::string reason);
    // The conversation's name changed (a DM peer resolved later): "Message …"
    // follows, unless locked or fixed.
    void               refreshPlaceholder();

    // ── Agent sessions ──────────────────────────────────────────────────────
    // The prompt history source (newest first): ↑ / ↓ from an empty
    // editor step through it, Ctrl+R searches it. Unset: no history.
    std::function<std::vector<std::string>()> historySource;
    HistorySearch                            *historySearch() const { return _historySearch; }
    // "/name args" for a known command msga runs itself (Command::local,
    // Capabilities::slashCommands): asked instead of sending; others go out
    // as messages (Claude Code's commandsAreMessages).
    std::function<void(const std::string &name, const std::string &args)> onCommand;
    // What the history search dims above the box (the message
    // area; default the composer's parent).
    void setPopupArea(ui::View *v) { _popupArea = v; }

    // ── Attachments ─────────────────────────────────────────────────────────
    // Adds files (absolute local paths; folders, missing files and ones
    // already attached are skipped). Returns how many were added.
    size_t                          addAttachments(const std::vector<std::string> &paths);
    // Files the user hands over (dropped on the composer or the message list
    // above it, pasted): attached, and the editor takes the focus so typing
    // goes on there. The one way in for every drop and paste.
    void                            attachFiles(const std::vector<std::string> &paths);
    void                            removeAttachment(size_t i);
    void                            clearAttachments(); // all of them, at once
    const std::vector<std::string> &attachments() const { return _files; }
    void                            chooseAttachments(); // the paperclip
    ui::View                       *attachmentStrip() const { return _chipRow; }

    // ── Pickers and popups ──────────────────────────────────────────────────
    void      openEmoji();
    void      openGif();
    void      openLinkPopup();
    void      openSchedule();
    PickList *pickList() const { return _pick; } // the open @ / # / : list
    // Re-reads the trigger before the caret: opens, filters or closes the pick list.
    void      updatePickList();
    // Tests: user labels folded into the @ filter's cache so far (each one
    // once, again only when it changes), and the pick list recomputes (an
    // edit's onChange + onSelectionChange count once).
    size_t    mentionFolds() const { return _mentionFolds; }
    size_t    pickRecomputes() const { return _pickRecomputes; }
    // Undo send: after a send, for 5 s, Ctrl+Z in the empty editor (or the
    // chip) deletes the message and puts the text and files back.
    bool      undoOffered() const { return _undoTs != 0; }
    bool      undoSend();
    // A message that never went out (Backend::onUnsent), as undo send puts
    // one back: its mrkdwn in front of what's typed since, its files
    // attached again. While editing, the draft set aside takes the text.
    void      giveBack(std::string_view mrkdwn, const std::vector<std::string> &files);

    // Avatars for the @ list (the shell's).
    void                                     setAvatars(Avatars *a) { _avatars = a; }
    // GIF picker hooks (the GIPHY key lives in Settings).
    std::function<std::string()>             gifKey;
    std::function<void(const std::string &)> setGifKey;
    // The attach chooser's start folder, and where the last pick came from
    // (the shell keeps it in Settings).
    std::function<std::string()>             attachDir;
    std::function<void(const std::string &)> setAttachDir;

    // ── Voice input ─────────────────────────────────────────────────────────
    // Dictation (llm::VoiceInput): the mic beside Send — shown while a
    // provider can do speech-to-text — records, the strip above the bottom
    // bar shows the level, time and progress, and the text lands at the
    // caret. The voice shortcut toggles it (held past 400 ms: push-to-talk),
    // Escape cancels. Leaving the conversation (or the composer hidden) stops
    // a recording and lets it finish: the text lands in that conversation's
    // draft, a failure shows when it is back. On by default; off for a
    // composer that is not a conversation's (the forward dialog's).
    void                 setVoiceInput(bool on);
    VoiceStrip          *voiceStrip() const { return _voiceStrip; }
    bool                 voiceActiveHere() const;
    static constexpr int kPushToTalkMs = 400;

    // The toolbar and bottom bar buttons (tests).
    GlyphButton *button(shortcuts::Id id) const;
    GlyphButton *sendButton() const { return _sendBtn; }
    GlyphButton *gifButton() const { return _gifBtn; }
    GlyphButton *mentionButton() const { return _mentionBtn; }

    bool onEvent(ui::Event &e) override; // file drops
    void layout() override;
    void visibilityChanged(bool on) override; // hidden (the canvas tab): no undo offer
    // Hidden by an ancestor: the thread panel closed, another page or workspace
    // shown — the same as being hidden itself.
    void hiddenByAncestor() override { visibilityChanged(false); }
    // The tooltips that name a key which can change (the send key).
    void refreshTips();

    std::function<void()> onSent;
    // The thread panel's "Also send to channel" tick, read at send.
    std::function<bool()> broadcastWanted;
    // Edit mode entered or left, attachments went from none to some or back.
    std::function<void()> onCompositionChanged;
    // Set: Enter / Send call it instead of sending (the
    // forward dialog's composer).
    std::function<bool()> onSendRequest;

private:
    void rebuildChips();
    bool pasteMedia(
        const std::vector<std::string> &mimes, plat::Selection sel
    ); // TextEdit::onPasteMedia
    // Spell checking (spell::Checker): the misspelled words of the editor
    // become its squiggles, a moment after typing stops; right-clicking one
    // offers suggestions, "Add to dictionary" and "Ignore".
    void scheduleSpell();
    void respell();
    bool spellMenu(uint32_t offset, ui::PointF windowPos);
    void refreshLook();               // focus colours, send button state
    bool keyDown(const ui::Event &e); // the composer-scope shortcuts
    void formatAction(shortcuts::Id id);
    void editLast();
    bool toggleVoiceInput();
    void cancelVoiceHere();
    void cancelAllVoice();
    void finishVoiceInBackground(); // a leave path: stop recording, keep going
    void updateMicVisibility();
    void updateVoiceUi();
    void insertVoiceText(const std::string &text);
    bool openPromptSearch();
    bool stepPromptHistory(bool older);
    void resetPromptHistory();
    void setHistoryText(const std::string &text, bool caretAtStart);
    void prefixLines(std::string_view prefix, bool numbered);
    void wrapCodeBlock();
    void wrapInline(std::string_view marker);
    void insertPill(uint32_t from, uint32_t to, const std::string &display, const std::string &raw);
    void pick(const PickList::Item &it);
    void typing();
    void compositionChanged();
    void offerUndo(const std::string &html, const std::vector<std::string> &files);
    void withdrawUndo();

    // A dictation started here, for the target it was started on (its
    // VoiceInput owner).
    struct VoiceJob {
        std::string     scope;
        DraftStash::Key key;
    };
    VoiceJob *voiceJobHere() const;
    void      voiceEnded(const void *owner, const std::string *text, const std::string *error);

    screens::Context        &_ctx;
    DraftStash              &_drafts;
    Avatars                 *_avatars = nullptr;
    DraftStash::Key          _key;
    ui::TextEdit            *_edit      = nullptr;
    GlyphButton             *_tools[10] = {};
    GlyphButton             *_bottom[4] = {}; // attach, emoji, GIF, mention
    GlyphButton             *_gifBtn = nullptr, *_mentionBtn = nullptr, *_emojiBtn = nullptr;
    GlyphButton             *_sendBtn = nullptr, *_dropBtn = nullptr, *_micBtn = nullptr;
    ui::View                *_sendGroup = nullptr;
    ui::View                *_seps[2]   = {};
    ui::View                *_editBar   = nullptr;
    std::string              _editStash; // the draft set aside while editing
    std::string              _placeholder, _fixedPlaceholder, _suggestion, _lock;
    std::vector<std::string> _history; // the source's, while stepping (_historyIndex >= 0)
    int                      _historyIndex  = -1;
    HistorySearch           *_historySearch = nullptr;
    ui::View                *_popupArea     = nullptr;
    model::Ts                _editTs        = 0;
    ui::View                *_box = nullptr, *_chips = nullptr;
    ui::View                *_chipRow  = nullptr;
    PickList                *_pick     = nullptr;
    uint32_t                 _pickFrom = 0; // the trigger character's offset
    // What the last updatePickList saw (text, caret, anchor…): the same again
    // (an edit's onSelectionChange after its onChange) changes nothing.
    struct PickInputs {
        uint32_t       caret = 0, anchor = 0;
        model::ConvRef conv    = model::kNoConv;
        bool           focused = false, thread = false, window = false;
        bool           operator==(const PickInputs &) const = default;
    };
    PickInputs  _pickIn;
    std::string _pickText;
    bool        _pickInValid = false, _pickShown = false;
    // The @ filter's folded user names, by UserRef: one buffer per user,
    // "label\nname\nreal name\nprofile name" folded, with where the label
    // and the name end. An entry refolds only when its user changed
    // (Store::userRevision; the id guards a slot reused by another user),
    // and all of them for another Store.
    struct FoldedUser {
        uint64_t    rev = ~uint64_t(0); // none yet
        std::string id, folded;
        uint32_t    labelEnd = 0, nameEnd = 0;
    };
    std::vector<FoldedUser> _folded;
    const model::Store     *_foldedStore = nullptr;
    // The # filter's folded channel names, by ConvRef (refolded on a rename).
    struct FoldedName {
        std::string name, folded;
    };
    std::vector<FoldedName>              _foldedConvs;
    // The / list: the conversation's commands as last fetched, and the same
    // sorted by name with their names folded (redone when the list changes).
    model::ConvRef                       _cmdConv = model::kNoConv;
    uint64_t                             _cmdRev  = 0; // Backend::commandsRevision
    std::vector<std::string>             _cmdNames;
    std::vector<model::Backend::Command> _cmdSorted;
    std::vector<std::string>             _cmdFolded;
    size_t                               _mentionFolds = 0, _pickRecomputes = 0;
    void                                 computePickList();
    void                                 refreshCommands();
    std::vector<std::string>             _files;
    // What a chip shows of its file (its name and size, a text file's first
    // lines), read when it was attached: kept while it stays attached.
    struct ChipInfo {
        std::string path, preview;
        int64_t     size = 0;
    };
    std::vector<ChipInfo>    _chipInfo;
    std::string              _shownPlaceholder; // what the editor was last given
    std::shared_ptr<int>     _alive      = std::make_shared<int>(0); // guards dialog callbacks
    double                   _lastTyping = -1e9;
    // The undo-send offer (the pill above the box, 5 s): the sent message,
    // what the editor held, the pill, its timer.
    model::Ts                _undoTs     = 0;
    std::string              _undoHtml;
    std::vector<std::string> _undoFiles;
    ui::Popup               *_undoPill    = nullptr;
    plat::TimerId            _undoTimer   = 0;
    int                      _composition = -1;
    bool                     _dropHover = false, _threadMode = false, _focusedLook = false;
    bool                     _active        = false; // something to send
    bool                     _inPick        = false; // updatePickList probing the caret
    plat::TimerId            _spellTimer    = 0;
    uint32_t                 _spellSeq      = 0; // the newest check; older answers are dropped
    uint32_t                 _spellObserver = 0;
    VoiceStrip              *_voiceStrip    = nullptr;
    uint32_t                 _voiceObserver = 0, _providersObserver = 0;
    bool                     _voiceOn = true, _pttArmed = false;
    int64_t                  _pttPressedMs = 0; // when the voice shortcut went down

    std::vector<std::unique_ptr<VoiceJob>>        _voiceJobs; // in flight
    // Failures of dictations finished elsewhere, shown when their target is.
    std::vector<std::pair<VoiceJob, std::string>> _voiceErrors;
};

} // namespace shell
