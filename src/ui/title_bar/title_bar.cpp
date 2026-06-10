// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "title_bar.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QWindow>

static constexpr QSize kBtnIconSize{12, 12};

TitleBar::TitleBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(22);
    setObjectName("titleBar");
    setAttribute(Qt::WA_StyledBackground);

    _tooltip = new PopupTooltip(this);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addStretch(1);

    auto makeBtn = [&](const QString &svgPath, const char *name) {
        auto *btn = new QPushButton(this);
        btn->setObjectName(name);
        btn->setFixedSize(40, 22);
        btn->setFlat(true);
        btn->setCursor(Qt::ArrowCursor);
        btn->setIconSize(kBtnIconSize);
        btn->setIcon(svgIcon(svgPath, kBtnIconSize, Th::c().titleBar.controlDefault));
        return btn;
    };

    if (QGuiApplication::platformName() != "wayland") {
        _pinBtn = makeBtn(":/ui/pin-off.svg", "titleBarPin");
        _pinBtn->installEventFilter(this);
        connect(_pinBtn, &QPushButton::clicked, this, [this] { togglePin(); });
        layout->addWidget(_pinBtn);
    }

    _minBtn = makeBtn(":/ui/wc-minimize.svg", "titleBarMin");
    _minBtn->installEventFilter(this);
    connect(_minBtn, &QPushButton::clicked, this, [this] { window()->showMinimized(); });
    layout->addWidget(_minBtn);

    _maxBtn = makeBtn(":/ui/wc-maximize.svg", "titleBarMax");
    _maxBtn->installEventFilter(this);
    connect(_maxBtn, &QPushButton::clicked, this, [this] {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
    });
    layout->addWidget(_maxBtn);

    _closeBtn = makeBtn(":/ui/wc-close.svg", "titleBarClose");
    _closeBtn->installEventFilter(this);
    connect(_closeBtn, &QPushButton::clicked, this, [this] { window()->close(); });
    layout->addWidget(_closeBtn);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &TitleBar::applyTheme);
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&TitleBar::update)
    );
}

void TitleBar::setTitle(const QString &) {}

void TitleBar::applyTheme() {
    setStyleSheet(
        QString("QWidget#titleBar { background: %1; }").arg(Th::qss(Th::c().titleBar.bg))
    );
    _minBtn->setIcon(
        svgIcon(":/ui/wc-minimize.svg", kBtnIconSize, Th::c().titleBar.controlDefault)
    );
    updateMaxButton();
    updatePinButton();
    _closeBtn->setIcon(svgIcon(":/ui/wc-close.svg", kBtnIconSize, Th::c().titleBar.controlDefault));
    _closeBtn->setStyleSheet(QString(
                                 "QPushButton#titleBarClose:hover { background-color: %1; "
                                 "border-top-right-radius: 8px; }"
    )
                                 .arg(Th::qss(Th::c().titleBar.controlClose)));
}

void TitleBar::updateMaxButton() {
    if (!_maxBtn || !window())
        return;
    const QString svg = window()->isMaximized() ? ":/ui/wc-restore.svg" : ":/ui/wc-maximize.svg";
    _maxBtn->setIcon(svgIcon(svg, kBtnIconSize, Th::c().titleBar.controlDefault));
}

void TitleBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        if (QGuiApplication::platformName() == "wayland") {
            if (auto *h = window()->windowHandle()) {
                _systemMovePending = true;
                h->startSystemMove();
            }
        } else {
            _dragging   = true;
            _dragOffset = e->globalPosition().toPoint() - window()->pos();
        }
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void TitleBar::mouseMoveEvent(QMouseEvent *e) {
    if (_dragging) {
        window()->move(e->globalPosition().toPoint() - _dragOffset);
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && (_dragging || _systemMovePending)) {
        _dragging          = false;
        _systemMovePending = false;
        QTimer::singleShot(0, this, [this]() { refreshHoverState(); });
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

// After any drag (system or manual) the widget under the cursor needs a nudge:
//
// X11: Qt's XCB plugin ignores EnterNotify(NotifyUngrab), so hover state is
//      stale after the implicit grab ends. XWarpPointer always emits a real
//      MotionNotify (even to the same position) which travels the full
//      spontaneous event pipeline including dispatchEnterLeave.
//
// Wayland: QCursor::setPos() is a no-op. Instead, inject a non-spontaneous
//      QEnterEvent + MouseMove directly into the widget under the cursor.
//      QWidget::event() sets WA_UnderMouse from Enter regardless of
//      spontaneity, which repaints CSS :hover. The MouseMove updates any
//      custom-painted hover state (e.g. hovered row in conv/message lists).
//
// macOS / Windows: QCursor::setPos() works, same as X11.
void TitleBar::refreshHoverState() {
    if (QGuiApplication::platformName() == "wayland") {
        const QPoint gp = QCursor::pos();
        const QPoint lp = window()->mapFromGlobal(gp);
        QWidget     *w  = window()->childAt(lp);
        if (!w)
            return;
        const QPointF clp = w->mapFromGlobal(gp).toPointF();
        const QPointF fgp = gp.toPointF();
        QEnterEvent   enter(clp, lp.toPointF(), fgp);
        QApplication::sendEvent(w, &enter);
        QMouseEvent move(QEvent::MouseMove, clp, fgp, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(w, &move);
    } else {
        const QPoint p = QCursor::pos();
        QCursor::setPos(p.x(), p.y());
    }
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TitleBar::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (auto *h = window()->windowHandle(); h && !_windowConnected) {
        _windowConnected = true;
        connect(h, &QWindow::windowStateChanged, this, [this](Qt::WindowState) {
            updateMaxButton();
        });
    }
    updateMaxButton();
}

bool TitleBar::eventFilter(QObject *watched, QEvent *e) {
    if (watched == _minBtn) {
        if (e->type() == QEvent::Enter)
            _minBtn->setIcon(
                svgIcon(":/ui/wc-minimize.svg", kBtnIconSize, Th::c().titleBar.controlHover)
            );
        else if (e->type() == QEvent::Leave)
            _minBtn->setIcon(
                svgIcon(":/ui/wc-minimize.svg", kBtnIconSize, Th::c().titleBar.controlDefault)
            );
    }
    if (watched == _maxBtn) {
        if (e->type() == QEvent::Enter) {
            const QString svg =
                window()->isMaximized() ? ":/ui/wc-restore.svg" : ":/ui/wc-maximize.svg";
            _maxBtn->setIcon(svgIcon(svg, kBtnIconSize, Th::c().titleBar.controlHover));
        } else if (e->type() == QEvent::Leave)
            updateMaxButton();
    }
    if (watched == _pinBtn) {
        if (e->type() == QEvent::Enter) {
            const QString text = _pinned ? tr("Unpin window") : tr("Pin window on top");
            _tooltip->showAbove(text, QRect(_pinBtn->mapToGlobal(QPoint(0, 0)), _pinBtn->size()));
            const QString svg = _pinned ? ":/ui/pin.svg" : ":/ui/pin-off.svg";
            _pinBtn->setIcon(svgIcon(svg, kBtnIconSize, Th::c().titleBar.controlHover));
        } else if (e->type() == QEvent::Leave) {
            _tooltip->hide();
            updatePinButton();
        }
    }
    if (watched == _closeBtn) {
        if (e->type() == QEvent::Enter)
            _closeBtn->setIcon(
                svgIcon(":/ui/wc-close.svg", kBtnIconSize, Th::c().titleBar.controlHover)
            );
        else if (e->type() == QEvent::Leave)
            _closeBtn->setIcon(
                svgIcon(":/ui/wc-close.svg", kBtnIconSize, Th::c().titleBar.controlDefault)
            );
    }
    return QWidget::eventFilter(watched, e);
}

void TitleBar::togglePin() {
    _pinned               = !_pinned;
    auto           *w     = window();
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
    if (!_pinBtn)
        return;
    if (_pinned) {
        _pinBtn->setIcon(svgIcon(":/ui/pin.svg", kBtnIconSize, Th::c().titleBar.controlClose));
        _pinBtn->setObjectName("titleBarPinActive");
    } else {
        _pinBtn->setIcon(
            svgIcon(":/ui/pin-off.svg", kBtnIconSize, Th::c().titleBar.controlDefault)
        );
        _pinBtn->setObjectName("titleBarPin");
    }
    _pinBtn->style()->unpolish(_pinBtn);
    _pinBtn->style()->polish(_pinBtn);
}
