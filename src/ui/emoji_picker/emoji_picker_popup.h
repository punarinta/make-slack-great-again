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
class StyledLineEdit;

// Virtual, sectioned emoji grid — zero-widget, custom-painted scroll area (same
// approach as the message list, taken from Telegram desktop). Only the rows
// currently in the viewport are painted, and only visible custom-emoji images
// are pulled from the shared ImageCache, so a workspace with thousands of custom
// emojis opens instantly instead of materialising thousands of QToolButtons and
// firing thousands of downloads up front.
//
// Unlike a flat grid it lays content out as a sequence of sections, each with a
// sticky-free header row ("Frequently Used", "Smileys & People", …) followed by
// its emoji cells, so the picker mirrors Slack's browse experience.
class EmojiGrid : public VirtualListWidget {
    Q_OBJECT
public:
    explicit EmojiGrid(QWidget *parent = nullptr);

    // Shared in-memory pixmap cache (disk-backed) for custom-emoji images.
    void setImageCache(ImageCache *cache);

    struct Cell {
        QString name;              // base short code without colons / tone suffix
        QString glyph;             // Unicode glyph, empty for custom-image emoji
        QString imageUrl;          // custom-emoji image URL, empty for Unicode emoji
        bool    skinnable = false; // base supports a Fitzpatrick skin variation
    };
    struct Section {
        QString id;        // category id (matches the category-bar tab)
        QString label;     // header text shown above the cells
        int     firstCell; // index into the cell vector
        int     cellCount;
    };
    // Replace the displayed content (called on open / filter change — cheap, no
    // widgets). Rebuilds the virtual row layout.
    void setContent(QVector<Cell> cells, QVector<Section> sections);

    int columns() const { return kCols; }
    int contentHeight() const; // full painted height in logical px

    // Apply a global skin tone (0 = none/default, or 2..6 mapping to the Slack
    // skin-tone-2..6 modifiers). Repaints; affects which glyph skinnable cells
    // render and the suffix appended to the emitted name.
    void setSkinTone(int tone);

    int  selected() const { return _sel; }
    void setSelected(int idx);              // highlight + scroll the idx-th cell into view
    void moveSelection(int dCol, int dRow); // move selection by columns/rows (clamped)
    void activateSelected();                // emit emojiActivated for the current selection

    // Scroll so the given section's header sits at the top of the viewport.
    void scrollToSection(int sectionIdx);

signals:
    // Emitted when the user picks an emoji. name is the short code without colons,
    // with a "::skin-tone-N" suffix appended when a skin tone is active and the
    // emoji supports it (e.g. "wave::skin-tone-3"). Callers wrap as ":name:".
    void emojiActivated(const QString &name);
    // The section currently at the top of the viewport changed (drives the
    // category-bar active highlight). -1 when empty.
    void topSectionChanged(int sectionIdx);

protected:
    void doPaint(QPaintEvent *event) override;
    void doMousePress(QMouseEvent *event) override;
    void doMouseMove(QMouseEvent *event) override;
    void doMouseRelease(QMouseEvent *event) override;
    void doMouseLeave() override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int kCols    = 9;  // emoji per row
    static constexpr int kCell    = 36; // cell side (logical px)
    static constexpr int kGlyphPx = 22; // Unicode emoji render size inside a cell
    static constexpr int kMargin  = 6;  // grid edge margin
    static constexpr int kHeaderH = 30; // section header row height

    // One laid-out visual row: either a section header or a row of cells.
    struct Row {
        int     y;         // top in content coords
        int     h;         // row height
        bool    header;    // true => section header
        QString label;     // header text (header rows only)
        int     section;   // owning section index
        int     cellStart; // first cell index (emoji rows only)
        int     cellCount; // cells in this row (emoji rows only)
    };

    void    relayout();
    QRect   cellRect(int idx) const;        // content coords (scroll not applied)
    int     cellAt(const QPoint &vp) const; // viewport coords → cell index, or -1
    int     rowOfCell(int idx) const;       // visual row index containing the cell
    void    updateScrollRange();
    void    ensureVisible(int idx);
    void    emitTopSection();
    QString emittedName(const Cell &c) const;  // base name + tone suffix
    QString displayGlyph(const Cell &c) const; // base glyph + tone modifier

    ImageCache      *_imgCache = nullptr;
    QVector<Cell>    _cells;
    QVector<Section> _sections;
    QVector<Row>     _rows;
    int              _contentH = 0;
    int              _skinTone = 0;    // 0 = default, else 2..6
    QString          _toneGlyph;       // cached modifier glyph for _skinTone
    int              _sel        = -1; // keyboard selection
    int              _hover      = -1; // mouse hover
    int              _topSection = -1;
};

// Floating emoji picker popup — reusable across the whole UI.
// Mount it once (parent = any widget) and call open() whenever you need it.
// Auto-dismisses on outside click (Qt::Popup window flag).
//
// Slack-style layout: a category icon bar, a prominent search field, a sectioned
// virtual emoji grid, and a skin-tone selector. The grid is built from a cached
// static catalog (categories()) plus the live "Frequently Used" and workspace
// "Custom" sections, so open() is instant even on huge workspaces.
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
    // Emitted when the user clicks an emoji. name is the short code without colons
    // (with a "::skin-tone-N" suffix when applicable). Callers wrap it as ":name:".
    void emojiSelected(const QString &name);

protected:
    // Routes arrow/enter/escape keys from the search field into grid navigation.
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    struct Tab {
        QString id;
        QString icon;    // QRC svg path
        int     section; // section index in the grid (set during rebuild)
    };

    void rebuild(const QString &filter = {});
    void buildCategoryBar();
    void setActiveTab(int sectionIdx);
    void applyTheme();
    void recordUse(const QString &baseName);
    void buildSkinToneRow();
    void toggleSkinToneRow();
    void applySkinTone(int tone);
    void updateSkinToneButton();

    StyledLineEdit    *_search  = nullptr;
    EmojiGrid         *_grid    = nullptr;
    QWidget           *_catBar  = nullptr;
    class QPushButton *_skinBtn = nullptr;
    QWidget           *_toneRow = nullptr; // inline skin-tone swatches (no popup)
    Session           *_session = nullptr;
    QVector<Tab>       _tabs;
    int                _skinTone  = 0; // 0 = default, else 2..6
    bool               _searching = false;
};
