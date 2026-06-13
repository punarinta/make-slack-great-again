// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPixmap>
#include <QStringList>
#include <QWidget>

// Dark rounded-rect tooltip with a downward-pointing chevron.
// Call showAbove(text, targetGlobalRect) to position and reveal it;
// hide() to dismiss.  The chevron tip points at the center-top of targetGlobalRect.
class PopupTooltip : public QWidget {
    Q_OBJECT
public:
    explicit PopupTooltip(QWidget *parent = nullptr);
    void showAbove(const QString &text, const QRect &targetGlobalRect);
    void showRightOf(const QString &text, const QRect &targetGlobalRect);

    // Reaction preview: a large emoji (unicode glyph or custom-emoji image) over
    // the list of display names that reacted with it.  Positioned like showAbove.
    void showReaction(
        const QString     &emojiGlyph,
        const QPixmap     &emojiImage,
        const QStringList &names,
        const QRect       &targetGlobalRect
    );

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QString     _text;
    int         _arrowX   = 0;     // arrow-tip x in widget coords (above/below modes)
    int         _arrowY   = 0;     // arrow-tip y in widget coords (rightOf mode)
    bool        _below    = false; // true when tooltip is shown below the target
    bool        _rightOf  = false; // true when tooltip is shown to the right of the target
    bool        _reaction = false; // true in reaction-preview mode
    QString     _emojiGlyph;       // unicode emoji glyph (empty when using an image)
    QPixmap     _emojiImage;       // custom-emoji pixmap (null when using a glyph)
    QStringList _names;            // reactor display names, one per line

    void paintReaction(QPainter &p);

    static constexpr int kEmojiPx   = 30; // rendered preview emoji side
    static constexpr int kReactGapV = 8;  // gap between emoji and the name list

    static constexpr int kPadH   = 10;
    static constexpr int kPadV   = 5;
    static constexpr int kRadius = 6;
    static constexpr int kArrowW = 7; // half-width of arrow base
    static constexpr int kArrowH = 6; // height of arrow
    static constexpr int kShadow = 5; // transparent padding around widget
    static constexpr int kGap    = 4; // gap between arrow tip and target top
};
