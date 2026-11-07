// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#pragma once

#include "http_queue.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>

namespace net {

class RateGovernor : public QObject {
    Q_OBJECT
public:
    explicit RateGovernor(QObject *parent = nullptr);
    ~RateGovernor() override;

    // Configuration
    void setMaxInflight(int count);
    int  maxInflight() const;

    void setMinDispatchIntervalMs(int ms);
    int  minDispatchIntervalMs() const;

    void setMaxRequestsPerMinute(int count);
    int  maxRequestsPerMinute() const;

    void setConservativeMode(bool conservative);
    // Strictest preset: one request at a time, >= 3 s apart, <= 15 per minute.
    void setMinimalMode();
    bool isConservativeMode() const;

    // Queue registration
    void registerQueue(HttpQueue *queue);
    void unregisterQueue(HttpQueue *queue);

    // Queries before dispatch: returns delay in ms (0 = eligible immediately)
    qint64 delayUntilEligibleMs(HttpQueue::Priority priority);

    // Dispatch lifecycle notifications from HttpQueue
    void recordDispatched(HttpQueue::Priority priority);
    void recordFinished(HttpQueue::Priority priority);

    // Checks if any registered queue has an eligible Normal priority call waiting
    bool hasAnyWaitingNormalCall() const;

signals:
    // Emitted when a slot opens up (cooldown elapsed or request finished)
    void slotAvailable();

private slots:
    void onCooldownTimer();

private:
    void scheduleCooldownTimer(qint64 delayMs);
    void pruneHistory(qint64 now);

    int  _maxInflight           = 2;
    int  _minDispatchIntervalMs = 150;
    int  _maxRequestsPerMinute  = 70;
    bool _conservativeMode      = false;

    int            _currentInflight    = 0;
    qint64         _lastDispatchTimeMs = 0;
    QQueue<qint64> _requestHistory; // timestamps of dispatches in the last 60 seconds

    QTimer           _cooldownTimer;
    QSet<HttpQueue*> _queues;
};

} // namespace net
