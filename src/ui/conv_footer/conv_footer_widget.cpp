// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/conv_footer/conv_footer_widget.h"

#include "ui/image_cache.h"
#include "ui/nav_ghost_button.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QMouseEvent>
#include <QPainter>

namespace {
constexpr int kHeight    = 64; // footer height (extra padding, on par with the gear button)
constexpr int kPadH      = 12; // left/right padding (aligns with conv-list rows)
constexpr int kBtn       = 40; // avatar + toggle button — same size as workspace icons
constexpr int kRadius    = 10; // rounded-square corner radius (matches workspace icons)
constexpr int kBottomPad = 14; // bottom inset — matches WorkspaceSwitcher::kBottomPad so the
                               // avatar/toggle line up with the gear button on the workspace bar

constexpr int   kAnimTickMs = 16;   // ~60 fps
constexpr qreal kAnimStep   = 0.16; // per tick → cross-fade settles in ~100 ms
constexpr int   kConfirmMs  = 5000; // revert the optimistic icon if the server never confirms
} // namespace

ConvFooterWidget::ConvFooterWidget(ImageCache *imgCache, QWidget *parent)
    : QWidget(parent), _imgCache(imgCache) {
    setFixedHeight(kHeight);
    setObjectName("convFooter");
    setMouseTracking(true);
    _tooltip = new PopupTooltip(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });

    _animTimer.setInterval(kAnimTickMs);
    connect(&_animTimer, &QTimer::timeout, this, &ConvFooterWidget::tickAnim);
    _confirmTimer.setSingleShot(true);
    // No confirmation in time (e.g. setPresence failed) → settle back to the truth.
    connect(&_confirmTimer, &QTimer::timeout, this, [this] { animateTo(_sp.manualAway); });
}

QString ConvFooterWidget::iconFor(bool hidden) {
    return hidden ? QStringLiteral(":/ui/hat-glasses.svg")
                  : QStringLiteral(":/ui/circle-user-round.svg");
}

void ConvFooterWidget::animateTo(bool hidden) {
    if (hidden == _displayHidden && _animProgress >= 1.0)
        return; // already showing it, nothing in flight
    _animFrom      = _displayHidden;
    _animTo        = hidden;
    _displayHidden = hidden;
    _animProgress  = 0.0;
    if (!_animTimer.isActive())
        _animTimer.start();
    update();
}

void ConvFooterWidget::tickAnim() {
    _animProgress += kAnimStep;
    if (_animProgress >= 1.0) {
        _animProgress = 1.0;
        _animTimer.stop();
    }
    update();
}

QRect ConvFooterWidget::avatarRect() const {
    return QRect(kPadH, height() - kBottomPad - kBtn, kBtn, kBtn);
}

QRect ConvFooterWidget::toggleRect() const {
    return QRect(width() - kPadH - kBtn, height() - kBottomPad - kBtn, kBtn, kBtn);
}

ConvFooterWidget::Hot ConvFooterWidget::hitTest(const QPoint &pos) const {
    if (toggleRect().contains(pos))
        return Hot::Toggle;
    if (avatarRect().contains(pos))
        return Hot::Avatar;
    return Hot::None;
}

void ConvFooterWidget::setUser(const QString &displayName, const QString &avatarUrl) {
    _displayName = displayName;
    if (avatarUrl != _avatarUrl) {
        _avatarUrl = avatarUrl;
        _avatar    = {};
        loadAvatar();
    }
    update();
}

void ConvFooterWidget::setSelfPresence(const SelfPresence &sp) {
    _sp                = sp;
    _state.isActive    = sp.active;
    // phantomAway() is false while manually away, so an explicit "hidden" shows
    // the hollow offline ring rather than the phantom-away tint.
    _state.phantomAway = sp.phantomAway();
    // Authoritative state arrived — stop the safety revert and settle the icon
    // there (a no-op cross-fade if the optimistic guess already matched).
    _confirmTimer.stop();
    animateTo(sp.manualAway);
    update();
}

void ConvFooterWidget::clear() {
    _displayName = {};
    _avatarUrl   = {};
    _avatar      = {};
    _sp          = {};
    _state       = {};
    _animTimer.stop();
    _confirmTimer.stop();
    _displayHidden = false;
    _animProgress  = 1.0;
    update();
}

void ConvFooterWidget::loadAvatar() {
    if (_avatarUrl.isEmpty() || !_imgCache)
        return;
    const QPixmap cached = _imgCache->get(_avatarUrl);
    if (!cached.isNull()) {
        _avatar = cached;
        update();
        return;
    }
    const QString url = _avatarUrl;
    connect(
        _imgCache,
        &ImageCache::loaded,
        this,
        [this, url](const QString &loadedUrl) {
            if (loadedUrl != url || url != _avatarUrl)
                return;
            _avatar = _imgCache->get(url);
            update();
        },
        Qt::SingleShotConnection
    );
}

QString ConvFooterWidget::presenceTooltip() const {
    if (_displayHidden)
        return tr("Hidden — you appear away to everyone. Click to use automatic presence.");
    if (_sp.phantomAway())
        return tr(
            "Visible — but you appear away if no official Slack client is connected. "
            "Click to hide."
        );
    return tr("Visible — using automatic presence. Click to appear hidden.");
}

void ConvFooterWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(
        rect(), Th::navGradient(this, Th::c().nav.primaryGradTop, Th::c().nav.primaryGradBottom)
    );

    // Avatar (rounded square) with presence/DND dot.
    UserAvatar::paint(
        p,
        avatarRect(),
        _avatar,
        _displayName,
        _state,
        kRadius,
        devicePixelRatioF(),
        /*borderColor=*/Th::c().nav.primary
    );

    // Presence toggle — same ghost-button chrome/colors as the workspace bar.
    const QRectF btnRect = toggleRect();
    const bool   hov     = (_hot == Hot::Toggle);
    NavGhostButton::paintChrome(p, btnRect, hov, kRadius);
    if (_animProgress < 1.0) {
        // Cross-fade the outgoing icon out and the incoming icon in.
        p.setOpacity(1.0 - _animProgress);
        NavGhostButton::paintIcon(p, btnRect, hov, iconFor(_animFrom));
        p.setOpacity(_animProgress);
        NavGhostButton::paintIcon(p, btnRect, hov, iconFor(_animTo));
        p.setOpacity(1.0);
    } else {
        NavGhostButton::paintIcon(p, btnRect, hov, iconFor(_displayHidden));
    }
}

void ConvFooterWidget::setHot(Hot hot) {
    if (_hot == hot)
        return;
    _hot = hot;
    setCursor(hot == Hot::None ? Qt::ArrowCursor : Qt::PointingHandCursor);

    if (hot == Hot::Toggle) {
        const QRect r = toggleRect();
        _tooltip->showAbove(presenceTooltip(), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (hot == Hot::Avatar) {
        const QRect r = avatarRect();
        _tooltip->showAbove(tr("Manage profile"), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else {
        _tooltip->hide();
    }
    update();
}

void ConvFooterWidget::mouseMoveEvent(QMouseEvent *e) {
    setHot(hitTest(e->pos()));
}

void ConvFooterWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton)
        _pressed = hitTest(e->pos());
}

void ConvFooterWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const Hot hit = hitTest(e->pos());
    if (hit == Hot::Toggle && _pressed == Hot::Toggle) {
        // Optimistic: flip the icon immediately so the network round-trip isn't
        // felt, then ask the session to apply it. setSelfPresence() reconciles
        // when the server confirms; the confirm timer reverts on failure.
        const bool target = !_displayHidden; // new "hidden" state
        animateTo(target);
        _confirmTimer.start(kConfirmMs);
        _tooltip->hide();
        emit presenceToggleRequested(target);
    } else if (hit == Hot::Avatar && _pressed == Hot::Avatar) {
        _tooltip->hide();
        emit profileRequested();
    }
    _pressed = Hot::None;
}

void ConvFooterWidget::leaveEvent(QEvent *) {
    _pressed = Hot::None;
    setHot(Hot::None);
}
