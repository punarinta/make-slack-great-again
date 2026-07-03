// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "popup_tooltip.h"
#include "ui/paint_utils.h"
#include "ui/popup_placement.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji_pixmap.h"

#include <QHideEvent>
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

    // Re-placing an already-visible tooltip (e.g. the task list re-rendering as a
    // task completes) may shrink it. The region the smaller box vacates must be
    // repainted by the host or it leaves a stale remnant — the same translucent-
    // overlay backing-store issue hideEvent() guards against. Capture the old rect
    // before moving, then invalidate old∪new on the parent.
    const bool  wasVisible = isVisible();
    const QRect oldGeom    = geometry();

    setFixedSize(w, h);
    if (host)
        move(host->mapFromGlobal(QPoint(gx, gy)));
    else
        move(gx, gy);
    if (wasVisible && parentWidget())
        parentWidget()->update(oldGeom.united(geometry()));
    show();
    raise();
    update();
}

void PopupTooltip::showAbove(const QString &text, const QRect &targetGlobalRect) {
    _text     = text;
    _rightOf  = false;
    _reaction = false;
    _taskList = false;

    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    const QFontMetrics fm(f);

    const int bodyW   = fm.horizontalAdvance(text) + 2 * kPadH;
    const int bodyH   = fm.height() + 2 * kPadV;
    const int widgetW = bodyW + 2 * kShadow;
    const int widgetH = kShadow + bodyH + kArrowH + kShadow;

    const int arrowTipGX = targetGlobalRect.center().x();

    // Prefer above; placePopup flips to below if there isn't room, then clamps
    // on-screen. _below drives which way the arrow points (see paintEvent).
    bool         flipped = false;
    const QPoint wpos    = Ui::placePopup(
        targetGlobalRect,
        QSize(widgetW, widgetH),
        availRect(),
        Ui::Edge::Above,
        kGap - kShadow,
        Ui::Align::Center,
        &flipped
    );
    _below = flipped;

    _arrowX = std::clamp(arrowTipGX - wpos.x(), kShadow + kArrowW, widgetW - kShadow - kArrowW);

    placeGlobal(wpos.x(), wpos.y(), widgetW, widgetH);
}

void PopupTooltip::showRightOf(const QString &text, const QRect &targetGlobalRect) {
    _text     = text;
    _below    = false;
    _rightOf  = true;
    _reaction = false;
    _taskList = false;

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
    _taskList   = false;
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

    // Prefer above; placePopup flips to below and clamps on-screen.
    bool         flipped = false;
    const QPoint wpos    = Ui::placePopup(
        targetGlobalRect,
        QSize(widgetW, widgetH),
        availRect(),
        Ui::Edge::Above,
        kGap - kShadow,
        Ui::Align::Center,
        &flipped
    );
    _below = flipped;

    _arrowX = std::clamp(arrowTipGX - wpos.x(), kShadow + kArrowW, widgetW - kShadow - kArrowW);

    placeGlobal(wpos.x(), wpos.y(), widgetW, widgetH);
}

void PopupTooltip::showTaskList(
    const QString &header, const QStringList &tasks, const QRect &targetGlobalRect
) {
    _taskList = true;
    _reaction = false;
    _rightOf  = false;
    _header   = header;
    _names    = tasks;

    QFont headerF = QApplication::font();
    headerF.setPointSizeF(headerF.pointSizeF() * 0.82);
    const QFontMetrics headerFm(headerF);

    QFont taskF = QApplication::font();
    taskF.setWeight(QFont::Weight(500));
    const QFontMetrics taskFm(taskF);

    int contentW = headerFm.horizontalAdvance(header);
    for (const QString &t : tasks)
        contentW = std::max(contentW, taskFm.horizontalAdvance(t));
    contentW = std::min(contentW, 320);

    const int contentH =
        headerFm.height() + kHeaderGapV + static_cast<int>(tasks.size()) * taskFm.height();

    const int bodyW   = contentW + 2 * kPadH;
    const int bodyH   = contentH + 2 * kPadV;
    const int widgetW = bodyW + 2 * kShadow;
    const int widgetH = kShadow + bodyH + kArrowH + kShadow;

    const int arrowTipGX = targetGlobalRect.center().x();

    // Prefer above; placePopup flips to below and clamps on-screen.
    bool         flipped = false;
    const QPoint wpos    = Ui::placePopup(
        targetGlobalRect,
        QSize(widgetW, widgetH),
        availRect(),
        Ui::Edge::Above,
        kGap - kShadow,
        Ui::Align::Center,
        &flipped
    );
    _below = flipped;

    _arrowX = std::clamp(arrowTipGX - wpos.x(), kShadow + kArrowW, widgetW - kShadow - kArrowW);

    placeGlobal(wpos.x(), wpos.y(), widgetW, widgetH);
}

void PopupTooltip::hideEvent(QHideEvent *e) {
    if (QWidget *p = parentWidget())
        p->update(geometry());
    QWidget::hideEvent(e);
}

void PopupTooltip::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (_reaction) {
        paintReaction(p);
        return;
    }
    if (_taskList) {
        paintTaskList(p);
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
    Paint::dropShadow(p, body, kRadius, kShadow, 0, 3);

    // Fill body
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().tooltip.bg);
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
    Paint::dropShadow(p, body, kRadius, kShadow, 0, 3);

    // Fill body
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().tooltip.bg);
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
        EmojiPix::draw(p, emojiRect, _emojiGlyph, kEmojiPx, Th::c().text.onDark);
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

void PopupTooltip::paintTaskList(QPainter &p) {
    const int    bodyH = height() - kShadow - kArrowH - kShadow;
    const QRectF body  = _below ? QRectF(kShadow, kShadow + kArrowH, width() - 2 * kShadow, bodyH)
                                : QRectF(kShadow, kShadow, width() - 2 * kShadow, bodyH);

    // Light drop shadow around the body only
    Paint::dropShadow(p, body, kRadius, kShadow, 0, 3);

    // Fill body
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().tooltip.bg);
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

    const qreal textW = body.width() - 2 * kPadH;
    const qreal textX = body.left() + kPadH;

    // Header — small, dimmed
    QFont headerF = QApplication::font();
    headerF.setPointSizeF(headerF.pointSizeF() * 0.82);
    p.setFont(headerF);
    const QFontMetrics headerFm(headerF);
    QColor             dim = Th::c().text.onDark;
    dim.setAlpha(160);
    p.setPen(dim);
    qreal ty = body.top() + kPadV;
    p.drawText(
        QRectF(textX, ty, textW, headerFm.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        headerFm.elidedText(_header, Qt::ElideRight, qRound(textW))
    );
    ty += headerFm.height() + kHeaderGapV;

    // Task descriptions — one per line, left-aligned
    QFont taskF = QApplication::font();
    taskF.setWeight(QFont::Weight(500));
    p.setFont(taskF);
    const QFontMetrics taskFm(taskF);
    p.setPen(Th::c().text.onDark);
    for (const QString &t : _names) {
        p.drawText(
            QRectF(textX, ty, textW, taskFm.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            taskFm.elidedText(t, Qt::ElideRight, qRound(textW))
        );
        ty += taskFm.height();
    }
}
