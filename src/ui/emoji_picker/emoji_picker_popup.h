// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/virtual_list/virtual_list_widget.h"

#include <QFont>
#include <QFrame>
#include <QString>
#include <QVector>

class QLineEdit;
class QWidget;
class Session;
class ImageCache;

// Virtual emoji grid — zero-widget, custom-painted scroll area (same approach as
// the message list, taken from Telegram desktop). Only the rows currently in the
// viewport are painted, and only visible custom-emoji images are pulled from the
// shared ImageCache, so a workspace with thousands of custom emojis opens instantly
// instead of materialising thousands of QToolButtons and firing thousands of
// downloads up front.
class EmojiGrid : public VirtualListWidget {
    Q_OBJECT
public:
    explicit EmojiGrid(QWidget *parent = nullptr);

    // Shared in-memory pixmap cache (disk-backed) for custom-emoji images.
    void setImageCache(ImageCache *cache);

    struct Cell {
        QString name;     // short code without colons (what we emit on activate)
        QString glyph;    // Unicode glyph, empty for custom-image emoji
        QString imageUrl; // custom-emoji image URL, empty for Unicode emoji
    };
    // Replace the displayed cells (called on every filter change — cheap, no widgets).
    void setCells(QVector<Cell> cells);

    int count() const { return static_cast<int>(_cells.size()); }
    int columns() const { return kCols; }
    int contentHeight() const; // full painted height in logical px

    int  selected() const { return _sel; }
    void setSelected(int idx);     // highlight + scroll the idx-th cell into view
    void moveSelection(int delta); // move selection by delta cells (clamped)
    void activateSelected();       // emit emojiActivated for the current selection

signals:
    void emojiActivated(const QString &name);

protected:
    void doPaint(QPaintEvent *event) override;
    void doMousePress(QMouseEvent *event) override;
    void doMouseMove(QMouseEvent *event) override;
    void doMouseRelease(QMouseEvent *event) override;
    void doMouseLeave() override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int kCols    = 8;  // emoji per row
    static constexpr int kCell    = 32; // cell side (logical px)
    static constexpr int kSpacing = 2;  // gap between cells
    static constexpr int kMargin  = 2;  // grid edge margin
    static constexpr int kRowH    = kCell + kSpacing;

    int   rowCount() const;
    QRect cellRect(int idx) const;        // content coords (scroll not applied)
    int   cellAt(const QPoint &vp) const; // viewport coords → cell index, or -1
    void  updateScrollRange();
    void  ensureVisible(int idx);

    ImageCache   *_imgCache = nullptr;
    QVector<Cell> _cells;
    int           _sel   = -1; // keyboard selection
    int           _hover = -1; // mouse hover
    QFont         _emojiFont;
};

// Floating emoji picker popup — reusable across the whole UI.
// Mount it once (parent = any widget) and call open() whenever you need it.
// Auto-dismisses on outside click (Qt::Popup window flag).
//
// Usage:
//   auto *picker = new EmojiPickerPopup(this);
//   connect(picker, &EmojiPickerPopup::emojiSelected,
//           this, [](const QString &name) { /* insert :name: */ });
//   picker->open(globalPos);
class EmojiPickerPopup : public QFrame {
    Q_OBJECT
public:
    explicit EmojiPickerPopup(QWidget *parent = nullptr);

    // Provide custom emoji from a Session (name → URL or "alias:name").
    // Call whenever a new session becomes available; pass nullptr to clear.
    void setSession(Session *session);

    // Shared image cache for custom-emoji thumbnails — forwarded to the grid.
    void setImageCache(ImageCache *cache);

    // Show the picker at globalPos and focus the search field.
    void open(const QPoint &globalPos);

signals:
    // Emitted when the user clicks an emoji. name is the short code without colons,
    // e.g. "thumbsup". Callers wrap it as ":name:" if needed.
    void emojiSelected(const QString &name);

protected:
    // Routes arrow/enter/escape keys from the search field into grid navigation.
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void rebuild(const QString &filter = {});
    void applyTheme();

    QLineEdit *_search  = nullptr;
    EmojiGrid *_grid    = nullptr;
    Session   *_session = nullptr;
};
