// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/composer/composer_draft.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <functional>

class QAbstractButton;
class QFrame;
class QFileDialog;
class QMimeData;
class QScrollArea;
class QTextEdit;
class QPushButton;
class QToolButton;
class Session;
class ImageCache;
class PopupTooltip;
class EmojiPickerPopup;
class GifPickerPopup;
class MentionCompleter;
class MentionPopup;
class HistorySearchPopup;
class FormattingToolbar;
class AttachmentStrip;
class EditModeBanner;
class StyledLineEdit;
class UndoSendPill;

// Slack-style composer: formatting toolbar + text area + bottom action bar.
// Enter sends and Shift+Enter inserts a newline — or, with the "send with
// Ctrl+Enter" option (Ui::Shortcuts::ctrlEnterSends), Ctrl+Enter sends and
// Enter inserts the newline. Ctrl+Enter sends in both modes.
#if defined(MSGA_DEMO)
namespace demo {
class Tour;
}
#endif
class ComposerWidget : public QWidget {
    Q_OBJECT
#if defined(MSGA_DEMO)
    friend class demo::Tour; // the scripted demo drives real widgets (--demo-tour)
#endif
public:
    explicit ComposerWidget(QWidget *parent = nullptr);

    void setPlaceholderText(const QString &text);

    // Provide a session for autocomplete and emoji; can be called at any time.
    void setSession(Session *session);

    // Where ↑ in the empty editor finds earlier prompts to step through
    // (newest first), the way Claude Code's prompt box does; ↓ steps back and,
    // past the newest, empties the editor again. Asked each time ↑ starts from
    // an empty editor; an empty list leaves ↑ to editLastRequested.
    void setPromptHistorySource(std::function<QStringList()> source);

    // Optional subject line for email backends (decision §3 #3): shown only when
    // the backend declares Capabilities::messageSubjects. The value travels to
    // Session::sendMessage; it is read during the sendRequested emit and cleared.
    void    setSubjectVisible(bool visible);
    QString subjectText() const;
    void    setSubjectText(const QString &text); // prefill the reply subject (email)

    // Show/hide the schedule-send dropdown (chevron beside the send button).
    // Gated on Capabilities::scheduledSend — only Slack can send at a future time.
    void setScheduleVisible(bool visible);

    // Embedded use (the Threads overview's per-card reply box): drop the outer
    // horizontal margins so the box aligns flush with the host's content edge
    // (the message avatars) instead of carrying the chat-footer gutter.
    void setFlushHorizontalMargins();

    // Image cache for app-command avatars in the slash-command palette.
    void setImageCache(ImageCache *cache) { _imgCache = cache; }

    // Tell the composer the current conversation kind so it can decide whether
    // to show @channel/@here aliases in the mention popup.
    void setConvKind(ConvKind kind);
    void setThreadMode(bool isThread);

    // Edit mode: pre-populate the editor with an existing message for editing.
    // exitEditMode() is a no-op if not currently in edit mode.
    void enterEditMode(
        const Ts &ts, const QString &existingText, const std::vector<File> &existingFiles = {}
    );
    void exitEditMode();

    // Draft support: read/write the editor content as mrkdwn. User mentions
    // are shown as "@Name" pills in the editor but always read back as the
    // raw <@U…> tokens, so the outgoing message format never changes.
    QString currentText() const;
    void    setText(const QString &text);

    // Conversation-switch stash. takeDraft() captures the whole unsent state
    // (text, pending attachments, subject) and empties the composer in the same
    // step — the single leave-a-conversation entry point. Its guarantee is the
    // security property: after it returns, nothing previously staged can ride
    // into whatever conversation is shown next. An in-progress message edit is
    // discarded, never turned into a draft (its text belongs to an existing
    // message); files attached while editing are new content and are captured.
    ComposerDraft takeDraft();
    // Make the composer show exactly `draft`: text and attachments are replaced
    // wholesale (an empty draft leaves an empty composer). The subject is only
    // overwritten when the draft carries one, so a host-set reply prefill
    // ("Re: …") applied for the incoming conversation survives restoring a
    // subject-less draft.
    void          restoreDraft(const ComposerDraft &draft);

    // Move keyboard focus to the message editor (e.g. when the window is
    // brought to the foreground onto an active conversation).
    void focusInput();

    // Undo send. The host calls this from INSIDE its sendRequested /
    // uploadRequested handler, once Session has handed back the ghost ts: for
    // kUndoSendMs a "Message sent · Undo" chip floats above the box, and Ctrl+Z
    // in the empty editor (or a click on the chip) runs `undo` and puts the
    // sent text, attachments and subject back into the editor. The offer is
    // withdrawn by the next send, a conversation switch (takeDraft), a session
    // change, or hiding the composer — the sent input belongs to the
    // conversation it was sent from and must never resurface anywhere else.
    void offerUndoSend(std::function<void()> undo);
    // The usual wiring: no-op unless the session can delete messages; undoes
    // through Session::undoSend(conv, ghostTs), then runs `restore` so the host
    // can put back send options of its own (the thread panel's broadcast tick)
    // before the editor refills.
    void offerUndoSend(
        const ConversationId &conv, const Ts &ghostTs, std::function<void()> restore = {}
    );
    bool                 undoSendOffered() const { return static_cast<bool>(_undoSend); }
    static constexpr int kUndoSendMs = 5000;

    // Pending file list (files queued for upload when the message is sent).
    const QStringList &pendingFiles() const { return _pendingFiles; }
    bool               isEditing() const { return !_editingTs.isEmpty(); }
    void               addPendingFile(const QString &filePath);
    void               clearPendingFiles();

signals:
    // What the next send would be changed: edit mode was entered or left, or
    // the pending attachments went from none to some or back. Not per keystroke.
    void compositionChanged();
    void sendRequested(const QString &text);
    // Emitted instead of sendRequested when the message is a known slash
    // command: "/remind me …" → ("remind", "me …"). Name is lowercase, no slash.
    void commandRequested(const QString &name, const QString &args);
    // Emitted instead of sendRequested when files are attached: the files and
    // the text travel together so they post as one Slack message.
    void uploadRequested(const QStringList &filePaths, const QString &text);
    // Emitted instead of sendRequested when in edit mode.
    void editRequested(const Ts &ts, const QString &newText);
    // Emitted when ↑ is pressed in an empty editor; caller should call enterEditMode().
    void editLastRequested();
    // Emitted when user schedules a message: text + Unix timestamp.
    void scheduleRequested(const QString &text, qint64 postAt);
    // Emitted on first keypress after a 3-second silence; used for typing indicator.
    void typingStarted();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void applyTheme();
    void trySend();
    void trySchedule();
    // ↑ (older) or ↓ in the prompt history; false when the key is the editor's
    // (a line to move to, no history, a message being edited).
    bool stepPromptHistory(bool older);
    void resetPromptHistory();
    // Ctrl+R: the search over the same history (HistorySearchPopup); false
    // when there's nothing to search.
    bool openHistorySearch();
    void updateSendState();
    void adjustEditorHeight();
    void setFocused(bool focused);
    void recolorBottomBarIcons(const QColor &color);
    void applyInlineFormat(const QString &marker);
    void prefixSelectedLines(const QString &prefix, bool ordered = false);
    void applyBlockFormat(const QString &fence);
    void openAttachDialog();
    // Attach pasted media (clipboard image or copied files) as pending files.
    // Returns true when consumed; false falls back to a normal text paste.
    bool attachFromMimeData(const QMimeData *source);
    void openLinkDialog(const QPoint &toolbarGlobalPos);
    void checkMentionPopup();
    // Fill the editor from mrkdwn, rendering <@U…> tokens as "@Name" pills.
    void setEditorMrkdwn(const QString &text);
    // Refresh pill colors after a theme change.
    void recolorMentionPills();
    // Undo send (see offerUndoSend): run the pending undo and restore the sent
    // input; drop the offer; keep the chip anchored to the box.
    void undoSend();
    void withdrawUndoSend();
    void placeUndoPill();

    QFrame            *_box          = nullptr;
    StyledLineEdit    *_subject      = nullptr; // email subject line (optional)
    QFrame            *_subjectSep   = nullptr; // horizontal rule under the subject
    FormattingToolbar *_formattingTb = nullptr;
    EditModeBanner    *_editBanner   = nullptr;
    AttachmentStrip   *_attachStrip  = nullptr;
    QTextEdit         *_edit         = nullptr;
    QWidget           *_bottomBar    = nullptr; // bottom action bar
    QToolButton       *_gifBtn       = nullptr; // GIF picker button (tooltip varies)
    QPushButton       *_sendBtn      = nullptr;
    QPushButton       *_dropBtn      = nullptr; // schedule-send dropdown
    QWidget           *_sendGroup    = nullptr; // pill container for send+drop
    QWidget           *_linkPopup    = nullptr; // LinkPopup instance, created lazily
    QFileDialog       *_attachDialog = nullptr; // persistent native file picker, reused
    Ts                 _editingTs;              // non-empty when in edit mode

    QStringList       _pendingFiles;  // local paths of files to upload on send
    std::vector<File> _editModeFiles; // existing files shown read-only in edit mode
    // Last styled send-button state (-1 = unstyled). updateSendState runs on
    // every keystroke but the pill only changes at the empty↔non-empty boundary;
    // skipping the redundant setStyleSheet/svgIcon work keeps typing cheap.
    // applyTheme() resets it so a theme switch restyles with the new colors.
    int               _sendActiveState  = -1;
    // Last state compositionChanged reported (bit 0: files pending, bit 1:
    // editing; -1 = never), so typing doesn't re-emit it.
    int               _compositionState = -1;

    PopupTooltip                            *_tooltip       = nullptr;
    EmojiPickerPopup                        *_emojiPicker   = nullptr;
    GifPickerPopup                          *_gifPicker     = nullptr;
    MentionCompleter                        *_mentionComp   = nullptr;
    MentionPopup                            *_mentionPopup  = nullptr;
    HistorySearchPopup                      *_historySearch = nullptr;
    Session                                 *_session       = nullptr;
    std::function<QStringList()>             _historySource;
    QStringList                              _history;             // while stepping through it
    int                                      _historyIndex   = -1; // shown entry; -1 = none
    ImageCache                              *_imgCache       = nullptr;
    ConvKind                                 _convKind       = ConvKind::PublicChannel;
    bool                                     _isThread       = false;
    int                                      _atTriggerStart = -1;
    QHash<QWidget *, QString>                _tooltipBtns; // bottom-bar buttons
    QList<QPair<QAbstractButton *, QString>> _iconBtns;    // bottom-bar icon buttons

    // Typing indicator debounce: fires typingStarted() at most once per 3 s while typing
    QTimer _typingTimer;
    bool   _typingPending = false;

    // Undo send: the chip, its countdown, the host's undo action and what the
    // last send took out of the editor (restored on undo).
    UndoSendPill         *_undoPill = nullptr;
    QTimer                _undoTimer;
    std::function<void()> _undoSend;
    ComposerDraft         _lastSent;
};
