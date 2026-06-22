// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "app_dialog.h"
#include "ui/icon_button/icon_button.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QEventLoop>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

static constexpr int kCardMinW = 480;
static constexpr int kCardMaxW = 560;
static constexpr int kCardPadH = 28; // left / right padding inside card
static constexpr int kCardPadT = 24; // top padding
static constexpr int kCardPadB = 24; // bottom padding

// Resolve the top-level window we should overlay (and parent ourselves to).
static QWidget *overlayHost(QWidget *parent) {
    return parent ? parent->window() : nullptr;
}

AppDialog::AppDialog(const QString &title, QWidget *parent) : QWidget(overlayHost(parent)) {
    buildCard(/*standardHeader=*/true, title);
}

AppDialog::AppDialog(QWidget *parent, Chrome chrome) : QWidget(overlayHost(parent)) {
    buildCard(/*standardHeader=*/chrome == Chrome::Standard, QString());
}

void AppDialog::buildCard(bool standardHeader, const QString &title) {
    // In-window child overlay: a free child of the host window, not in any
    // layout, manually sized to cover the whole window (see coverParent()).
    // WA_NoSystemBackground lets the semi-transparent backdrop blend over the
    // window content already in the backing store, exactly like the search bar.
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    hide(); // shown explicitly via exec()/open()
    if (QWidget *host = parentWidget())
        host->installEventFilter(this);

    // ── Card (rounded panel + soft shadow) ─────────────────────────────────────
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

        connect(_closeBtn, &QPushButton::clicked, this, &AppDialog::reject);

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
        connect(secondary, &QPushButton::clicked, this, &AppDialog::reject);
    }
    if (primary)
        row->addWidget(primary);
    _contentLayout->addLayout(row);
    return row;
}

// ── Modal API (QDialog-compatible) ──────────────────────────────────────────────

int AppDialog::exec() {
    if (isVisible())
        return _result;
    QPointer<AppDialog> guard(this);
    _result = QDialog::Rejected;
    show();

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    if (!guard)
        return QDialog::Rejected; // deleted while running (e.g. WA_DeleteOnClose)
    _loop = nullptr;
    return _result;
}

void AppDialog::open() {
    _result = QDialog::Rejected;
    show();
}

void AppDialog::accept() {
    done(QDialog::Accepted);
}

void AppDialog::reject() {
    done(QDialog::Rejected);
}

void AppDialog::done(int result) {
    _result = result;
    hide(); // fires hideEvent → quits any running exec() loop

    // Emit signals while the object is still alive (mirrors QDialog::done).
    emit finished(result);
    if (result == QDialog::Accepted)
        emit accepted();
    else if (result == QDialog::Rejected)
        emit rejected();

    if (testAttribute(Qt::WA_DeleteOnClose))
        deleteLater();
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

void AppDialog::coverParent() {
    if (QWidget *host = parentWidget()) {
        // We are a direct child of the host window, so its rect() (origin 0,0)
        // is exactly the geometry we must occupy — client coordinates, which the
        // compositor cannot offset.
        setGeometry(host->rect());
        raise();
    }
    updateCard();
}

// ── Events ────────────────────────────────────────────────────────────────────

void AppDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 140));
}

void AppDialog::showEvent(QShowEvent *e) {
    coverParent();
    QWidget::showEvent(e);
    // Pull focus into the dialog so the keyboard (incl. Escape) is captured and
    // can't reach widgets behind the backdrop, then hand off to the first
    // focusable child — mirroring QDialog's auto-focus of the first input.
    setFocus(Qt::PopupFocusReason);
    focusNextChild();
}

void AppDialog::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    if (_loop)
        _loop->quit();
}

void AppDialog::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    updateCard();
}

bool AppDialog::eventFilter(QObject *obj, QEvent *event) {
    // Track the host window so the overlay always covers it.
    if (obj == parentWidget() && isVisible() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        coverParent();
    return QWidget::eventFilter(obj, event);
}

void AppDialog::mousePressEvent(QMouseEvent *e) {
    // Click outside the card dismisses the dialog (backdrop click).
    if (!_card->geometry().contains(e->pos()))
        reject();
    else
        QWidget::mousePressEvent(e);
}

void AppDialog::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(e);
}
