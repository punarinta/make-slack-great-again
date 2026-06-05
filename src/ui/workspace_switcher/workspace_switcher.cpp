// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "workspace_switcher.h"
#include "ui/theme.h"
#include "ui/popup_tooltip/popup_tooltip.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <cmath>

WorkspaceSwitcher::WorkspaceSwitcher(QWidget *parent)
    : QWidget(parent)
{
    setFixedWidth(kW);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    _tooltip = new PopupTooltip(this);
}

void WorkspaceSwitcher::setWorkspaces(const std::vector<Entry> &entries) {
    _entries.clear();
    _entries.reserve(entries.size());
    for (const auto &e : entries)
        _entries.push_back({e, {}});
    loadIcons();
    update();
}

void WorkspaceSwitcher::setActive(const QString &teamId) {
    if (_activeId == teamId) return;
    _activeId = teamId;
    update();
}

// ── Icon loading ──────────────────────────────────────────────────────────────

void WorkspaceSwitcher::loadIcons() {
    for (auto &ep : _entries) {
        if (ep.info.iconUrl.isEmpty() || !ep.icon.isNull()) continue;
        if (!_nam) _nam = new QNetworkAccessManager(this);

        QNetworkRequest req(QUrl(ep.info.iconUrl));
        auto *reply = _nam->get(req);
        const QString teamId = ep.info.teamId;

        connect(reply, &QNetworkReply::finished, this, [this, reply, teamId] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;
            QPixmap px;
            if (!px.loadFromData(reply->readAll())) return;
            for (auto &ep2 : _entries) {
                if (ep2.info.teamId != teamId) continue;
                const qreal dpr   = devicePixelRatioF();
                const int   phys  = qRound(kBubble * dpr);
                ep2.icon = px.scaled(phys, phys,
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                ep2.icon.setDevicePixelRatio(dpr);
                break;
            }
            update();
        });
    }
}

// ── Gear icon helper ──────────────────────────────────────────────────────────

static void paintGear(QPainter &p, const QRectF &r, bool hovered) {
    // Circle background (same visual style as + button)
    const QColor fill(255, 255, 255, hovered ? 55 : 22);
    const QColor border(255, 255, 255, hovered ? 180 : 90);
    p.setBrush(fill);
    p.setPen(QPen(border, 1.5));
    const qreal rad = r.height() / 2.0;
    p.drawRoundedRect(QRectF(r).adjusted(0.75, 0.75, -0.75, -0.75), rad, rad);

    // Gear silhouette: 8 square teeth, punched hub
    const QPointF c   = r.center();
    const qreal R     = r.width() * 0.27;
    const qreal Ri    = R * 0.68;
    const qreal Rh    = R * 0.30;
    const int   N     = 8;
    const qreal toOuter = M_PI / (N * 3.0);
    const qreal toInner = M_PI / (N * 2.2);

    auto pt = [&](qreal radius, qreal a) {
        return c + QPointF(radius * std::cos(a), radius * std::sin(a));
    };

    QPainterPath gear;
    for (int i = 0; i < N; ++i) {
        const qreal a = -M_PI / 2.0 + 2.0 * M_PI * i / N;
        const QPointF pts[4] = {
            pt(Ri, a - toInner), pt(R, a - toOuter),
            pt(R,  a + toOuter), pt(Ri, a + toInner),
        };
        if (i == 0) gear.moveTo(pts[0]);
        else        gear.lineTo(pts[0]);
        gear.lineTo(pts[1]);
        gear.lineTo(pts[2]);
        gear.lineTo(pts[3]);
    }
    gear.closeSubpath();

    QPainterPath hub;
    hub.addEllipse(c, Rh, Rh);

    p.setBrush(hovered ? QColor(245, 240, 245) : QColor(180, 165, 180));
    p.setPen(Qt::NoPen);
    p.drawPath(gear.subtracted(hub));
}

// ── Geometry ──────────────────────────────────────────────────────────────────

QRect WorkspaceSwitcher::entryRect(int i) const {
    return QRect((kW - kBubble) / 2,
                 kTopPad + i * (kBubble + kGap),
                 kBubble, kBubble);
}

QRect WorkspaceSwitcher::addButtonRect() const {
    const int y = kTopPad
                + static_cast<int>(_entries.size()) * (kBubble + kGap)
                + kGap;
    return QRect((kW - kAddSize) / 2, y, kAddSize, kAddSize);
}

QRect WorkspaceSwitcher::gearButtonRect() const {
    return QRect((kW - kGearSize) / 2, height() - kBottomPad - kGearSize,
                 kGearSize, kGearSize);
}

int WorkspaceSwitcher::hitTest(const QPoint &pos) const {
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i)
        if (entryRect(i).contains(pos)) return i;
    if (addButtonRect().contains(pos)) return -2;
    if (gearButtonRect().contains(pos)) return -3;
    return -99;
}

QColor WorkspaceSwitcher::bubbleColor(const QString &teamId) const {
    const int hue = static_cast<int>(qHash(teamId) * 37u) % 360;
    return QColor::fromHsl(hue, 65, 42);
}

// ── Painting ──────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    p.fillRect(rect(), Theme::kSidebarBg);

    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        const auto &ep     = _entries[i];
        const QRect  r     = entryRect(i);
        const bool active  = (ep.info.teamId == _activeId);
        const bool hov     = (_hovered == i);

        // Active indicator: white pill on left edge
        if (active) {
            const int barH = 28;
            const int barY = r.top() + (r.height() - barH) / 2;
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(0, barY, kBarW, barH, kBarW / 2.0, kBarW / 2.0);
        }

        if (!ep.icon.isNull()) {
            QColor bg = bubbleColor(ep.info.teamId);
            if (active)   bg = bg.lighter(125);
            else if (hov) bg = bg.lighter(115);
            p.setBrush(bg);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(r, kRadius, kRadius);

            QPainterPath clip;
            clip.addRoundedRect(QRectF(r), kRadius, kRadius);
            p.setClipPath(clip);
            p.drawPixmap(r, ep.icon);
            p.setClipping(false);
        } else {
            // Letter fallback
            QColor bg = bubbleColor(ep.info.teamId);
            if (active)    bg = bg.lighter(125);
            else if (hov)  bg = bg.lighter(115);

            p.setBrush(bg);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(r, kRadius, kRadius);

            const QChar ch = ep.info.name.isEmpty()
                             ? QChar('?')
                             : ep.info.name.at(0).toUpper();
            p.setPen(Qt::white);
            QFont f = font();
            f.setPixelSize(17);
            f.setBold(true);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, ch);
        }

        // White ring for active / hover
        if (active || hov) {
            p.setPen(QPen(QColor(255, 255, 255, active ? 200 : 100),
                          active ? 2.0 : 1.5));
            p.setBrush(Qt::NoBrush);
            const qreal inset = 0.75;
            p.drawRoundedRect(QRectF(r).adjusted(inset, inset, -inset, -inset),
                              kRadius - inset, kRadius - inset);
        }
    }

    // "+" add button
    const QRect  ar     = addButtonRect();
    const bool   addHov = (_hovered == -2);
    const QColor addFill(255, 255, 255, addHov ? 55 : 22);
    const QColor addBorder(255, 255, 255, addHov ? 180 : 90);

    p.setBrush(addFill);
    p.setPen(QPen(addBorder, 1.5));
    const qreal r2 = ar.height() / 2.0;
    p.drawRoundedRect(QRectF(ar).adjusted(0.75, 0.75, -0.75, -0.75), r2, r2);

    // Draw + as two opaque lines — no alpha so the crossing pixel has no artifact.
    // Hover feedback already comes from the circle fill/border above.
    const QPointF c = QRectF(ar).center();
    const qreal arm = 5.0;
    const QColor plusColor = addHov ? QColor(245, 240, 245) : QColor(180, 165, 180);
    p.setPen(QPen(plusColor, 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));

    // Gear / settings button (bottom of column)
    paintGear(p, QRectF(gearButtonRect()), _hovered == -3);
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::mousePressEvent(QMouseEvent *e) {
    _pressed = hitTest(e->pos());
}

void WorkspaceSwitcher::mouseReleaseEvent(QMouseEvent *e) {
    const int hit = hitTest(e->pos());
    if (hit != _pressed) { _pressed = -99; return; }
    _pressed = -99;

    if (e->button() == Qt::LeftButton) {
        if (hit >= 0)
            emit workspaceClicked(_entries[hit].info.teamId);
        else if (hit == -2)
            emit addWorkspaceClicked();
        else if (hit == -3)
            emit settingsClicked();
    } else if (e->button() == Qt::RightButton && hit >= 0) {
        emit workspaceRightClicked(_entries[hit].info.teamId,
                                   e->globalPosition().toPoint());
    }
}

void WorkspaceSwitcher::mouseMoveEvent(QMouseEvent *e) {
    const int h = hitTest(e->pos());
    if (h != _hovered) {
        _hovered = h;
        setCursor((h >= -3 && h != -99) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    if (h == -2) {
        const QRect r = addButtonRect();
        _tooltip->showAbove("Add workspace", QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (h == -3) {
        const QRect r = gearButtonRect();
        _tooltip->showAbove("Settings", QRect(mapToGlobal(r.topLeft()), r.size()));
    } else {
        _tooltip->hide();
    }
}

void WorkspaceSwitcher::leaveEvent(QEvent *) {
    _tooltip->hide();
    if (_hovered == -99) return;
    _hovered = -99;
    update();
}

bool WorkspaceSwitcher::event(QEvent *e) {
    if (e->type() == QEvent::ToolTip) {
        auto *he  = static_cast<QHelpEvent *>(e);
        const int hit = hitTest(he->pos());
        if (hit >= 0 && hit < static_cast<int>(_entries.size()))
            QToolTip::showText(he->globalPos(), _entries[hit].info.name, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}
