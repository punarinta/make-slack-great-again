// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "browse_channels_dialog.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"

#include <QApplication>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>

static constexpr int kItemH       = 60;
static constexpr int kPeopleItemH = 60;
static constexpr int kAvatarSize  = 32;
static constexpr int kCardPadH    = 24;
static constexpr int kCardPadT    = 20;
static constexpr int kCardPadB    = 20;

// Intentional non-token (Slack brand semantic green)
static constexpr const char *kJoinedBadgeColor = "#2BAC76";

// ── Helpers ───────────────────────────────────────────────────────────────────

// Renders a circular avatar: either from a downloaded image or an initial-letter
// placeholder using a color derived from the userId.
static QPixmap
makeAvatarPixmap(const QPixmap &src, int size, const QString &initial, const QString &userId) {
    const qreal  dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    const QSize  phys(qRound(size * dpr), qRound(size * dpr));
    const QRectF rect(0, 0, size, size);

    QPixmap px(phys);
    px.fill(Qt::transparent);
    px.setDevicePixelRatio(dpr);

    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);

    if (!src.isNull()) {
        QPainterPath clip;
        clip.addEllipse(rect);
        p.setClipPath(clip);
        QPixmap scaled = src.scaled(phys, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(rect.toRect(), scaled);
    } else {
        const int hue =
            userId.isEmpty() ? 0 : qAbs(static_cast<int>(userId.at(0).unicode())) * 137 % 360;
        const QColor bg = QColor::fromHsl(hue, 60, 55);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawEllipse(rect);
        if (!initial.isEmpty()) {
            p.setPen(Qt::white);
            QFont f = QApplication::font();
            f.setBold(true);
            f.setPointSizeF(size * 0.38);
            p.setFont(f);
            p.drawText(rect.toRect(), Qt::AlignCenter, initial.left(1).toUpper());
        }
    }
    p.end();
    return px;
}

// ── Channel item widget ───────────────────────────────────────────────────────

static QWidget *makeChannelItemWidget(const Conversation &conv, QWidget *parent) {
    auto *w = new QWidget(parent);
    w->setAttribute(Qt::WA_TransparentForMouseEvents);
    w->setStyleSheet("background: transparent;");

    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(kCardPadH, 0, kCardPadH, 0);
    row->setSpacing(12);

    auto *left = new QVBoxLayout;
    left->setSpacing(2);

    auto *nameRow = new QHBoxLayout;
    nameRow->setSpacing(6);
    nameRow->setContentsMargins(0, 0, 0, 0);

    auto *iconLabel = new QLabel(w);
    if (conv.kind == ConvKind::PrivateChannel)
        iconLabel->setPixmap(svgPixmap(":/ui/lock.svg", QSize(14, 14), Th::c().text.secondary));
    else
        iconLabel->setPixmap(svgPixmap(":/ui/hash.svg", QSize(14, 14), Th::c().text.secondary));
    iconLabel->setFixedSize(14, 14);
    iconLabel->setStyleSheet("background: transparent;");
    nameRow->addWidget(iconLabel, 0, Qt::AlignVCenter);

    auto *nameLabel = new QLabel(conv.name, w);
    nameLabel->setStyleSheet("background: transparent;");
    QFont nf = nameLabel->font();
    nf.setBold(true);
    nameLabel->setFont(nf);
    nameRow->addWidget(nameLabel, 1, Qt::AlignVCenter);
    left->addLayout(nameRow);

    QString subtitle;
    if (conv.memberCount > 0) {
        subtitle = QObject::tr("%1 %2")
                       .arg(conv.memberCount)
                       .arg(conv.memberCount == 1 ? QObject::tr("member") : QObject::tr("members"));
        if (!conv.description.isEmpty())
            subtitle += " · " + conv.description;
    } else if (!conv.description.isEmpty()) {
        subtitle = conv.description;
    }
    if (!subtitle.isEmpty()) {
        auto *subLabel = new QLabel(subtitle, w);
        QFont sf       = subLabel->font();
        sf.setPointSizeF(sf.pointSizeF() * 0.88);
        subLabel->setFont(sf);
        subLabel->setStyleSheet(
            QString("color: %1; background: transparent;").arg(Th::qss(Th::c().text.secondary))
        );
        left->addWidget(subLabel);
    }
    row->addLayout(left, 1);

    if (conv.isMember) {
        auto *joinedRow = new QHBoxLayout;
        joinedRow->setSpacing(4);
        joinedRow->setContentsMargins(0, 0, 0, 0);

        auto *checkIcon = new QLabel(w);
        checkIcon->setStyleSheet("background: transparent;");
        checkIcon->setPixmap(svgPixmap(":/ui/check.svg", QSize(13, 13), Th::c().text.secondary));
        checkIcon->setFixedSize(13, 13);
        joinedRow->addWidget(checkIcon, 0, Qt::AlignVCenter);

        auto *joinedLabel = new QLabel(QObject::tr("Joined"), w);
        QFont jf          = joinedLabel->font();
        jf.setPointSizeF(jf.pointSizeF() * 0.88);
        joinedLabel->setFont(jf);
        joinedLabel->setStyleSheet(
            QString("color: %1; background: transparent;").arg(Th::qss(Th::c().text.secondary))
        );
        joinedRow->addWidget(joinedLabel, 0, Qt::AlignVCenter);

        row->addLayout(joinedRow);
    }
    return w;
}

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

        _searchEdit = new QLineEdit(topBar);
        _searchEdit->setPlaceholderText(tr("Search for channels"));
        _searchEdit->setClearButtonEnabled(true);
        _searchEdit->addAction(
            svgIcon(":/ui/search.svg", QSize(16, 16), Th::c().text.tertiary),
            QLineEdit::LeadingPosition
        );
        _searchEdit->setMinimumWidth(200);
        lay->addWidget(_searchEdit, 1);

        _createBtn = new QPushButton(tr("Create Channel"), topBar);
        _createBtn->setCursor(Qt::PointingHandCursor);
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

    // ── Content stack ─────────────────────────────────────────────────────────
    _stack = new QStackedWidget(_card);
    _stack->setMinimumHeight(kListMinH);

    // Channel page
    {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        _channelPage   = new QWidget;
        _channelLayout = new QVBoxLayout(_channelPage);
        _channelLayout->setContentsMargins(0, 0, 0, 0);
        _channelLayout->setSpacing(0);
        _channelLayout->addStretch();

        scroll->setWidget(_channelPage);
        _stack->addWidget(scroll);
    }

    // People page
    {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        _peoplePage   = new QWidget;
        _peopleLayout = new QVBoxLayout(_peoplePage);
        _peopleLayout->setContentsMargins(0, 0, 0, 0);
        _peopleLayout->setSpacing(0);
        _peopleLayout->addStretch();

        scroll->setWidget(_peoplePage);
        _stack->addWidget(scroll);
    }

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
    connect(_searchEdit, &QLineEdit::textChanged, this, &BrowseChannelsDialog::applyFilter);

    if (_imgCache) {
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
            auto it = _avatarLabels.constFind(url);
            if (it == _avatarLabels.constEnd() || !it.value())
                return;
            const QPixmap px = _imgCache->get(url);
            for (const auto &u : _users) {
                if (u.avatarUrl == url) {
                    const QString initial =
                        (u.displayName.isEmpty() ? u.name : u.displayName).left(1);
                    it.value()->setPixmap(makeAvatarPixmap(px, kAvatarSize, initial, u.id.value));
                    break;
                }
            }
        });
    }

    applyTheme();

    // QLineEdit adds internal vertical metrics beyond CSS padding; force the
    // button to match the input's actual computed height.
    _createBtn->setFixedHeight(_searchEdit->sizeHint().height());

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        update();
    });
}

void BrowseChannelsDialog::buildChannelItems() {
    while (_channelLayout->count() > 1)
        delete _channelLayout->takeAt(0)->widget();

    std::vector<const Conversation *> channels;
    for (const auto &c : _conversations) {
        if (c.kind == ConvKind::PublicChannel || c.kind == ConvKind::PrivateChannel)
            channels.push_back(&c);
    }
    std::sort(channels.begin(), channels.end(), [](const Conversation *a, const Conversation *b) {
        return a->name.toLower() < b->name.toLower();
    });

    for (const auto *conv : channels) {
        auto *itemFrame = new QFrame(_channelPage);
        itemFrame->setObjectName("channelItem");
        itemFrame->setFixedHeight(kItemH);
        itemFrame->setCursor(Qt::PointingHandCursor);
        itemFrame->setAttribute(Qt::WA_Hover);

        auto *lay = new QVBoxLayout(itemFrame);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(makeChannelItemWidget(*conv, itemFrame), 1, Qt::AlignVCenter);

        auto *line = new QFrame(itemFrame);
        line->setFrameShape(QFrame::HLine);
        line->setObjectName("itemLine");
        line->setFixedHeight(1);
        lay->addWidget(line);

        itemFrame->setProperty("convId", conv->id.value);
        itemFrame->installEventFilter(this);

        _channelLayout->insertWidget(_channelLayout->count() - 1, itemFrame);
    }
}

void BrowseChannelsDialog::buildPeopleItems() {
    _avatarLabels.clear();
    while (_peopleLayout->count() > 1)
        delete _peopleLayout->takeAt(0)->widget();

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

    for (const auto *user : people) {
        auto *itemFrame = new QFrame(_peoplePage);
        itemFrame->setObjectName("peopleItem");
        itemFrame->setFixedHeight(kPeopleItemH);
        itemFrame->setCursor(Qt::PointingHandCursor);
        itemFrame->setAttribute(Qt::WA_Hover);

        auto *lay = new QVBoxLayout(itemFrame);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        // ── Item content ─────────────────────────────────────────────────────
        auto *w = new QWidget(itemFrame);
        w->setAttribute(Qt::WA_TransparentForMouseEvents);
        w->setStyleSheet("background: transparent;");

        auto *row = new QHBoxLayout(w);
        row->setContentsMargins(kCardPadH, 0, kCardPadH, 0);
        row->setSpacing(12);

        // Avatar
        auto *avatarLabel = new QLabel(w);
        avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
        avatarLabel->setStyleSheet("background: transparent;");

        const QString initial =
            (user->displayName.isEmpty() ? user->name : user->displayName).left(1);
        const QPixmap cached =
            (_imgCache && !user->avatarUrl.isEmpty()) ? _imgCache->get(user->avatarUrl) : QPixmap{};
        avatarLabel->setPixmap(makeAvatarPixmap(cached, kAvatarSize, initial, user->id.value));

        if (!user->avatarUrl.isEmpty())
            _avatarLabels.insert(user->avatarUrl, avatarLabel);

        row->addWidget(avatarLabel, 0, Qt::AlignVCenter);

        // Name + username
        auto *left = new QVBoxLayout;
        left->setSpacing(1);

        const QString displayName = user->displayName.isEmpty() ? user->name : user->displayName;
        auto         *nameLabel   = new QLabel(displayName, w);
        nameLabel->setStyleSheet("background: transparent;");
        QFont nf = nameLabel->font();
        nf.setBold(true);
        nameLabel->setFont(nf);
        left->addWidget(nameLabel);

        if (!user->name.isEmpty() && user->name != displayName) {
            auto *unLabel = new QLabel("@" + user->name, w);
            QFont sf      = unLabel->font();
            sf.setPointSizeF(sf.pointSizeF() * 0.88);
            unLabel->setFont(sf);
            unLabel->setStyleSheet(
                QString("color: %1; background: transparent;").arg(Th::qss(Th::c().text.secondary))
            );
            left->addWidget(unLabel);
        }
        row->addLayout(left, 1);

        lay->addWidget(w, 1, Qt::AlignVCenter);

        auto *line = new QFrame(itemFrame);
        line->setFrameShape(QFrame::HLine);
        line->setObjectName("itemLine");
        line->setFixedHeight(1);
        lay->addWidget(line);

        itemFrame->setProperty("userId", user->id.value);
        itemFrame->installEventFilter(this);

        _peopleLayout->insertWidget(_peopleLayout->count() - 1, itemFrame);
    }
}

void BrowseChannelsDialog::applyFilter(const QString &query) {
    const QString q = query.trimmed().toLower();

    for (int i = 0; i < _channelLayout->count() - 1; ++i) {
        auto *item = _channelLayout->itemAt(i)->widget();
        if (!item)
            continue;
        bool match = true;
        if (!q.isEmpty()) {
            const QString convId = item->property("convId").toString();
            for (const auto &c : _conversations) {
                if (c.id.value == convId) {
                    match = c.name.toLower().contains(q) || c.description.toLower().contains(q);
                    break;
                }
            }
        }
        item->setVisible(match);
    }

    for (int i = 0; i < _peopleLayout->count() - 1; ++i) {
        auto *item = _peopleLayout->itemAt(i)->widget();
        if (!item)
            continue;
        bool match = true;
        if (!q.isEmpty()) {
            const QString userId = item->property("userId").toString();
            for (const auto &u : _users) {
                if (u.id.value == userId) {
                    match = u.displayName.toLower().contains(q) || u.name.toLower().contains(q);
                    break;
                }
            }
        }
        item->setVisible(match);
    }
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

bool BrowseChannelsDialog::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QVariant convId = obj->property("convId");
            if (convId.isValid()) {
                accept();
                emit channelActivated(ConversationId{convId.toString()});
                return true;
            }
            const QVariant userId = obj->property("userId");
            if (userId.isValid()) {
                accept();
                emit userActivated(UserId{userId.toString()});
                return true;
            }
        }
    }
    return QDialog::eventFilter(obj, event);
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void BrowseChannelsDialog::applyTheme() {
    _card->setStyleSheet(
        "QFrame#browseCard { background: white; border-radius: 12px; border: none; }"
    );

    _searchEdit->setStyleSheet(QString("QLineEdit {"
                                       "  border: 1.5px solid %1; border-radius: 8px;"
                                       "  padding: 6px 8px; background: %2; color: %3;"
                                       "}"
                                       "QLineEdit:focus { border-color: %4; }")
                                   .arg(
                                       Th::qss(Th::c().divider.strong),
                                       Th::qss(Th::c().surface.raised),
                                       Th::qss(Th::c().text.primary),
                                       Th::qss(Th::c().accent.def)
                                   ));

    _createBtn->setStyleSheet(
        QString("QPushButton {"
                "  background: %1; color: %2; border: 1.5px solid transparent;"
                "  border-radius: 6px; padding: 6px 16px;"
                "}"
                "QPushButton:hover { background: %3; }")
            .arg(
                Th::qss(Th::c().accent.def),
                Th::qss(Th::c().accent.text),
                Th::qss(Th::c().accent.dark)
            )
    );

    _closeBtn->setStyleSheet(QString("QPushButton {"
                                     "  border: none; border-radius: 16px;"
                                     "  color: %1; background: transparent;"
                                     "}"
                                     "QPushButton:hover { background: %2; color: %3; }")
                                 .arg(
                                     Th::qss(Th::c().text.tertiary),
                                     Th::qss(Th::c().surface.highlight),
                                     Th::qss(Th::c().text.secondary)
                                 ));

    const QString tabActive = QString("QPushButton {"
                                      "  border: none; border-bottom: 2px solid %1;"
                                      "  padding: 8px 16px; background: transparent;"
                                      "  color: %2; font-weight: bold;"
                                      "}")
                                  .arg(Th::qss(Th::c().accent.def), Th::qss(Th::c().text.primary));

    const QString tabInactive =
        QString("QPushButton {"
                "  border: none; border-bottom: 2px solid transparent;"
                "  padding: 8px 16px; background: transparent; color: %1;"
                "}"
                "QPushButton:hover { color: %2; }")
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

    // Scroll areas and their content pages: no background fill so item hover shows through
    for (auto *sa : _card->findChildren<QScrollArea *>()) {
        sa->setStyleSheet("QScrollArea { border: none; background: transparent; }");
        if (sa->widget())
            sa->widget()->setStyleSheet("background: transparent;");
    }

    // Item hover
    const QString channelHover = QString("QFrame#channelItem { background: transparent; }"
                                         "QFrame#channelItem:hover { background: %1; }")
                                     .arg(Th::qss(Th::c().surface.highlight));
    for (auto *f : _channelPage->findChildren<QFrame *>("channelItem", Qt::FindDirectChildrenOnly))
        f->setStyleSheet(channelHover);

    const QString peopleHover = QString("QFrame#peopleItem { background: transparent; }"
                                        "QFrame#peopleItem:hover { background: %1; }")
                                    .arg(Th::qss(Th::c().surface.highlight));
    for (auto *f : _peoplePage->findChildren<QFrame *>("peopleItem", Qt::FindDirectChildrenOnly))
        f->setStyleSheet(peopleHover);

    // Primary text color on unlabeled QLabels
    const QString labelColor =
        QString("color: %1; background: transparent;").arg(Th::qss(Th::c().text.primary));
    for (auto *label : _card->findChildren<QLabel *>()) {
        if (label->styleSheet().isEmpty() && label->pixmap().isNull())
            label->setStyleSheet(labelColor);
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
