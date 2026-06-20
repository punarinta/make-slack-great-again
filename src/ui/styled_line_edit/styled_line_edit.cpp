// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "styled_line_edit.h"
#include "ui/control_metrics.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>

StyledLineEdit::StyledLineEdit(QWidget *parent) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);
    setCursor(Qt::IBeamCursor);
    setFixedHeight(Ui::kControlHeight);

    _layout        = new QHBoxLayout(this);
    const auto &sp = Th::c().spacing;
    _layout->setContentsMargins(sp.lg, 0, sp.lg, 0);
    _layout->setSpacing(sp.md);

    _leadingIcon = new QLabel(this);
    _leadingIcon->setAlignment(Qt::AlignCenter);
    _leadingIcon->hide();

    _prefixLabel = new QLabel(this);
    _prefixLabel->hide();

    _edit = new QLineEdit(this);
    _edit->setFrame(false);
    _edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    _edit->installEventFilter(this);

    _counterLabel = new QLabel(this);
    _counterLabel->hide();

    _layout->addWidget(_leadingIcon);
    _layout->addWidget(_prefixLabel);
    _layout->addWidget(_edit, 1);
    _layout->addWidget(_counterLabel);

    connect(_edit, &QLineEdit::textChanged, this, [this](const QString &t) {
        updateCounter();
        emit textChanged(t);
    });
    connect(_edit, &QLineEdit::returnPressed, this, &StyledLineEdit::returnPressed);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void StyledLineEdit::setSize(Size size) {
    setFixedHeight(size == Size::Small ? Ui::kControlHeightSmall : Ui::kControlHeight);
}

void StyledLineEdit::setPrefix(const QString &text) {
    _prefixLabel->setText(text);
    _prefixLabel->setVisible(!text.isEmpty());
}

void StyledLineEdit::setLeadingIcon(const QString &svgPath, const QSize &size) {
    _leadingIconPath = svgPath;
    _leadingIconSize = size;
    _leadingIcon->setVisible(!svgPath.isEmpty());
    if (!svgPath.isEmpty())
        _leadingIcon->setFixedSize(size);
    applyTheme(); // re-render the glyph in the current theme tint
}

void StyledLineEdit::setMaxLength(int max) {
    _maxLength = max;
    if (max > 0) {
        _edit->setMaxLength(max);
        _counterLabel->show();
        updateCounter();
    } else {
        _counterLabel->hide();
    }
}

void StyledLineEdit::setPlaceholderText(const QString &text) {
    _edit->setPlaceholderText(text);
}

QString StyledLineEdit::text() const {
    return _edit->text();
}

void StyledLineEdit::setText(const QString &text) {
    _edit->setText(text);
}

void StyledLineEdit::clear() {
    _edit->clear();
}

bool StyledLineEdit::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _edit) {
        if (event->type() == QEvent::FocusIn) {
            _focused = true;
            updateBorderStyle(true);
        } else if (event->type() == QEvent::FocusOut) {
            _focused = false;
            updateBorderStyle(false);
        }
    }
    return QFrame::eventFilter(obj, event);
}

void StyledLineEdit::mousePressEvent(QMouseEvent *event) {
    _edit->setFocus();
    QFrame::mousePressEvent(event);
}

void StyledLineEdit::applyTheme() {
    updateBorderStyle(_focused);
    if (_leadingIcon && !_leadingIconPath.isEmpty())
        _leadingIcon->setPixmap(svgPixmap(_leadingIconPath, _leadingIconSize, Th::c().icon.def));
    if (_prefixLabel)
        _prefixLabel->setStyleSheet(QString("color: %1; font-size: %2px;")
                                        .arg(Th::qss(Th::c().text.tertiary))
                                        .arg(Th::c().fonts.base));
    if (_counterLabel)
        _counterLabel->setStyleSheet(QString("color: %1; font-size: %2px;")
                                         .arg(Th::qss(Th::c().text.tertiary))
                                         .arg(Th::c().fonts.sm));
    if (_edit)
        _edit->setStyleSheet(
            QString(
                "QLineEdit { background: transparent; border: none; color: %1; font-size: %2px; }"
            )
                .arg(Th::qss(Th::c().text.primary))
                .arg(Th::c().fonts.base)
        );
}

void StyledLineEdit::updateCounter() {
    if (_maxLength > 0 && _counterLabel)
        _counterLabel->setText(QString::number(_maxLength - _edit->text().length()));
}

void StyledLineEdit::updateBorderStyle(bool focused) {
    const QColor border = focused ? Th::c().composer.borderFocus : Th::c().composer.border;
    const int    bw     = focused ? 2 : 1;
    setStyleSheet(QString(
                      "StyledLineEdit {"
                      "  border: %1px solid %2;"
                      "  border-radius: %3px;"
                      "  background: %4;"
                      "}"
    )
                      .arg(bw)
                      .arg(Th::qss(border))
                      .arg(Ui::kControlRadius)
                      .arg(Th::qss(Th::c().surface.raised)));
}
