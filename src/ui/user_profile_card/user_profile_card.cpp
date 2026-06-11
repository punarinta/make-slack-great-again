// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "user_profile_card.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji.h"
#include "util/emoji_font.h"

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
    const QTime t = QDateTime::currentDateTimeUtc().addSecs(_user.tzOffset).time();
    return tr("%1 local time").arg(t.toString("h:mm AP"));
}

void UserProfileCard::relayout() {
    const QFontMetrics detailFm(detailFont());

    _headerH = roleLabel().isEmpty() ? 0 : 34;
    _statusH =
        (_user.statusText.isEmpty() && _user.statusEmoji.isEmpty()) ? 0 : detailFm.height() + 4;
    _titleH = _user.title.isEmpty() ? 0 : detailFm.height() + 4;
    _clockH = _user.hasTz ? 24 : 0;

    const QFontMetrics nameFm(nameFont());
    const int          textColH = nameFm.height() + _statusH + _titleH;
    _bodyH                      = kPad + std::max(int(kAvSize), textColH) + kPad;

    int bottomH = 0;
    if (_clockH > 0 || showMessageButton()) {
        bottomH = 12 + _clockH + 12;
        if (showMessageButton())
            bottomH += (_clockH > 0 ? 10 : 0) + kBtnH;
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
    const int   by   = card.top() + _headerH + _bodyH + 1 + 12 + (_clockH > 0 ? _clockH + 10 : 0);
    return QRect(card.left() + kPad, by, btnW, kBtnH);
}

void UserProfileCard::showFor(
    const User &user, const QPixmap &avatar, const QRect &targetGlobalRect
) {
    _user       = user;
    _avatar     = avatar;
    _btnHovered = false;
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
    _btnHovered = false;
    update();
    scheduleHide();
}

void UserProfileCard::mouseMoveEvent(QMouseEvent *e) {
    const bool hovered = messageButtonRect().contains(e->pos());
    if (hovered != _btnHovered) {
        _btnHovered = hovered;
        setCursor(hovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void UserProfileCard::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && messageButtonRect().contains(e->pos()))
        emit messageRequested(_user.id);
}

void UserProfileCard::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr  = devicePixelRatioF();
    const QRect card = cardRect();

    // Drop shadow
    for (int i = kShadow; i >= 2; --i) {
        const int alpha = (kShadow - i) * 4;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(
            QRectF(card).adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), kRadius + i, kRadius + i
        );
    }

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
    if (!_avatar.isNull()) {
        QPainterPath avClip;
        avClip.addRoundedRect(QRectF(avRect), kAvRadius, kAvRadius);
        p.save();
        p.setClipPath(avClip);
        QPixmap scaled = _avatar.scaled(
            avRect.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
        );
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(avRect, scaled);
        p.restore();
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().presence.away);
        p.drawRoundedRect(avRect, kAvRadius, kAvRadius);
        const QString initial = _user.displayLabel().left(1).toUpper();
        if (!initial.isEmpty()) {
            QFont f = QApplication::font();
            f.setBold(true);
            f.setPointSizeF(kAvSize * 0.38);
            p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(avRect, Qt::AlignCenter, initial);
        }
    }

    const QFont        nFont = nameFont();
    const QFont        dFont = detailFont();
    const QFontMetrics nameFm(nFont);
    const QFontMetrics detailFm(dFont);

    const int textX    = avRect.right() + 1 + kAvGap;
    const int textW    = card.right() - kPad - textX;
    const int textColH = nameFm.height() + _statusH + _titleH;
    int       ty       = bodyTop + std::max(0, (kAvSize - textColH) / 2);

    // Name + presence dot
    p.setFont(nFont);
    p.setPen(Th::c().text.primary);
    constexpr int kDotD   = 8;
    constexpr int kDotGap = 8;
    const QString name =
        nameFm.elidedText(_user.displayLabel(), Qt::ElideRight, textW - kDotGap - kDotD);
    const int nameW = nameFm.horizontalAdvance(name);
    p.drawText(QRect(textX, ty, textW, nameFm.height()), Qt::AlignLeft | Qt::AlignVCenter, name);
    {
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
                p.setFont(emojiFont(Th::c().fonts.md));
                const QFontMetrics emojiFm(p.font());
                p.setPen(Th::c().text.primary);
                p.drawText(QRect(sx, ty, textW, _statusH), Qt::AlignLeft | Qt::AlignVCenter, glyph);
                sx += emojiFm.horizontalAdvance(glyph) + 5;
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

    int sy = card.top() + _headerH + _bodyH;

    // ── Bottom section: divider, local time, Message button ───────────
    if (_clockH > 0 || showMessageButton()) {
        p.setPen(Th::c().divider.def);
        p.drawLine(card.left(), sy, card.right(), sy);
        sy += 1 + 12;

        if (_clockH > 0) {
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
            sy += _clockH + (showMessageButton() ? 10 : 0);
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
