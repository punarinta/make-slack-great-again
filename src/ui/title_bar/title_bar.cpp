// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "title_bar.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#ifdef Q_OS_MACOS
#include "mac_title_bar.h"
#endif

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QWindow>

static constexpr QSize kBtnIconSize{12, 12};

TitleBar::TitleBar(QWidget *parent) : QWidget(parent) {
#ifdef Q_OS_MACOS
    setFixedHeight(52);
#else
    setFixedHeight(22);
#endif
    setObjectName("titleBar");
    setAttribute(Qt::WA_StyledBackground);

#ifdef Q_OS_MACOS
    _contentLayout = new QStackedLayout(this);
    _contentLayout->setContentsMargins(0, 0, 0, 0);
    _contentLayout->setStackingMode(QStackedLayout::StackAll);
    // Leave the native traffic lights clear; symmetric margins keep the
    // fallback workspace title centered in the whole window.
    _titleLabel = new QLabel(this);
    _titleLabel->setContentsMargins(112, 0, 112, 0);
    _titleLabel->setMinimumWidth(0);
    _titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _titleLabel->setAlignment(Qt::AlignCenter);
    _titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    _contentLayout->addWidget(_titleLabel);
#else
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
#endif

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &TitleBar::applyTheme);
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&TitleBar::update)
    );
}

void TitleBar::setTitle(const QString &title) {
    if (_titleLabel)
        _titleLabel->setText(title.isEmpty() ? tr("msga") : title);
}

void TitleBar::setContent(QWidget *content) {
    if (!_contentLayout || !content)
        return;
    // The fallback title remains visible whenever the conversation header hides.
    _contentLayout->addWidget(content);
    _contentLayout->setCurrentWidget(content);
}

void TitleBar::contextMenuEvent(QContextMenuEvent *e) {
#ifdef Q_OS_MACOS
    // Keep the existing always-on-top feature available without adding a
    // fourth control beside macOS's native traffic lights.
    QMenu menu(this);
    auto *pin = menu.addAction(tr("Pin window on top"));
    pin->setCheckable(true);
    pin->setChecked(_pinned);
    if (menu.exec(e->globalPos()) == pin)
        togglePin();
#else
    QWidget::contextMenuEvent(e);
#endif
}

void TitleBar::applyTheme() {
#ifdef Q_OS_MACOS
    Th::setStyleSheetIfChanged(
        this,
        QString("QWidget#titleBar { background: %1; border-bottom: 1px solid %2; }")
            .arg(Th::qss(Th::c().surface.content), Th::qss(Th::c().divider.subtle))
    );
    Th::setStyleSheetIfChanged(
        _titleLabel,
        QString("color: %1; font-size: %2px; font-weight: 600;")
            .arg(Th::qss(Th::c().text.primary))
            .arg(Th::c().fonts.xl)
    );
    if (window()->windowHandle())
        configureMacTitleBar(window());
#else
    Th::setStyleSheetIfChanged(
        this, QString("QWidget#titleBar { background: %1; }").arg(Th::qss(Th::c().titleBar.bg))
    );
    _minBtn->setIcon(
        svgIcon(":/ui/wc-minimize.svg", kBtnIconSize, Th::c().titleBar.controlDefault)
    );
    updateMaxButton();
    updatePinButton();
    _closeBtn->setIcon(svgIcon(":/ui/wc-close.svg", kBtnIconSize, Th::c().titleBar.controlDefault));
    Th::setStyleSheetIfChanged(
        _closeBtn,
        QString(
            "QPushButton#titleBarClose:hover { background-color: %1; "
            "border-top-right-radius: 8px; }"
        )
            .arg(Th::qss(Th::c().titleBar.controlClose))
    );
#endif
}

void TitleBar::updateMaxButton() {
    if (!_maxBtn || !window())
        return;
    const QString svg = window()->isMaximized() ? ":/ui/wc-restore.svg" : ":/ui/wc-maximize.svg";
    _maxBtn->setIcon(svgIcon(svg, kBtnIconSize, Th::c().titleBar.controlDefault));
}

void TitleBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
#ifdef Q_OS_MACOS
        if (auto *h = window()->windowHandle())
            h->startSystemMove();
#else
        if (QGuiApplication::platformName() == "wayland") {
            if (auto *h = window()->windowHandle()) {
                _systemMovePending = true;
                h->startSystemMove();
            }
        } else {
            _dragging   = true;
            _dragOffset = e->globalPosition().toPoint() - window()->pos();
        }
#endif
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
#ifdef Q_OS_MACOS
        // AppKit owns the frame here: honour the system "Double-click a
        // window's title bar to" preference instead of always zooming.
        performMacTitleBarDoubleClick(window());
#else
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
#endif
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TitleBar::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
#ifdef Q_OS_MACOS
    configureMacTitleBar(window());
#endif
    connectWindowHandle();
    updateMaxButton();
}

void TitleBar::connectWindowHandle() {
    auto *h = window()->windowHandle();
    if (!h || _windowConnected)
        return;
    _windowConnected = true;
    connect(h, &QWindow::windowStateChanged, this, [this](Qt::WindowState) {
#ifdef Q_OS_MACOS
        configureMacTitleBar(window());
#endif
        updateMaxButton();
    });
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
    // setWindowFlags() destroys and recreates the top-level QWindow, taking the
    // windowStateChanged connection with it. Reconnect to the fresh handle.
    _windowConnected = false;
    w->setWindowFlags(flags);
    w->show();
    connectWindowHandle();
    updateMaxButton();
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
