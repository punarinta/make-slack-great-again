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
class QScrollArea;
class QTextEdit;
class QPushButton;
class QToolButton;
class Session;
class PopupTooltip;
class EmojiPickerPopup;
class MentionCompleter;
class MentionPopup;
class FormattingToolbar;
class AttachmentStrip;
class EditModeBanner;

// Slack-style composer: formatting toolbar + text area + bottom action bar.
// Enter sends; Shift+Enter inserts a newline.
class ComposerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ComposerWidget(QWidget *parent = nullptr);

    void setPlaceholderText(const QString &text);

    // Provide a session for autocomplete and emoji; can be called at any time.
    void setSession(Session *session);

    // Tell the composer the current conversation kind so it can decide whether
    // to show @channel/@here aliases in the mention popup.
    void setConvKind(ConvKind kind);

    // Edit mode: pre-populate the editor with an existing message for editing.
    // exitEditMode() is a no-op if not currently in edit mode.
    void enterEditMode(const Ts &ts, const QString &existingText,
                       const std::vector<File> &existingFiles = {});
    void exitEditMode();

    // Draft support: read/write the editor's plain text directly.
    QString currentText() const;
    void    setText(const QString &text);

    // Pending file list (files queued for upload when the message is sent).
    const QStringList& pendingFiles() const { return _pendingFiles; }
    void addPendingFile(const QString &filePath);
    void clearPendingFiles();

signals:
    void sendRequested(const QString &text);
    void uploadRequested(const QString &filePath);
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

private:
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
    void openLinkDialog(const QPoint &toolbarGlobalPos);
    void checkMentionPopup();

    QFrame            *_box          = nullptr;
    FormattingToolbar *_formattingTb = nullptr;
    EditModeBanner    *_editBanner   = nullptr;
    AttachmentStrip   *_attachStrip  = nullptr;
    QTextEdit         *_edit         = nullptr;
    QPushButton       *_sendBtn      = nullptr;
    QPushButton       *_dropBtn      = nullptr; // schedule-send dropdown
    QWidget           *_sendGroup    = nullptr; // pill container for send+drop
    QWidget           *_linkPopup    = nullptr; // LinkPopup instance, created lazily
    Ts                 _editingTs;              // non-empty when in edit mode

    QStringList        _pendingFiles;   // local paths of files to upload on send
    std::vector<File>  _editModeFiles;  // existing files shown read-only in edit mode

    PopupTooltip                           *_tooltip      = nullptr;
    EmojiPickerPopup                       *_emojiPicker  = nullptr;
    MentionCompleter                       *_mentionComp  = nullptr;
    MentionPopup                           *_mentionPopup = nullptr;
    Session                                *_session      = nullptr;
    ConvKind                                _convKind     = ConvKind::PublicChannel;
    int                                     _atTriggerStart = -1;
    QHash<QWidget*, QString>                _tooltipBtns;           // bottom-bar buttons
    QList<QPair<QAbstractButton*, QString>> _iconBtns;              // bottom-bar icon buttons

    // Typing indicator debounce: fires typingStarted() at most once per 3 s while typing
    QTimer _typingTimer;
    bool   _typingPending = false;
};
