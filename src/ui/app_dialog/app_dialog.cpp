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
#include <QScrollArea>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>

static constexpr int kCardMinW = 480;
static constexpr int kCardMaxW = 560;
static constexpr int kCardPadH = 28; // left / right padding inside card
static constexpr int kCardPadT = 24; // top padding
static constexpr int kCardPadB = 24; // bottom padding

// Scroll host for the card content. QScrollArea's own sizeHint is capped at
// 36x24 character cells and it never forwards height-for-width, so the card
// would size itself from a truncated, unwrapped guess — exactly the two things
// updateCard() works hard to get right. Report the real content metrics instead;
// the scrolling then only ever engages once updateCard() has had to clamp the
// card to a window too short to show it whole.
class CardScrollArea final : public QScrollArea {
public:
    explicit CardScrollArea(QWidget *parent) : QScrollArea(parent) {}

    QSize sizeHint() const override {
        if (const QWidget *w = widget())
            return w->sizeHint();
        return QScrollArea::sizeHint();
    }
    QSize minimumSizeHint() const override { return QSize(0, 0); }
    bool  hasHeightForWidth() const override { return widget() && widget()->hasHeightForWidth(); }
    int   heightForWidth(int w) const override {
        return widget() ? widget()->heightForWidth(w) : QScrollArea::heightForWidth(w);
    }
};

// Resolve the top-level window we should overlay (and parent ourselves to).
static QWidget *overlayHost(QWidget *parent) {
    return parent ? parent->window() : nullptr;
}

AppDialog::AppDialog(const QString &title, QWidget *parent, Scroll scroll)
    : QWidget(overlayHost(parent)) {
    buildCard(/*standardHeader=*/true, title, scroll);
}

AppDialog::AppDialog(QWidget *parent, Chrome chrome, Scroll scroll) : QWidget(overlayHost(parent)) {
    buildCard(/*standardHeader=*/chrome == Chrome::Standard, QString(), scroll);
}

void AppDialog::buildCard(bool standardHeader, const QString &title, Scroll scroll) {
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

    // Unless the subclass scrolls its own body (Scroll::Disabled), content goes
    // inside a scroll area so a card the window is too short to show whole
    // scrolls instead of clipping its bottom — a real case now that the main
    // window shrinks itself to fit small screens. widgetResizable keeps the
    // no-overflow case pixel-identical: the host is stretched to the viewport,
    // so expanding children (lists, stretches) still fill the card.
    const auto buildContent = [this, scroll](int spacing) {
        if (scroll == Scroll::Disabled) {
            _contentLayout = new QVBoxLayout;
            _contentLayout->setContentsMargins(0, 0, 0, 0);
            _contentLayout->setSpacing(spacing);
            _cardLayout->addLayout(_contentLayout);
            return;
        }
        _contentHost   = new QWidget;
        _contentLayout = new QVBoxLayout(_contentHost);
        _contentLayout->setContentsMargins(0, 0, 0, 0);
        _contentLayout->setSpacing(spacing);

        _scroll = new CardScrollArea(_card);
        _scroll->setFrameShape(QFrame::NoFrame);
        _scroll->setWidgetResizable(true);
        _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        // Don't join the tab chain — the dialog's own controls own it. Wheel and
        // focus-follows scrolling work regardless.
        _scroll->setFocusPolicy(Qt::NoFocus);
        _scroll->viewport()->setAutoFillBackground(false);
        _scroll->setWidget(_contentHost);
        _contentHost->installEventFilter(this);
        _cardLayout->addWidget(_scroll, 1);
    };

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

        buildContent(sp.lg);
    } else {
        // Custom chrome: no header, content fills the card edge-to-edge.
        _cardLayout->setContentsMargins(0, 0, 0, 0);
        buildContent(0);
    }

    // Escape closes the dialog from anywhere inside it. keyPressEvent only sees
    // Escape when the overlay itself is focused; a focused child that swallows
    // the key (e.g. the read-only QTextBrowser preview) would otherwise eat it.
    // A WidgetWithChildren shortcut fires for any focused descendant.
    auto *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escShortcut, &QShortcut::activated, this, &AppDialog::reject);

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

AppDialog *AppDialog::topmostVisible(QWidget *window) {
    if (!window)
        return nullptr;
    // Every dialog parents itself to the host window (see overlayHost) and
    // raise()s on show, and raise() moves a child to the end of the parent's
    // child list — so among the visible ones the last in child order is the one
    // painted on top (e.g. the session-import dialog over the settings overlay).
    const auto dialogs = window->findChildren<AppDialog *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto it = dialogs.crbegin(); it != dialogs.crend(); ++it)
        if ((*it)->isVisible())
            return *it;
    return nullptr;
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
    _card->setStyleSheet(QString(
                             "QFrame#appDialogCard { background: %1; border-radius: 12px;"
                             " border: none; }"
    )
                             .arg(Th::qss(Th::c().surface.raised)));
    if (_titleLabel)
        _titleLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
    if (_scroll) {
        // Transparent all the way down so the card's rounded fill shows through
        // (the viewport is its own widget, hence the descendant selector).
        _scroll->setStyleSheet(
            QStringLiteral(
                "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"
                "QScrollArea { border: none; }"
            ) +
            Th::scrollBarQss()
        );
    }
    // _closeBtn (IconButton) self-themes.
}

// ── Layout ────────────────────────────────────────────────────────────────────

int AppDialog::cardWidth(int availOverlayWidth) const {
    // The floor gives way on a window too narrow to hold it — a card wider than
    // the window it sits in loses its own edges.
    const int minW = std::min(kCardMinW, std::max(availOverlayWidth, 1));
    return std::clamp(availOverlayWidth, minW, kCardMaxW);
}

void AppDialog::updateCard() {
    // Determine card width from the available overlay space (big fallback before
    // the overlay has a real size, so cardWidth() clamps to its own maximum).
    const int avail = width() > 0 ? width() - 80 : 2000;
    const int cardW = cardWidth(avail);
    _card->setFixedWidth(cardW);

    // Let Qt calculate the preferred height from the current content. sizeHint()
    // alone ignores height-for-width, so a column of word-wrapped labels reports
    // its UNWRAPPED (too-short) height and the card clips / overlaps its content
    // at larger fonts or fractional scaling. Prefer the layout's real
    // height-for-width at the fixed card width when the content provides it.
    // A QScrollArea does not react when the widget inside it changes size hint,
    // so the card's layout would still be holding the hints it cached before a
    // subclass revealed or swapped content. updateGeometry() invalidates that
    // cache up the parent chain synchronously — without it the card measures the
    // old content and scrolls when it should have grown.
    if (_scroll)
        _scroll->updateGeometry();
    _card->adjustSize();
    int wantH = _card->sizeHint().height();
    if (_card->hasHeightForWidth())
        wantH = std::max(wantH, _card->heightForWidth(cardW));
    // Same for the height floor: prefer minCardHeight() and the 80px breathing
    // room around the card, but never let either push the card past the window.
    // Content that no longer fits scrolls (see the CardScrollArea in buildCard).
    const int maxH  = std::max(std::min(minCardHeight(), height()), height() - 80);
    const int cardH = std::min(wantH, maxH);
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
    // Content grew or shrank on its own (an async status line, a swapped page):
    // pass that up through the scroll area, which otherwise hides it from the
    // card's layout. The card itself only resizes on the next updateCard().
    if (obj == _contentHost && event->type() == QEvent::LayoutRequest && _scroll)
        _scroll->updateGeometry();
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
