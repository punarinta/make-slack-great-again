// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "app_dialog.h"
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

static constexpr int kCardMinW = 480;
static constexpr int kCardMaxW = 560;
static constexpr int kCardPadH = 28; // left / right padding inside card
static constexpr int kCardPadT = 24; // top padding
static constexpr int kCardPadB = 24; // bottom padding
static constexpr int kRadius   = 12; // card corner radius

AppDialog::AppDialog(const QString &title, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    // ── Card ─────────────────────────────────────────────────────────────────
    _card = new QFrame(this);
    _card->setObjectName("appDialogCard");

    auto *shadow = new QGraphicsDropShadowEffect(_card);
    shadow->setBlurRadius(40);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 70));
    _card->setGraphicsEffect(shadow);

    auto *cardLayout = new QVBoxLayout(_card);
    cardLayout->setContentsMargins(kCardPadH, kCardPadT, kCardPadH, kCardPadB);
    cardLayout->setSpacing(0);

    // ── Header row ────────────────────────────────────────────────────────────
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(12);

    _titleLabel = new QLabel(title, _card);
    QFont tf    = _titleLabel->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.45);
    _titleLabel->setFont(tf);

    _closeBtn = new QPushButton(_card);
    _closeBtn->setFixedSize(32, 32);
    _closeBtn->setFlat(true);
    _closeBtn->setCursor(Qt::PointingHandCursor);
    _closeBtn->setFocusPolicy(Qt::NoFocus);
    // Draw a plain × glyph via text
    _closeBtn->setText("✕");
    QFont cf = _closeBtn->font();
    cf.setPointSizeF(cf.pointSizeF() * 1.1);
    _closeBtn->setFont(cf);

    headerRow->addWidget(_titleLabel, 1, Qt::AlignVCenter);
    headerRow->addWidget(_closeBtn, 0, Qt::AlignTop);
    cardLayout->addLayout(headerRow);
    cardLayout->addSpacing(20);

    // ── Content placeholder ──────────────────────────────────────────────────
    _contentLayout = new QVBoxLayout;
    _contentLayout->setContentsMargins(0, 0, 0, 0);
    _contentLayout->setSpacing(12);
    cardLayout->addLayout(_contentLayout);

    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        update();
    });
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void AppDialog::applyTheme() {
    _card->setStyleSheet(
        "QFrame#appDialogCard { background: white; border-radius: 12px; border: none; }"
    );
    _titleLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
    _closeBtn->setStyleSheet(QString(
                                 "QPushButton {"
                                 "  border: none; border-radius: 16px;"
                                 "  color: %1; background: transparent;"
                                 "}"
                                 "QPushButton:hover { background: %2; color: %3; }"
    )
                                 .arg(
                                     Th::qss(Th::c().text.tertiary),
                                     Th::qss(Th::c().surface.highlight),
                                     Th::qss(Th::c().text.secondary)
                                 ));
}

// ── Layout ────────────────────────────────────────────────────────────────────

void AppDialog::updateCard() {
    // Determine card width: constrained by the available space.
    const int avail = width() > 0 ? width() - 80 : kCardMaxW;
    const int cardW = std::clamp(avail, kCardMinW, kCardMaxW);
    _card->setFixedWidth(cardW);

    // Let Qt calculate the preferred height from the current content.
    _card->adjustSize();
    const int cardH = std::min(_card->sizeHint().height(), std::max(200, height() - 80));
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
