// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji_font.h"

#include <QPainter>
#include <QPaintEvent>
#include <QApplication>
#include <QFontMetrics>
#include <QScreen>
#include <QGuiApplication>
#include <algorithm>

PopupTooltip::PopupTooltip(QWidget *parent) : QWidget(parent) {
    // In-window child overlay (no Qt::ToolTip window flag): see placeGlobal().
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
}

QRect PopupTooltip::availRect() const {
    if (QWidget *p = parentWidget()) {
        if (QWidget *host = p->window())
            return QRect(host->mapToGlobal(QPoint(0, 0)), host->size());
    }
    if (QScreen *s = QGuiApplication::primaryScreen())
        return s->availableGeometry();
    return QRect();
}

void PopupTooltip::placeGlobal(int gx, int gy, int w, int h) {
    // Float above all siblings of the top-level window.  Reparenting onto window()
    // is essential: the construction parent may be a tiny widget (e.g. a 28px
    // button) that would clip the tooltip.
    QWidget *host = parentWidget() ? parentWidget()->window() : nullptr;
    if (host && parentWidget() != host)
        setParent(host);

    setFixedSize(w, h);
    if (host)
        move(host->mapFromGlobal(QPoint(gx, gy)));
    else
        move(gx, gy);
    show();
    raise();
    update();
}

void PopupTooltip::showAbove(const QString &text, const QRect &targetGlobalRect) {
    _text     = text;
    _rightOf  = false;
    _reaction = false;

    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    const QFontMetrics fm(f);

    const int bodyW   = fm.horizontalAdvance(text) + 2 * kPadH;
    const int bodyH   = fm.height() + 2 * kPadV;
    const int widgetW = bodyW + 2 * kShadow;
    const int widgetH = kShadow + bodyH + kArrowH + kShadow;

    const int arrowTipGX = targetGlobalRect.center().x();

    const QRect avail = availRect();

    // Prefer above; fall back to below if there isn't enough room
    const int neededAbove = kShadow + bodyH + kArrowH + kGap;
    _below                = avail.isValid() && (targetGlobalRect.top() - avail.top() < neededAbove);

    int wx, wy;
    if (_below) {
        // Arrow tip just below the target's bottom edge; tip is at widget y=kShadow
        wy = targetGlobalRect.bottom() + kGap - kShadow;
    } else {
        // Arrow tip just above the target's top edge; tip is at widget y=kShadow+bodyH+kArrowH
        wy = targetGlobalRect.top() - kGap - (kShadow + bodyH + kArrowH);
    }
    wx = arrowTipGX - widgetW / 2;

    if (avail.isValid()) {
        wx = std::max(avail.left(), std::min(wx, avail.right() - widgetW));
        if (_below)
            wy = std::min(wy, avail.bottom() - widgetH);
        else
            wy = std::max(avail.top(), wy);
    }

    _arrowX = std::clamp(arrowTipGX - wx, kShadow + kArrowW, widgetW - kShadow - kArrowW);

    placeGlobal(wx, wy, widgetW, widgetH);
}

void PopupTooltip::showRightOf(const QString &text, const QRect &targetGlobalRect) {
    _text     = text;
    _below    = false;
    _rightOf  = true;
    _reaction = false;

    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    const QFontMetrics fm(f);

    const int bodyW   = fm.horizontalAdvance(text) + 2 * kPadH;
    const int bodyH   = fm.height() + 2 * kPadV;
    const int widgetW = kShadow + kArrowH + bodyW + kShadow;
    const int widgetH = bodyH + 2 * kShadow;

    const int arrowTipGY = targetGlobalRect.center().y();

    const QRect avail = availRect();

    int wx = targetGlobalRect.right() + kGap - kShadow;
    int wy = arrowTipGY - widgetH / 2;

    if (avail.isValid()) {
        wx = std::min(wx, avail.right() - widgetW);
        wy = std::max(avail.top(), std::min(wy, avail.bottom() - widgetH));
    }

    _arrowY = std::clamp(arrowTipGY - wy, kShadow + kArrowW, widgetH - kShadow - kArrowW);

    placeGlobal(wx, wy, widgetW, widgetH);
}

void PopupTooltip::showReaction(
    const QString     &emojiGlyph,
    const QPixmap     &emojiImage,
    const QStringList &names,
    const QRect       &targetGlobalRect
) {
    _reaction   = true;
    _rightOf    = false;
    _emojiGlyph = emojiGlyph;
    _emojiImage = emojiImage;
    _names      = names;

    const QFontMetrics nameFm(QApplication::font());
    const int          lineH = nameFm.height();

    int namesW = 0;
    for (const QString &n : names)
        namesW = std::max(namesW, nameFm.horizontalAdvance(n));

    const int contentW = std::min(std::max(kEmojiPx, namesW), 320);
    const int contentH = kEmojiPx + kReactGapV + (int)names.size() * lineH;

    const int bodyW   = contentW + 2 * kPadH;
    const int bodyH   = contentH + 2 * kPadV;
    const int widgetW = bodyW + 2 * kShadow;
    const int widgetH = kShadow + bodyH + kArrowH + kShadow;

    const int arrowTipGX = targetGlobalRect.center().x();

    const QRect avail = availRect();

    const int neededAbove = kShadow + bodyH + kArrowH + kGap;
    _below                = avail.isValid() && (targetGlobalRect.top() - avail.top() < neededAbove);

    int wx, wy;
    if (_below)
        wy = targetGlobalRect.bottom() + kGap - kShadow;
    else
        wy = targetGlobalRect.top() - kGap - (kShadow + bodyH + kArrowH);
    wx = arrowTipGX - widgetW / 2;

    if (avail.isValid()) {
        wx = std::max(avail.left(), std::min(wx, avail.right() - widgetW));
        if (_below)
            wy = std::min(wy, avail.bottom() - widgetH);
        else
            wy = std::max(avail.top(), wy);
    }

    _arrowX = std::clamp(arrowTipGX - wx, kShadow + kArrowW, widgetW - kShadow - kArrowW);

    placeGlobal(wx, wy, widgetW, widgetH);
}

void PopupTooltip::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (_reaction) {
        paintReaction(p);
        return;
    }

    QRectF body;
    if (_rightOf) {
        body = QRectF(
            kShadow + kArrowH,
            kShadow,
            width() - kShadow - kArrowH - kShadow,
            height() - 2 * kShadow
        );
    } else {
        const int bodyH = height() - kShadow - kArrowH - kShadow;
        body            = _below ? QRectF(kShadow, kShadow + kArrowH, width() - 2 * kShadow, bodyH)
                                 : QRectF(kShadow, kShadow, width() - 2 * kShadow, bodyH);
    }

    // Light drop shadow around the body only
    for (int i = kShadow; i >= 2; --i) {
        const int alpha = (kShadow - i) * 3;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(
            body.adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), kRadius + i, kRadius + i
        );
    }

    // Fill body
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().text.primary);
    p.drawRoundedRect(body, kRadius, kRadius);

    // Arrow — base overlaps body by 3px to cover antialiased edge seam
    QPolygonF arrow;
    if (_rightOf) {
        // Tip points left toward the target
        const qreal baseX = body.left();
        const qreal tipX  = kShadow;
        const qreal cy    = _arrowY;
        arrow << QPointF(baseX + 3, cy - kArrowW) << QPointF(baseX + 3, cy + kArrowW)
              << QPointF(tipX, cy);
    } else {
        const qreal cx = _arrowX;
        if (_below) {
            const qreal baseY = body.top();
            const qreal tipY  = kShadow;
            arrow << QPointF(cx - kArrowW, baseY + 3) << QPointF(cx + kArrowW, baseY + 3)
                  << QPointF(cx, tipY);
        } else {
            const qreal baseY = body.bottom();
            const qreal tipY  = baseY + kArrowH;
            arrow << QPointF(cx - kArrowW, baseY - 3) << QPointF(cx + kArrowW, baseY - 3)
                  << QPointF(cx, tipY);
        }
    }
    p.drawPolygon(arrow);

    // Text
    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    p.setFont(f);
    p.setPen(Th::c().text.onDark);
    p.drawText(
        QRectF(
            body.left() + kPadH,
            body.top() + kPadV,
            body.width() - 2 * kPadH,
            body.height() - 2 * kPadV
        ),
        Qt::AlignCenter,
        _text
    );
}

void PopupTooltip::paintReaction(QPainter &p) {
    const int    bodyH = height() - kShadow - kArrowH - kShadow;
    const QRectF body  = _below ? QRectF(kShadow, kShadow + kArrowH, width() - 2 * kShadow, bodyH)
                                : QRectF(kShadow, kShadow, width() - 2 * kShadow, bodyH);

    // Light drop shadow around the body only
    for (int i = kShadow; i >= 2; --i) {
        const int alpha = (kShadow - i) * 3;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(
            body.adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), kRadius + i, kRadius + i
        );
    }

    // Fill body
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().text.primary);
    p.drawRoundedRect(body, kRadius, kRadius);

    // Arrow
    QPolygonF   arrow;
    const qreal cx = _arrowX;
    if (_below) {
        const qreal baseY = body.top();
        arrow << QPointF(cx - kArrowW, baseY + 3) << QPointF(cx + kArrowW, baseY + 3)
              << QPointF(cx, kShadow);
    } else {
        const qreal baseY = body.bottom();
        arrow << QPointF(cx - kArrowW, baseY - 3) << QPointF(cx + kArrowW, baseY - 3)
              << QPointF(cx, baseY + kArrowH);
    }
    p.drawPolygon(arrow);

    // Emoji preview — centered near the top of the body
    const QRect emojiRect(
        qRound(body.center().x() - kEmojiPx / 2.0), qRound(body.top() + kPadV), kEmojiPx, kEmojiPx
    );
    if (!_emojiImage.isNull()) {
        const QSize tgt = _emojiImage.size().scaled(kEmojiPx, kEmojiPx, Qt::KeepAspectRatio);
        p.drawPixmap(
            QRect(
                emojiRect.x() + (kEmojiPx - tgt.width()) / 2,
                emojiRect.y() + (kEmojiPx - tgt.height()) / 2,
                tgt.width(),
                tgt.height()
            ),
            _emojiImage
        );
    } else {
        p.setFont(emojiFont(kEmojiPx));
        p.setPen(Th::c().text.onDark);
        p.drawText(emojiRect, Qt::AlignCenter, _emojiGlyph);
    }

    // Reactor names — one per line below the emoji
    QFont nameF = QApplication::font();
    p.setFont(nameF);
    const QFontMetrics nameFm(nameF);
    const int          lineH = nameFm.height();
    const qreal        textW = body.width() - 2 * kPadH;
    qreal              ty    = body.top() + kPadV + kEmojiPx + kReactGapV;
    p.setPen(Th::c().text.onDark);
    for (const QString &n : _names) {
        const QString line = nameFm.elidedText(n, Qt::ElideRight, qRound(textW));
        p.drawText(QRectF(body.left() + kPadH, ty, textW, lineH), Qt::AlignCenter, line);
        ty += lineH;
    }
}
