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
#include "ui/user_avatar.h"
#include "util/time_format.h"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

// Match the message list's avatar geometry so saved rows read like chat rows.
constexpr int kAvatarSize   = 36;
constexpr int kAvatarRadius = 4;
constexpr int kAvatarGap    = 10;

} // namespace

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

        if (!_item.botAvatarUrl.isEmpty()) {
            _avatarUrl = _item.botAvatarUrl;
        } else if (_session && !_item.author.value.isEmpty()) {
            if (const User *u = _session->findUser(_item.author))
                _avatarUrl = u->avatarUrl;
        }
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
        const QFontMetrics nfm(nameFont());
        const QFontMetrics bfm(QApplication::font());
        const int          textH = padV() + nfm.height() + hdrGap() + bfm.height() + padV();
        return {kAvatarSize * 3, std::max(textH, padV() + kAvatarSize + padV())};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Avatar.
        const QRect   avRect(0, padV(), kAvatarSize, kAvatarSize);
        const QPixmap px =
            (_imgCache && !_avatarUrl.isEmpty()) ? _imgCache->get(_avatarUrl) : QPixmap{};
        if (!px.isNull())
            UserAvatar::paintPhoto(p, avRect, px, devicePixelRatioF(), kAvatarRadius);
        else
            UserAvatar::paintInitial(
                p,
                avRect,
                displayName().left(1),
                Th::c().presence.away,
                Qt::white,
                kAvatarRadius,
                avRect.height() * 0.38
            );

        // Header: name + the message's own timestamp.
        const QFont        nf = nameFont();
        const QFontMetrics nfm(nf);
        p.setFont(nf);
        p.setPen(Th::c().text.primary);
        const QString name = nfm.elidedText(displayName(), Qt::ElideRight, width() - textLeft());
        p.drawText(textLeft(), padV() + nfm.ascent(), name);

        QFont tf = QApplication::font();
        tf.setPointSizeF(tf.pointSizeF() * Th::c().fontScales.timestamp);
        p.setFont(tf);
        p.setPen(Th::c().text.secondary);
        p.drawText(
            textLeft() + nfm.horizontalAdvance(name) + Th::c().spacing.md,
            padV() + nfm.ascent(),
            MsgRender::dateTimeLabel(decimalTsToMicros(_item.ts))
        );

        // Snippet, one elided line.
        const QFont        bf = QApplication::font();
        const QFontMetrics bfm(bf);
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
    static QFont nameFont() {
        QFont f = QApplication::font();
        f.setWeight(QFont::DemiBold);
        return f;
    }
    int padV() const { return Th::c().spacing.sm; }
    int hdrGap() const { return Th::c().spacing.xs; }
    int textLeft() const { return kAvatarSize + kAvatarGap; }

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
        auto *nameRow = new QHBoxLayout();
        nameRow->setContentsMargins(0, 0, 0, 0);
        nameRow->setSpacing(sp.sm);
        _chanIcon = new QLabel(this);
        nameRow->addWidget(_chanIcon);
        _chanBtn = new QPushButton(channelLabel(), this);
        _chanBtn->setFlat(true);
        _chanBtn->setCursor(Qt::PointingHandCursor);
        connect(_chanBtn, &QPushButton::clicked, this, [this] {
            if (_cbs.openChannel)
                _cbs.openChannel(_item.conv);
        });
        nameRow->addWidget(_chanBtn);
        nameRow->addStretch(1);
        outer->addLayout(nameRow);

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
        _body->setStyleSheet(
            QString(
                "QWidget#savedCardBody { background: %1; border: 1px solid %2; "
                "border-radius: 8px; }"
            )
                .arg(Th::qss(th.surface.content), Th::qss(th.message.attachmentBorder))
        );
        _chanIcon->setPixmap(svgPixmap(channelIconPath(), QSize(15, 15), th.text.primary));
        _chanIcon->setStyleSheet(QStringLiteral("background: transparent;"));
        _chanBtn->setStyleSheet(
            QString(
                "QPushButton { border: none; background: transparent; padding: 0; "
                "color: %1; font-size: %2px; font-weight: 700; text-align: left; }"
                "QPushButton:hover { text-decoration: underline; }"
            )
                .arg(Th::qss(th.text.primary))
                .arg(th.fonts.lg)
        );
        // Overdue reminders already alarmed — tint the clock line like the
        // mention badge so "waiting for you" is visible at a glance.
        const bool    overdue = _item.dueAt <= QDateTime::currentSecsSinceEpoch();
        const QColor &dueCol  = overdue ? th.badge.mention : th.text.secondary;
        _dueIcon->setPixmap(
            svgPixmap(QStringLiteral(":/ui/alarm-clock.svg"), QSize(13, 13), dueCol)
        );
        _dueIcon->setStyleSheet(QStringLiteral("background: transparent;"));
        _dueLabel->setStyleSheet(QString("background: transparent; color: %1; font-size: %2px;")
                                     .arg(Th::qss(dueCol))
                                     .arg(th.fonts.caption));
        for (auto *row : findChildren<SavedMsgRow *>())
            row->update();
    }

private:
    QString channelLabel() const {
        const Conversation *c = _session ? _session->findConversation(_item.conv) : nullptr;
        if (!c)
            return _item.conv.value;
        if (c->kind == ConvKind::Im && c->dmUser)
            return _session->userDisplayName(*c->dmUser);
        return c->name;
    }

    QString channelIconPath() const {
        const Conversation *c = _session ? _session->findConversation(_item.conv) : nullptr;
        if (c && (c->kind == ConvKind::Im || c->kind == ConvKind::Mpim))
            return QStringLiteral(":/ui/message-square.svg");
        if (c && c->kind == ConvKind::PrivateChannel)
            return QStringLiteral(":/ui/lock.svg");
        return QStringLiteral(":/ui/hash.svg");
    }

    QString dueText() const {
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
    setObjectName("savedPage");
    setAttribute(Qt::WA_StyledBackground);

    const auto &sp   = Th::c().spacing;
    auto       *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header matches the threads page's: 48px, bold, lg.
    _headerRow = new QWidget(this);
    _headerRow->setObjectName("savedHeader");
    _headerRow->setAttribute(Qt::WA_StyledBackground);
    _headerRow->setFixedHeight(48);
    auto *headerLayout = new QHBoxLayout(_headerRow);
    headerLayout->setContentsMargins(sp.xl, 0, sp.md, 0);
    _titleLabel = new QLabel(tr("Saved messages"), _headerRow);
    headerLayout->addWidget(_titleLabel, 1);
    root->addWidget(_headerRow);

    _scroll = new QScrollArea(this);
    _scroll->setWidgetResizable(true);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->viewport()->setAutoFillBackground(false);
    root->addWidget(_scroll, 1);

    _listHost   = new QWidget(_scroll);
    _listLayout = new QVBoxLayout(_listHost);
    _listLayout->setContentsMargins(sp.xxl, sp.xxl, sp.xxl, sp.xxl);
    _listLayout->setSpacing(sp.xxl);

    _statusLabel = new QLabel(_listHost);
    _statusLabel->setAlignment(Qt::AlignHCenter);
    _statusLabel->setWordWrap(true);
    _statusLabel->hide();
    _listLayout->addWidget(_statusLabel);

    _listLayout->addStretch(1);
    _listHost->setObjectName("savedList");
    // Styled, not palette-filled — see ThreadsPage on why setWidget() +
    // autoFillBackground would paint the wrong color.
    _listHost->setAttribute(Qt::WA_StyledBackground);
    _scroll->setWidget(_listHost);
    _listHost->setAutoFillBackground(false);

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
        auto *card = new SavedCard(r, _session, _imgCache, cbs, _listHost);
        // Above the stretch.
        _listLayout->insertWidget(_listLayout->count() - 1, card);
        _cards.push_back(card);
    }
    setStatus(_cards.empty() ? tr("Messages you set reminders on will appear here.") : QString());
}

void SavedMessagesPage::setStatus(const QString &text) {
    _statusLabel->setText(text);
    _statusLabel->setVisible(!text.isEmpty());
}

void SavedMessagesPage::applyTheme() {
    const auto &th = Th::c();
    setStyleSheet(QString("QWidget#savedPage { background: %1; }").arg(Th::qss(th.surface.sunken)));
    _headerRow->setStyleSheet(QString(
                                  "QWidget#savedHeader { background: %1; "
                                  "border-bottom: 1px solid %2; }"
    )
                                  .arg(Th::qss(th.surface.sunken), Th::qss(th.divider.subtle)));
    _listHost->setStyleSheet(
        QString("QWidget#savedList { background: %1; }").arg(Th::qss(th.surface.sunken))
    );
    _titleLabel->setStyleSheet(
        QString("background: transparent; font-weight: bold; font-size: %1px; color: %2;")
            .arg(th.fonts.xxl)
            .arg(Th::qss(th.text.primary))
    );
    _statusLabel->setStyleSheet(QString("background: transparent; color: %1; padding: %2px;")
                                    .arg(Th::qss(th.text.secondary))
                                    .arg(th.spacing.xxl));
    _scroll->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; }") + Th::scrollBarQss()
    );
    for (auto *card : _cards)
        card->applyTheme();
}

#include "saved_messages_page.moc"
