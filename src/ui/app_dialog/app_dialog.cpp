// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "app_dialog.h"
#include "ui/icon_button/icon_button.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

static constexpr int kCardMinW = 480;
static constexpr int kCardMaxW = 560;
static constexpr int kCardPadH = 28; // left / right padding inside card
static constexpr int kCardPadT = 24; // top padding
static constexpr int kCardPadB = 24; // bottom padding

AppDialog::AppDialog(const QString &title, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint) {
    buildCard(/*standardHeader=*/true, title);
}

AppDialog::AppDialog(QWidget *parent, Chrome chrome)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint) {
    buildCard(/*standardHeader=*/chrome == Chrome::Standard, QString());
}

void AppDialog::buildCard(bool standardHeader, const QString &title) {
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    // ── Card (frameless overlay panel + soft shadow) ───────────────────────────
    _card = new QFrame(this);
    _card->setObjectName("appDialogCard");

    auto *shadow = new QGraphicsDropShadowEffect(_card);
    shadow->setBlurRadius(40);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 70));
    _card->setGraphicsEffect(shadow);

    _cardLayout    = new QVBoxLayout(_card);
    const auto &sp = Th::c().spacing;
    _cardLayout->setSpacing(0);

    if (standardHeader) {
        _cardLayout->setContentsMargins(kCardPadH, kCardPadT, kCardPadH, kCardPadB);

        // ── Header row: bold title + × close ───────────────────────────────────
        auto *headerRow = new QHBoxLayout;
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(sp.lg);

        _titleLabel = new QLabel(title, _card);
        QFont tf    = _titleLabel->font();
        tf.setBold(true);
        tf.setPointSizeF(tf.pointSizeF() * 1.45);
        _titleLabel->setFont(tf);

        _closeBtn = new IconButton(QStringLiteral(":/ui/x.svg"), 32, 14, _card);

        headerRow->addWidget(_titleLabel, 1, Qt::AlignVCenter);
        headerRow->addWidget(_closeBtn, 0, Qt::AlignTop);
        _cardLayout->addLayout(headerRow);
        _cardLayout->addSpacing(sp.xl);

        connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

        _contentLayout = new QVBoxLayout;
        _contentLayout->setContentsMargins(0, 0, 0, 0);
        _contentLayout->setSpacing(sp.lg);
        _cardLayout->addLayout(_contentLayout);
    } else {
        // Custom chrome: no header, content fills the card edge-to-edge.
        _cardLayout->setContentsMargins(0, 0, 0, 0);
        _contentLayout = new QVBoxLayout;
        _contentLayout->setContentsMargins(0, 0, 0, 0);
        _contentLayout->setSpacing(0);
        _cardLayout->addLayout(_contentLayout);
    }

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        update();
    });
}

QHBoxLayout *
AppDialog::addButtonRow(QPushButton *primary, QPushButton *secondary, QWidget *leadingExtra) {
    auto *row = new QHBoxLayout;
    row->setSpacing(Th::c().spacing.md);
    if (leadingExtra)
        row->addWidget(leadingExtra);
    row->addStretch();
    if (secondary) {
        row->addWidget(secondary);
        connect(secondary, &QPushButton::clicked, this, &QDialog::reject);
    }
    if (primary)
        row->addWidget(primary);
    _contentLayout->addLayout(row);
    return row;
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void AppDialog::applyTheme() {
    _card->setStyleSheet(
        "QFrame#appDialogCard { background: white; border-radius: 12px; border: none; }"
    );
    if (_titleLabel)
        _titleLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
    // _closeBtn (IconButton) self-themes.
}

// ── Layout ────────────────────────────────────────────────────────────────────

int AppDialog::cardWidth(int availOverlayWidth) const {
    return std::clamp(availOverlayWidth, kCardMinW, kCardMaxW);
}

void AppDialog::updateCard() {
    // Determine card width from the available overlay space (big fallback before
    // the overlay has a real size, so cardWidth() clamps to its own maximum).
    const int avail = width() > 0 ? width() - 80 : 2000;
    const int cardW = cardWidth(avail);
    _card->setFixedWidth(cardW);

    // Let Qt calculate the preferred height from the current content.
    _card->adjustSize();
    const int cardH =
        std::min(_card->sizeHint().height(), std::max(minCardHeight(), height() - 80));
    _card->resize(cardW, cardH);

    // Centre in the overlay.
    _card->move((width() - cardW) / 2, (height() - cardH) / 2);
}

// ── Events ────────────────────────────────────────────────────────────────────

void AppDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 140));
}

void AppDialog::showEvent(QShowEvent *e) {
    // Size the overlay to cover the parent (top-level) window exactly.
    if (QWidget *top = parentWidget() ? parentWidget()->window() : nullptr) {
        setGeometry(top->geometry());
    }
    updateCard();
    QDialog::showEvent(e);
}

void AppDialog::resizeEvent(QResizeEvent *e) {
    QDialog::resizeEvent(e);
    updateCard();
}

void AppDialog::mousePressEvent(QMouseEvent *e) {
    // Click outside the card dismisses the dialog (backdrop click).
    if (!_card->geometry().contains(e->pos()))
        reject();
    else
        QDialog::mousePressEvent(e);
}
