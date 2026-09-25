// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPixmap>
#include <QStringList>
#include <QWidget>

// Dark rounded-rect tooltip with a downward-pointing chevron.
// Call showAbove(text, targetGlobalRect) to position and reveal it;
// hide() to dismiss.  The chevron tip points at the center-top of targetGlobalRect.
//
// A mouse press anywhere hides every visible tooltip, and a tooltip for the
// pressed target stays suppressed (show* calls are ignored) until the cursor
// leaves that target — a hint for an action that has just been taken is noise.
// Call sites need no press handling of their own; feedback shown in response to
// the click itself goes through showToast(), which bypasses the suppression.
class PopupTooltip : public QWidget {
    Q_OBJECT
public:
    explicit PopupTooltip(QWidget *parent = nullptr);
    ~PopupTooltip() override;
    void showAbove(const QString &text, const QRect &targetGlobalRect);
    // As showAbove, but never suppressed by a press on the target: for click
    // feedback ("address copied") that is meant to appear under the cursor.
    void showToast(const QString &text, const QRect &targetGlobalRect);
    void showRightOf(const QString &text, const QRect &targetGlobalRect);

    // Reaction preview: a large emoji (unicode glyph or custom-emoji image) over
    // the list of display names that reacted with it.  Positioned like showAbove.
    void showReaction(
        const QString     &emojiGlyph,
        const QPixmap     &emojiImage,
        const QStringList &names,
        const QRect       &targetGlobalRect
    );

    // Task list: a small dimmed header line (e.g. "3 background tasks running")
    // over a left-aligned list of task descriptions.  Positioned like showAbove.
    void
    showTaskList(const QString &header, const QStringList &tasks, const QRect &targetGlobalRect);

protected:
    void paintEvent(QPaintEvent *e) override;
    // Force the host window to repaint the region we vacated. A translucent child
    // overlay leaves stale pixels in the backing store on hide under Windows (a
    // thin vertical seam to the right of where the tooltip stood); Linux/macOS
    // invalidate it for us. update(geometry()) on the parent is a harmless no-op
    // where it isn't needed.
    void hideEvent(QHideEvent *e) override;

private:
    friend class PopupTooltipPressWatcher;

    // True (and the tooltip is hidden) when a press on this target suppresses it.
    bool suppressedFor(const QRect &targetGlobalRect);
    void showAboveImpl(const QString &text, const QRect &targetGlobalRect);

    QRect       _target; // global rect of the target the tooltip currently points at
    QString     _text;
    int         _arrowX   = 0;     // arrow-tip x in widget coords (above/below modes)
    int         _arrowY   = 0;     // arrow-tip y in widget coords (rightOf mode)
    bool        _below    = false; // true when tooltip is shown below the target
    bool        _rightOf  = false; // true when tooltip is shown to the right of the target
    bool        _reaction = false; // true in reaction-preview mode
    bool        _taskList = false; // true in task-list mode
    QString     _emojiGlyph;       // unicode emoji glyph (empty when using an image)
    QPixmap     _emojiImage;       // custom-emoji pixmap (null when using a glyph)
    QString     _header;           // small dimmed header line (task-list mode)
    QStringList _names;            // reactor display names / task descriptions, one per line

    void paintReaction(QPainter &p);
    void paintTaskList(QPainter &p);

    // Reparent onto the top-level window and place at a global position.  Done as
    // an in-window child overlay (not a Qt::ToolTip top-level) because Wayland
    // compositors don't honour absolute move() of top-level popups — the box would
    // drift from where we computed it and the arrow would miss its target.
    void placeGlobal(int gx, int gy, int w, int h);

    // The region the tooltip must stay within: the host top-level window's global
    // rect (so a child overlay is never clipped), falling back to the screen.
    QRect availRect() const;

    static constexpr int kEmojiPx    = 30; // rendered preview emoji side
    static constexpr int kReactGapV  = 8;  // gap between emoji and the name list
    static constexpr int kHeaderGapV = 5;  // gap between the task-list header and the list

    static constexpr int kPadH   = 10;
    static constexpr int kPadV   = 5;
    static constexpr int kRadius = 6;
    static constexpr int kArrowW = 7; // half-width of arrow base
    static constexpr int kArrowH = 6; // height of arrow
    static constexpr int kShadow = 5; // transparent padding around widget
    static constexpr int kGap    = 4; // gap between arrow tip and target top
};
