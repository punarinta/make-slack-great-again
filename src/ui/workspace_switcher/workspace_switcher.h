// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Vertical workspace icon strip for the leftmost sidebar column.
// Shows workspace bubbles (image or letter fallback) and an add (+) button.
// Bubbles can be reordered by drag and drop; displaced bubbles animate.
#pragma once

#include <QWidget>
#include <QPixmap>
#include <QTimer>
#include <vector>

class PopupTooltip;
class ImageCache;

class WorkspaceSwitcher : public QWidget {
    Q_OBJECT
public:
    struct Entry {
        QString teamId;
        QString name;
        QString iconUrl;
        int     unread   = 0;
        int     mentions = 0;
    };

    explicit WorkspaceSwitcher(QWidget *parent = nullptr);

    void            setImageCache(ImageCache *cache);
    void            setWorkspaces(const std::vector<Entry> &entries);
    void            setActive(const QString &teamId);
    void            setUnreadCounts(const QString &teamId, int total, int mentions);
    // Current counts for a workspace — {total, mentions}; zeros if unknown.
    QPair<int, int> unreadCounts(const QString &teamId) const;
    // Team ids in current visual (top-to-bottom) order.
    QStringList     workspaceIds() const;

signals:
    void workspaceClicked(const QString &teamId);
    void addWorkspaceClicked();
    void workspaceRightClicked(const QString &teamId, const QPoint &globalPos);
    void settingsClicked();
    void workspacesReordered(const QStringList &teamIds);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    struct EntryPrivate {
        Entry   info;
        QPixmap icon;
        qreal   y = -1.0; // animated bubble top; <0 means "snap to slot"
    };

    int     hitTest(const QPoint &pos) const; // >=0 entry, -2 add button, -3 gear, -99 miss
    int     slotY(int i) const;               // resting Y of slot i
    QRect   entryRect(int i) const;
    QRect   addButtonRect() const;
    QRect   gearButtonRect() const;
    void    loadIcons();
    QPixmap scaleIcon(const QPixmap &src) const;
    QColor  bubbleColor(const QString &teamId) const;
    void    paintBubble(QPainter &p, const EntryPrivate &ep, const QRectF &r, bool hov) const;
    void    updateCursor(const QPoint &pos);
    void    beginDrag(const QPoint &pos);
    void    updateDrag(const QPoint &pos);
    void    endDrag();
    void    startAnim();
    void    tickAnim();

    std::vector<EntryPrivate> _entries;
    QString                   _activeId;
    int                       _hovered              = -99;
    int                       _pressed              = -99;
    bool                      _cursorOverrideActive = false;

    // Drag-reorder state
    QTimer      _animTimer;
    bool        _dragging  = false;
    int         _dragIndex = -1; // index of dragged entry in _entries (tracks live reorder)
    QPoint      _pressPos;
    qreal       _dragY      = 0;
    qreal       _grabOffset = 0;
    QStringList _orderAtDragStart;

    ImageCache   *_imgCache = nullptr;
    PopupTooltip *_tooltip  = nullptr;

    static constexpr int kW         = 64;
    static constexpr int kBubble    = 40;
    static constexpr int kGap       = 8;
    static constexpr int kTopPad    = 16;
    static constexpr int kAddSize   = 40; // rounded square, same as workspace bubbles
    static constexpr int kRadius    = 10;
    static constexpr int kBarW      = 3;
    static constexpr int kGearSize  = 40; // rounded square, same as workspace bubbles
    static constexpr int kBottomPad = 14;
};
