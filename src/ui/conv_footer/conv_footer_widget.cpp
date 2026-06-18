// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/conv_footer/conv_footer_widget.h"

#include "ui/context_menu/context_menu.h"
#include "ui/image_cache.h"
#include "ui/nav_ghost_button.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/background_tasks.h"

#include <QCursor>
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

constexpr int   kTaskGap     = 8;   // gap between the task spinner and the toggle button
constexpr int   kSpinTickMs  = 16;  // ~60 fps
constexpr qreal kSpinDegStep = 3.0; // per tick → full cog turn in ~2 s
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

    _taskTimer.setInterval(kSpinTickMs);
    connect(&_taskTimer, &QTimer::timeout, this, &ConvFooterWidget::tickTaskSpin);
    auto &tasks = BackgroundTasks::instance();
    connect(&tasks, &BackgroundTasks::countChanged, this, &ConvFooterWidget::setTaskCount);
    setTaskCount(tasks.count());
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

QRect ConvFooterWidget::tasksRect() const {
    if (!hasTasks())
        return {};
    const int x = toggleRect().left() - kTaskGap - kBtn;
    return QRect(x, height() - kBottomPad - kBtn, kBtn, kBtn);
}

ConvFooterWidget::Hot ConvFooterWidget::hitTest(const QPoint &pos) const {
    if (toggleRect().contains(pos))
        return Hot::Toggle;
    if (hasTasks() && tasksRect().contains(pos))
        return Hot::Tasks;
    if (avatarRect().contains(pos))
        return Hot::Avatar;
    return Hot::None;
}

void ConvFooterWidget::setTaskCount(int count) {
    if (count == _taskCount)
        return;
    const bool wasRunning = _taskCount > 0;
    _taskCount            = count;
    if (_taskCount > 0 && !_taskTimer.isActive())
        _taskTimer.start();
    else if (_taskCount == 0 && _taskTimer.isActive())
        _taskTimer.stop();
    // Spinner just vanished from under the cursor — drop a now-stale tooltip,
    // or refresh the count if it's still hovered.
    if (_hot == Hot::Tasks) {
        if (_taskCount == 0)
            setHot(hitTest(mapFromGlobal(QCursor::pos())));
        else
            showTasksTooltip();
    }
    if (wasRunning != (_taskCount > 0))
        update();
}

void ConvFooterWidget::tickTaskSpin() {
    _taskAngle += kSpinDegStep;
    if (_taskAngle >= 360.0)
        _taskAngle -= 360.0;
    update();
}

QString ConvFooterWidget::tasksTooltip() const {
    return tr("%n background task(s) running", "", _taskCount);
}

void ConvFooterWidget::showTasksTooltip() {
    if (!hasTasks())
        return;
    const QRect r = tasksRect();
    _tooltip->showTaskList(
        tasksTooltip(),
        BackgroundTasks::instance().descriptions(),
        QRect(mapToGlobal(r.topLeft()), r.size())
    );
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
    disconnect(_avatarConn);
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
    // Drop any in-flight subscription from a previous URL before starting over.
    disconnect(_avatarConn);

    if (_avatarUrl.isEmpty() || !_imgCache)
        return;
    const QPixmap cached = _imgCache->get(_avatarUrl);
    if (!cached.isNull()) {
        _avatar = cached;
        update();
        return;
    }
    const QString url = _avatarUrl;
    // Persistent connection: keep listening across unrelated `loaded` emissions
    // until OUR url arrives, then tear ourselves down.
    _avatarConn =
        connect(_imgCache, &ImageCache::loaded, this, [this, url](const QString &loadedUrl) {
            if (loadedUrl != url)
                return;
            disconnect(_avatarConn);
            if (url != _avatarUrl)
                return; // user changed under us while loading
            _avatar = _imgCache->get(url);
            update();
        });
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

    // Background-task spinner — same ghost chrome, with a continuously rotating cog.
    if (hasTasks()) {
        const QRectF taskR = tasksRect();
        const bool   thov  = (_hot == Hot::Tasks);
        NavGhostButton::paintChrome(p, taskR, thov, kRadius);
        const qreal   dim = taskR.width() * 0.52;
        const int     sz  = qMax(1, qRound(dim));
        const QPixmap px  = svgPixmap(
            QStringLiteral(":/ui/cog.svg"), QSize(sz, sz), NavGhostButton::iconColor(thov)
        );
        p.save();
        p.translate(taskR.center());
        p.rotate(_taskAngle);
        p.drawPixmap(QRectF(-dim / 2.0, -dim / 2.0, dim, dim).toRect(), px);
        p.restore();
    }
}

void ConvFooterWidget::setHot(Hot hot) {
    if (_hot == hot)
        return;
    _hot                 = hot;
    // The spinner is a status indicator, not a button — keep the arrow over it.
    const bool clickable = (hot == Hot::Avatar || hot == Hot::Toggle);
    setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);

    if (hot == Hot::Toggle) {
        const QRect r = toggleRect();
        _tooltip->showAbove(presenceTooltip(), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (hot == Hot::Tasks) {
        showTasksTooltip();
    } else if (hot == Hot::Avatar) {
        const QRect r = avatarRect();
        _tooltip->showAbove(tr("Profile & status"), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else {
        _tooltip->hide();
    }
    update();
}

void ConvFooterWidget::mouseMoveEvent(QMouseEvent *e) {
    setHot(hitTest(e->pos()));
}

void ConvFooterWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        _pressed = hitTest(e->pos());
    } else if (e->button() == Qt::RightButton && hitTest(e->pos()) == Hot::Avatar) {
        // Right-click the avatar → same profile/status menu (left-click also works).
        _tooltip->hide();
        showAvatarMenu();
    }
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
        showAvatarMenu();
    }
    _pressed = Hot::None;
}

void ConvFooterWidget::showAvatarMenu() {
    auto *menu = new ContextMenu(this);
    menu->addItem(
        tr("Manage profile"),
        [this] { emit manageProfileRequested(); }, /*destructive=*/
        false,
        QStringLiteral(":/ui/circle-user-round.svg")
    );
    menu->addItem(
        tr("Manage status"),
        [this] { emit manageStatusRequested(); }, /*destructive=*/
        false,
        QStringLiteral(":/ui/smile.svg")
    );
    // Anchor at the avatar's top — ContextMenu flips upward near the screen edge,
    // so the menu opens above the footer.
    const QRect r = avatarRect();
    menu->popup(mapToGlobal(r.topLeft()));
}

void ConvFooterWidget::leaveEvent(QEvent *) {
    _pressed = Hot::None;
    setHot(Hot::None);
}
