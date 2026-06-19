// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QDialog>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;
class IconButton;

// Base for all application modal dialogs.
//
// Renders as a frameless overlay that:
//  • Covers the parent window with a semi-transparent dark backdrop.
//  • Shows a white rounded card centred on that backdrop (drop shadow).
//  • Puts a bold title + × close button in the card header (Standard chrome).
//  • Dismisses on a backdrop click; suppresses OS window chrome.
//
// Subclasses add their widgets to contentLayout() and call updateCard() after
// construction so the card centres correctly.
//
// Dialogs that need their own chrome (e.g. a tab bar instead of a title) use the
// protected Chrome::Custom constructor: no title header is built and the content
// layout fills the card edge-to-edge (zero padding) — the subclass supplies
// everything itself.
class AppDialog : public QDialog {
    Q_OBJECT
public:
    explicit AppDialog(const QString &title, QWidget *parent = nullptr);

    // Returns the VBox inside the card where subclasses add content.
    QVBoxLayout *contentLayout() const { return _contentLayout; }

    // Re-sizes and re-centres the card.  Call once at the end of
    // a subclass constructor after all content has been added.
    void updateCard();

    // Builds the standard footer button row and appends it to contentLayout():
    //   [leadingExtra]  →stretch→  [secondary] [primary]   (md gaps)
    // `secondary` (typically Cancel) is wired to reject(). `primary`'s click is
    // left to the caller (it varies: accept / custom navigation / etc.).
    // `leadingExtra` (optional) sits flush-left before the stretch.
    QHBoxLayout *addButtonRow(
        QPushButton *primary, QPushButton *secondary = nullptr, QWidget *leadingExtra = nullptr
    );

protected:
    enum class Chrome { Standard, Custom };
    // Custom-chrome ctor for subclasses that build their own header.
    explicit AppDialog(QWidget *parent, Chrome chrome);

    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

    virtual void applyTheme();

    // Card width given the available overlay width; default clamps to 480..560.
    // Override for a wider/fixed card (e.g. the browse dialog at 720).
    virtual int cardWidth(int availOverlayWidth) const;
    // Minimum card height used when clamping to the overlay.
    virtual int minCardHeight() const { return 200; }

    QFrame      *card() const { return _card; }
    QVBoxLayout *cardLayout() const { return _cardLayout; }

private:
    void buildCard(bool standardHeader, const QString &title);

    QFrame      *_card          = nullptr;
    QLabel      *_titleLabel    = nullptr;
    IconButton  *_closeBtn      = nullptr;
    QVBoxLayout *_cardLayout    = nullptr;
    QVBoxLayout *_contentLayout = nullptr;
};
