// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "welcome_widget.h"
#include "ui/shortcuts.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QShowEvent>

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
        _rows.append(row);
    };

    // Generated from the shortcut registry (ui/shortcuts.h) in table order —
    // this panel used to be a hand-typed list and had silently fallen behind the
    // real bindings.
    for (const auto &def : Ui::Shortcuts::all()) {
        if (!def.inHelp)
            continue;
        addRow(Ui::Shortcuts::label(def.id), Ui::Shortcuts::keyChips(def.id));
    }

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

    // The panel is a fixed list in a container it doesn't get to grow: at the
    // 800x600 minimum window size the message area is shorter than the full set
    // of rows. Drop rows from the bottom (the registry lists them in descending
    // usefulness) instead of clipping them mid-glyph.
    const int available = qMax(0, height() - 2 * hMargin);
    for (int i = 0; i < _rows.size(); ++i)
        _rows.at(i)->setVisible(true);
    for (int i = _rows.size() - 1; i >= 0 && _content->sizeHint().height() > available; --i)
        _rows.at(i)->setVisible(false);

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
