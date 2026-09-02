// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/virtual_list/virtual_list_widget.h"

#include <QPixmap>
#include <QString>
#include <functional>
#include <vector>

class ImageCache;

// Zero-widget virtual list of either channels or people for BrowseChannelsDialog.
// Only the rows visible in the viewport are painted, so construction and
// scrolling cost is constant regardless of how many channels/users the workspace
// has. (The previous implementation built a QFrame + nested layouts + QLabels +
// a smooth-scaled avatar pixmap per item up front — O(n) work on the main thread
// that lagged badly on large workspaces when opening the dialog / DM picker.)
// Built on the same VirtualListWidget primitive as ConvListWidget.
class BrowseListView : public VirtualListWidget {
    Q_OBJECT
public:
    struct Item {
        QString id;        // convId or userId
        QString title;     // channel name / user display name
        QString subtitle;  // member count·description / @username
        QString avatarUrl; // people only
        QString initial;   // placeholder letter
        QString searchKey; // lowercased haystack for filtering
        bool    isPerson  = false;
        bool    isPrivate = false; // channel → lock vs hash icon
        bool    isMember  = false; // channel → "Joined" badge
    };

    explicit BrowseListView(ImageCache *imgCache, QWidget *parent = nullptr);

    void setItems(std::vector<Item> items);
    void applyFilter(const QString &query);

    // Inspection (rows currently held / passing the active filter).
    int     count() const { return static_cast<int>(_items.size()); }
    int     visibleCount() const { return static_cast<int>(_filtered.size()); }
    QString idAt(int visibleRow) const; // id of the nth visible row, or empty

    // ── Keyboard selection (opt-in) ───────────────────────────────────────────
    // Off by default: with no selection set the list paints exactly as it always
    // has (hover highlight only), so the browse dialog is unaffected. A caller
    // that drives the list from a search field (the quick switcher) sets a row
    // and moves it with the arrow keys.
    int     selectedRow() const { return _selected; }
    QString selectedId() const { return idAt(_selected); }
    void    setSelectedRow(int visibleRow); // clamped; -1 clears
    void    moveSelection(int delta);       // wraps at both ends
    void    activateSelected();             // fires onActivated for the selected row

    // Invoked with the item id when a row is activated (clicked).
    std::function<void(const QString &id)> onActivated;

protected:
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void doPaint(QPaintEvent *) override;
    void doMouseMove(QMouseEvent *) override;
    void doMousePress(QMouseEvent *) override;
    void doMouseRelease(QMouseEvent *) override;
    void doMouseLeave() override;

private:
    void rebuildIcons();
    void updateScrollRange();
    int  rowAt(int vpY) const;
    void setHovered(int row);
    void paintRow(QPainter &p, const Item &it, int y, bool hovered, bool selected);
    void scrollRowIntoView(int row);

    ImageCache       *_imgCache = nullptr;
    std::vector<Item> _items;
    std::vector<int>  _filtered; // indices into _items passing the current filter
    QString           _filterText;
    int               _hovered  = -1;
    int               _selected = -1; // keyboard selection; -1 = none
    QPixmap           _hashPx, _lockPx, _checkPx;

    static constexpr int kRowH       = 60;
    static constexpr int kAvatarSize = 36;
    static constexpr int kRowPadH    = 24;
};
