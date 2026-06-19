// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "formatting_toolbar.h"
#include "ui/icon_utils.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QHBoxLayout>
#include <QToolButton>
#include <QFrame>
#include <QEvent>

static constexpr QSize kToolIconSize{18, 18};

static QString sc(const char *keys) {
#ifdef Q_OS_MAC
    QString s = QString::fromLatin1(keys);
    s.replace("Ctrl+Alt+Shift+", "⌘⌥⇧");
    s.replace("Ctrl+Shift+", "⌘⇧");
    s.replace("Ctrl+", "⌘");
    return s;
#else
    return QString::fromLatin1(keys);
#endif
}

static QString tip(const QString &label, const char *shortcut = nullptr) {
    return shortcut ? label + " (" + sc(shortcut) + ")" : label;
}

static QFrame *makeVSep(QWidget *parent) {
    auto *sep = new QFrame(parent);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedSize(1, 16);
    sep->setStyleSheet(
        QString("QFrame { color: %1; }").arg(Th::qss(Th::c().composer.toolbarBorder))
    );
    return sep;
}

FormattingToolbar::FormattingToolbar(QWidget *parent) : QWidget(parent) {
    setObjectName("composerToolbar");
    setFixedHeight(32);
    _tooltip = new PopupTooltip(this);

    auto       *layout = new QHBoxLayout(this);
    const auto &sp     = Th::c().spacing;
    layout->setContentsMargins(sp.md, sp.xs, sp.md, sp.xs);
    layout->setSpacing(sp.sm);

    auto makeBtn = [&](const QString &svgPath, const QString &tooltipText) {
        auto *btn = new QToolButton(this);
        btn->setFixedSize(26, 26);
        btn->setIconSize(kToolIconSize);
        btn->setIcon(svgIcon(svgPath, kToolIconSize, Th::c().composer.toolbarIcon));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setAttribute(Qt::WA_Hover);
        btn->installEventFilter(this);
        _iconBtns.append({btn, svgPath});
        _tooltipBtns[btn] = tooltipText;
        return btn;
    };

    auto *boldBtn      = makeBtn(":/ui/bold.svg", tip(tr("Bold"), "Ctrl+B"));
    auto *italicBtn    = makeBtn(":/ui/italic.svg", tip(tr("Italic"), "Ctrl+I"));
    auto *underlineBtn = makeBtn(":/ui/underline.svg", tip(tr("Underline"), "Ctrl+U"));
    auto *strikeBtn = makeBtn(":/ui/strikethrough.svg", tip(tr("Strikethrough"), "Ctrl+Shift+X"));
    auto *linkBtn   = makeBtn(":/ui/link.svg", tip(tr("Link"), "Ctrl+Shift+U"));
    auto *olBtn     = makeBtn(":/ui/list-ordered.svg", tip(tr("Ordered list"), "Ctrl+Shift+7"));
    auto *ulBtn     = makeBtn(":/ui/list.svg", tip(tr("Bullet list"), "Ctrl+Shift+8"));
    auto *bqBtn     = makeBtn(":/ui/quote.svg", tip(tr("Blockquote"), "Ctrl+Shift+9"));
    auto *codeBtn   = makeBtn(":/ui/code.svg", tip(tr("Inline code"), "Ctrl+Shift+C"));
    auto *snipBtn   = makeBtn(":/ui/braces.svg", tip(tr("Code block"), "Ctrl+Alt+Shift+C"));

    layout->addWidget(boldBtn);
    layout->addWidget(italicBtn);
    layout->addWidget(underlineBtn);
    layout->addWidget(strikeBtn);
    layout->addWidget(makeVSep(this));
    layout->addWidget(linkBtn);
    layout->addWidget(olBtn);
    layout->addWidget(ulBtn);
    layout->addWidget(makeVSep(this));
    layout->addWidget(bqBtn);
    layout->addWidget(codeBtn);
    layout->addWidget(snipBtn);
    layout->addStretch();

    connect(boldBtn, &QToolButton::clicked, this, &FormattingToolbar::boldClicked);
    connect(italicBtn, &QToolButton::clicked, this, &FormattingToolbar::italicClicked);
    connect(underlineBtn, &QToolButton::clicked, this, &FormattingToolbar::underlineClicked);
    connect(strikeBtn, &QToolButton::clicked, this, &FormattingToolbar::strikeClicked);
    connect(codeBtn, &QToolButton::clicked, this, &FormattingToolbar::inlineCodeClicked);
    connect(snipBtn, &QToolButton::clicked, this, &FormattingToolbar::codeBlockClicked);
    connect(olBtn, &QToolButton::clicked, this, &FormattingToolbar::orderedListClicked);
    connect(ulBtn, &QToolButton::clicked, this, &FormattingToolbar::bulletListClicked);
    connect(bqBtn, &QToolButton::clicked, this, &FormattingToolbar::blockquoteClicked);
    connect(linkBtn, &QToolButton::clicked, this, [this, linkBtn] {
        emit linkClicked(linkBtn->mapToGlobal(QPoint(0, linkBtn->height() + 4)));
    });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void FormattingToolbar::applyTheme() {
    recolor(Th::c().composer.toolbarIcon);
}

void FormattingToolbar::recolor(const QColor &color) {
    for (auto &[btn, path] : _iconBtns)
        btn->setIcon(svgIcon(path, btn->iconSize(), color));
    setStyleSheet(
        QString(
            "QWidget#composerToolbar {"
            "  background: %1;"
            "  border-radius: 7px 7px 0 0;"
            "}"
            "QWidget#composerToolbar QToolButton {"
            "  border: none; border-radius: 3px;"
            "  background: transparent;"
            "}"
            "QWidget#composerToolbar QToolButton:hover   { background: %2; }"
            "QWidget#composerToolbar QToolButton:pressed { background: %2; }"
        )
            .arg(Th::qss(Th::c().composer.toolbarBg), Th::qss(Th::c().surface.highlightStrong))
    );
    (void)color; // stylesheet is static; only icons change with color
}

bool FormattingToolbar::eventFilter(QObject *obj, QEvent *event) {
    if (auto *w = qobject_cast<QWidget *>(obj); w && _tooltipBtns.contains(w)) {
        if (event->type() == QEvent::HoverEnter) {
            _tooltip->showAbove(_tooltipBtns[w], QRect(w->mapToGlobal(QPoint(0, 0)), w->size()));
        } else if (event->type() == QEvent::HoverLeave) {
            _tooltip->hide();
        }
    }
    return QWidget::eventFilter(obj, event);
}
