// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "typing_indicator.h"

#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QHBoxLayout>
#include <QLabel>

namespace {
// How long a single user_typing event keeps a user "typing" before they fall
// off, absent a refresh.  Slack re-sends user_typing roughly every 3 s while a
// person is actively typing, so a 6 s window comfortably bridges the gap.
constexpr int kExpiryMs = 6000;
constexpr int kPurgeMs  = 1000; // how often we drop stale typers
} // namespace

TypingIndicatorWidget::TypingIndicatorWidget(QWidget *parent) : QWidget(parent) {
    setObjectName("typingIndicator");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 1, 14, 1);
    layout->setSpacing(0);

    _label = new QLabel(this);
    _label->setTextFormat(Qt::RichText);
    _label->setTextInteractionFlags(Qt::NoTextInteraction);
    layout->addWidget(_label, 1);

    _clock.start();
    _purgeTimer.setInterval(kPurgeMs);
    connect(&_purgeTimer, &QTimer::timeout, this, [this] { purge(); });

    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void TypingIndicatorWidget::userTyping(const UserId &id, const QString &name, bool isSelf) {
    const qint64 deadline = _clock.elapsed() + kExpiryMs;
    for (auto &e : _typers) {
        if (e.id == id.value) {
            e.name     = name;
            e.deadline = deadline;
            e.isSelf   = isSelf;
            rebuild();
            return;
        }
    }
    _typers.push_back({id.value, name, deadline, isSelf});
    if (!_purgeTimer.isActive())
        _purgeTimer.start();
    rebuild();
}

void TypingIndicatorWidget::userStopped(const UserId &id) {
    for (int i = 0; i < _typers.size(); ++i) {
        if (_typers[i].id == id.value) {
            _typers.remove(i);
            rebuild();
            return;
        }
    }
}

void TypingIndicatorWidget::clearAll() {
    if (_typers.isEmpty())
        return;
    _typers.clear();
    rebuild();
}

void TypingIndicatorWidget::purge() {
    const qint64 now    = _clock.elapsed();
    const int    before = _typers.size();
    _typers.removeIf([now](const Entry &e) { return e.deadline <= now; });
    if (_typers.size() != before)
        rebuild();
}

void TypingIndicatorWidget::rebuild() {
    if (_typers.isEmpty()) {
        _purgeTimer.stop();
        hide();
        return;
    }

    // Sole self typer gets a dedicated cue — we never echo local typing, so this
    // only ever means "you're typing on one of your other clients".
    if (_typers.size() == 1 && _typers.front().isSelf) {
        _label->setText(tr("<b>You</b> are typing on another device…"));
        show();
        return;
    }

    QStringList names;
    names.reserve(_typers.size());
    for (const auto &e : _typers)
        names << QStringLiteral("<b>%1</b>").arg(e.isSelf ? tr("You") : e.name.toHtmlEscaped());

    const QString joined = names.join(QStringLiteral(", "));
    const QString text =
        _typers.size() == 1 ? tr("%1 is typing…").arg(joined) : tr("%1 are typing…").arg(joined);
    _label->setText(text);
    show();
}

void TypingIndicatorWidget::applyTheme() {
    setStyleSheet(QString(
                      "QWidget#typingIndicator { background: transparent; }"
                      "QLabel { background: transparent; border: none;"
                      "  font-size: %1px; color: %2; }"
    )
                      .arg(Th::c().fonts.sm)
                      .arg(Th::qss(Th::c().text.secondary)));
}
