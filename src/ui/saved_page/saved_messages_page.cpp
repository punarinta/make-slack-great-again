// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "saved_messages_page.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/message_list/message_render.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/time_format.h"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

using OverviewCard::kAvatarSize;
using OverviewCard::kTextLeft;

// ── SavedMsgRow ───────────────────────────────────────────────────────────────
// The reminded message, chat-style: avatar + name + time header over the stored
// snippet (plain text — the reminder keeps a preview, not the full message).
// Clicking anywhere jumps to the real message.
class SavedMsgRow : public QWidget {
    Q_OBJECT
public:
    SavedMsgRow(
        MessageReminder       item,
        Session              *session,
        ImageCache           *imgCache,
        std::function<void()> onClicked,
        QWidget              *parent
    )
        : QWidget(parent), _item(std::move(item)), _session(session), _imgCache(imgCache),
          _onClicked(std::move(onClicked)) {
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        _avatarUrl = OverviewCard::avatarUrl(_session, _item.author, _item.botAvatarUrl);
        if (_imgCache) {
            if (!_avatarUrl.isEmpty())
                _imgCache->get(_avatarUrl); // kick off the download
            connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
                if (url == _avatarUrl)
                    update();
            });
        }
    }

    QSize sizeHint() const override {
        const QFontMetrics &nfm = OverviewCard::nameFontMetrics();
        const QFontMetrics  bfm(QApplication::font());
        const int           textH = padV() + nfm.height() + hdrGap() + bfm.height() + padV();
        return {kAvatarSize * 3, std::max(textH, padV() + kAvatarSize + padV())};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Avatar + name + the message's own timestamp.
        OverviewCard::paintRowHeader(
            p,
            width(),
            devicePixelRatioF(),
            _imgCache,
            _avatarUrl,
            displayName(),
            MsgRender::dateTimeLabel(decimalTsToMicros(_item.ts))
        );

        // Snippet, one elided line.
        const QFontMetrics &nfm = OverviewCard::nameFontMetrics();
        const QFont         bf  = QApplication::font();
        const QFontMetrics  bfm(bf);
        p.setFont(bf);
        p.setPen(Th::c().text.primary);
        const QString body = _item.snippet.isEmpty() ? tr("No preview available") : _item.snippet;
        p.drawText(
            textLeft(),
            padV() + nfm.height() + hdrGap() + bfm.ascent(),
            bfm.elidedText(body, Qt::ElideRight, width() - textLeft())
        );
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && _onClicked)
            _onClicked();
    }

private:
    static int padV() { return OverviewCard::rowPadV(); }
    static int hdrGap() { return OverviewCard::rowHdrGap(); }
    static int textLeft() { return kTextLeft; }

    QString displayName() const {
        if (!_item.botName.isEmpty())
            return _item.botName;
        if (_session && !_item.author.value.isEmpty())
            return _session->userDisplayName(_item.author);
        // A reminder set from another client carries no author.
        return tr("Message");
    }

    MessageReminder       _item;
    Session              *_session  = nullptr;
    ImageCache           *_imgCache = nullptr;
    std::function<void()> _onClicked;
    QString               _avatarUrl;
};

// ── SavedCard ─────────────────────────────────────────────────────────────────
// One reminder: conversation header (outside the bordered body, like a
// ThreadCard), the message row, and a footer with the due time and a remove
// action.
class SavedCard : public QWidget {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<void(ConversationId, Ts, Ts)> openMessage;
        std::function<void(ConversationId)>         openChannel;
    };

    SavedCard(
        MessageReminder item, Session *session, ImageCache *imgCache, Callbacks cbs, QWidget *parent
    )
        : QWidget(parent), _item(std::move(item)), _session(session), _cbs(std::move(cbs)) {
        const auto &sp    = Th::c().spacing;
        auto       *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(sp.lg);

        // ── Header: conversation name on the grey page ─────────────────
        outer->addLayout(
            OverviewCard::makeChannelHeader(
                this,
                OverviewCard::conversationLabel(_session, _item.conv),
                [this] {
                    if (_cbs.openChannel)
                        _cbs.openChannel(_item.conv);
                },
                &_chanIcon,
                &_chanBtn
            )
        );

        // ── White card body: the message row + due footer ──────────────
        _body = new QWidget(this);
        _body->setObjectName("savedCardBody");
        _body->setAttribute(Qt::WA_StyledBackground);
        auto *bodyLayout = new QVBoxLayout(_body);
        bodyLayout->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
        bodyLayout->setSpacing(sp.sm);
        outer->addWidget(_body);

        const auto openMessage = [this] {
            if (_cbs.openMessage)
                _cbs.openMessage(_item.conv, _item.ts, _item.threadRoot);
        };
        bodyLayout->addWidget(new SavedMsgRow(_item, _session, imgCache, openMessage, _body));

        auto *footer = new QHBoxLayout();
        footer->setContentsMargins(0, 0, 0, 0);
        footer->setSpacing(sp.sm);
        _dueIcon = new QLabel(_body);
        footer->addWidget(_dueIcon);
        _dueLabel = new QLabel(dueText(), _body);
        footer->addWidget(_dueLabel);
        footer->addStretch(1);
        auto *removeBtn = new StyledButton(tr("Remove"), StyledButton::Variant::Link, _body);
        connect(removeBtn, &QPushButton::clicked, this, [this] {
            if (_session)
                _session->removeMessageReminder(_item.conv, _item.ts);
        });
        footer->addWidget(removeBtn);
        bodyLayout->addLayout(footer);

        applyTheme();
    }

    void applyTheme() {
        const auto &th = Th::c();
        // MainWindow cascades "QWidget { background: content }" over the whole
        // right panel, so header widgets pin explicit transparent backgrounds.
        OverviewCard::styleCardBody(_body);
        OverviewCard::styleChannelHeader(_chanIcon, channelIconPath(), _chanBtn);
        // Overdue reminders already alarmed — tint the clock line like the
        // mention badge so "waiting for you" is visible at a glance. A plain
        // bookmark (no due date) shows a bookmark instead of the clock.
        const bool    reminder = _item.dueAt > 0;
        const bool    overdue  = reminder && _item.dueAt <= QDateTime::currentSecsSinceEpoch();
        const QColor &dueCol   = overdue ? th.badge.mention : th.text.secondary;
        _dueIcon->setPixmap(svgPixmap(
            reminder ? QStringLiteral(":/ui/alarm-clock.svg") : QStringLiteral(":/ui/bookmark.svg"),
            QSize(13, 13),
            dueCol
        ));
        _dueIcon->setStyleSheet(QStringLiteral("background: transparent;"));
        _dueLabel->setStyleSheet(QString("background: transparent; color: %1; font-size: %2px;")
                                     .arg(Th::qss(dueCol))
                                     .arg(th.fonts.caption));
        for (auto *row : findChildren<SavedMsgRow *>())
            row->update();
    }

private:
    QString channelIconPath() const {
        const Conversation *c = _session ? _session->findConversation(_item.conv) : nullptr;
        if (c && (c->kind == ConvKind::Im || c->kind == ConvKind::Mpim))
            return QStringLiteral(":/ui/message-square.svg");
        if (c && c->kind == ConvKind::PrivateChannel)
            return QStringLiteral(":/ui/lock.svg");
        return QStringLiteral(":/ui/hash.svg");
    }

    QString dueText() const {
        if (_item.dueAt <= 0)
            return tr("Saved for later");
        return tr("Reminder set for %1").arg(TimeFmt::formatDateTime(_item.dueAt));
    }

    MessageReminder _item;
    Session        *_session = nullptr;
    Callbacks       _cbs;

    QWidget     *_body     = nullptr;
    QLabel      *_chanIcon = nullptr;
    QPushButton *_chanBtn  = nullptr;
    QLabel      *_dueIcon  = nullptr;
    QLabel      *_dueLabel = nullptr;
};

// ── SavedMessagesPage ─────────────────────────────────────────────────────────

SavedMessagesPage::SavedMessagesPage(ImageCache *imgCache, QWidget *parent)
    : QWidget(parent), _imgCache(imgCache) {
    _page = OverviewCard::buildPage(this, QStringLiteral("saved"), tr("Saved messages"));

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void SavedMessagesPage::setSession(Session *session) {
    if (_session == session)
        return;
    _session           = session;
    _remindersLifetime = rpl::lifetime();
    clear();
    if (!_session)
        return;
    // Follow set/remove/server-sync/fired live — but only rebuild while the
    // page is on screen; open() rebuilds anyway when it comes back.
    _session->remindersChanged() | rpl::on_next(
                                       [this] {
                                           if (isVisible())
                                               rebuild();
                                       },
                                       _remindersLifetime
                                   );
}

void SavedMessagesPage::open() {
    rebuild();
    // Cards whose reminder carries no preview (set from another client, or its
    // enrichment lost) fetch their message now; remindersChanged() rebuilds
    // them as the answers land.
    if (_session)
        _session->resolveReminderPreviews();
}

void SavedMessagesPage::clear() {
    for (auto *card : _cards)
        card->deleteLater();
    _cards.clear();
    setStatus({});
}

void SavedMessagesPage::rebuild() {
    for (auto *card : _cards)
        card->deleteLater();
    _cards.clear();
    if (!_session)
        return;

    SavedCard::Callbacks cbs;
    cbs.openMessage = [this](ConversationId conv, Ts ts, Ts root) {
        emit openMessageRequested(conv, ts, root);
    };
    cbs.openChannel = [this](ConversationId conv) { emit openChannelRequested(conv); };
    for (const auto &r : _session->messageReminders()) {
        auto *card = new SavedCard(r, _session, _imgCache, cbs, _page.listHost);
        // Above the stretch.
        _page.listLayout->insertWidget(_page.listLayout->count() - 1, card);
        _cards.push_back(card);
    }
    setStatus(
        _cards.empty() ? tr("Messages you save for later or set reminders on will appear here.")
                       : QString()
    );
}

void SavedMessagesPage::setStatus(const QString &text) {
    _page.statusLabel->setText(text);
    _page.statusLabel->setVisible(!text.isEmpty());
}

void SavedMessagesPage::applyTheme() {
    OverviewCard::stylePage(this, _page);
    for (auto *card : _cards)
        card->applyTheme();
}

#include "saved_messages_page.moc"
