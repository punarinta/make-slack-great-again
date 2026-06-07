// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QPoint>
#include <QWidget>

class QAbstractButton;
class PopupTooltip;

// The formatting toolbar row of a Slack-style composer.
// Emits typed signals for each action; never touches the text editor directly.
class FormattingToolbar : public QWidget {
    Q_OBJECT
public:
    explicit FormattingToolbar(QWidget *parent = nullptr);

    // Call when the composer gains or loses focus to recolor the toolbar icons.
    void recolor(const QColor &color);

signals:
    void boldClicked();
    void italicClicked();
    void underlineClicked();
    void strikeClicked();
    void inlineCodeClicked();
    void codeBlockClicked();
    void orderedListClicked();
    void bulletListClicked();
    void blockquoteClicked();
    // Global screen position for placing the link dialog popup.
    void linkClicked(QPoint globalPos);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void applyTheme();

    QList<QPair<QAbstractButton *, QString>> _iconBtns;
    QHash<QWidget *, QString>                _tooltipBtns;
    PopupTooltip                            *_tooltip = nullptr;
};
