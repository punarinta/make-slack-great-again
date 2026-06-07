// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QDialog>

class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

// Base for all application modal dialogs.
//
// Renders as a frameless overlay that:
//  • Covers the parent window with a semi-transparent dark backdrop.
//  • Shows a white rounded card centred on that backdrop.
//  • Puts a bold title + × close button in the card header.
//  • Suppresses OS window chrome (no minimise / maximise buttons).
//
// Subclasses add their widgets to contentLayout() and call
// updateCard() after construction so the card centres correctly.
class AppDialog : public QDialog {
    Q_OBJECT
public:
    explicit AppDialog(const QString &title, QWidget *parent = nullptr);

    // Returns the VBox inside the card where subclasses add content.
    QVBoxLayout *contentLayout() const { return _contentLayout; }

    // Re-sizes and re-centres the card.  Call once at the end of
    // a subclass constructor after all content has been added.
    void updateCard();

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

    virtual void applyTheme();

private:
    QFrame      *_card;
    QLabel      *_titleLabel;
    QPushButton *_closeBtn;
    QVBoxLayout *_contentLayout;
};
