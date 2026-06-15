// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "workspace_switcher.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/image_cache.h"
#include "ui/nav_ghost_button.h"
#include "ui/popup_tooltip/popup_tooltip.h"

#include <QGuiApplication>
#include <QStyleHints>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <cmath>

namespace {
// Exponential settle toward the slot: ~95% of the distance in ~130 ms at 60 fps.
constexpr int   kAnimTickMs    = 16;
constexpr qreal kAnimFactor    = 0.35;
constexpr qreal kDragLiftScale = 1.06;
} // namespace

WorkspaceSwitcher::WorkspaceSwitcher(QWidget *parent) : QWidget(parent) {
    setFixedWidth(kW);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    _tooltip = new PopupTooltip(this);
    _animTimer.setInterval(kAnimTickMs);
    connect(&_animTimer, &QTimer::timeout, this, &WorkspaceSwitcher::tickAnim);
    connect(
        &ThemeManager::instance(),
        &ThemeManager::themeChanged,
        this,
        QOverload<>::of(&QWidget::update)
    );
}

void WorkspaceSwitcher::setWorkspaces(const std::vector<Entry> &entries) {
    // A rebuild mid-drag would invalidate _dragIndex — abandon the drag.
    if (_dragging) {
        _dragging  = false;
        _dragIndex = -1;
        _pressed   = -99;
    }

    std::vector<EntryPrivate> next;
    next.reserve(entries.size());
    for (const auto &e : entries) {
        EntryPrivate ep{e, {}};
        // Carry over what the caller doesn't know: the downloaded icon, live
        // unread counts and the animated position (entries built from
        // TokenStore carry zeros).
        for (const auto &old : _entries)
            if (old.info.teamId == e.teamId) {
                ep.icon          = old.icon;
                ep.info.unread   = old.info.unread;
                ep.info.mentions = old.info.mentions;
                ep.y             = old.y;
                break;
            }
        next.push_back(std::move(ep));
    }
    _entries = std::move(next);
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i)
        if (_entries[i].y < 0)
            _entries[i].y = slotY(i);
    startAnim(); // carried entries whose index changed glide to their new slot
    loadIcons();
    update();
}

void WorkspaceSwitcher::setActive(const QString &teamId) {
    if (_activeId == teamId)
        return;
    _activeId = teamId;
    update();
}

void WorkspaceSwitcher::setUnreadCounts(const QString &teamId, int total, int mentions) {
    for (auto &ep : _entries) {
        if (ep.info.teamId != teamId)
            continue;
        if (ep.info.unread == total && ep.info.mentions == mentions)
            return;
        ep.info.unread   = total;
        ep.info.mentions = mentions;
        update();
        return;
    }
}

QPair<int, int> WorkspaceSwitcher::unreadCounts(const QString &teamId) const {
    for (const auto &ep : _entries)
        if (ep.info.teamId == teamId)
            return {ep.info.unread, ep.info.mentions};
    return {0, 0};
}

QStringList WorkspaceSwitcher::workspaceIds() const {
    QStringList ids;
    ids.reserve(static_cast<int>(_entries.size()));
    for (const auto &ep : _entries)
        ids.append(ep.info.teamId);
    return ids;
}

// ── Icon loading ──────────────────────────────────────────────────────────────

void WorkspaceSwitcher::setImageCache(ImageCache *cache) {
    _imgCache = cache;
    connect(cache, &ImageCache::loaded, this, [this](const QString &url) {
        for (auto &ep : _entries) {
            if (ep.info.iconUrl != url || !ep.icon.isNull())
                continue;
            const QPixmap px = _imgCache->get(url);
            if (!px.isNull())
                ep.icon = scaleIcon(px);
            break;
        }
        update();
    });
}

QPixmap WorkspaceSwitcher::scaleIcon(const QPixmap &src) const {
    const qreal dpr  = devicePixelRatioF();
    const int   phys = qRound(kBubble * dpr);
    QPixmap     px   = src.scaled(phys, phys, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    px.setDevicePixelRatio(dpr);
    return px;
}

void WorkspaceSwitcher::loadIcons() {
    if (!_imgCache)
        return;
    for (auto &ep : _entries) {
        if (ep.info.iconUrl.isEmpty() || !ep.icon.isNull())
            continue;
        const QPixmap px = _imgCache->get(ep.info.iconUrl);
        if (!px.isNull())
            ep.icon = scaleIcon(px);
        // null means in-flight or not yet fetched — loaded() signal will fire
    }
}

// ── Geometry ──────────────────────────────────────────────────────────────────

int WorkspaceSwitcher::slotY(int i) const {
    return kTopPad + i * (kBubble + kGap);
}

QRect WorkspaceSwitcher::entryRect(int i) const {
    return QRect((kW - kBubble) / 2, slotY(i), kBubble, kBubble);
}

QRect WorkspaceSwitcher::addButtonRect() const {
    const int y = kTopPad + static_cast<int>(_entries.size()) * (kBubble + kGap) + kGap;
    return QRect((kW - kAddSize) / 2, y, kAddSize, kAddSize);
}

QRect WorkspaceSwitcher::gearButtonRect() const {
    return QRect((kW - kGearSize) / 2, height() - kBottomPad - kGearSize, kGearSize, kGearSize);
}

int WorkspaceSwitcher::hitTest(const QPoint &pos) const {
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i)
        if (entryRect(i).contains(pos))
            return i;
    if (addButtonRect().contains(pos))
        return -2;
    if (gearButtonRect().contains(pos))
        return -3;
    return -99;
}

QColor WorkspaceSwitcher::bubbleColor(const QString &teamId) const {
    const int hue = static_cast<int>((qHash(teamId) * 37u) % 360u);
    return QColor::fromHsl(hue, Th::c().workspaceHslSaturation, Th::c().workspaceHslLightness);
}

// ── Painting ──────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::paintBubble(
    QPainter &p, const EntryPrivate &ep, const QRectF &r, bool hov
) const {
    const bool  active = (ep.info.teamId == _activeId);
    const qreal radius = kRadius * (r.width() / kBubble); // keep shape under lift scale

    QColor bg = bubbleColor(ep.info.teamId);
    if (active)
        bg = bg.lighter(125);
    else if (hov)
        bg = bg.lighter(115);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(r, radius, radius);

    if (!ep.icon.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(r, radius, radius);
        p.setClipPath(clip);
        p.drawPixmap(r, ep.icon, QRectF(ep.icon.rect()));
        p.setClipping(false);
    } else {
        // Letter fallback
        const QChar ch = ep.info.name.isEmpty() ? QChar('?') : ep.info.name.at(0).toUpper();
        p.setPen(Qt::white);
        QFont f = font();
        f.setPixelSize(17);
        f.setBold(true);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, QString(ch));
    }

    // White ring for active / hover
    if (active || hov) {
        p.setPen(QPen(QColor(255, 255, 255, active ? 200 : 100), active ? 2.0 : 1.5));
        p.setBrush(Qt::NoBrush);
        const qreal inset = 0.75;
        p.drawRoundedRect(r.adjusted(inset, inset, -inset, -inset), radius - inset, radius - inset);
    }

    // Unread dot: red for important (DMs/mentions), blue for regular unreads
    if (ep.info.unread > 0 || ep.info.mentions > 0) {
        const QColor dotColor =
            ep.info.mentions > 0 ? Th::c().badge.mention : Th::c().badge.activity;
        constexpr int d  = 10;
        const qreal   cx = r.right() - 3.0;
        const qreal   cy = r.bottom() - 3.0;
        p.setBrush(dotColor);
        p.setPen(QPen(Th::c().nav.bg, 2.5));
        p.drawEllipse(QRectF(cx - d / 2.0, cy - d / 2.0, d, d));
    }
}

void WorkspaceSwitcher::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    p.fillRect(rect(), Th::navGradient(this, Th::c().nav.bgGradTop, Th::c().nav.bgGradBottom));

    const qreal bubbleX = (kW - kBubble) / 2.0;
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        if (_dragging && i == _dragIndex)
            continue; // lifted bubble paints last, on top
        const auto  &ep = _entries[i];
        const QRectF r(bubbleX, ep.y, kBubble, kBubble);

        // Active indicator: white pill on left edge
        if (ep.info.teamId == _activeId) {
            const qreal barH = 28;
            const qreal barY = r.top() + (r.height() - barH) / 2;
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(0, barY, kBarW, barH), kBarW / 2.0, kBarW / 2.0);
        }

        paintBubble(p, ep, r, !_dragging && _hovered == i);
    }

    // "+" add button — rounded square, same chrome as the workspace bubbles.
    const QRect ar     = addButtonRect();
    const bool  addHov = (_hovered == -2);
    NavGhostButton::paintChrome(p, QRectF(ar), addHov, kRadius);

    // Draw + as two opaque lines — no alpha so the crossing pixel has no artifact.
    // Hover feedback already comes from the chrome fill/border above.
    const QPointF c   = QRectF(ar).center();
    const qreal   arm = 5.0;
    p.setPen(QPen(NavGhostButton::iconColor(addHov), 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));

    // Settings button (bottom of column)
    const QRect gr = gearButtonRect();
    NavGhostButton::paintChrome(p, QRectF(gr), _hovered == -3, kRadius);
    NavGhostButton::paintIcon(p, QRectF(gr), _hovered == -3, QStringLiteral(":/ui/settings-2.svg"));

    // Lifted bubble: soft shadow + slight scale, drawn above everything else
    if (_dragging && _dragIndex >= 0 && _dragIndex < static_cast<int>(_entries.size())) {
        const QRectF base(bubbleX, _dragY, kBubble, kBubble);
        const qreal  grow   = (kDragLiftScale - 1.0) * kBubble / 2.0;
        const QRectF lifted = base.adjusted(-grow, -grow, grow, grow);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawRoundedRect(lifted.translated(0, 3).adjusted(-1, -1, 1, 1), kRadius + 2, kRadius + 2);

        paintBubble(p, _entries[_dragIndex], lifted, false);
    }
}

// ── Drag reorder ──────────────────────────────────────────────────────────────

void WorkspaceSwitcher::startAnim() {
    if (!_animTimer.isActive())
        _animTimer.start();
}

void WorkspaceSwitcher::tickAnim() {
    bool moving = false;
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        if (_dragging && i == _dragIndex)
            continue;
        const qreal target = slotY(i);
        qreal      &y      = _entries[i].y;
        const qreal d      = target - y;
        if (std::abs(d) < 0.5) {
            y = target;
        } else {
            y += d * kAnimFactor;
            moving = true;
        }
    }
    if (!moving)
        _animTimer.stop();
    update();
}

void WorkspaceSwitcher::beginDrag(const QPoint &pos) {
    _dragging         = true;
    _dragIndex        = _pressed;
    _grabOffset       = _pressPos.y() - _entries[_dragIndex].y;
    _orderAtDragStart = workspaceIds();
    _hovered          = -99;
    _tooltip->hide();
    if (_cursorOverrideActive) {
        QGuiApplication::changeOverrideCursor(Qt::ClosedHandCursor);
    } else {
        QGuiApplication::setOverrideCursor(Qt::ClosedHandCursor);
        _cursorOverrideActive = true;
    }
    updateDrag(pos);
}

void WorkspaceSwitcher::updateDrag(const QPoint &pos) {
    const int n = static_cast<int>(_entries.size());
    _dragY      = qBound<qreal>(slotY(0), pos.y() - _grabOffset, slotY(n - 1));

    const int idx = qBound(0, qRound((_dragY - kTopPad) / qreal(kBubble + kGap)), n - 1);
    if (idx != _dragIndex) {
        auto ep = std::move(_entries[_dragIndex]);
        _entries.erase(_entries.begin() + _dragIndex);
        _entries.insert(_entries.begin() + idx, std::move(ep));
        _dragIndex = idx;
        startAnim(); // displaced bubbles glide to their new slots
    }
    update();
}

void WorkspaceSwitcher::endDrag() {
    _dragging              = false;
    _entries[_dragIndex].y = _dragY; // settle animation starts from the drop point
    _dragIndex             = -1;
    startAnim();

    const QStringList ids = workspaceIds();
    if (ids != _orderAtDragStart)
        emit workspacesReordered(ids);
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::updateCursor(const QPoint &pos) {
    const bool clickable = (hitTest(pos) != -99);
    if (clickable && !_cursorOverrideActive) {
        QGuiApplication::setOverrideCursor(Qt::PointingHandCursor);
        _cursorOverrideActive = true;
    } else if (!clickable && _cursorOverrideActive) {
        QGuiApplication::restoreOverrideCursor();
        _cursorOverrideActive = false;
    } else if (clickable && _cursorOverrideActive) {
        QGuiApplication::changeOverrideCursor(Qt::PointingHandCursor);
    }
}

void WorkspaceSwitcher::mousePressEvent(QMouseEvent *e) {
    _pressed = hitTest(e->pos());
    if (e->button() == Qt::LeftButton)
        _pressPos = e->pos();
}

void WorkspaceSwitcher::mouseReleaseEvent(QMouseEvent *e) {
    if (_dragging) {
        endDrag();
        _pressed = -99;
        updateCursor(e->pos());
        return;
    }

    const int hit = hitTest(e->pos());
    if (hit != _pressed) {
        _pressed = -99;
        return;
    }
    _pressed = -99;

    if (e->button() == Qt::LeftButton) {
        if (hit >= 0) {
            // Copy before emitting: a handler may call setWorkspaces(), which
            // rebuilds _entries and would leave a reference argument dangling.
            const QString teamId = _entries[hit].info.teamId;
            emit          workspaceClicked(teamId);
        } else if (hit == -2) {
            emit addWorkspaceClicked();
        } else if (hit == -3) {
            emit settingsClicked();
        }
    } else if (e->button() == Qt::RightButton && hit >= 0) {
        const QString teamId = _entries[hit].info.teamId;
        emit          workspaceRightClicked(teamId, e->globalPosition().toPoint());
    }
}

void WorkspaceSwitcher::mouseMoveEvent(QMouseEvent *e) {
    if (_dragging) {
        updateDrag(e->pos());
        return;
    }

    if ((e->buttons() & Qt::LeftButton) && _pressed >= 0 && _entries.size() > 1 &&
        (e->pos() - _pressPos).manhattanLength() >=
            QGuiApplication::styleHints()->startDragDistance()) {
        beginDrag(e->pos());
        return;
    }

    const int h = hitTest(e->pos());
    updateCursor(e->pos());
    if (h != _hovered) {
        _hovered = h;
        update();
    }

    if (h >= 0 && h < static_cast<int>(_entries.size())) {
        const QRect r = entryRect(h);
        _tooltip->showRightOf(_entries[h].info.name, QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (h == -2) {
        const QRect r = addButtonRect();
        _tooltip->showRightOf(tr("Add workspace"), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (h == -3) {
        const QRect r = gearButtonRect();
        _tooltip->showRightOf(tr("Settings"), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else {
        _tooltip->hide();
    }
}

void WorkspaceSwitcher::leaveEvent(QEvent *) {
    _tooltip->hide();
    if (_dragging)
        return; // mouse grab keeps move events coming; keep drag state and cursor
    if (_cursorOverrideActive) {
        QGuiApplication::restoreOverrideCursor();
        _cursorOverrideActive = false;
    }
    if (_hovered == -99)
        return;
    _hovered = -99;
    update();
}
