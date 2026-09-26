// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "thread_panel.h"
#include "backend/backend.h"
#include "ui/message_list/message_list.h"
#include "ui/composer/composer_widget.h"
#include "ui/file_dialog_utils.h"
#include "ui/icon_button/icon_button.h"
#include "ui/icon_utils.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/thread_panel/thread_export_job.h"
#include "ui/typing_indicator/typing_indicator.h"
#include "session/session.h"

#include <QDateTime>
#include <QDir>
#include <QLabel>
#include <QHideEvent>
#include <QLinearGradient>
#include <QMoveEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QSignalBlocker>

namespace {

constexpr int kShadowW = 6; // width of the left-edge shadow gradient, in px

// Soft shadow the thread panel casts outward onto the main chat to its left, so
// the panel reads as a raised surface instead of being fenced off by a hard 1px
// divider. Lives just left of the panel edge; darkest against the edge, fading
// out into the chat. Transparent to mouse events.
class EdgeShadow : public QWidget {
public:
    explicit EdgeShadow(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter        p(this);
        QLinearGradient g(0, 0, width(), 0);
        g.setColorAt(0.0, QColor(0, 0, 0, 0));  // fades out into the chat
        g.setColorAt(1.0, QColor(0, 0, 0, 28)); // darkest against the panel edge
        p.fillRect(rect(), g);
    }
};

} // namespace

ThreadPanel::ThreadPanel(ImageCache *imgCache, QWidget *parent) : QWidget(parent) {
    setObjectName("threadPanel");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header bar
    _headerWidget = new QWidget(this);
    _headerWidget->setObjectName("threadHeader");
    _headerWidget->setFixedHeight(48);
    auto       *headerLayout = new QHBoxLayout(_headerWidget);
    const auto &sp           = Th::c().spacing;
    headerLayout->setContentsMargins(sp.xl, 0, sp.md, 0);
    headerLayout->setSpacing(sp.md);

    _header = new QLabel(tr("Thread"), _headerWidget);
    headerLayout->addWidget(_header, 1);

    _tooltip = new PopupTooltip(this);

    // Thread-wide toggle; the bell doubles as the "this thread is muted" indicator.
    _muteBtn = new IconButton(QStringLiteral(":/ui/bell.svg"), 32, 18, _headerWidget);
    _muteBtn->setObjectName("threadMuteBtn");
    _muteBtn->installEventFilter(this);
    connect(_muteBtn, &QPushButton::clicked, this, &ThreadPanel::toggleMuted);
    headerLayout->addWidget(_muteBtn);

    _downloadBtn = new IconButton(QStringLiteral(":/ui/download.svg"), 32, 18, _headerWidget);
    _downloadBtn->setObjectName("threadDownloadBtn");
    _downloadBtn->installEventFilter(this);
    connect(_downloadBtn, &QPushButton::clicked, this, &ThreadPanel::downloadThread);
    headerLayout->addWidget(_downloadBtn);

    // A branched agent conversation (/btw) can move to the list as a session.
    _openSessionBtn =
        new IconButton(QStringLiteral(":/ui/external-link.svg"), 32, 18, _headerWidget);
    _openSessionBtn->setObjectName("threadOpenSessionBtn");
    _openSessionBtn->installEventFilter(this);
    _openSessionBtn->hide();
    connect(_openSessionBtn, &QPushButton::clicked, this, [this] {
        if (!_conv.value.isEmpty() && !_rootTs.isEmpty())
            emit openAsSessionRequested(_conv, _rootTs);
    });
    headerLayout->addWidget(_openSessionBtn);

    _closeBtn = new IconButton(QStringLiteral(":/ui/x.svg"), 32, 18, _headerWidget);
    _closeBtn->setObjectName("threadCloseBtn");
    connect(_closeBtn, &QPushButton::clicked, this, &ThreadPanel::closeRequested);
    headerLayout->addWidget(_closeBtn);
    layout->addWidget(_headerWidget);

    // Outward shadow overlay. It must sit to the LEFT of our edge, so it can't
    // be a child of ours (children are clipped to our bounds). It also can't be
    // a child of the splitter — QSplitter auto-adopts any child as a managed
    // pane, which would stretch it across the whole gap. So parent it to the
    // splitter's parent (a plain container) and position it in that widget's
    // coordinate space via layoutShadow().
    if (auto *splitter = parentWidget())
        if (auto *host = splitter->parentWidget())
            _leftShadow = new EdgeShadow(host);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });

    _msgList = new MessageListWidget(nullptr, imgCache, this);
    layout->addWidget(_msgList, 1);
    connect(_msgList, &MessageListWidget::openDmRequested, this, &ThreadPanel::openDmRequested);
    connect(
        _msgList, &MessageListWidget::openChannelRequested, this, &ThreadPanel::openChannelRequested
    );
    connect(
        _msgList, &MessageListWidget::aiSettingsRequested, this, &ThreadPanel::aiSettingsRequested
    );
    connect(
        _msgList, &MessageListWidget::messageLinkRequested, this, &ThreadPanel::messageLinkRequested
    );
    // "Forward message" has no local handling — the host owns the picker dialog.
    connect(_msgList, &MessageListWidget::forwardMessageRequested, this, [this](const Message &m) {
        emit forwardMessageRequested(_conv, m);
    });

    _typingIndicator = new TypingIndicatorWidget(this);
    layout->addWidget(_typingIndicator);

    _composer = new ComposerWidget(this);
    _composer->setThreadMode(true);
    _composer->setImageCache(imgCache);
    _composer->setEnabled(false);
    layout->addWidget(_composer);

    _broadcastRow         = new QWidget(this);
    auto *broadcastLayout = new QHBoxLayout(_broadcastRow);
    // In line with the composer's contents (its side margin plus the box's
    // inner padding) rather than the panel edge, and clear of the bottom edge.
    // The composer's own bottom margin is the gap above.
    broadcastLayout->setContentsMargins(sp.lg + sp.md, 0, sp.lg, sp.lg);
    _broadcastBox = new QCheckBox(tr("Also send to channel"), _broadcastRow);
    _broadcastBox->setObjectName("threadBroadcastBox");
    applyBroadcastBoxTheme(); // applyTheme() ran above, before this box existed
    broadcastLayout->addWidget(_broadcastBox);
    broadcastLayout->addStretch();
    _broadcastRow->hide();
    layout->addWidget(_broadcastRow);
    connect(_broadcastBox, &QCheckBox::toggled, this, [this](bool on) { _broadcastWanted = on; });
    connect(_composer, &ComposerWidget::compositionChanged, this, [this] {
        refreshBroadcastCheckbox();
    });

    // The tick covers one reply, so a send clears it; Undo Send puts it back
    // along with the text.
    connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &text) {
        if (!_session || _conv.value.isEmpty() || _rootTs.isEmpty())
            return;
        const bool broadcast = _broadcastBox->isChecked();
        const Ts   ghost     = _session->sendMessage(_conv, text, _rootTs, {}, broadcast);
        setBroadcastWanted(false);
        _composer->offerUndoSend(_conv, ghost, [this, broadcast] {
            setBroadcastWanted(broadcast);
        });
    });
    connect(
        _composer,
        &ComposerWidget::uploadRequested,
        this,
        [this](const QStringList &filePaths, const QString &text) {
            if (!_session || _conv.value.isEmpty() || _rootTs.isEmpty())
                return;
            // Files aren't broadcast, but the send still uses up a tick the
            // attachment set aside; otherwise it would come back once the
            // composer empties.
            const bool wanted = _broadcastWanted;
            const Ts   ghost  = _session->uploadFiles(_conv, filePaths, text, _rootTs);
            setBroadcastWanted(false);
            _composer->offerUndoSend(_conv, ghost, [this, wanted] { setBroadcastWanted(wanted); });
        }
    );
    connect(
        _composer,
        &ComposerWidget::editRequested,
        this,
        [this](const Ts &ts, const QString &newText) {
            if (_session && !_conv.value.isEmpty())
                _session->editMessage(_conv, ts, newText);
        }
    );
    connect(_composer, &ComposerWidget::editLastRequested, this, [this] {
        // Only where the backend can edit: Claude Code and email have no edit,
        // so edit mode there would swallow the rewrite into a no-op.
        if (!_session || !_msgList || !_session->capabilities().editMessage)
            return;
        const auto msg = _msgList->lastOwnMessage(_session->meUserId());
        if (!msg)
            return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    // The other route into edit mode: "Edit message" from a reply's context
    // menu. editLastRequested (↑ in an empty editor) only ever reaches the
    // newest own message, so without this wire every older reply is uneditable.
    connect(
        _msgList,
        &MessageListWidget::editMessageRequested,
        this,
        [this](const Ts &ts, const QString &rawText, const std::vector<File> &files) {
            _composer->enterEditMode(ts, rawText, files);
        }
    );
    connect(_composer, &ComposerWidget::typingStarted, this, [this] {
        if (_session && !_conv.value.isEmpty())
            _session->sendTyping(_conv);
    });
}

// The stash key must be workspace-qualified: two workspaces can (in principle)
// carry the same conversation id and root ts, and a reply drafted in one must
// never surface in the other.
static QString threadDraftKey(Session *session, const ConversationId &conv, const Ts &rootTs) {
    return (session ? session->teamId() : QString()) + QLatin1Char('\x1f') + conv.value +
           QLatin1Char('\x1f') + rootTs;
}

void ThreadPanel::stashDraft() {
    if (!_composer)
        return;
    // Always take: emptying the composer is the point, even when the input has
    // no thread to be filed under (then it is discarded).
    const ComposerDraft draft = _composer->takeDraft();
    if (!_session || _conv.value.isEmpty() || _rootTs.isEmpty())
        return;
    const QString key = threadDraftKey(_session, _conv, _rootTs);
    if (draft.isEmpty())
        _drafts.remove(key);
    else
        _drafts.insert(key, draft);
}

void ThreadPanel::purgeDrafts(const QString &teamId) {
    const QString prefix = teamId + QLatin1Char('\x1f');
    _drafts.removeIf([&prefix](const auto &it) { return it.key().startsWith(prefix); });
}

void ThreadPanel::setSession(Session *session) {
    if (session != _session) {
        // Stash under the OLD session's key while it is still current, and
        // forget the open thread — it belongs to the outgoing workspace, and a
        // later close()/openThread must not file anything under its ids.
        stashDraft();
        _conv            = {};
        _rootTs          = {};
        _broadcastThread = false;
        _broadcastWanted = false;
    }
    _session = session;
    _msgList->setSession(session);
    _composer->setSession(session);
    refreshBroadcastCheckbox();
}

void ThreadPanel::openThread(ConversationId conv, Ts rootTs) {
    // Re-opening the same thread (a second click on the same reply bar) leaves
    // the composer — including a pending edit — intact. Opening a different one
    // stashes the old thread's unsent reply (a pending edit is dropped: its ts
    // would be applied to the new thread's conversation on the next Enter) and
    // restores whatever was staged for the new thread.
    // The broadcast tick is dropped the same way: it was set for a reply in the
    // thread being left, whose channel may not be the new one's.
    const bool changed = (_conv != conv || _rootTs != rootTs);
    if (changed) {
        stashDraft();
        _broadcastWanted = false;
        _typingIndicator->clearAll();
    }
    _conv                 = conv;
    _rootTs               = rootTs;
    const Conversation *c = _session ? _session->findConversation(conv) : nullptr;
    _composer->setConvKind(c ? c->kind : ConvKind::PublicChannel);
    // Only a channel thread has a channel to also send the reply to. Neither
    // the kind nor the backend changes while the thread is open, so this is
    // settled here rather than on every composer change.
    _broadcastThread = c && _session->capabilities().replyBroadcast &&
                       (c->kind == ConvKind::PublicChannel || c->kind == ConvKind::PrivateChannel);
    if (changed)
        _composer->restoreDraft(_drafts.value(threadDraftKey(_session, _conv, _rootTs)));
    _msgList->openThread(conv, rootTs);
    // An agent session's thread is a subagent's run, which a reply goes on to
    // (relayed by the session), or a side conversation branched off it (/btw),
    // which is also a session of its own. A subagent that hasn't started yet
    // takes nothing.
    const bool agent    = _session && _session->capabilities().agentSessions;
    const bool readOnly = agent && !_session->backend()->threadAcceptsReplies(conv, rootTs);
    _openSessionBtn->setVisible(agent && _session->backend()->threadOpensAsSession(conv, rootTs));
    _composer->setVisible(!readOnly);
    _composer->setEnabled(!readOnly);
    _composer->setPlaceholderText(tr("Reply in thread…"));
    refreshBroadcastCheckbox();
    refreshMuteButton();
}

void ThreadPanel::jumpToTs(const Ts &ts) {
    _msgList->jumpToTs(ts);
}

void ThreadPanel::userTyping(
    const ConversationId &conv,
    const Ts             &rootTs,
    const UserId         &user,
    const QString        &name,
    bool                  isSelf,
    qint64                thinkingSinceMs
) {
    if (conv == _conv && rootTs == _rootTs && !rootTs.isEmpty())
        _typingIndicator->userTyping(user, name, isSelf, thinkingSinceMs);
}

void ThreadPanel::close() {
    stashDraft(); // before _conv/_rootTs clear — the stash is filed under them
    _conv   = {};
    _rootTs = {};
    _msgList->clear();
    _typingIndicator->clearAll();
    _composer->setEnabled(false);
    _broadcastThread = false;
    _broadcastWanted = false;
    refreshBroadcastCheckbox();
    refreshMuteButton();
}

void ThreadPanel::toggleMuted() {
    if (!_session || _conv.value.isEmpty() || _rootTs.isEmpty())
        return;
    _session->setThreadMuted(_conv, _rootTs, !_session->isThreadMuted(_conv, _rootTs));
    refreshMuteButton();
}

void ThreadPanel::refreshMuteButton() {
    const bool open  = _session && !_conv.value.isEmpty() && !_rootTs.isEmpty();
    const bool muted = open && _session->isThreadMuted(_conv, _rootTs);
    _muteBtn->setSvgPath(
        muted ? QStringLiteral(":/ui/bell-off.svg") : QStringLiteral(":/ui/bell.svg")
    );
}

void ThreadPanel::refreshBroadcastCheckbox() {
    if (!_broadcastBox || !_composer)
        return;
    _broadcastRow->setVisible(_broadcastThread);
    // A broadcast is a new text reply: an edit, or attachments (Slack can't
    // broadcast a file reply), untick the box until they're gone; then the
    // user's own tick comes back.
    const bool canBroadcast =
        _broadcastThread && !_composer->isEditing() && _composer->pendingFiles().isEmpty();
    const QSignalBlocker keepWanted(_broadcastBox); // toggled would overwrite _broadcastWanted
    _broadcastBox->setEnabled(canBroadcast);
    _broadcastBox->setChecked(canBroadcast && _broadcastWanted);
}

void ThreadPanel::setBroadcastWanted(bool wanted) {
    _broadcastWanted = wanted;
    refreshBroadcastCheckbox();
}

void ThreadPanel::refreshTimestamps() {
    _msgList->viewport()->update();
}

void ThreadPanel::setLinkPreviewsEnabled(bool on) {
    _msgList->setLinkPreviewsEnabled(on);
}

void ThreadPanel::setEmojiAnimationsEnabled(bool on) {
    _msgList->setEmojiAnimationsEnabled(on);
}

void ThreadPanel::setMediaAnimationsEnabled(bool on) {
    _msgList->setMediaAnimationsEnabled(on);
}

void ThreadPanel::pauseGifPlayback() {
    _msgList->pauseGifPlayback();
}

// Small and subdued: an option under the composer, not something to compete
// with the reply being written.
void ThreadPanel::applyBroadcastBoxTheme() {
    Th::setStyleSheetIfChanged(
        _broadcastBox, Th::checkBoxQss(Th::c().fonts.xs, Th::c().text.secondary)
    );
}

void ThreadPanel::applyTheme() {
    // No left border (a soft shadow stands in for it) and no distinct header
    // background: the panel reads as one continuous surface with the chat. The
    // only horizontal line above the header is the one the tab strip paints.
    Th::setStyleSheetIfChanged(
        this,
        QString("QWidget#threadPanel { background: %1; }").arg(Th::qss(Th::c().surface.content))
    );
    Th::setStyleSheetIfChanged(_headerWidget, "QWidget#threadHeader { background: transparent; }");
    if (_broadcastBox)
        applyBroadcastBoxTheme();
    Th::setStyleSheetIfChanged(
        _header,
        QString("font-weight: bold; font-size: %1px; color: %2;")
            .arg(Th::c().fonts.lg)
            .arg(Th::qss(Th::c().text.primary))
    );
    // _closeBtn (IconButton) self-themes.
}

void ThreadPanel::downloadThread() {
    if (!_session || _conv.value.isEmpty() || _rootTs.isEmpty())
        return;
    // Copy the label out right away — findConversation pointers don't survive
    // Session mutations.
    QString title;
    if (const Conversation *c = _session->findConversation(_conv)) {
        if (c->kind == ConvKind::Im && c->dmUser)
            title = _session->userDisplayName(*c->dmUser);
        else if (c->kind == ConvKind::PublicChannel || c->kind == ConvKind::PrivateChannel)
            title = QStringLiteral("#") + c->name;
        else
            title = c->name;
    }
    const qint64  rootSecs = _rootTs.section(QLatin1Char('.'), 0, 0).toLongLong();
    const QString defaultName =
        QStringLiteral("thread-%1.txt")
            .arg(QDateTime::fromSecsSinceEpoch(rootSecs).toString(QStringLiteral("yyyy-MM-dd")));
    const QString savePath =
        Ui::getSaveFileName(this, tr("Save thread"), QDir::homePath() + "/" + defaultName);
    if (savePath.isEmpty())
        return;
    ThreadExportJob::start(_session, _conv, _rootTs, title, savePath, window());
}

bool ThreadPanel::eventFilter(QObject *watched, QEvent *e) {
    if (watched == _downloadBtn || watched == _muteBtn || watched == _openSessionBtn) {
        auto *btn = static_cast<QWidget *>(watched);
        if (e->type() == QEvent::Enter) {
            QString text =
                watched == _openSessionBtn ? tr("Open as session") : tr("Download thread as text");
            if (watched == _muteBtn) {
                const bool muted = _session && _session->isThreadMuted(_conv, _rootTs);
                text             = muted ? tr("Unmute thread") : tr("Mute thread");
            }
            _tooltip->showAbove(text, QRect(btn->mapToGlobal(QPoint(0, 0)), btn->size()));
        } else if (e->type() == QEvent::Leave || e->type() == QEvent::MouseButtonPress) {
            _tooltip->hide();
        }
    }
    return QWidget::eventFilter(watched, e);
}

void ThreadPanel::layoutShadow() {
    if (!_leftShadow)
        return;
    QWidget *host = _leftShadow->parentWidget();
    if (!isVisible() || !host) {
        _leftShadow->hide();
        return;
    }
    // Our top-left mapped into the host's coordinate space; the shadow sits in
    // the kShadowW-wide strip immediately to the left of that edge.
    const QPoint tl = mapTo(host, QPoint(0, 0));
    _leftShadow->setGeometry(tl.x() - kShadowW, tl.y(), kShadowW, height());
    _leftShadow->show();
    _leftShadow->raise();
}

void ThreadPanel::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutShadow();
}

void ThreadPanel::moveEvent(QMoveEvent *e) {
    QWidget::moveEvent(e);
    layoutShadow();
}

void ThreadPanel::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    layoutShadow();
}

void ThreadPanel::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    if (_leftShadow)
        _leftShadow->hide();
}
