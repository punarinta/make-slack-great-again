// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "browse_channels_dialog.h"
#include "browse_list_view.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>

static constexpr int kCardPadH = 24;
static constexpr int kCardPadT = 20;
static constexpr int kCardPadB = 20;

// ── BrowseChannelsDialog ──────────────────────────────────────────────────────

BrowseChannelsDialog::BrowseChannelsDialog(
    const std::vector<Conversation> &conversations,
    const std::vector<User>         &users,
    ImageCache                      *imgCache,
    QWidget                         *parent
)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint),
      _conversations(conversations), _users(users), _imgCache(imgCache) {
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    // ── Card ─────────────────────────────────────────────────────────────────
    _card = new QFrame(this);
    _card->setObjectName("browseCard");
    auto *shadow = new QGraphicsDropShadowEffect(_card);
    shadow->setBlurRadius(40);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 70));
    _card->setGraphicsEffect(shadow);

    auto *cardLayout = new QVBoxLayout(_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // ── Top bar: search + Create Channel + close ──────────────────────────────
    auto *topBar = new QWidget(_card);
    topBar->setStyleSheet("background: transparent;");
    {
        auto *lay = new QHBoxLayout(topBar);
        lay->setContentsMargins(kCardPadH, kCardPadT, kCardPadH, 12);
        lay->setSpacing(10);

        _searchEdit = new StyledLineEdit(topBar);
        _searchEdit->setPlaceholderText(tr("Search for channels"));
        _searchEdit->setLeadingIcon(":/ui/search.svg");
        _searchEdit->lineEdit()->setClearButtonEnabled(true);
        _searchEdit->setMinimumWidth(200);
        lay->addWidget(_searchEdit, 1);

        _createBtn = new StyledButton(tr("Create Channel"), StyledButton::Variant::Primary, topBar);
        _createBtn->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(_createBtn);

        _closeBtn = new QPushButton("✕", topBar);
        _closeBtn->setFixedSize(32, 32);
        _closeBtn->setFlat(true);
        _closeBtn->setCursor(Qt::PointingHandCursor);
        _closeBtn->setFocusPolicy(Qt::NoFocus);
        QFont cf = _closeBtn->font();
        cf.setPointSizeF(cf.pointSizeF() * 1.1);
        _closeBtn->setFont(cf);
        lay->addWidget(_closeBtn, 0, Qt::AlignVCenter);
    }
    cardLayout->addWidget(topBar);

    // ── Tab bar ───────────────────────────────────────────────────────────────
    auto *tabBar = new QWidget(_card);
    tabBar->setStyleSheet("background: transparent;");
    {
        auto *lay = new QHBoxLayout(tabBar);
        lay->setContentsMargins(kCardPadH - 4, 0, kCardPadH, 0);
        lay->setSpacing(0);

        _channelsTab = new QPushButton(tr("Channels"), tabBar);
        _channelsTab->setCheckable(true);
        _channelsTab->setChecked(true);
        _channelsTab->setFocusPolicy(Qt::NoFocus);
        _channelsTab->setCursor(Qt::PointingHandCursor);
        lay->addWidget(_channelsTab);

        _peopleTab = new QPushButton(tr("People"), tabBar);
        _peopleTab->setCheckable(true);
        _peopleTab->setFocusPolicy(Qt::NoFocus);
        _peopleTab->setCursor(Qt::PointingHandCursor);
        lay->addWidget(_peopleTab);

        lay->addStretch();
    }
    cardLayout->addWidget(tabBar);

    auto *tabDivider = new QFrame(_card);
    tabDivider->setFrameShape(QFrame::HLine);
    tabDivider->setObjectName("tabDivider");
    tabDivider->setFixedHeight(1);
    cardLayout->addWidget(tabDivider);

    // ── Content stack: two virtual lists ───────────────────────────────────────
    _stack = new QStackedWidget(_card);
    _stack->setMinimumHeight(kListMinH);

    _channelList = new BrowseListView(_imgCache, _stack);
    _channelList->setObjectName("browseChannelList");
    _channelList->onActivated = [this](const QString &id) {
        accept();
        emit channelActivated(ConversationId{id});
    };
    _stack->addWidget(_channelList);

    _peopleList = new BrowseListView(_imgCache, _stack);
    _peopleList->setObjectName("browsePeopleList");
    _peopleList->onActivated = [this](const QString &id) {
        accept();
        emit userActivated(UserId{id});
    };
    _stack->addWidget(_peopleList);

    cardLayout->addWidget(_stack, 1);
    cardLayout->addSpacing(kCardPadB);

    // ── Populate ──────────────────────────────────────────────────────────────
    buildChannelItems();
    buildPeopleItems();

    // ── Connections ───────────────────────────────────────────────────────────
    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(_createBtn, &QPushButton::clicked, this, [this] {
        reject();
        emit createChannelRequested();
    });
    connect(_channelsTab, &QPushButton::clicked, this, [this] { selectTab(0); });
    connect(_peopleTab, &QPushButton::clicked, this, [this] { selectTab(1); });
    connect(_searchEdit, &StyledLineEdit::textChanged, this, &BrowseChannelsDialog::applyFilter);

    applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        update();
    });
}

void BrowseChannelsDialog::buildChannelItems() {
    std::vector<const Conversation *> channels;
    for (const auto &c : _conversations) {
        if (c.kind == ConvKind::PublicChannel || c.kind == ConvKind::PrivateChannel)
            channels.push_back(&c);
    }
    std::sort(channels.begin(), channels.end(), [](const Conversation *a, const Conversation *b) {
        return a->name.toLower() < b->name.toLower();
    });

    std::vector<BrowseListView::Item> items;
    items.reserve(channels.size());
    for (const auto *conv : channels) {
        QString subtitle;
        if (conv->memberCount > 0) {
            subtitle = tr("%1 %2")
                           .arg(conv->memberCount)
                           .arg(conv->memberCount == 1 ? tr("member") : tr("members"));
            if (!conv->description.isEmpty())
                subtitle += " · " + conv->description;
        } else if (!conv->description.isEmpty()) {
            subtitle = conv->description;
        }

        BrowseListView::Item it;
        it.id        = conv->id.value;
        it.title     = conv->name;
        it.subtitle  = subtitle;
        it.isPrivate = conv->kind == ConvKind::PrivateChannel;
        it.isMember  = conv->isMember;
        it.searchKey = (conv->name + " " + conv->description).toLower();
        items.push_back(std::move(it));
    }
    _channelList->setItems(std::move(items));
}

void BrowseChannelsDialog::buildPeopleItems() {
    std::vector<const User *> people;
    for (const auto &u : _users) {
        if (!u.isDeactivated)
            people.push_back(&u);
    }
    std::sort(people.begin(), people.end(), [](const User *a, const User *b) {
        const QString na = a->displayName.isEmpty() ? a->name : a->displayName;
        const QString nb = b->displayName.isEmpty() ? b->name : b->displayName;
        return na.toLower() < nb.toLower();
    });

    std::vector<BrowseListView::Item> items;
    items.reserve(people.size());
    for (const auto *user : people) {
        const QString displayName = user->displayName.isEmpty() ? user->name : user->displayName;

        BrowseListView::Item it;
        it.id        = user->id.value;
        it.title     = displayName;
        it.avatarUrl = user->avatarUrl;
        it.initial   = displayName.left(1);
        it.isPerson  = true;
        if (!user->name.isEmpty() && user->name != displayName)
            it.subtitle = "@" + user->name;
        it.searchKey = (displayName + " " + user->name).toLower();
        items.push_back(std::move(it));
    }
    _peopleList->setItems(std::move(items));
}

void BrowseChannelsDialog::applyFilter(const QString &query) {
    _channelList->applyFilter(query);
    _peopleList->applyFilter(query);
}

void BrowseChannelsDialog::selectTab(int tab) {
    _activeTab = tab;
    _channelsTab->setChecked(tab == 0);
    _peopleTab->setChecked(tab == 1);
    _stack->setCurrentIndex(tab);
    _searchEdit->setPlaceholderText(tab == 0 ? tr("Search for channels") : tr("Search for people"));
    applyFilter(_searchEdit->text());
    applyTheme();
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void BrowseChannelsDialog::applyTheme() {
    _card->setStyleSheet(
        "QFrame#browseCard { background: white; border-radius: 12px; border: none; }"
    );

    // Search input and Create button self-theme (StyledLineEdit / StyledButton).

    _closeBtn->setStyleSheet(QString(
                                 "QPushButton {"
                                 "  border: none; border-radius: 16px;"
                                 "  color: %1; background: transparent;"
                                 "}"
                                 "QPushButton:hover { background: %2; color: %3; }"
    )
                                 .arg(
                                     Th::qss(Th::c().text.tertiary),
                                     Th::qss(Th::c().surface.highlight),
                                     Th::qss(Th::c().text.secondary)
                                 ));

    const QString tabActive = QString(
                                  "QPushButton {"
                                  "  border: none; border-bottom: 2px solid %1;"
                                  "  padding: 8px 16px; background: transparent;"
                                  "  color: %2; font-weight: bold;"
                                  "}"
    )
                                  .arg(Th::qss(Th::c().accent.def), Th::qss(Th::c().text.primary));

    const QString tabInactive =
        QString(
            "QPushButton {"
            "  border: none; border-bottom: 2px solid transparent;"
            "  padding: 8px 16px; background: transparent; color: %1;"
            "}"
            "QPushButton:hover { color: %2; }"
        )
            .arg(Th::qss(Th::c().text.secondary), Th::qss(Th::c().text.primary));

    _channelsTab->setStyleSheet(_activeTab == 0 ? tabActive : tabInactive);
    _peopleTab->setStyleSheet(_activeTab == 1 ? tabActive : tabInactive);

    // Divider lines
    const QString lineStyle =
        QString("background: %1; border: none;").arg(Th::qss(Th::c().divider.subtle));
    for (auto *f : _card->findChildren<QFrame *>()) {
        if (f->frameShape() == QFrame::HLine)
            f->setStyleSheet(lineStyle);
    }
}

// ── Layout ────────────────────────────────────────────────────────────────────

void BrowseChannelsDialog::updateCard() {
    const int avail = width() > 0 ? width() - 80 : kCardW;
    const int cardW = std::min(avail, kCardW);
    _card->setFixedWidth(cardW);
    _card->adjustSize();
    const int cardH = std::min(_card->sizeHint().height(), std::max(kCardMinH, height() - 80));
    _card->resize(cardW, cardH);
    _card->move((width() - cardW) / 2, (height() - cardH) / 2);
}

// ── Events ────────────────────────────────────────────────────────────────────

void BrowseChannelsDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 140));
}

void BrowseChannelsDialog::showEvent(QShowEvent *e) {
    if (QWidget *top = parentWidget() ? parentWidget()->window() : nullptr)
        setGeometry(top->geometry());
    updateCard();
    QDialog::showEvent(e);
}

void BrowseChannelsDialog::resizeEvent(QResizeEvent *e) {
    QDialog::resizeEvent(e);
    updateCard();
}

void BrowseChannelsDialog::mousePressEvent(QMouseEvent *e) {
    if (!_card->geometry().contains(e->pos()))
        reject();
    else
        QDialog::mousePressEvent(e);
}
