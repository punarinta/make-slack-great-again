// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QTimer>
#include <QWidget>

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
class MentionCompleter;
class MentionPopup;
class FormattingToolbar;
class AttachmentStrip;
class EditModeBanner;
class StyledLineEdit;

// Slack-style composer: formatting toolbar + text area + bottom action bar.
// Enter sends; Shift+Enter inserts a newline.
class ComposerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ComposerWidget(QWidget *parent = nullptr);

    void setPlaceholderText(const QString &text);

    // Provide a session for autocomplete and emoji; can be called at any time.
    void setSession(Session *session);

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

    // Move keyboard focus to the message editor (e.g. when the window is
    // brought to the foreground onto an active conversation).
    void focusInput();

    // Pending file list (files queued for upload when the message is sent).
    const QStringList &pendingFiles() const { return _pendingFiles; }
    void               addPendingFile(const QString &filePath);
    void               clearPendingFiles();

signals:
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
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void applyTheme();
    void trySend();
    void trySchedule();
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

    QFrame            *_box          = nullptr;
    StyledLineEdit    *_subject      = nullptr; // email subject line (optional)
    QFrame            *_subjectSep   = nullptr; // horizontal rule under the subject
    FormattingToolbar *_formattingTb = nullptr;
    EditModeBanner    *_editBanner   = nullptr;
    AttachmentStrip   *_attachStrip  = nullptr;
    QTextEdit         *_edit         = nullptr;
    QWidget           *_bottomBar    = nullptr; // bottom action bar
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
    int               _sendActiveState = -1;

    PopupTooltip                            *_tooltip        = nullptr;
    EmojiPickerPopup                        *_emojiPicker    = nullptr;
    MentionCompleter                        *_mentionComp    = nullptr;
    MentionPopup                            *_mentionPopup   = nullptr;
    Session                                 *_session        = nullptr;
    ImageCache                              *_imgCache       = nullptr;
    ConvKind                                 _convKind       = ConvKind::PublicChannel;
    int                                      _atTriggerStart = -1;
    QHash<QWidget *, QString>                _tooltipBtns; // bottom-bar buttons
    QList<QPair<QAbstractButton *, QString>> _iconBtns;    // bottom-bar icon buttons

    // Typing indicator debounce: fires typingStarted() at most once per 3 s while typing
    QTimer _typingTimer;
    bool   _typingPending = false;
};
