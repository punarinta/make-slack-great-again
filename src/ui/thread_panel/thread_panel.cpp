// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "thread_panel.h"
#include "ui/message_list/message_list.h"
#include "ui/composer/composer_widget.h"
#include "ui/file_dialog_utils.h"
#include "ui/icon_button/icon_button.h"
#include "ui/icon_utils.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/thread_panel/thread_export_job.h"
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

    _downloadBtn = new IconButton(QStringLiteral(":/ui/download.svg"), 32, 18, _headerWidget);
    _downloadBtn->setObjectName("threadDownloadBtn");
    _downloadBtn->installEventFilter(this);
    connect(_downloadBtn, &QPushButton::clicked, this, &ThreadPanel::downloadThread);
    headerLayout->addWidget(_downloadBtn);

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

    _composer = new ComposerWidget(this);
    _composer->setEnabled(false);
    layout->addWidget(_composer);

    connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &text) {
        if (_session && !_conv.value.isEmpty() && !_rootTs.isEmpty())
            _session->sendMessage(_conv, text, _rootTs);
    });
    connect(
        _composer,
        &ComposerWidget::uploadRequested,
        this,
        [this](const QStringList &filePaths, const QString &text) {
            if (_session && !_conv.value.isEmpty() && !_rootTs.isEmpty())
                _session->uploadFiles(_conv, filePaths, text, _rootTs);
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
        if (!_session || !_msgList)
            return;
        const auto msg = _msgList->lastOwnMessage(_session->meUserId());
        if (!msg)
            return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    connect(_composer, &ComposerWidget::typingStarted, this, [this] {
        if (_session && !_conv.value.isEmpty())
            _session->sendTyping(_conv);
    });
}

void ThreadPanel::setSession(Session *session) {
    _session = session;
    _msgList->setSession(session);
    _composer->setSession(session);
}

void ThreadPanel::openThread(ConversationId conv, Ts rootTs) {
    _conv   = conv;
    _rootTs = rootTs;
    _msgList->openThread(conv, rootTs);
    _composer->setEnabled(true);
    _composer->setPlaceholderText(tr("Reply in thread…"));
}

void ThreadPanel::jumpToTs(const Ts &ts) {
    _msgList->jumpToTs(ts);
}

void ThreadPanel::close() {
    _conv   = {};
    _rootTs = {};
    _msgList->clear();
    _composer->setEnabled(false);
}

void ThreadPanel::refreshTimestamps() {
    _msgList->viewport()->update();
}

void ThreadPanel::pauseGifPlayback() {
    _msgList->pauseGifPlayback();
}

void ThreadPanel::applyTheme() {
    // No left border (a soft shadow stands in for it) and no distinct header
    // background: the panel reads as one continuous surface with the chat. The
    // only horizontal line above the header is the one the tab strip paints.
    setStyleSheet(
        QString("QWidget#threadPanel { background: %1; }").arg(Th::qss(Th::c().surface.content))
    );
    _headerWidget->setStyleSheet("QWidget#threadHeader { background: transparent; }");
    _header->setStyleSheet(QString("font-weight: bold; font-size: %1px; color: %2;")
                               .arg(Th::c().fonts.lg)
                               .arg(Th::qss(Th::c().text.primary)));
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
    if (watched == _downloadBtn) {
        if (e->type() == QEvent::Enter)
            _tooltip->showAbove(
                tr("Download thread as text"),
                QRect(_downloadBtn->mapToGlobal(QPoint(0, 0)), _downloadBtn->size())
            );
        else if (e->type() == QEvent::Leave || e->type() == QEvent::MouseButtonPress)
            _tooltip->hide();
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
