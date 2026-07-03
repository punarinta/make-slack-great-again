// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "user_profile_card.h"
#include "ui/icon_utils.h"
#include "util/clipboard.h"
#include "ui/paint_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/user_avatar.h"
#include "util/emoji.h"
#include "util/emoji_pixmap.h"
#include "util/time_format.h"

#include <QApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <algorithm>

// Fonts used by both relayout() (metrics) and paintEvent() (drawing).
static QFont nameFont() {
    QFont f = QApplication::font();
    f.setPixelSize(Th::c().fonts.xl);
    f.setBold(true);
    return f;
}
static QFont detailFont() {
    QFont f = QApplication::font();
    f.setPixelSize(Th::c().fonts.md);
    return f;
}
static QFont headerFont() {
    QFont f = QApplication::font();
    f.setPixelSize(Th::c().fonts.md);
    f.setBold(true);
    return f;
}

UserProfileCard::UserProfileCard(QWidget *parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);

    _hideTimer.setSingleShot(true);
    _hideTimer.setInterval(kHideDelay);
    connect(&_hideTimer, &QTimer::timeout, this, [this] {
        if (!underMouse())
            hideNow();
    });

    _clockTimer.setInterval(15000);
    connect(&_clockTimer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));

    _copiedTimer.setSingleShot(true);
    _copiedTimer.setInterval(1200);
    connect(&_copiedTimer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
}

QString UserProfileCard::roleLabel() const {
    if (_user.isDeactivated)
        return tr("Deactivated account");
    if (_user.isOwner)
        return tr("Workspace Owner");
    if (_user.isAdmin)
        return tr("Workspace Admin");
    if (_user.isBot)
        return tr("App");
    return {};
}

QString UserProfileCard::localTimeText() const {
    if (!_user.hasTz)
        return {};
    const QDateTime dt = QDateTime::currentDateTimeUtc().addSecs(_user.tzOffset);
    return tr("%1 local time").arg(TimeFmt::formatTime(dt));
}

void UserProfileCard::relayout() {
    const QFontMetrics detailFm(detailFont());

    _headerH = roleLabel().isEmpty() ? 0 : 34;
    _statusH =
        (_user.statusText.isEmpty() && _user.statusEmoji.isEmpty()) ? 0 : detailFm.height() + 4;
    _titleH = _user.title.isEmpty() ? 0 : detailFm.height() + 4;
    _emailH = _user.email.isEmpty() ? 0 : 24;
    _clockH = _user.hasTz ? 24 : 0;

    const QFontMetrics nameFm(nameFont());
    const int          textColH = nameFm.height() + _statusH + _titleH;
    _bodyH                      = kPad + std::max(int(kAvSize), textColH) + kPad;

    // Mirrors bottomTop()/clockTop()/buttonTop() — change them together.
    int bottomH = 0;
    if (_emailH > 0 || _clockH > 0 || showMessageButton()) {
        bottomH = 12 + _emailH + (_clockH > 0 ? (_emailH > 0 ? 6 : 0) + _clockH : 0) + 12;
        if (showMessageButton())
            bottomH += ((_emailH > 0 || _clockH > 0) ? 10 : 0) + kBtnH;
    }

    _cardH = _headerH + _bodyH + (bottomH > 0 ? 1 + bottomH : 0);
}

QRect UserProfileCard::cardRect() const {
    return QRect(kShadow, kShadow, kCardW, _cardH);
}

QRect UserProfileCard::messageButtonRect() const {
    if (!showMessageButton())
        return {};
    QFont btnFont = QApplication::font();
    btnFont.setPixelSize(Th::c().fonts.md);
    btnFont.setBold(true);
    const QFontMetrics fm(btnFont);
    const int          btnW = 12 + 16 + 8 + fm.horizontalAdvance(tr("Message")) + 12;

    const QRect card = cardRect();
    return QRect(card.left() + kPad, card.top() + buttonTop(), btnW, kBtnH);
}

QRect UserProfileCard::emailRowRect() const {
    if (_emailH <= 0)
        return {};
    const QRect card = cardRect();
    return QRect(card.left() + kPad, card.top() + bottomTop(), card.width() - 2 * kPad, _emailH);
}

void UserProfileCard::showFor(
    const User &user, const QPixmap &avatar, const QRect &targetGlobalRect, bool showPresence
) {
    _user         = user;
    _avatar       = avatar;
    _showPresence = showPresence;
    _btnHovered   = false;
    _emailHovered = false;
    _copiedTimer.stop();
    _hideTimer.stop();
    relayout();

    const int widgetW = kCardW + 2 * kShadow;
    const int widgetH = _cardH + 2 * kShadow;
    setFixedSize(widgetW, widgetH);

    QScreen    *s     = QGuiApplication::screenAt(targetGlobalRect.center());
    const QRect avail = s ? s->availableGeometry() : QRect();

    // Prefer above the mention; fall back to below when there is no room.
    int wy = targetGlobalRect.top() - kGap - widgetH + kShadow;
    if (avail.isValid() && wy < avail.top())
        wy = targetGlobalRect.bottom() + kGap - kShadow;
    int wx = targetGlobalRect.left() - kPad - kShadow;
    if (avail.isValid()) {
        wx = std::max(avail.left(), std::min(wx, avail.right() - widgetW));
        wy = std::max(avail.top(), std::min(wy, avail.bottom() - widgetH));
    }

    move(wx, wy);
    if (_clockH > 0)
        _clockTimer.start();
    show();
    raise();
    update();
}

void UserProfileCard::updateAvatar(const QPixmap &avatar) {
    _avatar = avatar;
    update();
}

void UserProfileCard::setActive(bool active) {
    if (_user.isActive == active)
        return;
    _user.isActive = active;
    update();
}

void UserProfileCard::scheduleHide() {
    if (isVisible() && !_hideTimer.isActive())
        _hideTimer.start();
}

void UserProfileCard::cancelHide() {
    _hideTimer.stop();
}

void UserProfileCard::hideNow() {
    _hideTimer.stop();
    _clockTimer.stop();
    hide();
}

void UserProfileCard::enterEvent(QEnterEvent *) {
    _hideTimer.stop();
}

void UserProfileCard::leaveEvent(QEvent *) {
    _btnHovered   = false;
    _emailHovered = false;
    update();
    scheduleHide();
}

void UserProfileCard::mouseMoveEvent(QMouseEvent *e) {
    const bool btnHovered   = messageButtonRect().contains(e->pos());
    const bool emailHovered = emailRowRect().contains(e->pos());
    if (btnHovered != _btnHovered || emailHovered != _emailHovered) {
        _btnHovered   = btnHovered;
        _emailHovered = emailHovered;
        setCursor((btnHovered || emailHovered) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void UserProfileCard::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    if (messageButtonRect().contains(e->pos())) {
        emit messageRequested(_user.id);
    } else if (emailRowRect().contains(e->pos())) {
        Clipboard::setText(_user.email);
        _copiedTimer.start();
        update();
    }
}

void UserProfileCard::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr  = devicePixelRatioF();
    const QRect card = cardRect();

    // Drop shadow
    Paint::dropShadow(p, QRectF(card), kRadius, kShadow, 0, 4);

    // Card body + border
    p.setPen(Th::c().divider.strong);
    p.setBrush(Th::c().surface.raised);
    p.drawRoundedRect(QRectF(card), kRadius, kRadius);

    // Clip all section fills to the rounded card shape.
    QPainterPath clip;
    clip.addRoundedRect(QRectF(card), kRadius, kRadius);
    p.save();
    p.setClipPath(clip);

    // ── Role header strip ──────────────────────────────────────────────
    int           y    = card.top();
    const QString role = roleLabel();
    if (_headerH > 0) {
        p.fillRect(QRect(card.left(), y, card.width(), _headerH), Th::c().surface.highlight);
        p.setPen(Th::c().divider.def);
        p.drawLine(card.left(), y + _headerH, card.right(), y + _headerH);
        p.setPen(Th::c().text.primary);
        p.setFont(headerFont());
        p.drawText(
            QRect(card.left() + kPad, y, card.width() - 2 * kPad, _headerH),
            Qt::AlignLeft | Qt::AlignVCenter,
            role
        );
        y += _headerH;
    }
    p.restore();

    // ── Avatar + name block ────────────────────────────────────────────
    const int   bodyTop = y + kPad;
    const QRect avRect(card.left() + kPad, bodyTop, kAvSize, kAvSize);
    if (!_avatar.isNull())
        UserAvatar::paintPhoto(p, avRect, _avatar, dpr, kAvRadius);
    else
        UserAvatar::paintInitial(
            p,
            avRect,
            _user.displayLabel(),
            Th::c().presence.away,
            Qt::white,
            kAvRadius,
            kAvSize * 0.38
        );

    const QFont        nFont = nameFont();
    const QFont        dFont = detailFont();
    const QFontMetrics nameFm(nFont);
    const QFontMetrics detailFm(dFont);

    const int textX    = avRect.right() + 1 + kAvGap;
    const int textW    = card.right() - kPad - textX;
    const int textColH = nameFm.height() + _statusH + _titleH;
    int       ty       = bodyTop + std::max(0, (kAvSize - textColH) / 2);

    // Name + presence dot (dot only when the workspace has presence at all)
    p.setFont(nFont);
    p.setPen(Th::c().text.primary);
    constexpr int kDotD     = 8;
    constexpr int kDotGap   = 8;
    const int     nameAvail = _showPresence ? textW - kDotGap - kDotD : textW;
    const QString name      = nameFm.elidedText(_user.displayLabel(), Qt::ElideRight, nameAvail);
    const int     nameW     = nameFm.horizontalAdvance(name);
    p.drawText(QRect(textX, ty, textW, nameFm.height()), Qt::AlignLeft | Qt::AlignVCenter, name);
    if (_showPresence) {
        const int    cy = ty + nameFm.height() / 2;
        const QRectF dot(textX + nameW + kDotGap, cy - kDotD / 2.0, kDotD, kDotD);
        if (_user.dndEnabled) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().presence.away);
            p.drawEllipse(dot);
            p.setPen(QPen(Th::c().surface.raised, 1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(
                QPointF(dot.left() + 2, dot.center().y()),
                QPointF(dot.right() - 2, dot.center().y())
            );
        } else if (_user.isActive) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().presence.online);
            p.drawEllipse(dot);
        } else {
            p.setPen(QPen(Th::c().text.tertiary, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(dot.adjusted(0.6, 0.6, -0.6, -0.6));
        }
    }
    ty += nameFm.height();

    // Status line: emoji (colour font) + text
    if (_statusH > 0) {
        int sx = textX;
        if (!_user.statusEmoji.isEmpty()) {
            const QString glyph = Emoji::fromName(_user.statusEmoji);
            if (!glyph.isEmpty()) {
                const int px = Th::c().fonts.md;
                const int w  = EmojiPix::width(glyph, px, devicePixelRatioF());
                EmojiPix::draw(p, QRect(sx, ty, w, _statusH), glyph, px, Th::c().text.primary);
                sx += w + 5;
            }
        }
        if (!_user.statusText.isEmpty()) {
            p.setFont(dFont);
            p.setPen(Th::c().text.secondary);
            p.drawText(
                QRect(sx, ty, textX + textW - sx, _statusH),
                Qt::AlignLeft | Qt::AlignVCenter,
                detailFm.elidedText(_user.statusText, Qt::ElideRight, textX + textW - sx)
            );
        }
        ty += _statusH;
    }

    // Job title
    if (_titleH > 0) {
        p.setFont(dFont);
        p.setPen(Th::c().text.secondary);
        p.drawText(
            QRect(textX, ty, textW, _titleH),
            Qt::AlignLeft | Qt::AlignVCenter,
            detailFm.elidedText(_user.title, Qt::ElideRight, textW)
        );
    }

    // ── Bottom section: divider, email, local time, Message button ────
    if (_emailH > 0 || _clockH > 0 || showMessageButton()) {
        const int dividerY = card.top() + _headerH + _bodyH;
        p.setPen(Th::c().divider.def);
        p.drawLine(card.left(), dividerY, card.right(), dividerY);

        if (_emailH > 0) {
            // Click-to-copy row: mail icon + address, swapped for a check +
            // "Copied" while the feedback timer runs.
            const QRect   row    = emailRowRect();
            const bool    copied = _copiedTimer.isActive();
            const QColor  fg     = copied          ? Th::c().accent.def
                                   : _emailHovered ? Th::c().accent.def
                                                   : Th::c().text.primary;
            const QPixmap icon   = svgPixmapPhys(
                copied ? ":/ui/check.svg" : ":/ui/mail.svg",
                QSize(16, 16),
                copied ? Th::c().accent.def : Th::c().icon.def,
                dpr
            );
            p.drawPixmap(row.left(), row.top() + (_emailH - 16) / 2, icon);
            QFont emailFont = dFont;
            emailFont.setUnderline(_emailHovered && !copied);
            p.setFont(emailFont);
            p.setPen(fg);
            const int textAvail = row.width() - 24;
            p.drawText(
                QRect(row.left() + 16 + 8, row.top(), textAvail, _emailH),
                Qt::AlignLeft | Qt::AlignVCenter,
                copied ? tr("Copied")
                       : QFontMetrics(emailFont).elidedText(_user.email, Qt::ElideMiddle, textAvail)
            );
        }

        if (_clockH > 0) {
            const int     sy = card.top() + clockTop();
            const QPixmap clock =
                svgPixmapPhys(":/ui/clock.svg", QSize(16, 16), Th::c().icon.def, dpr);
            p.drawPixmap(card.left() + kPad, sy + (_clockH - 16) / 2, clock);
            p.setFont(dFont);
            p.setPen(Th::c().text.primary);
            p.drawText(
                QRect(card.left() + kPad + 16 + 8, sy, card.width() - 2 * kPad - 24, _clockH),
                Qt::AlignLeft | Qt::AlignVCenter,
                localTimeText()
            );
        }

        if (showMessageButton()) {
            const QRect btn = messageButtonRect();
            p.setPen(QPen(Th::c().divider.strong, 1));
            p.setBrush(_btnHovered ? QBrush(Th::c().surface.highlight) : QBrush(Qt::NoBrush));
            p.drawRoundedRect(QRectF(btn), 8, 8);

            const QPixmap msgIcon =
                svgPixmapPhys(":/ui/message-square.svg", QSize(16, 16), Th::c().text.primary, dpr);
            p.drawPixmap(btn.left() + 12, btn.top() + (kBtnH - 16) / 2, msgIcon);

            QFont btnFont = QApplication::font();
            btnFont.setPixelSize(Th::c().fonts.md);
            btnFont.setBold(true);
            p.setFont(btnFont);
            p.setPen(Th::c().text.primary);
            p.drawText(
                QRect(btn.left() + 12 + 16 + 8, btn.top(), btn.width(), kBtnH),
                Qt::AlignLeft | Qt::AlignVCenter,
                tr("Message")
            );
        }
    }
}
