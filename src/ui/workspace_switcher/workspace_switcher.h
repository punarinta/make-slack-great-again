// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Vertical workspace icon strip for the leftmost sidebar column.
// Shows workspace bubbles (image or letter fallback) and an add (+) button.
#pragma once

#include <QWidget>
#include <QPixmap>
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
        int     unread = 0;
    };

    explicit WorkspaceSwitcher(QWidget *parent = nullptr);

    void setImageCache(ImageCache *cache);
    void setWorkspaces(const std::vector<Entry> &entries);
    void setActive(const QString &teamId);
    void setUnread(const QString &teamId, int count);

signals:
    void workspaceClicked(const QString &teamId);
    void addWorkspaceClicked();
    void workspaceRightClicked(const QString &teamId, const QPoint &globalPos);
    void settingsClicked();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    bool event(QEvent *e) override;

private:
    struct EntryPrivate {
        Entry   info;
        QPixmap icon;
    };

    int     hitTest(const QPoint &pos) const; // >=0 entry, -2 add button, -3 gear, -99 miss
    QRect   entryRect(int i) const;
    QRect   addButtonRect() const;
    QRect   gearButtonRect() const;
    void    loadIcons();
    QPixmap scaleIcon(const QPixmap &src) const;
    QColor  bubbleColor(const QString &teamId) const;

    std::vector<EntryPrivate> _entries;
    QString                   _activeId;
    int                       _hovered = -99;
    int                       _pressed = -99;

    ImageCache   *_imgCache = nullptr;
    PopupTooltip *_tooltip  = nullptr;

    static constexpr int kW         = 64;
    static constexpr int kBubble    = 40;
    static constexpr int kGap       = 8;
    static constexpr int kTopPad    = 16;
    static constexpr int kAddSize   = 36;
    static constexpr int kRadius    = 10;
    static constexpr int kBarW      = 3;
    static constexpr int kGearSize  = 36;
    static constexpr int kBottomPad = 14;
};
