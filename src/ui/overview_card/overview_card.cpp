// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "overview_card.h"
#include "session/session.h"
#include "ui/fonts.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/theme.h"
#include "ui/user_avatar.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace OverviewCard {

const QFont &nameFont() {
    return Ui::Fonts::get().demiBold;
}

const QFontMetrics &nameFontMetrics() {
    return Ui::Fonts::get().demiBoldFm;
}

int rowPadV() {
    return Th::c().spacing.sm;
}

int rowHdrGap() {
    return Th::c().spacing.xs;
}

QString avatarUrl(Session *session, const UserId &author, const QString &botAvatarUrl) {
    if (!botAvatarUrl.isEmpty())
        return botAvatarUrl;
    if (session && !author.value.isEmpty()) {
        if (const User *u = session->findUser(author))
            return u->avatarUrl;
    }
    return {};
}

void paintRowHeader(
    QPainter      &p,
    int            width,
    qreal          dpr,
    ImageCache    *imgCache,
    const QString &avatarUrl,
    const QString &name,
    const QString &timeLabel
) {
    // Avatar (no presence dot — this is a digest, not the roster).
    const QRect   avRect(0, rowPadV(), kAvatarSize, kAvatarSize);
    const QPixmap px = (imgCache && !avatarUrl.isEmpty()) ? imgCache->get(avatarUrl) : QPixmap{};
    if (!px.isNull())
        UserAvatar::paintPhoto(p, avRect, px, dpr, kAvatarRadius);
    else
        UserAvatar::paintInitial(
            p,
            avRect,
            name.left(1),
            Th::c().presence.away,
            Qt::white,
            kAvatarRadius,
            avRect.height() * 0.38
        );

    // Header: name + timestamp.
    const QFontMetrics &nfm = nameFontMetrics();
    p.setFont(nameFont());
    p.setPen(Th::c().text.primary);
    const QString shown = nfm.elidedText(name, Qt::ElideRight, width - kTextLeft);
    p.drawText(kTextLeft, rowPadV() + nfm.ascent(), shown);

    p.setFont(Ui::Fonts::get().cardTs);
    p.setPen(Th::c().text.secondary);
    p.drawText(
        kTextLeft + nfm.horizontalAdvance(shown) + Th::c().spacing.md,
        rowPadV() + nfm.ascent(),
        timeLabel
    );
}

QHBoxLayout *makeChannelHeader(
    QWidget              *card,
    const QString        &label,
    std::function<void()> onClicked,
    QLabel              **icon,
    QPushButton         **button
) {
    auto *nameRow = new QHBoxLayout();
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(Th::c().spacing.sm);
    *icon = new QLabel(card);
    nameRow->addWidget(*icon);
    *button = new QPushButton(label, card);
    (*button)->setFlat(true);
    (*button)->setCursor(Qt::PointingHandCursor);
    QObject::connect(*button, &QPushButton::clicked, card, std::move(onClicked));
    nameRow->addWidget(*button);
    nameRow->addStretch(1);
    return nameRow;
}

void styleChannelHeader(QLabel *icon, const QString &iconPath, QPushButton *button) {
    const auto &th = Th::c();
    icon->setPixmap(svgPixmap(iconPath, QSize(15, 15), th.text.primary));
    icon->setStyleSheet(QStringLiteral("background: transparent;"));
    button->setStyleSheet(QString(
                              "QPushButton { border: none; background: transparent; padding: 0; "
                              "color: %1; font-size: %2px; font-weight: 700; text-align: left; }"
                              "QPushButton:hover { text-decoration: underline; }"
    )
                              .arg(Th::qss(th.text.primary))
                              .arg(th.fonts.lg));
}

QString conversationLabel(Session *session, const ConversationId &conv) {
    const Conversation *c = session ? session->findConversation(conv) : nullptr;
    if (!c)
        return conv.value;
    if (c->kind == ConvKind::Im && c->dmUser)
        return session->userDisplayName(*c->dmUser);
    return c->name;
}

void styleCardBody(QWidget *body) {
    const auto &th = Th::c();
    body->setStyleSheet(QString(
                            "QWidget#%1 { background: %2; border: 1px solid %3; "
                            "border-radius: 8px; }"
    )
                            .arg(
                                body->objectName(),
                                Th::qss(th.surface.content),
                                Th::qss(th.message.attachmentBorder)
                            ));
}

Page buildPage(QWidget *page, const QString &prefix, const QString &title) {
    Page parts;
    page->setObjectName(prefix + QLatin1String("Page"));
    page->setAttribute(Qt::WA_StyledBackground);

    const auto &sp   = Th::c().spacing;
    auto       *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header matches the thread panel's: 48px, bold, lg. The hairline in
    // stylePage separates it from the card list below.
    parts.headerRow = new QWidget(page);
    parts.headerRow->setObjectName(prefix + QLatin1String("Header"));
    parts.headerRow->setAttribute(Qt::WA_StyledBackground);
    parts.headerRow->setFixedHeight(48);
    auto *headerLayout = new QHBoxLayout(parts.headerRow);
    headerLayout->setContentsMargins(sp.xl, 0, sp.md, 0);
    parts.titleLabel = new QLabel(title, parts.headerRow);
    headerLayout->addWidget(parts.titleLabel, 1);
    root->addWidget(parts.headerRow);

    parts.scroll = new QScrollArea(page);
    parts.scroll->setWidgetResizable(true);
    parts.scroll->setFrameShape(QFrame::NoFrame);
    parts.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    parts.scroll->viewport()->setAutoFillBackground(false);
    root->addWidget(parts.scroll, 1);

    parts.listHost   = new QWidget(parts.scroll);
    parts.listLayout = new QVBoxLayout(parts.listHost);
    // Generous official-client rhythm: ~24px page gutter and between sections.
    parts.listLayout->setContentsMargins(sp.xxl, sp.xxl, sp.xxl, sp.xxl);
    parts.listLayout->setSpacing(sp.xxl);

    parts.statusLabel = new QLabel(parts.listHost);
    parts.statusLabel->setAlignment(Qt::AlignHCenter);
    parts.statusLabel->setWordWrap(true);
    parts.statusLabel->hide();
    parts.listLayout->addWidget(parts.statusLabel);

    parts.listLayout->addStretch(1);
    parts.listHost->setObjectName(prefix + QLatin1String("List"));
    // Styled, not palette-filled: setWidget() force-enables autoFillBackground,
    // which would paint the palette's window color (see scrollWrap() in
    // settings_dialog); the grey comes from the stylesheet in stylePage.
    parts.listHost->setAttribute(Qt::WA_StyledBackground);
    parts.scroll->setWidget(parts.listHost);
    parts.listHost->setAutoFillBackground(false);
    return parts;
}

void stylePage(QWidget *page, const Page &parts) {
    const auto &th = Th::c();
    // The whole page is one grey surface (official-client look): the title sits
    // on the same grey as the card list, separated only by a subtle hairline.
    page->setStyleSheet(QString("QWidget#%1 { background: %2; }")
                            .arg(page->objectName(), Th::qss(th.surface.sunken)));
    parts.headerRow->setStyleSheet(QString(
                                       "QWidget#%1 { background: %2; "
                                       "border-bottom: 1px solid %3; }"
    )
                                       .arg(
                                           parts.headerRow->objectName(),
                                           Th::qss(th.surface.sunken),
                                           Th::qss(th.divider.subtle)
                                       ));
    parts.listHost->setStyleSheet(
        QString("QWidget#%1 { background: %2; }")
            .arg(parts.listHost->objectName(), Th::qss(th.surface.sunken))
    );
    parts.titleLabel->setStyleSheet(
        QString("background: transparent; font-weight: bold; font-size: %1px; color: %2;")
            .arg(th.fonts.xxl)
            .arg(Th::qss(th.text.primary))
    );
    parts.statusLabel->setStyleSheet(QString("background: transparent; color: %1; padding: %2px;")
                                         .arg(Th::qss(th.text.secondary))
                                         .arg(th.spacing.xxl));
    parts.scroll->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; }") + Th::scrollBarQss()
    );
}

} // namespace OverviewCard
