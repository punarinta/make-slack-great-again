// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QFont>
#include <QFontMetrics>
#include <QString>
#include <functional>

class ImageCache;
class QHBoxLayout;
class QLabel;
class QPainter;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;
class Session;

// What the workspace-wide overview pages (ThreadsPage, SavedMessagesPage)
// share: the page chrome (title bar over a grey scrolling card list), the card
// header with the conversation name outside a white bordered body, and the
// chat-style message row header (avatar + name + timestamp).
namespace OverviewCard {

// ── Message row ──────────────────────────────────────────────────────────────
// Match the message list's avatar geometry (kAvSize/kAvGap/radius 4 in
// message_list) so the rows read exactly like chat rows.
constexpr int kAvatarSize   = 36;
constexpr int kAvatarRadius = 4;
constexpr int kAvatarGap    = 10;
constexpr int kTextLeft     = kAvatarSize + kAvatarGap;

const QFont        &nameFont();        // the row's bold author name
const QFontMetrics &nameFontMetrics(); // ...and its metrics
int                 rowPadV();         // above and below the row
int                 rowHdrGap();       // between the name line and the body

// The author's picture: a bot's own icon, else the user's avatar ({} if none).
QString avatarUrl(Session *session, const UserId &author, const QString &botAvatarUrl);

// Avatar (photo, or an initial chip while it downloads) plus the name and
// `timeLabel` line at the top of a `width`-wide row.
void paintRowHeader(
    QPainter      &p,
    int            width,
    qreal          dpr,
    ImageCache    *imgCache,
    const QString &avatarUrl,
    const QString &name,
    const QString &timeLabel
);

// ── Card ─────────────────────────────────────────────────────────────────────
// The header row "[icon] conversation name" (the name is a flat button calling
// `onClicked`) that sits on the grey page above a card's body. Parents the
// widgets to `card`; style them with styleChannelHeader().
QHBoxLayout *makeChannelHeader(
    QWidget              *card,
    const QString        &label,
    std::function<void()> onClicked,
    QLabel              **icon,
    QPushButton         **button
);
void styleChannelHeader(QLabel *icon, const QString &iconPath, QPushButton *button);

// A DM's peer name, else the conversation name, else its raw id.
QString conversationLabel(Session *session, const ConversationId &conv);

// The white bordered body, selected by its objectName().
void styleCardBody(QWidget *body);

// ── Page ─────────────────────────────────────────────────────────────────────
struct Page {
    QWidget     *headerRow   = nullptr;
    QLabel      *titleLabel  = nullptr;
    QScrollArea *scroll      = nullptr;
    QWidget     *listHost    = nullptr;
    QVBoxLayout *listLayout  = nullptr; // status label, then stretch
    QLabel      *statusLabel = nullptr;
};

// Builds the chrome into `page` (which must not have a layout yet) and names
// the styled parts `<prefix>Page`, `<prefix>Header` and `<prefix>List`.
Page buildPage(QWidget *page, const QString &prefix, const QString &title);
void stylePage(QWidget *page, const Page &parts);

} // namespace OverviewCard
