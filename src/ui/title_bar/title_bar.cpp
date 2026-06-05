// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "title_bar.h"
#include "ui/popup_tooltip/popup_tooltip.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QStyle>
#include <QWindow>

// Unicode window-control glyphs
static const QString kGlyphMin     = QString(QChar(0x2014)); // — (minimize line)
static const QString kGlyphMax     = QString(QChar(0x25A1)); // □ (maximize)
static const QString kGlyphRestore = QString(QChar(0x2750)); // ❐ (restore)
static const QString kGlyphClose   = QString(QChar(0x2715)); // ✕ (close)
static const QString kGlyphPin     = QString(QChar(0x22A4)); // ⊤ (pin / always-on-top)

TitleBar::TitleBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(22);
    setObjectName("titleBar");
    setAttribute(Qt::WA_StyledBackground);

    _tooltip = new PopupTooltip(this);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addStretch(1);

    auto makeBtn = [&](const QString &glyph, const char *name) {
        auto *btn = new QPushButton(glyph, this);
        btn->setObjectName(name);
        btn->setFixedSize(40, 22);
        btn->setFlat(true);
        btn->setCursor(Qt::ArrowCursor);
        return btn;
    };

    if (QGuiApplication::platformName() != "wayland") {
        _pinBtn = makeBtn(kGlyphPin, "titleBarPin");
        _pinBtn->installEventFilter(this);
        connect(_pinBtn, &QPushButton::clicked, this, [this] { togglePin(); });
        layout->addWidget(_pinBtn);
    }

    auto *minBtn = makeBtn(kGlyphMin, "titleBarMin");
    connect(minBtn, &QPushButton::clicked, this, [this] { window()->showMinimized(); });
    layout->addWidget(minBtn);

    _maxBtn = makeBtn(kGlyphMax, "titleBarMax");
    connect(_maxBtn, &QPushButton::clicked, this, [this] {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
    });
    layout->addWidget(_maxBtn);

    auto *closeBtn = makeBtn(kGlyphClose, "titleBarClose");
    connect(closeBtn, &QPushButton::clicked, this, [this] { window()->close(); });
    layout->addWidget(closeBtn);
}

void TitleBar::setTitle(const QString &) {
}

void TitleBar::updateMaxButton() {
    if (_maxBtn && window())
        _maxBtn->setText(window()->isMaximized() ? kGlyphRestore : kGlyphMax);
}

void TitleBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        if (auto *h = window()->windowHandle()) {
            h->startSystemMove();
            e->accept();
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TitleBar::changeEvent(QEvent *e) {
    if (e->type() == QEvent::WindowStateChange)
        updateMaxButton();
    QWidget::changeEvent(e);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *e) {
    if (watched == _pinBtn) {
        if (e->type() == QEvent::Enter) {
            const QString text = _pinned ? "Unpin window" : "Pin window on top";
            _tooltip->showAbove(text, QRect(_pinBtn->mapToGlobal(QPoint(0, 0)), _pinBtn->size()));
        } else if (e->type() == QEvent::Leave) {
            _tooltip->hide();
        }
    }
    return QWidget::eventFilter(watched, e);
}

void TitleBar::togglePin() {
    _pinned = !_pinned;
    auto *w = window();
    Qt::WindowFlags flags = w->windowFlags();
    if (_pinned)
        flags |= Qt::WindowStaysOnTopHint;
    else
        flags &= ~Qt::WindowStaysOnTopHint;
    w->setWindowFlags(flags);
    w->show();
    updatePinButton();
}

void TitleBar::updatePinButton() {
    if (!_pinBtn) return;
    _pinBtn->setObjectName(_pinned ? "titleBarPinActive" : "titleBarPin");
    _pinBtn->style()->unpolish(_pinBtn);
    _pinBtn->style()->polish(_pinBtn);
}
