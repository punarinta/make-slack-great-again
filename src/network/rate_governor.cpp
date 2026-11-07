// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include "rate_governor.h"

#include <algorithm>

namespace net {

RateGovernor::RateGovernor(QObject *parent) : QObject(parent) {
    _cooldownTimer.setSingleShot(true);
    connect(&_cooldownTimer, &QTimer::timeout, this, &RateGovernor::onCooldownTimer);
}

RateGovernor::~RateGovernor() {
    _cooldownTimer.stop();
}

void RateGovernor::setMaxInflight(int count) {
    _maxInflight = qMax(1, count);
}

int RateGovernor::maxInflight() const {
    return _maxInflight;
}

void RateGovernor::setMinDispatchIntervalMs(int ms) {
    _minDispatchIntervalMs = qMax(0, ms);
}

int RateGovernor::minDispatchIntervalMs() const {
    return _minDispatchIntervalMs;
}

void RateGovernor::setMaxRequestsPerMinute(int count) {
    _maxRequestsPerMinute = qMax(1, count);
}

int RateGovernor::maxRequestsPerMinute() const {
    return _maxRequestsPerMinute;
}

void RateGovernor::setConservativeMode(bool conservative) {
    _conservativeMode = conservative;
    if (_conservativeMode) {
        _maxInflight           = 1;
        _minDispatchIntervalMs = 800;
        _maxRequestsPerMinute  = 25;
    } else {
        _maxInflight           = 2;
        _minDispatchIntervalMs = 150;
        _maxRequestsPerMinute  = 70;
    }
}

void RateGovernor::setMinimalMode() {
    _conservativeMode      = true;
    _maxInflight           = 1;
    _minDispatchIntervalMs = 3000;
    _maxRequestsPerMinute  = 15;
}

bool RateGovernor::isConservativeMode() const {
    return _conservativeMode;
}

void RateGovernor::registerQueue(HttpQueue *queue) {
    if (queue)
        _queues.insert(queue);
}

void RateGovernor::unregisterQueue(HttpQueue *queue) {
    if (queue)
        _queues.remove(queue);
}

bool RateGovernor::hasAnyWaitingNormalCall() const {
    for (const auto &q : _queues) {
        if (q && q->hasWaitingNormalCall())
            return true;
    }
    return false;
}

void RateGovernor::pruneHistory(qint64 now) {
    constexpr qint64 kWindowMs = 60'000;
    while (!_requestHistory.isEmpty() && (now - _requestHistory.head()) >= kWindowMs)
        _requestHistory.dequeue();
}

qint64 RateGovernor::delayUntilEligibleMs(HttpQueue::Priority priority) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    pruneHistory(now);

    // 1. Max concurrent in-flight ceiling
    if (_currentInflight >= _maxInflight) {
        // Must wait until an in-flight request finishes
        return 50;
    }

    // 2. Background calls yield to Normal calls waiting on any registered queue
    if (priority == HttpQueue::Priority::Background && hasAnyWaitingNormalCall()) {
        return 100;
    }

    // 3. Spacing between consecutive dispatches
    const qint64 interval = (priority == HttpQueue::Priority::Background && _conservativeMode)
                                ? (_minDispatchIntervalMs * 2)
                                : _minDispatchIntervalMs;
    const qint64 sinceLast = now - _lastDispatchTimeMs;
    if (sinceLast < interval) {
        return interval - sinceLast;
    }

    // 4. Rolling 60-second window rate limit
    if (_requestHistory.size() >= _maxRequestsPerMinute) {
        const qint64 oldest    = _requestHistory.head();
        const qint64 expiresAt = oldest + 60'000;
        if (expiresAt > now) {
            return expiresAt - now;
        }
    }

    return 0; // Eligible now!
}

void RateGovernor::recordDispatched(HttpQueue::Priority priority) {
    const qint64 now    = QDateTime::currentMSecsSinceEpoch();
    _currentInflight++;
    _lastDispatchTimeMs = now;
    _requestHistory.enqueue(now);

    const qint64 interval = (priority == HttpQueue::Priority::Background && _conservativeMode)
                                ? (_minDispatchIntervalMs * 2)
                                : _minDispatchIntervalMs;
    if (interval > 0)
        scheduleCooldownTimer(interval);
}

void RateGovernor::recordFinished(HttpQueue::Priority priority) {
    Q_UNUSED(priority);
    if (_currentInflight > 0)
        _currentInflight--;

    const qint64 wait = delayUntilEligibleMs(HttpQueue::Priority::Normal);
    if (wait == 0) {
        emit slotAvailable();
    } else {
        scheduleCooldownTimer(wait);
    }
}

void RateGovernor::scheduleCooldownTimer(qint64 delayMs) {
    const int delay = static_cast<int>(qBound<qint64>(qint64(1), delayMs, qint64(60'000)));
    if (_cooldownTimer.isActive() && _cooldownTimer.remainingTime() <= delay)
        return; // A wake timer is already due sooner
    _cooldownTimer.start(delay);
}

void RateGovernor::onCooldownTimer() {
    emit slotAvailable();
}

} // namespace net
