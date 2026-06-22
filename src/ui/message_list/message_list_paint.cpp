// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_list.h"
#include "message_render.h"
#include "session/session.h"
#include "ui/theme.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/paint_utils.h"
#include "ui/user_avatar.h"
#include "util/emoji_font.h"

#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QTextCursor>
#include <QApplication>
#include <QUrl>
#include <QtMath>

#include <algorithm>
#include <cmath>

// ── PaintContext ──────────────────────────────────────────────────────────────

PaintContext MessageListWidget::makePaintContext() const {
    const int vw       = viewport()->width();
    const int textLeft = kPadH + kAvSize + kAvGap;
    return PaintContext{
        vw,
        verticalScrollBar()->value(),
        viewport()->height(),
        textLeft,
        vw - textLeft - kPadH,
    };
}

// ── Paint entry point ─────────────────────────────────────────────────────────

void MessageListWidget::doPaint(QPaintEvent *event) {
    triggerMissingDownloads();
    triggerMissingAvatarDownloads();

    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(event->rect(), Th::c().surface.content);

    if ((_loading || _waiting) && _items.empty()) {
        // Nothing visible — make sure gif players from the previous conversation stop.
        _visibleGifs.clear();
        syncGifPlayback();

        _loadingAnim.paint(p, viewport()->rect());

        if (_loadingElapsedTimer.isValid()) {
            const qint64 ms = _loadingElapsedTimer.elapsed();
            QString      hint;
            if (ms >= 15000)
                hint =
                    tr("Oh my gosh, I really apologize, but your company is a reaaaly active "
                       "Slack user. Still loading...");
            else if (ms >= 5000)
                hint = tr("Oh, you must have a lot of co-workers and messages! Still loading...");
            else if (ms >= 1000)
                hint = tr("Loading your stuff...");

            if (!hint.isEmpty()) {
                QFont f = QApplication::font();
                f.setPointSizeF(f.pointSizeF() * 1.15);
                p.setFont(f);
                p.setPen(Th::c().text.secondary);
                const QRect vr      = viewport()->rect();
                const int   textTop = vr.center().y() + 26 + 24; // spinner radius + gap
                const QRect textRect(vr.left() + 32, textTop, vr.width() - 64, 80);
                p.drawText(textRect, Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, hint);
            }
        }

        return;
    }

    const PaintContext ctx     = makePaintContext();
    const int          scrollY = ctx.scrollY;
    const int          vh      = ctx.vh;

    // Intro header (channel/DM name + description before first message)
    if (_showIntro) {
        const int ih         = introHeight();
        const int introVpTop = -scrollY; // intro lives at document y=0
        if (introVpTop + ih >= 0 && introVpTop <= vh)
            paintIntro(p, introVpTop);
    }

    _visibleGifs.clear();
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        const int rowTop = _tops[i] - scrollY;
        const int rh     = rowHeight(i);
        if (rowTop + rh < 0)
            continue;
        if (rowTop > vh)
            break;
        paintRow(p, i, rowTop, ctx);
    }
    // Animated images: play the ones drawn this pass, pause everything else.
    syncGifPlayback();

    // Thin Telegram-style scrollbar overlay
    paintScrollThumb(p, _totalH, Th::c().divider.strong);
}

void MessageListWidget::paintRow(
    QPainter &p, int index, int rowTop, const PaintContext &ctx
) const {
    const auto &item = _items[index];
    ensureDocLayout(item);
    const bool collapsed = isCollapsed(index);

    // Paint date separator at the top of the row if needed, then shift content down.
    const int sepH = needsDateSep(index) ? kSepH : 0;
    if (sepH > 0)
        paintDateSep(p, rowTop, ctx.vw, item.msg.date);
    const int msgTop = rowTop + sepH;

    // System/activity lines (joins, topic changes, …) get a centered single
    // line — no avatar, header, hover background, toolbar, reactions or replies.
    if (isSystemEvent(item.msg)) {
        paintSystemRow(p, index, msgTop, ctx);
        return;
    }

    const int vw        = ctx.vw;
    const int textLeft  = ctx.textLeft;
    const int textWidth = ctx.textWidth;
    const int padV      = collapsed ? kPadVCollapsed : kPadV;
    const int rh        = rowHeight(index);
    const int msgH      = rh - sepH;

    // Hover background (message area only, not separator)
    if (index == _hoveredRow)
        p.fillRect(QRect(0, msgTop, vw, msgH), Th::c().message.hover);

    // New-message highlight: fade from Slack green tint → transparent
    if (_newMsgTs.contains(item.msg.ts)) {
        const double alpha = _highlightAnim.currentValue().toDouble();
        QColor       highlight(0x14, 0x85, 0x67, static_cast<int>(alpha * 40));
        p.fillRect(QRect(0, msgTop, vw, msgH), highlight);
    }

    // Pending (not yet delivered) messages render translucent until the server
    // confirms them; backgrounds above stay opaque, content below fades.
    const bool pending = item.msg.pending;
    if (pending)
        p.setOpacity(0.5);

    // ── Pinned banner — sits at the very top of the message, before padding ──
    const int pinnedBannerH = item.msg.pinned ? 18 : 0;
    if (item.msg.pinned) {
        const QRect bannerRect(0, msgTop, vw, pinnedBannerH);
        p.fillRect(bannerRect, Th::c().message.pinnedBg); // subtle yellow tint

        // Pin icon
        // NOTE: captures theme value once at first bake (acceptable for V1), but
        // re-bakes when the device pixel ratio changes so it stays crisp at
        // fractional scale (and after a move between differently-scaled monitors).
        static qreal   kPinDpr = 0;
        static QPixmap kPinPx;
        if (const qreal d = p.device()->devicePixelRatioF(); !qFuzzyCompare(d, kPinDpr)) {
            kPinDpr = d;
            kPinPx =
                svgPixmapPhys(":/ui/pin.svg", QSize(12, 12), Th::c().message.attachmentDismiss, d);
        }
        if (!kPinPx.isNull())
            p.drawPixmap(kPadH, msgTop + (pinnedBannerH - 12) / 2, kPinPx);

        // "Pinned by <name>" label
        const auto   *pinner  = _session ? _session->findUser(item.msg.pinnedBy) : nullptr;
        const QString label   = pinner ? tr("Pinned by %1").arg(pinner->displayName) : tr("Pinned");
        QFont         pinFont = QApplication::font();
        pinFont.setPointSizeF(pinFont.pointSizeF() * 0.78);
        p.save();
        p.setFont(pinFont);
        p.setPen(Th::c().message.attachmentDismiss);
        p.drawText(
            kPadH + 16,
            msgTop,
            vw - kPadH - 16,
            pinnedBannerH,
            Qt::AlignVCenter | Qt::AlignLeft,
            label
        );
        p.restore();
    }

    // Content starts below banner, then padV
    const int contTop = msgTop + pinnedBannerH + padV;

    if (!collapsed) {
        // Avatar
        paintAvatar(p, item, QRect(kPadH, contTop + 2, kAvSize, kAvSize));

        // ── Header: name + timestamp ──────────────────────────────────
        auto         *user = _session->findUser(item.msg.author);
        const QString name =
            user ? user->displayName
                 : (!item.msg.botName.isEmpty() ? item.msg.botName : item.msg.author.value);

        QFont nameFont = QApplication::font();
        nameFont.setBold(true);
        p.setFont(nameFont);
        p.setPen(Th::c().text.primary);
        const QFontMetrics nameFm(nameFont);
        const int          headerBaseline = contTop + nameFm.ascent();
        p.drawText(textLeft, headerBaseline, name);
        const int nameW = nameFm.horizontalAdvance(name);
        int       tsX   = textLeft + nameW + 8;

        // Slack-style "APP" tag after bot names
        const bool isBot = !item.msg.botName.isEmpty() || (user && user->isBot);
        if (isBot) {
            QFont badgeFont = QApplication::font();
            badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.62);
            badgeFont.setBold(true);
            const QFontMetrics bFm(badgeFont);
            const QString      label = tr("APP");
            const int          bH    = 14;
            const QRect        bRect(
                textLeft + nameW + 6,
                contTop + (nameFm.height() - bH) / 2,
                bFm.horizontalAdvance(label) + 8,
                bH
            );
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().message.appBadgeBg);
            p.drawRoundedRect(bRect, 2, 2);
            p.setFont(badgeFont);
            p.setPen(Th::c().message.appBadgeText);
            p.drawText(bRect, Qt::AlignCenter, label);
            p.restore();
            tsX = bRect.right() + 8;
        }

        QFont tsFont = QApplication::font();
        tsFont.setPointSizeF(tsFont.pointSizeF() * 0.85);
        p.setFont(tsFont);
        p.setPen(Th::c().text.secondary);
        const QFontMetrics tsFm(tsFont);
        const QString      tsText = MsgRender::formatTs(item.msg.date);
        // Align timestamp to the same baseline as the bold name
        p.drawText(tsX, headerBaseline, tsText);

        if (item.msg.edited) {
            const int tsW = tsFm.horizontalAdvance(tsText);
            p.drawText(tsX + tsW + 6, headerBaseline, tr("(edited)"));
        }
    }

    // ── Message text via QTextDocument ───────────────────────────────
    // Swap current animation frames into the doc image resources first.
    pullGifFrames(item);
    p.setFont(QApplication::font());
    int contentY = collapsed ? contTop : (contTop + kHdrH + kHdrGap);
    p.save();
    p.translate(textLeft, contentY);
    {
        QAbstractTextDocumentLayout::PaintContext pCtx;
        pCtx.palette = QApplication::palette();
        pCtx.clip    = QRectF(0, 0, textWidth, item.docHeight);

        // Muted notices (e.g. reminder_add) keep the normal message layout but
        // grey the body — the default text color drives any span without an
        // explicit color of its own (plain reminder text has none).
        if (isMutedMessage(item.msg))
            pCtx.palette.setColor(QPalette::Text, Th::c().text.secondary);

        // Compute normalized selection for this row.
        if (_selAnchor.row >= 0 && _selFocus.row >= 0) {
            int aRow = _selAnchor.row, aOff = _selAnchor.offset;
            int fRow = _selFocus.row, fOff = _selFocus.offset;
            if (aRow > fRow || (aRow == fRow && aOff > fOff)) {
                std::swap(aRow, fRow);
                std::swap(aOff, fOff);
            }
            int selFrom = -1, selTo = -1;
            if (index == aRow && index == fRow) {
                selFrom = aOff;
                selTo   = fOff;
            } else if (index == aRow) {
                selFrom = aOff;
                selTo   = item.textDoc->characterCount();
            } else if (index == fRow) {
                selFrom = 0;
                selTo   = fOff;
            } else if (index > aRow && index < fRow) {
                selFrom = 0;
                selTo   = item.textDoc->characterCount();
            }
            if (selFrom >= 0 && selTo > selFrom) {
                QAbstractTextDocumentLayout::Selection sel;
                QTextCursor                            cur(item.textDoc.get());
                cur.setPosition(selFrom);
                cur.setPosition(selTo, QTextCursor::KeepAnchor);
                sel.cursor = cur;
                sel.format.setBackground(QApplication::palette().highlight());
                sel.format.setForeground(QApplication::palette().highlightedText());
                pCtx.selections.append(sel);
            }
        }

        MsgRender::paintCodeBlockChrome(p, item.textDoc.get());
        MsgRender::paintBotButtonChrome(p, item.textDoc.get());
        item.textDoc->documentLayout()->draw(&p, pCtx);
    }
    p.restore();
    contentY += item.docHeight;

    // ── Attachments ──────────────────────────────────────────────────
    paintAttachments(p, item, ctx, contentY, index);
    for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
        if (isDismissed(item.msg.ts, ai))
            continue;
        contentY += kAttachGap + attachTotalH(item, ai);
    }

    // ── Inline file previews (images + prerendered docs) ─────────────
    paintFileImages(p, item, ctx, contentY);
    {
        const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
        for (const auto &f : item.msg.files) {
            if (!f.hasPreview())
                continue;
            const int imgGap = hasAbove ? kImgGap : 0;
            contentY += imgGap + kImgNameH + filePreviewSize(f, ctx.textWidth).height();
        }
    }

    // ── File chips (files without a preview) ─────────────────────────
    paintFileChips(p, item, ctx, contentY);
    {
        bool anyImg = false;
        for (const auto &f : item.msg.files)
            if (f.hasPreview()) {
                anyImg = true;
                break;
            }
        const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
        bool       firstChip     = true;
        for (const auto &f : item.msg.files) {
            if (f.hasPreview())
                continue;
            if (!firstChip || hasAboveChips)
                contentY += kFileChipGap;
            firstChip = false;
            contentY += kFileChipH;
        }
    }

    // ── Reactions ────────────────────────────────────────────────────
    if (!item.msg.reactions.empty())
        paintReactions(p, item, ctx, contentY + 2, index);

    // ── Reply bar (thread-root messages in channel view) ─────────────
    if (!_isThreadMode && item.msg.replyCount > 0) {
        int replyBarTop = contentY;
        if (!item.msg.reactions.empty())
            replyBarTop += kReactH + 2;
        replyBarTop += kReplyBarGap;
        paintReplyBar(p, item, ctx, replyBarTop, index);
    }

    // ── Collapsed-row timestamp (shown on hover) ──────────────────────
    if (collapsed && index == _hoveredRow) {
        QFont tsFont = QApplication::font();
        tsFont.setPointSizeF(tsFont.pointSizeF() * 0.82);
        p.save();
        p.setFont(tsFont);
        p.setPen(Th::c().text.secondary);
        const QString tsText  = MsgRender::formatTs(item.msg.date);
        const int     tsRight = kPadH + kAvSize;
        p.drawText(
            QRect(0, contTop, tsRight, msgH - 2 * kPadVCollapsed),
            Qt::AlignRight | Qt::AlignVCenter,
            tsText
        );
        p.restore();
    }

    if (pending) {
        p.setOpacity(1.0);
        // No action bars on a pending message — there is nothing on the
        // server yet to react to, forward, download or delete.
        return;
    }

    // ── File action bar ──────────────────────────────────────────────────
    if (_hoveredFile.first == index) {
        const QRect fr = fileViewportRect(index, _hoveredFile.second);
        if (!fr.isNull())
            paintFileActionBar(p, fr);
    }

    // ── Hover toolbar ─────────────────────────────────────────────────
    if (index == _hoveredRow)
        paintHoverToolbar(p, index, rowTop, rh);
}

// ── Avatar / presence ─────────────────────────────────────────────────────────

void MessageListWidget::triggerMissingAvatarDownloads() {
    if (!_session || !_imgCache)
        return;
    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    for (int i = 0; i < (int)_items.size(); ++i) {
        const int top = _tops.empty() ? 0 : _tops[i] - scrollY;
        if (top > vh)
            break;
        if (top + rowHeight(i) < 0)
            continue;

        auto *user = _session->findUser(_items[i].msg.author);
        if (user && !user->avatarUrl.isEmpty())
            _imgCache->get(user->avatarUrl);
        else if (!_items[i].msg.botAvatarUrl.isEmpty())
            _imgCache->get(_items[i].msg.botAvatarUrl);

        for (const auto &uid : _items[i].msg.replyUsers) {
            auto *ru = _session->findUser(uid);
            if (ru && !ru->avatarUrl.isEmpty())
                _imgCache->get(ru->avatarUrl);
        }
    }
}

void MessageListWidget::paintAvatar(QPainter &p, const MessageItem &item, QRect rect) const {
    auto *user = _session->findUser(item.msg.author);

    // Resolve avatar URL: user profile first, then bot_profile / icon_url.
    const QString avatarUrl =
        (user && !user->avatarUrl.isEmpty()) ? user->avatarUrl : item.msg.botAvatarUrl;

    // Try to draw a real photo if cached.
    if (!avatarUrl.isEmpty() && _imgCache) {
        const QPixmap cached = _imgCache->get(avatarUrl);
        if (!cached.isNull()) {
            UserAvatar::paintPhoto(p, rect, cached, p.device()->devicePixelRatioF(), 4);
            return;
        }
    }

    // Fallback: colored square with initial letter.
    const QString initial =
        user ? user->displayName
             : (!item.msg.botName.isEmpty() ? item.msg.botName : item.msg.author.value);
    const QChar ch  = initial.isEmpty() ? QChar('?') : initial[0];
    const int   hue = ch.unicode() * 37 % 360;

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    UserAvatar::paintInitial(
        p,
        rect,
        QString(ch),
        QColor::fromHsl(
            hue, Th::c().message.avatarHslSaturation, Th::c().message.avatarHslLightness
        ),
        Qt::white,
        4,
        14
    );
    p.restore();
}

// ── Attachments ───────────────────────────────────────────────────────────────

void MessageListWidget::paintAttachments(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
) const {
    const auto &attachments = item.msg.attachments;
    const int   left        = ctx.textLeft;
    const int   width       = ctx.textWidth;
    const int   textX       = left + kAttachBarW + kAttachBarGap;
    const int   textW       = width - kAttachBarW - kAttachBarGap;
    int         y           = top;
    for (int ai = 0; ai < (int)attachments.size(); ++ai) {
        if (isDismissed(item.msg.ts, ai))
            continue;

        const auto &att = attachments[ai];
        const auto &ad  = item.attachDocs[ai];
        y += kAttachGap;

        const int docH   = ad.docHeight;
        const int imgH   = attachImageH(att);
        const int totalH = docH + imgH;

        // GIF-picker attachments (image blocks only) render bar-less and
        // un-indented, like the official client.
        const bool imageOnly = MsgRender::attachIsImageOnly(att);
        const int  attX      = imageOnly ? left : textX;

        // Dismiss "×" button — only visible when this specific attachment is hovered
        if (_hoveredAttach.first == index && _hoveredAttach.second == ai) {
            const int   btnX = left - kDismissGap - kDismissW;
            const QRect btnRect(btnX, y, kDismissW, kDismissW);
            p.save();
            QFont xFont = QApplication::font();
            xFont.setPointSizeF(xFont.pointSizeF() * 1.15);
            p.setFont(xFont);
            p.setPen(Th::c().message.attachmentDismiss);
            p.drawText(btnRect, Qt::AlignCenter, "\xC3\x97"); // UTF-8 × (U+00D7)
            p.restore();
        }

        // Colored left bar (full attachment height)
        if (!imageOnly) {
            QColor barColor("#AAAAAA");
            if (!att.color.isEmpty()) {
                QColor c(att.color.startsWith('#') ? att.color : "#" + att.color);
                if (c.isValid())
                    barColor = c;
            }
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(barColor);
            p.drawRect(QRect(left, y, kAttachBarW, totalH > 0 ? totalH : docH));
            p.restore();
        }

        // Favicon: draw 16×16 to the left of the title, only if it loaded successfully.
        // We indent the text doc by 20px horizontally so the text starts right of the icon.
        int textIndent = 0;
        if (!imageOnly && !att.faviconUrl.isEmpty() && _imgCache) {
            const QPixmap fav = _imgCache->get(att.faviconUrl);
            if (!fav.isNull()) {
                textIndent            = 20;
                const int faviconSide = 16;
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                const qreal dpr       = p.device()->devicePixelRatioF();
                QPixmap     favScaled = fav.scaled(
                    QSize(faviconSide, faviconSide) * dpr,
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation
                );
                favScaled.setDevicePixelRatio(dpr);
                p.drawPixmap(textX, y + 2, favScaled);
                p.restore();
            }
        }

        // Attachment text doc
        if (ad.textDoc && docH > 0) {
            p.save();
            p.translate(attX + textIndent, y);
            MsgRender::paintCodeBlockChrome(p, ad.textDoc.get());
            MsgRender::paintBotButtonChrome(p, ad.textDoc.get());
            ad.textDoc->drawContents(&p, QRectF(0, 0, textW - textIndent, docH));
            p.restore();
        }

        // Preview image (thumbnail when large enough for this DPR, else full image)
        if (imgH > 0 && _imgCache) {
            const QString imgUrl = attachPreviewUrl(att);
            const QPixmap img    = _imgCache->get(imgUrl);
            if (!img.isNull()) {
                const double scale = std::min(
                    1.0, std::min((double)kImgMaxW / img.width(), (double)kImgMaxH / img.height())
                );
                const QSize sz((int)(img.width() * scale), (int)(img.height() * scale));
                const QRect target(QPoint(attX, y + docH + kImgGap), sz);
                const qreal dpr = p.device()->devicePixelRatioF();
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                // Animated previews (legacy Giphy attachments) draw the current
                // movie frame; static ones use the cached pre-scaled pixmap.
                QMovie *movie = gifMovieFor(imgUrl);
                QPixmap frame = movie ? movie->currentPixmap() : QPixmap();
                if (movie)
                    _visibleGifs.insert(imgUrl);
                if (!frame.isNull())
                    p.drawPixmap(target, frame);
                else
                    p.drawPixmap(target, scaledPreview(imgUrl, img, sz, dpr));
                p.restore();
            }
        }

        y += totalH;
    }
}

// ── Inline file images ────────────────────────────────────────────────────────

QSize MessageListWidget::filePreviewSize(const File &f, int maxW) const {
    // Original dimensions first: geometry must not depend on which thumbnail
    // resolution was fetched, and the no-upscale clamp applies to the original.
    if (f.imageWidth > 0 && f.imageHeight > 0) {
        const double scale = std::min(
            1.0, std::min((double)kImgMaxW / f.imageWidth, (double)kImgMaxH / f.imageHeight)
        );
        return {static_cast<int>(f.imageWidth * scale), static_cast<int>(f.imageHeight * scale)};
    }
    const auto it = _fileImages.constFind(filePreviewUrl(f));
    if (it != _fileImages.constEnd() && !it->isNull()) {
        const double scale = std::min(
            1.0, std::min((double)kImgMaxW / it->width(), (double)kImgMaxH / it->height())
        );
        return {static_cast<int>(it->width() * scale), static_cast<int>(it->height() * scale)};
    }
    return {std::min(maxW, kImgMaxW), 24};
}

QString MessageListWidget::filePreviewUrl(const File &f) const {
    return f.previewUrl(qCeil(kImgMaxW * devicePixelRatioF()));
}

QString MessageListWidget::attachPreviewUrl(const Attachment &att) const {
    return att.previewUrl(qCeil(kImgMaxW * devicePixelRatioF()));
}

QPixmap MessageListWidget::scaledPreview(
    const QString &key, const QPixmap &src, QSize logical, qreal dpr
) const {
    const QSize phys(qRound(logical.width() * dpr), qRound(logical.height() * dpr));
    QPixmap    &out = _scaledPreviews[key];
    if (out.size() != phys) {
        // IgnoreAspectRatio: `logical` is derived from the same image, so the
        // aspect already matches up to rounding.
        out = src.scaled(phys, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.setDevicePixelRatio(dpr);
    }
    return out;
}

void MessageListWidget::paintFileImages(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top
) const {
    const int  left     = ctx.textLeft;
    const int  width    = ctx.textWidth;
    int        y        = top;
    const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
    for (const auto &f : item.msg.files) {
        if (!f.hasPreview())
            continue;
        const QString imgUrl = filePreviewUrl(f);

        const auto it = _fileImages.constFind(imgUrl);
        if (hasAbove)
            y += kImgGap;

        // Filename label
        if (!f.name.isEmpty()) {
            p.save();
            QFont nameFont = p.font();
            nameFont.setPointSizeF(nameFont.pointSizeF() * 0.82);
            p.setFont(nameFont);
            p.setPen(Th::c().message.fileNameDim);
            p.drawText(
                QRect(left, y, width, kImgNameH), Qt::AlignVCenter | Qt::TextSingleLine, f.name
            );
            p.restore();
        }
        y += kImgNameH;

        // Same geometry for placeholder and loaded image — matches rowHeight(),
        // so nothing jumps when the download lands.
        const QSize sz = filePreviewSize(f, width);
        if (it != _fileImages.constEnd() && !it->isNull()) {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const qreal dpr   = p.device()->devicePixelRatioF();
            // Uploaded GIFs animate: draw the current movie frame when one exists.
            QMovie     *movie = _gifMovies.value(imgUrl, nullptr);
            QPixmap     frame = movie ? movie->currentPixmap() : QPixmap();
            if (movie)
                _visibleGifs.insert(imgUrl);
            if (!frame.isNull())
                p.drawPixmap(QRect(QPoint(left, y), sz), frame);
            else
                p.drawPixmap(
                    QRect(QPoint(left, y), sz), scaledPreview(imgUrl, it.value(), sz, dpr)
                );
            p.restore();
        } else {
            p.save();
            p.setPen(Th::c().message.imagePlaceholderBorder);
            p.setBrush(Th::c().message.imagePlaceholderBg);
            p.drawRect(QRect(QPoint(left, y), sz));
            p.setPen(Th::c().text.tertiary);
            p.drawText(QRect(QPoint(left, y), sz), Qt::AlignCenter, tr("Loading image…"));
            p.restore();
        }
        y += sz.height();
    }
}

void MessageListWidget::triggerMissingDownloads() {
    if (!_session)
        return;

    // Screen density changed (window moved between monitors): the preview URL
    // choice depends on it, so re-request images at the new density.
    const qreal dpr = devicePixelRatioF();
    if (!qFuzzyCompare(dpr, _previewDpr)) {
        _previewDpr = dpr;
        _scaledPreviews.clear();
        for (auto &item : _items) {
            item.fileImgsRequested   = false;
            item.attachImgsRequested = false;
        }
    }

    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    for (int i = 0; i < (int)_items.size(); ++i) {
        const int rowTop = _tops.empty() ? 0 : _tops[i] - scrollY;
        if (rowTop > vh)
            break;
        if (rowTop + rowHeight(i) < 0)
            continue;

        auto &item = _items[i];

        // File image downloads (auth required → via Session::downloadFile).
        // Results stored in _fileImages (separate from public-URL _imgCache).
        if (!item.fileImgsRequested) {
            bool needsDownload = false;
            for (const auto &f : item.msg.files) {
                if (!f.hasPreview())
                    continue;
                const QString url = filePreviewUrl(f);
                if (!_fileImages.contains(url)) {
                    needsDownload = true;
                    break;
                }
            }
            if (needsDownload) {
                item.fileImgsRequested = true;
                for (const auto &f : item.msg.files) {
                    if (!f.hasPreview())
                        continue;
                    const QString url = filePreviewUrl(f);
                    if (_fileImages.contains(url))
                        continue;

                    // Pending uploads point at the local file — read it from
                    // disk for an instant preview, no network involved.
                    if (url.startsWith("file://")) {
                        QPixmap   px(QUrl(url).toLocalFile());
                        const int maxSrcW = qCeil(kImgMaxW * devicePixelRatioF());
                        if (!px.isNull() && px.width() > maxSrcW)
                            px = px.scaledToWidth(maxSrcW, Qt::SmoothTransformation);
                        _fileImages[url] = px;
                        rebuildLayout();
                        viewport()->update();
                        continue;
                    }

                    const auto cached = _session->cachedImage(url);
                    if (!cached.isEmpty()) {
                        QPixmap px;
                        if (px.loadFromData(cached) && !px.isNull()) {
                            _fileImages[url] = px;
                            maybeCreateFileGifMovie(url, cached);
                            rebuildLayout();
                            viewport()->update();
                            continue;
                        }
                    }

                    _fileImages[url] = QPixmap(); // in-flight sentinel
                    _session->downloadFile(url, [this, url](QByteArray data) {
                        if (_session)
                            _session->cacheImage(url, data);
                        const bool wasAtBottom =
                            verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
                        QPixmap px;
                        px.loadFromData(data);
                        _fileImages[url] = px;
                        maybeCreateFileGifMovie(url, data);
                        _scaledPreviews.remove(url);
                        rebuildLayout();
                        if (wasAtBottom)
                            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
                        viewport()->update();
                    });
                }
            }
        }

        // Attachment preview images and favicons (public CDN URLs → via shared ImageCache).
        if (!item.attachImgsRequested && _imgCache) {
            bool needsAttach = false;
            for (const auto &att : item.msg.attachments) {
                const QString imgUrl = attachPreviewUrl(att);
                if (!imgUrl.isEmpty()) {
                    needsAttach = true;
                    break;
                }
                if (!att.faviconUrl.isEmpty()) {
                    needsAttach = true;
                    break;
                }
            }
            if (needsAttach) {
                item.attachImgsRequested = true;
                for (const auto &att : item.msg.attachments) {
                    const QString imgUrl = attachPreviewUrl(att);
                    if (!imgUrl.isEmpty())
                        _imgCache->get(imgUrl);
                    if (!att.faviconUrl.isEmpty())
                        _imgCache->get(att.faviconUrl);
                }
            }
        }
    }
}

// ── Reactions ─────────────────────────────────────────────────────────────────

// Chip sizing: color emoji fonts report advance width ≈ 2× pixelSize, so we use
// a fixed slot for the glyph and measure only the count with the regular font.
static constexpr int kReactPad   = 6;  // horizontal padding inside chip
static constexpr int kEmojiSlot  = 17; // fixed pixel budget for one emoji glyph
static constexpr int kReactEmoji = 16; // rendered emoji size (glyph px / custom image side)

static int reactChipW(const QString &countStr) {
    static const QFont kCntFont = [] {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * 0.82);
        return f;
    }();
    return kReactPad + kEmojiSlot + QFontMetrics(kCntFont).horizontalAdvance(countStr) + kReactPad;
}

void MessageListWidget::paintReactions(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
) const {
    const int left  = ctx.textLeft;
    const int width = ctx.textWidth;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    static const QFont kEmojiF = emojiFont(kReactEmoji);
    static const QFont kCountF = [] {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * 0.82);
        return f;
    }();
    const int chipH = kReactH;
    int       x     = left;

    const UserId me = _session ? _session->meUserId() : UserId{};

    for (int j = 0; j < (int)item.msg.reactions.size(); ++j) {
        const auto   &r        = item.msg.reactions[j];
        const auto    emoji    = MsgRender::resolveEmojiRich(r.name, _session);
        const QString countStr = " " + QString::number(r.count);
        const int     chipW    = reactChipW(countStr);
        if (x + chipW > left + width)
            break;

        const bool mine =
            std::any_of(r.users.begin(), r.users.end(), [&me](const UserId &u) { return u == me; });
        const bool hovered = _hoveredReaction.first == index && _hoveredReaction.second == j;

        const QRect chip(x, top, chipW, chipH);
        if (mine) {
            // Slack: own reactions get a blue border on a light blue fill
            p.setPen(Th::c().text.link);
            p.setBrush(Th::c().message.mentionBg);
        } else if (hovered) {
            // Hovering a foreign chip outlines it without changing the fill
            p.setPen(Th::c().text.secondary);
            p.setBrush(Th::c().surface.highlight);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().surface.highlight);
        }
        p.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), chipH / 2.0, chipH / 2.0);

        const QColor textCol = mine ? Th::c().text.link : Th::c().text.primary;

        // Emoji — drawn in fixed left slot (custom emojis as downloaded images)
        if (!emoji.imageUrl.isEmpty()) {
            const QPixmap px = _imgCache ? _imgCache->get(emoji.imageUrl) : QPixmap();
            if (!px.isNull()) {
                const QSize tgt = px.size().scaled(kReactEmoji, kReactEmoji, Qt::KeepAspectRatio);
                p.drawPixmap(
                    QRect(
                        chip.x() + kReactPad,
                        chip.y() + (chipH - tgt.height()) / 2,
                        tgt.width(),
                        tgt.height()
                    ),
                    px
                );
            }
        } else {
            p.setFont(kEmojiF);
            p.setPen(textCol);
            p.drawText(
                QRect(chip.x() + kReactPad, chip.y(), kEmojiSlot, chipH),
                Qt::AlignVCenter | Qt::AlignLeft,
                emoji.unicode
            );
        }

        // Count — drawn right after emoji slot
        p.setFont(kCountF);
        p.setPen(textCol);
        p.drawText(
            QRect(
                chip.x() + kReactPad + kEmojiSlot, chip.y(), chipW - kReactPad - kEmojiSlot, chipH
            ),
            Qt::AlignVCenter | Qt::AlignLeft,
            countStr
        );

        x += chipW + 4;
    }
    p.restore();
}

// ── File chips ────────────────────────────────────────────────────────────────

void MessageListWidget::paintFileChips(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top
) const {
    const int left   = ctx.textLeft;
    const int width  = ctx.textWidth;
    bool      anyImg = false;
    for (const auto &f : item.msg.files)
        if (f.hasPreview()) {
            anyImg = true;
            break;
        }
    const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;

    int  y     = top;
    bool first = true;
    for (const auto &f : item.msg.files) {
        if (f.hasPreview())
            continue;
        if (!first || hasAbove)
            y += kFileChipGap;
        first = false;
        MsgRender::paintFileChip(p, f, QRect(left, y, width, kFileChipH));
        y += kFileChipH;
    }
}

const File *MessageListWidget::fileChipAt(const QPoint &viewportPos) const {
    const PaintContext ctx       = makePaintContext();
    const int          scrollY   = ctx.scrollY;
    const int          textLeft  = ctx.textLeft;
    const int          textWidth = ctx.textWidth;

    for (int i = 0; i < (int)_items.size(); ++i) {
        const int rowTop = _tops[i] - scrollY;
        if (rowTop > viewportPos.y())
            break;
        if (rowTop + rowHeight(i) <= viewportPos.y())
            continue;

        const auto &item = _items[i];

        // Quick check: are there any files rendered as chips?
        bool hasChips = false;
        for (const auto &f : item.msg.files)
            if (!f.hasPreview()) {
                hasChips = true;
                break;
            }
        if (!hasChips)
            continue;

        // Reproduce the contentY tracking from paintRow up to the file chips section.
        ensureDocLayout(item);
        const bool collapsed = isCollapsed(i);
        const int  padV      = collapsed ? kPadVCollapsed : kPadV;
        const int  sep2      = needsDateSep(i) ? kSepH : 0;
        const int  pinnedH2  = item.msg.pinned ? 18 : 0;
        int        chipY =
            rowTop + sep2 + padV + pinnedH2 + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;
        for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai)
            chipY += kAttachGap + attachTotalH(item, ai);

        const bool hasAbove0 = item.docHeight > 0 || !item.attachDocs.empty();
        bool       anyImg    = false;
        for (const auto &f : item.msg.files) {
            if (!f.hasPreview())
                continue;
            anyImg = true;
            chipY += (hasAbove0 ? kImgGap : 0) + kImgNameH + filePreviewSize(f, textWidth).height();
        }

        const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
        bool       firstChip     = true;
        for (const auto &f : item.msg.files) {
            if (f.hasPreview())
                continue;
            if (!firstChip || hasAboveChips)
                chipY += kFileChipGap;
            firstChip       = false;
            const int chipW = std::min(textWidth, kFileChipMaxW);
            if (QRect(textLeft, chipY, chipW, kFileChipH).contains(viewportPos))
                return &f;
            chipY += kFileChipH;
        }
    }
    return nullptr;
}

const File *MessageListWidget::previewFileAt(const QPoint &viewportPos) const {
    const auto [mi, fi] = _hoveredFile;
    if (mi < 0 || mi >= (int)_items.size())
        return nullptr;
    const auto &files = _items[mi].msg.files;
    if (fi < 0 || fi >= (int)files.size())
        return nullptr;
    const File &f = files[fi];
    if (!f.hasPreview())
        return nullptr;
    if (!fileViewportRect(mi, fi).contains(viewportPos))
        return nullptr;
    return &f;
}

// ── Reply bar ─────────────────────────────────────────────────────────────────

// Small rounded-square avatar used in the reply bar (matches Slack's official client).
static void paintReplyAvatar(
    QPainter      &p,
    const QPixmap &px,
    const QRect   &rect,
    const QString &fallbackInitial,
    const QColor  &bgColor
) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect), 4, 4);
    p.setClipPath(clip);

    if (!px.isNull()) {
        const qreal dpr = p.device()->devicePixelRatioF();
        QPixmap     scaled =
            px.scaled(rect.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(rect, scaled);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(bgColor);
        p.drawRoundedRect(QRectF(rect), 4, 4);
        p.setPen(Qt::white);
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 0.75);
        p.setFont(f);
        const QChar ch = fallbackInitial.isEmpty() ? QChar('?') : fallbackInitial[0].toUpper();
        p.drawText(rect, Qt::AlignCenter, ch);
    }
    p.restore();
}

void MessageListWidget::paintReplyBar(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
) const {
    const int count = item.msg.replyCount;
    if (count <= 0)
        return;

    const bool  hovered = (index == _hoveredReplyRow);
    const int   barW    = ctx.textWidth / 2;
    const QRect bar(ctx.textLeft, top, barW, kReplyBarH);

    // ── Background / border on hover ─────────────────────────────────────────
    if (hovered) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Th::c().message.replyBarHoverBorder, 1));
        p.setBrush(Th::c().message.replyBarHover);
        p.drawRoundedRect(QRectF(bar).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.restore();
    }

    // Fixed inset so content never shifts between normal and hover states
    static constexpr int kInnerPad = 6;
    int                  x         = bar.left() + kInnerPad;
    const int            avY       = bar.top() + (kReplyBarH - kThreadAvSize) / 2;

    // ── Avatars (side by side, rounded squares — Slack style) ─────────────────
    const int maxAv = std::min((int)item.msg.replyUsers.size(), 5);
    for (int i = 0; i < maxAv; ++i) {
        const UserId &uid  = item.msg.replyUsers[i];
        auto         *user = _session ? _session->findUser(uid) : nullptr;
        QPixmap       px;
        QString       initial;
        QColor        bg;
        if (user) {
            initial = user->displayName;
            if (!user->avatarUrl.isEmpty() && _imgCache)
                px = _imgCache->get(user->avatarUrl);
        } else {
            initial = uid.value;
        }
        const QChar ch = initial.isEmpty() ? QChar('?') : initial[0];
        bg             = QColor::fromHsl(
            ch.unicode() * 37 % 360,
            Th::c().message.avatarHslSaturation,
            Th::c().message.avatarHslLightness
        );

        const QRect avRect(
            x + i * (kThreadAvSize + kThreadAvGap), avY, kThreadAvSize, kThreadAvSize
        );
        paintReplyAvatar(p, px, avRect, initial, bg);
    }

    if (maxAv > 0)
        x += maxAv * (kThreadAvSize + kThreadAvGap) - kThreadAvGap + 8;

    // ── Text ──────────────────────────────────────────────────────────────────
    p.save();
    QFont boldF = QApplication::font();
    boldF.setBold(true);
    boldF.setPointSizeF(boldF.pointSizeF() * 0.88);
    p.setFont(boldF);
    p.setPen(Th::c().message.replyLink);

    const QString countLabel = count == 1 ? tr("1 reply") : tr("%1 replies").arg(count);
    const int     countW     = p.fontMetrics().horizontalAdvance(countLabel);
    p.drawText(x, bar.top(), countW, kReplyBarH, Qt::AlignVCenter | Qt::AlignLeft, countLabel);
    x += countW + 6;

    QFont normF = QApplication::font();
    normF.setPointSizeF(normF.pointSizeF() * 0.88);
    p.setFont(normF);
    p.setPen(Th::c().text.secondary);

    if (hovered) {
        p.drawText(
            x,
            bar.top(),
            bar.right() - x - kInnerPad - 16,
            kReplyBarH,
            Qt::AlignVCenter | Qt::AlignLeft,
            tr("View thread")
        );
        p.drawText(
            bar.right() - kInnerPad - 14,
            bar.top(),
            14,
            kReplyBarH,
            Qt::AlignVCenter | Qt::AlignRight,
            "›"
        );
    } else {
        QString sub;
        if (item.msg.latestReply) {
            const QString when = MsgRender::lastReplyLabel(*item.msg.latestReply);
            sub                = when.isEmpty() ? tr("Last reply") : tr("Last reply %1").arg(when);
        }
        if (!sub.isEmpty())
            p.drawText(
                x, bar.top(), bar.right() - x, kReplyBarH, Qt::AlignVCenter | Qt::AlignLeft, sub
            );
    }
    p.restore();
}

int MessageListWidget::replyBarIndexAt(const QPoint &viewportPos) const {
    if (_isThreadMode)
        return -1;
    const PaintContext ctx       = makePaintContext();
    const int          scrollY   = ctx.scrollY;
    const int          textLeft  = ctx.textLeft;
    const int          textWidth = ctx.textWidth;

    for (int i = 0; i < (int)_items.size(); ++i) {
        if (_items[i].msg.replyCount <= 0)
            continue;
        const int rowTop = _tops[i] - scrollY;
        const int rh     = rowHeight(i);
        if (rowTop > viewportPos.y())
            break;
        if (rowTop + rh <= viewportPos.y())
            continue;

        ensureDocLayout(_items[i]);
        const bool collapsed = isCollapsed(i);
        const int  padV      = collapsed ? kPadVCollapsed : kPadV;
        const int  sep3      = needsDateSep(i) ? kSepH : 0;
        const int  pinnedH3  = _items[i].msg.pinned ? 18 : 0;
        int        y         = rowTop + sep3 + padV + pinnedH3 + (collapsed ? 0 : kHdrH + kHdrGap) +
                _items[i].docHeight;
        for (int ai = 0; ai < (int)_items[i].attachDocs.size(); ++ai) {
            if (isDismissed(_items[i].msg.ts, ai))
                continue;
            y += kAttachGap + attachTotalH(_items[i], ai);
        }

        // Inline file previews (images + prerendered docs) — mirror paint.
        const bool hasAboveImages = _items[i].docHeight > 0 || !_items[i].attachDocs.empty();
        bool       anyImgFiles    = false;
        for (const auto &f : _items[i].msg.files) {
            if (!f.hasPreview())
                continue;
            anyImgFiles      = true;
            const int imgGap = hasAboveImages ? kImgGap : 0;
            y += imgGap + kImgNameH + filePreviewSize(f, textWidth).height();
        }

        // File chips (files without a preview) — mirror paint.
        const bool hasAboveChips =
            _items[i].docHeight > 0 || !_items[i].attachDocs.empty() || anyImgFiles;
        bool firstChip = true;
        for (const auto &f : _items[i].msg.files) {
            if (f.hasPreview())
                continue;
            if (!firstChip || hasAboveChips)
                y += kFileChipGap;
            firstChip = false;
            y += kFileChipH;
        }

        if (!_items[i].msg.reactions.empty())
            y += kReactH + 2;
        y += kReplyBarGap;

        if (QRect(textLeft, y, textWidth / 2, kReplyBarH).contains(viewportPos))
            return i;
    }
    return -1;
}

// ── Reaction hit-test ─────────────────────────────────────────────────────────

std::pair<int, int>
MessageListWidget::reactionAt(const QPoint &viewportPos, QRect *outChipRect) const {
    const PaintContext ctx       = makePaintContext();
    const int          scrollY   = ctx.scrollY;
    const int          textLeft  = ctx.textLeft;
    const int          textWidth = ctx.textWidth;

    for (int i = 0; i < (int)_items.size(); ++i) {
        if (_items[i].msg.reactions.empty())
            continue;
        const int rowTop = _tops[i] - scrollY;
        const int rh     = rowHeight(i);
        if (rowTop > viewportPos.y())
            break;
        if (rowTop + rh <= viewportPos.y())
            continue;

        ensureDocLayout(_items[i]);
        const auto &item      = _items[i];
        const bool  collapsed = isCollapsed(i);
        const int   padV      = collapsed ? kPadVCollapsed : kPadV;
        const int   sep       = needsDateSep(i) ? kSepH : 0;
        const int   pinH      = item.msg.pinned ? 18 : 0;

        int y = rowTop + sep + pinH + padV + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;

        for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
            if (!isDismissed(item.msg.ts, ai))
                y += kAttachGap + attachTotalH(item, ai);
        }

        // File previews
        const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
        for (const auto &f : item.msg.files) {
            if (!f.hasPreview())
                continue;
            y += hasAbove ? kImgGap : 0;
            y += kImgNameH + filePreviewSize(f, textWidth).height();
        }

        // File chips
        bool anyImg = false;
        for (const auto &f : item.msg.files)
            if (f.hasPreview()) {
                anyImg = true;
                break;
            }
        const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
        bool       firstChip     = true;
        for (const auto &f : item.msg.files) {
            if (f.hasPreview())
                continue;
            if (!firstChip || hasAboveChips)
                y += kFileChipGap;
            firstChip = false;
            y += kFileChipH;
        }

        const int reactTop = y + 2;
        if (viewportPos.y() < reactTop || viewportPos.y() >= reactTop + kReactH)
            continue;

        // Check which chip — sizes must match paintReactions exactly
        int x = textLeft;
        for (int j = 0; j < (int)item.msg.reactions.size(); ++j) {
            const QString countStr = " " + QString::number(item.msg.reactions[j].count);
            const int     chipW    = reactChipW(countStr);
            if (x + chipW > textLeft + textWidth)
                break;
            if (viewportPos.x() >= x && viewportPos.x() < x + chipW) {
                if (outChipRect)
                    *outChipRect = QRect(x, reactTop, chipW, kReactH);
                return {i, j};
            }
            x += chipW + 4;
        }
    }
    return {-1, -1};
}

// ── Hover toolbar ─────────────────────────────────────────────────────────────

QRect MessageListWidget::toolbarButtonRect(int btn, int rowTop, int rowH) const {
    // Toolbar card sits at the top-right of the row, vertically centered.
    const int vw       = viewport()->width();
    const int nButtons = 3;
    const int cardW = kToolbarPadH * 2 + nButtons * kToolbarBtnSize + (nButtons - 1) * kToolbarGap;
    const int cardH = kToolbarPadV * 2 + kToolbarBtnSize;
    const int cardTop  = rowTop - cardH / 2; // straddle the top edge (Slack style)
    const int cardLeft = vw - kToolbarRight - cardW;

    const int btnX = cardLeft + kToolbarPadH + btn * (kToolbarBtnSize + kToolbarGap);
    const int btnY = cardTop + kToolbarPadV;
    return QRect(btnX, btnY, kToolbarBtnSize, kToolbarBtnSize);
}

int MessageListWidget::toolbarButtonAt(const QPoint &viewportPos) const {
    if (_hoveredRow < 0 || _hoveredRow >= (int)_tops.size())
        return -1;
    const int scrollY = verticalScrollBar()->value();
    const int rowTop  = _tops[_hoveredRow] - scrollY;
    const int rh      = rowHeight(_hoveredRow);
    const int sep     = needsDateSep(_hoveredRow) ? kSepH : 0;
    for (int b = 0; b < 3; ++b)
        if (toolbarButtonRect(b, rowTop + sep, rh - sep).contains(viewportPos))
            return b;
    return -1;
}

void MessageListWidget::paintHoverToolbar(QPainter &p, int index, int rowTop, int rowH) const {
    const int sep    = needsDateSep(index) ? kSepH : 0;
    const int msgTop = rowTop + sep;
    const int msgH   = rowH - sep;
    (void)msgH;

    const int vw       = viewport()->width();
    const int nButtons = 3;
    const int cardW = kToolbarPadH * 2 + nButtons * kToolbarBtnSize + (nButtons - 1) * kToolbarGap;
    const int cardH = kToolbarPadV * 2 + kToolbarBtnSize;
    const int cardTop  = msgTop - cardH / 2;
    const int cardLeft = vw - kToolbarRight - cardW;

    // Card background with shadow
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF cardRect(cardLeft, cardTop, cardW, cardH);
    Paint::toolbarCard(p, cardRect, kToolbarRadius);

    // SVG icons: 0=emoji (smile), 1=forward, 2=more-horizontal
    // NOTE: captures Th::c().icon.strong once at first bake (acceptable for V1),
    // but re-bakes when the device pixel ratio changes so the icons stay crisp
    // at fractional scale (and after a move between differently-scaled monitors).
    static const QSize kIconSz(16, 16);
    static qreal       kIconDpr = 0;
    static QPixmap     kPxSmile, kPxForward, kPxMore;
    if (const qreal d = p.device()->devicePixelRatioF(); !qFuzzyCompare(d, kIconDpr)) {
        kIconDpr            = d;
        const QColor kColor = Th::c().icon.strong;
        kPxSmile            = svgPixmapPhys(":/ui/smile.svg", kIconSz, kColor, d);
        kPxForward          = svgPixmapPhys(":/ui/forward.svg", kIconSz, kColor, d);
        kPxMore             = svgPixmapPhys(":/ui/more-horizontal.svg", kIconSz, kColor, d);
    }
    const QPixmap *kIcons[] = {&kPxSmile, &kPxForward, &kPxMore};

    for (int b = 0; b < nButtons; ++b) {
        const QRect br = toolbarButtonRect(b, msgTop, rowH - sep);

        // Hovered button gets a slight tint
        if (b == _hoveredToolBtn) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 14));
            p.drawRoundedRect(QRectF(br).adjusted(1, 1, -1, -1), 5, 5);
        }

        if (!kIcons[b]->isNull()) {
            const int ix = br.left() + (br.width() - kIconSz.width()) / 2;
            const int iy = br.top() + (br.height() - kIconSz.height()) / 2;
            p.drawPixmap(ix, iy, *kIcons[b]);
        }
    }
    p.restore();
    (void)index;
}

// ── File action bar ───────────────────────────────────────────────────────────

QRect MessageListWidget::fileViewportRect(int msgIdx, int fileIdx) const {
    if (msgIdx < 0 || msgIdx >= (int)_items.size())
        return {};
    const auto &item = _items[msgIdx];
    if (fileIdx < 0 || fileIdx >= (int)item.msg.files.size())
        return {};

    ensureDocLayout(item);
    const PaintContext ctx       = makePaintContext();
    const int          scrollY   = ctx.scrollY;
    const int          left      = ctx.textLeft;
    const int          width     = ctx.textWidth;
    const bool         collapsed = isCollapsed(msgIdx);
    const int          padV      = collapsed ? kPadVCollapsed : kPadV;
    const int          sep       = needsDateSep(msgIdx) ? kSepH : 0;
    const int          pinnedH   = item.msg.pinned ? 18 : 0;
    const int          rowTop    = _tops[msgIdx] - scrollY;

    // Replicate contentY buildup from paintRow up to the file sections.
    int contentY =
        rowTop + sep + pinnedH + padV + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;
    for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
        if (!isDismissed(item.msg.ts, ai))
            contentY += kAttachGap + attachTotalH(item, ai);
    }

    // Walk file previews (same logic as paintFileImages).
    int        y        = contentY;
    const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
    for (int fi = 0; fi < (int)item.msg.files.size(); ++fi) {
        const auto &f = item.msg.files[fi];
        if (!f.hasPreview())
            continue;
        if (hasAbove)
            y += kImgGap;
        y += kImgNameH; // name label height; image starts below this
        const QSize sz = filePreviewSize(f, width);
        if (fi == fileIdx)
            return QRect(left, y, sz.width(), sz.height());
        y += sz.height();
    }

    // Walk file chips (same logic as paintFileChips).
    bool anyImg = false;
    for (const auto &f : item.msg.files)
        if (f.hasPreview()) {
            anyImg = true;
            break;
        }
    const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
    bool       firstChip     = true;
    for (int fi = 0; fi < (int)item.msg.files.size(); ++fi) {
        const auto &f = item.msg.files[fi];
        if (f.hasPreview())
            continue;
        if (!firstChip || hasAboveChips)
            y += kFileChipGap;
        firstChip = false;
        if (fi == fileIdx) {
            const int chipW = std::min(width, kFileChipMaxW);
            return QRect(left, y, chipW, kFileChipH);
        }
        y += kFileChipH;
    }
    return {};
}

QRect MessageListWidget::fileActionBarButtonRect(int btn, const QRect &fileRect) const {
    const int nButtons = 3;
    const int cardW = kToolbarPadH * 2 + nButtons * kToolbarBtnSize + (nButtons - 1) * kToolbarGap;
    const int cardH = kToolbarPadV * 2 + kToolbarBtnSize;
    const int cardTop  = fileRect.top() - cardH / 2;
    const int cardLeft = fileRect.right() - cardW;
    const int btnX     = cardLeft + kToolbarPadH + btn * (kToolbarBtnSize + kToolbarGap);
    const int btnY     = cardTop + kToolbarPadV;
    return QRect(btnX, btnY, kToolbarBtnSize, kToolbarBtnSize);
}

int MessageListWidget::fileActionBarButtonAt(const QPoint &viewportPos) const {
    const auto [mi, fi] = _hoveredFile;
    if (mi < 0)
        return -1;
    const QRect fr = fileViewportRect(mi, fi);
    if (fr.isNull())
        return -1;
    for (int b = 0; b < 3; ++b) {
        if (fileActionBarButtonRect(b, fr).contains(viewportPos))
            return b;
    }
    return -1;
}

void MessageListWidget::paintFileActionBar(QPainter &p, const QRect &fileRect) const {
    const int nButtons = 3;
    const int cardW = kToolbarPadH * 2 + nButtons * kToolbarBtnSize + (nButtons - 1) * kToolbarGap;
    const int cardH = kToolbarPadV * 2 + kToolbarBtnSize;
    const int cardTop  = fileRect.top() - cardH / 2;
    const int cardLeft = fileRect.right() - cardW;

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF cardRect(cardLeft, cardTop, cardW, cardH);
    Paint::toolbarCard(p, cardRect, kToolbarRadius);

    // NOTE: captures Th::c().icon.strong once at first bake (acceptable for V1),
    // but re-bakes when the device pixel ratio changes so the icons stay crisp
    // at fractional scale (and after a move between differently-scaled monitors).
    static const QSize kIconSz(16, 16);
    static qreal       kIconDpr = 0;
    static QPixmap     kPxDownload, kPxShare, kPxMore;
    if (const qreal d = p.device()->devicePixelRatioF(); !qFuzzyCompare(d, kIconDpr)) {
        kIconDpr            = d;
        const QColor kColor = Th::c().icon.strong;
        kPxDownload         = svgPixmapPhys(":/ui/download.svg", kIconSz, kColor, d);
        kPxShare            = svgPixmapPhys(":/ui/share-2.svg", kIconSz, kColor, d);
        kPxMore             = svgPixmapPhys(":/ui/more-horizontal.svg", kIconSz, kColor, d);
    }
    const QPixmap *kIcons[] = {&kPxDownload, &kPxShare, &kPxMore};

    for (int b = 0; b < nButtons; ++b) {
        const QRect br = fileActionBarButtonRect(b, fileRect);

        if (b == _hoveredFileBtn) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 14));
            p.drawRoundedRect(QRectF(br).adjusted(1, 1, -1, -1), 5, 5);
        }

        if (!kIcons[b]->isNull()) {
            const int ix = br.left() + (br.width() - kIconSz.width()) / 2;
            const int iy = br.top() + (br.height() - kIconSz.height()) / 2;
            p.drawPixmap(ix, iy, *kIcons[b]);
        }
    }
    p.restore();
}

// ── Intro header + date separator painting ────────────────────────────────────

void MessageListWidget::paintIntro(QPainter &p, int top) const {
    const int vw   = viewport()->width();
    const int padX = kPadH * 2;

    QFont nameFont = QApplication::font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.55);
    nameFont.setWeight(QFont::Medium);

    p.save();
    p.setFont(nameFont);
    p.setPen(Th::c().text.primary);
    const QRect nameRect(padX, top + kIntroPadTop, vw - padX * 2, kIntroNameH);
    p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, _convName);

    if (!_convDescription.isEmpty()) {
        QFont descFont = QApplication::font();
        p.setFont(descFont);
        p.setPen(Th::c().text.secondary);
        const int descY = top + kIntroPadTop + kIntroNameH + kIntroGap;
        p.drawText(
            QRect(padX, descY, vw - padX * 2, kIntroDescH),
            Qt::AlignLeft | Qt::AlignVCenter,
            _convDescription
        );
    }
    p.restore();
}

void MessageListWidget::paintDateSep(QPainter &p, int top, int vw, qint64 dateMicros) const {
    const QString label = MsgRender::formatDateLabel(dateMicros);
    if (label.isEmpty())
        return;

    QFont font = QApplication::font();
    font.setPointSizeF(font.pointSizeF() * 0.82);
    const QFontMetrics fm(font);
    const int          pillW = fm.horizontalAdvance(label) + 20;
    const int          pillH = 20;
    const int          midY  = top + kSepH / 2;
    const int          pillX = (vw - pillW) / 2;

    p.save();
    p.setPen(Th::c().divider.def);
    p.drawLine(kPadH, midY, pillX - 8, midY);
    p.drawLine(pillX + pillW + 8, midY, vw - kPadH, midY);

    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Th::c().divider.def);
    p.setBrush(Th::c().surface.content);
    const QRect pill(pillX, midY - pillH / 2, pillW, pillH);
    Paint::pill(p, pill);

    p.setFont(font);
    p.setPen(Th::c().text.tertiary);
    p.drawText(pill, Qt::AlignCenter, label);
    p.restore();
}

namespace {
QFont systemLineFont() {
    QFont font = QApplication::font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    return font;
}
} // namespace

int MessageListWidget::systemRowHeight() const {
    const QFontMetrics fm(systemLineFont());
    return fm.height() + 2 * kSysRowPadV;
}

void MessageListWidget::paintSystemRow(
    QPainter &p, int index, int msgTop, const PaintContext &ctx
) const {
    const Message &msg = _items[index].msg;

    QString text = MsgRender::notificationText(msg.text, _session);
    if (text.isEmpty() && msg.subtype)
        text = *msg.subtype; // last-resort label if Slack sent no text

    const QFont        font = systemLineFont();
    const QFontMetrics fm(font);
    const int          availW = std::max(0, ctx.vw - 2 * kPadH);
    const QString      elided = fm.elidedText(text, Qt::ElideRight, availW);

    p.save();
    p.setFont(font);
    p.setPen(Th::c().text.tertiary);
    p.drawText(
        QRect(kPadH, msgTop + kSysRowPadV, availW, fm.height()),
        Qt::AlignHCenter | Qt::AlignVCenter,
        elided
    );
    p.restore();
}
