// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "welcome_widget.h"
#include "ui/theme.h"
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QShowEvent>
#include <QCoreApplication>

namespace {

// ── Visual constants ──────────────────────────────────────────────────────────

const char *kChipStyle =
    "QLabel {"
    "  background-color: #F3F3F4;"
    "  border: 1px solid #D4D3D3;"
    "  border-bottom: 2px solid #BBBBBB;"
    "  border-radius: 5px;"
    "  padding: 3px 9px;"
    "  font-size: 12px;"
    "  color: #333333;"
    "}";

// ── Platform key labels ───────────────────────────────────────────────────────

#ifdef Q_OS_MAC
const QString kMod   = QString(QChar(0x2318)); // ⌘
const QString kShift = QString(QChar(0x21E7)); // ⇧
#else
const QString kMod   = QStringLiteral("Ctrl");
const QString kShift = QStringLiteral("Shift");
#endif

// ── Helpers ───────────────────────────────────────────────────────────────────

QLabel *keyChip(const QString &text, QWidget *parent) {
    auto *lbl = new QLabel(text, parent);
    lbl->setStyleSheet(kChipStyle);
    lbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    lbl->setAlignment(Qt::AlignCenter);
    return lbl;
}

QLabel *plusLabel(QWidget *parent) {
    auto *lbl = new QLabel("+", parent);
    lbl->setStyleSheet("font-size: 11px; color: #BBBBBB; padding: 0 1px;");
    lbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return lbl;
}

// Adds one row: action name on the left, key chips on the right.
// keys = list of key labels that form one chord, joined with "+" separators.
void addRow(QVBoxLayout *vbox, const QString &action, const QStringList &keys) {
    auto *row = new QWidget;
    row->setFixedHeight(36);
    auto *hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(5);

    auto *actionLbl = new QLabel(action, row);
    actionLbl->setStyleSheet(QString("font-size: 14px; color: %1;")
                             .arg(Theme::kTextPrimary.name()));
    hl->addWidget(actionLbl);
    hl->addStretch(1);

    for (int i = 0; i < keys.size(); ++i) {
        if (i > 0) hl->addWidget(plusLabel(row));
        hl->addWidget(keyChip(keys.at(i), row));
    }

    vbox->addWidget(row);
}

// ── Shortcut table ────────────────────────────────────────────────────────────

void buildRows(QVBoxLayout *vbox) {
    const QString up = QString(QChar(0x2191)); // ↑

    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Send message"),
        { "Enter" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "New line in message"),
        { kShift, "Enter" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Edit last message"),
        { up });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Bold"),
        { kMod, "B" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Italic"),
        { kMod, "I" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Strikethrough"),
        { kMod, kShift, "X" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Inline code"),
        { kMod, kShift, "C" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Attach file"),
        { kMod, "O" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Emoji picker"),
        { kMod, kShift, "\\" });
    addRow(vbox,
        QCoreApplication::translate("WelcomeWidget", "Cancel / exit edit"),
        { "Esc" });
}

} // namespace

// ── WelcomeWidget ─────────────────────────────────────────────────────────────

WelcomeWidget::WelcomeWidget(QWidget *parent)
    : QWidget(parent)
{
    _content = new QWidget(this);
    auto *vbox = new QVBoxLayout(_content);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Title
    auto *title = new QLabel(tr("Keyboard shortcuts"), _content);
    title->setStyleSheet(QString("font-size: 15px; color: %1; font-weight: 500;")
                         .arg(Theme::kTextSecondary.name()));
    vbox->addWidget(title);

    vbox->addSpacing(14);

    // Thin rule beneath the title
    auto *rule = new QFrame(_content);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    rule->setStyleSheet("color: #EBEBEB;");
    rule->setFixedHeight(1);
    vbox->addWidget(rule);

    vbox->addSpacing(8);

    buildRows(vbox);
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
    if (!_content) return;

    const int hMargin = 48;
    const int contentW = qMax(300, qMin(420, width() - 2 * hMargin));
    _content->setFixedWidth(contentW);

    const int contentH = _content->sizeHint().height();
    const int x = (width() - contentW) / 2;

    int y;
    QWidget *win = window();
    if (win && win != this) {
        const int winH       = win->height();
        const int myTopInWin = mapTo(win, QPoint(0, 0)).y();
        y = (winH - contentH) / 2 - myTopInWin;
    } else {
        y = (height() - contentH) / 2;
    }
    y = qBound(hMargin, y, qMax(hMargin, height() - contentH - hMargin));

    _content->setGeometry(x, y, contentW, contentH);
}
