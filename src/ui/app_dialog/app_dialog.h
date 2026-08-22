// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QDialog> // for QDialog::Accepted / Rejected enum values used by callers
#include <QWidget>

class QEventLoop;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class IconButton;

// Base for all application modal dialogs.
//
// Renders as an in-window overlay (a child widget of the parent's top-level
// window, NOT a separate OS window) that:
//  • Covers the parent window with a semi-transparent dark backdrop.
//  • Shows a white rounded card centred on that backdrop (drop shadow).
//  • Puts a bold title + × close button in the card header (Standard chrome).
//  • Dismisses on a backdrop click / Escape; behaves modally while shown.
//
// Why an in-window child and not a top-level QDialog: on Wayland (and XWayland)
// the compositor — not the client — decides where a top-level window goes, so a
// frameless backdrop window cannot be pinned over its parent and ends up
// offset. Painting the backdrop inside the parent's own surface, in client
// coordinates, is the only reliable approach (the same reason PopupTooltip, the
// search bar and the context menu are all in-window overlays here).
//
// The QDialog-style API (exec()/open()/accept()/reject()/done() + the
// accepted()/rejected()/finished() signals) is reimplemented so existing call
// sites are unchanged. QDialog::Accepted / QDialog::Rejected remain the result
// codes.
//
// Subclasses add their widgets to contentLayout() and call updateCard() after
// construction so the card centres correctly.
//
// Dialogs that need their own chrome (e.g. a tab bar instead of a title) use the
// protected Chrome::Custom constructor: no title header is built and the content
// layout fills the card edge-to-edge (zero padding) — the subclass supplies
// everything itself.
class AppDialog : public QWidget {
    Q_OBJECT
public:
    // Whether the card's content is wrapped in a scroll area (the default) so a
    // card too tall for the window scrolls rather than clipping. Dialogs that
    // already scroll their own body — and size the card around it — pass
    // Disabled to avoid nesting one scroll area inside another.
    enum class Scroll { Enabled, Disabled };

    explicit AppDialog(
        const QString &title, QWidget *parent = nullptr, Scroll scroll = Scroll::Enabled
    );

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

    // QDialog-compatible modal API.
    int  exec(); // blocks in a nested event loop; returns the result code
    void open(); // shows without blocking

    // The frontmost dialog currently overlaying `window`, or nullptr if none is
    // up. Lets the window answer a "close" request (Cmd+W) by dismissing the
    // dialog on top of it instead of hiding itself out from under it — the
    // dialogs are in-window children, so they are invisible to the window
    // system's own close handling.
    static AppDialog *topmostVisible(QWidget *window);

public slots:
    void accept(); // done(QDialog::Accepted)
    void reject(); // done(QDialog::Rejected)
    void done(int result);

signals:
    void accepted();
    void rejected();
    void finished(int result);

protected:
    enum class Chrome { Standard, Custom };
    // Custom-chrome ctor for subclasses that build their own header.
    explicit AppDialog(QWidget *parent, Chrome chrome, Scroll scroll = Scroll::Enabled);

    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    virtual void applyTheme();

    // Card width given the available overlay width; default clamps to 480..560.
    // Override for a wider/fixed card (e.g. the browse dialog at 720).
    virtual int cardWidth(int availOverlayWidth) const;
    // Minimum card height used when clamping to the overlay.
    virtual int minCardHeight() const { return 200; }

    QFrame      *card() const { return _card; }
    QVBoxLayout *cardLayout() const { return _cardLayout; }

private:
    void buildCard(bool standardHeader, const QString &title, Scroll scroll);
    // Re-cover the parent window's client rect and re-centre the card.
    void coverParent();

    QFrame      *_card          = nullptr;
    QLabel      *_titleLabel    = nullptr;
    IconButton  *_closeBtn      = nullptr;
    QVBoxLayout *_cardLayout    = nullptr;
    QVBoxLayout *_contentLayout = nullptr;
    // With Scroll::Enabled, contentLayout() lives on _contentHost inside _scroll,
    // so a card the window is too short to show in full scrolls instead of
    // clipping. Both stay null with Scroll::Disabled.
    QScrollArea *_scroll        = nullptr;
    QWidget     *_contentHost   = nullptr;

    QEventLoop *_loop   = nullptr;
    int         _result = QDialog::Rejected;
};
