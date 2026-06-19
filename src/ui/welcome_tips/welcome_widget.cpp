// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "welcome_widget.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QShowEvent>
#include <QCoreApplication>

namespace {

// ── Platform key labels ───────────────────────────────────────────────────────

#ifdef Q_OS_MAC
const QString kMod   = QString(QChar(0x2318)); // ⌘
const QString kShift = QString(QChar(0x21E7)); // ⇧
#else
const QString kMod   = QStringLiteral("Ctrl");
const QString kShift = QStringLiteral("Shift");
#endif

} // namespace

// ── WelcomeWidget ─────────────────────────────────────────────────────────────

WelcomeWidget::WelcomeWidget(QWidget *parent) : QWidget(parent) {
    _content         = new QWidget(this);
    auto       *vbox = new QVBoxLayout(_content);
    const auto &sp   = Th::c().spacing;
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Title
    _title = new QLabel(tr("Keyboard shortcuts"), _content);
    vbox->addWidget(_title);

    vbox->addSpacing(sp.lg);

    // Thin rule beneath the title
    _rule = new QFrame(_content);
    _rule->setFrameShape(QFrame::HLine);
    _rule->setFrameShadow(QFrame::Plain);
    _rule->setFixedHeight(1);
    vbox->addWidget(_rule);

    vbox->addSpacing(sp.md);

    // ── Build shortcut rows ───────────────────────────────────────────────────

    const QString up = QString(QChar(0x2191)); // ↑

    // Helper lambdas that build widgets and register them for applyTheme().
    auto makeChip = [this](const QString &text, QWidget *parent) -> QLabel * {
        auto *lbl = new QLabel(text, parent);
        lbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        lbl->setAlignment(Qt::AlignCenter);
        _chipLabels.append(lbl);
        return lbl;
    };

    auto makePlus = [this](QWidget *parent) -> QLabel * {
        auto *lbl = new QLabel("+", parent);
        lbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _plusLabels.append(lbl);
        return lbl;
    };

    auto addRow = [&](const QString &action, const QStringList &keys) {
        auto *row = new QWidget;
        row->setFixedHeight(36);
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(sp.sm);

        auto *actionLbl = new QLabel(action, row);
        _actionLabels.append(actionLbl);
        hl->addWidget(actionLbl);
        hl->addStretch(1);

        for (int i = 0; i < keys.size(); ++i) {
            if (i > 0)
                hl->addWidget(makePlus(row));
            hl->addWidget(makeChip(keys.at(i), row));
        }

        vbox->addWidget(row);
    };

    addRow(QCoreApplication::translate("WelcomeWidget", "Send message"), {"Enter"});
    addRow(QCoreApplication::translate("WelcomeWidget", "New line in message"), {kShift, "Enter"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Edit last message"), {up});
    addRow(QCoreApplication::translate("WelcomeWidget", "Bold"), {kMod, "B"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Italic"), {kMod, "I"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Strikethrough"), {kMod, kShift, "X"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Inline code"), {kMod, kShift, "C"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Attach file"), {kMod, "O"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Emoji picker"), {kMod, kShift, "\\"});
    addRow(QCoreApplication::translate("WelcomeWidget", "Cancel / exit edit"), {"Esc"});

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &WelcomeWidget::applyTheme
    );
}

void WelcomeWidget::applyTheme() {
    const auto &th = Th::c();

    _title->setStyleSheet(QString("font-size: %1px; color: %2; font-weight: 500;")
                              .arg(th.fonts.lg)
                              .arg(Th::qss(th.text.secondary)));

    _rule->setStyleSheet(QString("color: %1;").arg(Th::qss(th.surface.highlight)));

    const QString chipSS = QString(
                               "QLabel {"
                               "  background-color: %1;"
                               "  border: 1px solid %2;"
                               "  border-bottom: 2px solid %2;"
                               "  border-radius: 5px;"
                               "  padding: 3px 9px;"
                               "  font-size: %3px;"
                               "  color: %4;"
                               "}"
    )
                               .arg(
                                   Th::qss(th.surface.highlight),
                                   Th::qss(th.divider.def),
                                   QString::number(th.fonts.caption),
                                   Th::qss(th.text.primary)
                               );
    for (QLabel *lbl : std::as_const(_chipLabels))
        lbl->setStyleSheet(chipSS);

    const QString plusSS = QString("font-size: %1px; color: %2; padding: 0 1px;")
                               .arg(th.fonts.sm)
                               .arg(Th::qss(th.divider.def));
    for (QLabel *lbl : std::as_const(_plusLabels))
        lbl->setStyleSheet(plusSS);

    const QString actionSS =
        QString("font-size: %1px; color: %2;").arg(th.fonts.base).arg(Th::qss(th.text.primary));
    for (QLabel *lbl : std::as_const(_actionLabels))
        lbl->setStyleSheet(actionSS);
}

void WelcomeWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    repositionContent();
}

void WelcomeWidget::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    repositionContent();
}

void WelcomeWidget::repositionContent() {
    if (!_content)
        return;

    const int hMargin  = 48;
    const int contentW = qMax(300, qMin(420, width() - 2 * hMargin));
    _content->setFixedWidth(contentW);

    const int contentH = _content->sizeHint().height();
    const int x        = (width() - contentW) / 2;

    int      y;
    QWidget *win = window();
    if (win && win != this) {
        const int winH       = win->height();
        const int myTopInWin = mapTo(win, QPoint(0, 0)).y();
        y                    = (winH - contentH) / 2 - myTopInWin;
    } else {
        y = (height() - contentH) / 2;
    }
    y = qBound(hMargin, y, qMax(hMargin, height() - contentH - hMargin));

    _content->setGeometry(x, y, contentW, contentH);
}
