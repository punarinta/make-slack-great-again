// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_list.h"
#include "message_render.h"
#include "session/session.h"
#include "ui/theme.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "util/emoji_font.h"
#include "util/relative_time.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QTextCursor>
#include <QApplication>

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
    p.fillRect(event->rect(), Th::c().surface.content);

    if ((_loading || _waiting) && _items.empty()) {
        _loadingAnim.paint(p, viewport()->rect());

        if (_loadingElapsedTimer.isValid()) {
            const qint64 ms = _loadingElapsedTimer.elapsed();
            QString      hint;
            if (ms >= 15000)
                hint = tr("Oh my gosh, I really apologize, but your company is a reaaaly active "
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

    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        const int rowTop = _tops[i] - scrollY;
        const int rh     = rowHeight(i);
        if (rowTop + rh < 0)
            continue;
        if (rowTop > vh)
            break;
        paintRow(p, i, rowTop, ctx);
    }

    // Thin Telegram-style scrollbar overlay
    paintScrollThumb(p, _totalH, QColor(0, 0, 0, 80));
}

void MessageListWidget::paintRow(QPainter &p, int index, int rowTop, const PaintContext &ctx)
    const {
    const auto &item = _items[index];
    ensureDocLayout(item);
    const bool collapsed = isCollapsed(index);

    // Paint date separator at the top of the row if needed, then shift content down.
    const int sepH = needsDateSep(index) ? kSepH : 0;
    if (sepH > 0)
        paintDateSep(p, rowTop, ctx.vw, item.msg.ts);
    const int msgTop = rowTop + sepH;

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

    // ── Pinned banner — sits at the very top of the message, before padding ──
    const int pinnedBannerH = item.msg.pinned ? 18 : 0;
    if (item.msg.pinned) {
        const QRect bannerRect(0, msgTop, vw, pinnedBannerH);
        p.fillRect(bannerRect, Th::c().message.pinnedBg); // subtle yellow tint

        // Pin icon
        // NOTE: static — captures theme value once at first paint (acceptable for V1).
        static const QPixmap kPinPx =
            svgPixmap(":/ui/pin.svg", QSize(12, 12), Th::c().message.attachmentDismiss);
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

        QFont tsFont = QApplication::font();
        tsFont.setPointSizeF(tsFont.pointSizeF() * 0.85);
        p.setFont(tsFont);
        p.setPen(Th::c().text.secondary);
        const QFontMetrics tsFm(tsFont);
        const QString      tsText = MsgRender::formatTs(item.msg.ts);
        // Align timestamp to the same baseline as the bold name
        p.drawText(textLeft + nameW + 8, headerBaseline, tsText);

        if (item.msg.edited) {
            const int tsW = tsFm.horizontalAdvance(tsText);
            p.drawText(textLeft + nameW + 8 + tsW + 6, headerBaseline, tr("(edited)"));
        }
    }

    // ── Message text via QTextDocument ───────────────────────────────
    p.setFont(QApplication::font());
    int contentY = collapsed ? contTop : (contTop + kHdrH + kHdrGap);
    p.save();
    p.translate(textLeft, contentY);
    {
        QAbstractTextDocumentLayout::PaintContext pCtx;
        pCtx.palette = QApplication::palette();
        pCtx.clip    = QRectF(0, 0, textWidth, item.docHeight);

        // Compute normalized selection for this row.
        if (_selAnchor.row >= 0 && _selFocus.row >= 0) {
            int aRow = _selAnchor.row, aOff = _selAnchor.offset;
            int fRow = _selFocus.row,  fOff = _selFocus.offset;
            if (aRow > fRow || (aRow == fRow && aOff > fOff)) {
                std::swap(aRow, fRow);
                std::swap(aOff, fOff);
            }
            int selFrom = -1, selTo = -1;
            if (index == aRow && index == fRow) {
                selFrom = aOff; selTo = fOff;
            } else if (index == aRow) {
                selFrom = aOff; selTo = item.textDoc->characterCount();
            } else if (index == fRow) {
                selFrom = 0;    selTo = fOff;
            } else if (index > aRow && index < fRow) {
                selFrom = 0;    selTo = item.textDoc->characterCount();
            }
            if (selFrom >= 0 && selTo > selFrom) {
                QAbstractTextDocumentLayout::Selection sel;
                QTextCursor cur(item.textDoc.get());
                cur.setPosition(selFrom);
                cur.setPosition(selTo, QTextCursor::KeepAnchor);
                sel.cursor = cur;
                sel.format.setBackground(QApplication::palette().highlight());
                sel.format.setForeground(QApplication::palette().highlightedText());
                pCtx.selections.append(sel);
            }
        }

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

    // ── Inline file images ───────────────────────────────────────────
    paintFileImages(p, item, ctx, contentY);
    {
        const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
        bool       anyImg   = false;
        for (const auto &f : item.msg.files) {
            if (!f.isImage())
                continue;
            anyImg               = true;
            const QString imgUrl = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
            const int     imgGap = hasAbove ? kImgGap : 0;
            auto          it     = _fileImages.constFind(imgUrl);
            if (it != _fileImages.constEnd() && !it->isNull()) {
                const auto  &px    = it.value();
                const double scale = std::min(
                    1.0, std::min((double)kImgMaxW / px.width(), (double)kImgMaxH / px.height())
                );
                contentY += imgGap + kImgNameH + static_cast<int>(px.height() * scale);
            } else {
                contentY += imgGap + kImgNameH + 24;
            }
        }
        (void)anyImg;
    }

    // ── Non-image file chips ─────────────────────────────────────────
    paintFileChips(p, item, ctx, contentY);
    {
        bool anyImg = false;
        for (const auto &f : item.msg.files)
            if (f.isImage()) {
                anyImg = true;
                break;
            }
        const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
        bool       firstChip     = true;
        for (const auto &f : item.msg.files) {
            if (f.isImage())
                continue;
            if (!firstChip || hasAboveChips)
                contentY += kFileChipGap;
            firstChip = false;
            contentY += kFileChipH;
        }
    }

    // ── Reactions ────────────────────────────────────────────────────
    if (!item.msg.reactions.empty())
        paintReactions(p, item, ctx, contentY + 2);

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
        const QString tsText  = MsgRender::formatTs(item.msg.ts);
        const int     tsRight = kPadH + kAvSize;
        p.drawText(
            QRect(0, contTop, tsRight, msgH - 2 * kPadVCollapsed),
            Qt::AlignRight | Qt::AlignVCenter,
            tsText
        );
        p.restore();
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
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(rect), 4, 4);
            p.setClipPath(clip);
            const qreal dpr    = p.device()->devicePixelRatioF();
            QPixmap     scaled = cached.scaled(
                rect.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
            );
            scaled.setDevicePixelRatio(dpr);
            p.drawPixmap(rect, scaled);
            p.restore();
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
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromHsl(
        hue, Th::c().message.avatarHslSaturation, Th::c().message.avatarHslLightness
    ));
    p.drawRoundedRect(rect, 4, 4);

    p.setPen(Qt::white);
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPointSize(14);
    p.setFont(f);
    p.drawText(rect, Qt::AlignCenter, ch.toUpper());
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

        // Favicon: draw 16×16 to the left of the title, only if it loaded successfully.
        // We indent the text doc by 20px horizontally so the text starts right of the icon.
        int textIndent = 0;
        if (!att.faviconUrl.isEmpty() && _imgCache) {
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
            p.translate(textX + textIndent, y);
            ad.textDoc->drawContents(&p, QRectF(0, 0, textW - textIndent, docH));
            p.restore();
        }

        // Preview image (thumbUrl preferred over imageUrl)
        if (imgH > 0 && _imgCache) {
            const QString imgUrl = att.thumbUrl.isEmpty() ? att.imageUrl : att.thumbUrl;
            const QPixmap img    = _imgCache->get(imgUrl);
            if (!img.isNull()) {
                const double scale = std::min(
                    1.0, std::min((double)kImgMaxW / img.width(), (double)kImgMaxH / img.height())
                );
                const int iw = (int)(img.width() * scale);
                const int ih = (int)(img.height() * scale);
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.drawPixmap(QRect(textX, y + docH + kImgGap, iw, ih), img);
                p.restore();
            }
        }

        y += totalH;
    }
}

// ── Inline file images ────────────────────────────────────────────────────────

void MessageListWidget::paintFileImages(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top
) const {
    const int  left     = ctx.textLeft;
    const int  width    = ctx.textWidth;
    int        y        = top;
    const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
    for (const auto &f : item.msg.files) {
        if (!f.isImage())
            continue;
        const QString imgUrl = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;

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

        if (it != _fileImages.constEnd() && !it->isNull()) {
            const auto  &px    = it.value();
            const double scale = std::min(
                1.0, std::min((double)kImgMaxW / px.width(), (double)kImgMaxH / px.height())
            );
            const int iw = static_cast<int>(px.width() * scale);
            const int ih = static_cast<int>(px.height() * scale);
            p.drawPixmap(QRect(left, y, iw, ih), px);
            y += ih;
        } else {
            // Placeholder while loading — size must match rowHeight() to avoid jumps.
            int phW, phH;
            if (f.imageWidth > 0 && f.imageHeight > 0) {
                const double scale = std::min(
                    1.0, std::min((double)kImgMaxW / f.imageWidth, (double)kImgMaxH / f.imageHeight)
                );
                phW = static_cast<int>(f.imageWidth * scale);
                phH = static_cast<int>(f.imageHeight * scale);
            } else {
                phW = std::min(width, kImgMaxW);
                phH = 24;
            }
            p.save();
            p.setPen(Th::c().message.imagePlaceholderBorder);
            p.setBrush(Th::c().message.imagePlaceholderBg);
            p.drawRect(QRect(left, y, phW, phH));
            p.setPen(Th::c().text.tertiary);
            p.drawText(QRect(left, y, phW, phH), Qt::AlignCenter, tr("Loading image…"));
            p.restore();
            y += phH;
        }
    }
}

void MessageListWidget::triggerMissingDownloads() {
    if (!_session)
        return;
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
                if (!f.isImage())
                    continue;
                const QString url = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
                if (!_fileImages.contains(url)) {
                    needsDownload = true;
                    break;
                }
            }
            if (needsDownload) {
                item.fileImgsRequested = true;
                for (const auto &f : item.msg.files) {
                    if (!f.isImage())
                        continue;
                    const QString url = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
                    if (_fileImages.contains(url))
                        continue;

                    const auto cached = _session->cachedImage(url);
                    if (!cached.isEmpty()) {
                        QPixmap px;
                        if (px.loadFromData(cached) && !px.isNull()) {
                            _fileImages[url] = px;
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
                const QString imgUrl = att.thumbUrl.isEmpty() ? att.imageUrl : att.thumbUrl;
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
                    const QString imgUrl = att.thumbUrl.isEmpty() ? att.imageUrl : att.thumbUrl;
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
static constexpr int kReactPad  = 6;  // horizontal padding inside chip
static constexpr int kEmojiSlot = 15; // fixed pixel budget for one emoji glyph

static int reactChipW(const QString &countStr) {
    static const QFont kCntFont = [] {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * 0.82);
        return f;
    }();
    return kReactPad + kEmojiSlot + QFontMetrics(kCntFont).horizontalAdvance(countStr) + kReactPad;
}

void MessageListWidget::paintReactions(
    QPainter &p, const MessageItem &item, const PaintContext &ctx, int top
) const {
    const int left  = ctx.textLeft;
    const int width = ctx.textWidth;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    static const QFont kEmojiF = emojiFont(14);
    static const QFont kCountF = [] {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * 0.82);
        return f;
    }();
    const int chipH = kReactH;
    int       x     = left;

    const UserId me = _session ? _session->meUserId() : UserId{};

    for (const auto &r : item.msg.reactions) {
        const QString emojiStr = MsgRender::resolveEmoji(r.name);
        const QString countStr = " " + QString::number(r.count);
        const int     chipW    = reactChipW(countStr);
        if (x + chipW > left + width)
            break;

        const bool mine =
            std::any_of(r.users.begin(), r.users.end(), [&me](const UserId &u) { return u == me; });

        const QRect chip(x, top, chipW, chipH);
        if (mine) {
            p.setPen(Th::c().text.link);
            p.setBrush(Th::c().accent.subtleBg);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().surface.highlight);
        }
        p.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), chipH / 2.0, chipH / 2.0);

        const QColor textCol = mine ? Th::c().text.link : Th::c().text.primary;

        // Emoji — drawn in fixed left slot
        p.setFont(kEmojiF);
        p.setPen(textCol);
        p.drawText(
            QRect(chip.x() + kReactPad, chip.y(), kEmojiSlot, chipH),
            Qt::AlignVCenter | Qt::AlignLeft,
            emojiStr
        );

        // Count — drawn right after emoji slot
        p.setFont(kCountF);
        p.setPen(textCol);
        p.drawText(
            QRect(
                chip.x() + kReactPad + kEmojiSlot,
                chip.y() - 1,
                chipW - kReactPad - kEmojiSlot,
                chipH
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
        if (f.isImage()) {
            anyImg = true;
            break;
        }
    const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;

    int                y     = top;
    bool               first = true;
    const QFontMetrics nameFm([] {
        QFont f = QApplication::font();
        f.setBold(true);
        return f;
    }());
    QFont              subFont = QApplication::font();
    subFont.setPointSizeF(subFont.pointSizeF() * 0.82);
    const QFontMetrics subFm(subFont);
    const int          totalTextH = nameFm.height() + 3 + subFm.height();

    for (const auto &f : item.msg.files) {
        if (f.isImage())
            continue;

        if (!first || hasAbove)
            y += kFileChipGap;
        first = false;

        const int   chipW = std::min(width, kFileChipMaxW);
        const QRect chipRect(left, y, chipW, kFileChipH);

        // Clip chip area to rounded shape so icon fill has matching rounded-left corners.
        QPainterPath chipPath;
        chipPath.addRoundedRect(QRectF(chipRect), 4, 4);

        p.save();
        p.setClipPath(chipPath);
        p.fillRect(chipRect, Th::c().message.fileChipBg);
        p.fillRect(QRect(left, y, kFileChipIconW, kFileChipH), MsgRender::fileTypeColor(f));
        p.restore();

        // Card border
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Th::c().message.fileChipBorder);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(chipRect), 4, 4);
        p.restore();

        // Icon label (file extension, e.g. "PDF")
        {
            QFont iconFont = QApplication::font();
            iconFont.setBold(true);
            iconFont.setPointSizeF(iconFont.pointSizeF() * 0.72);
            p.save();
            p.setFont(iconFont);
            p.setPen(Qt::white);
            p.drawText(
                QRect(left, y, kFileChipIconW, kFileChipH),
                Qt::AlignCenter,
                MsgRender::fileIconLabel(f)
            );
            p.restore();
        }

        // Filename + type/size — vertically centred in the card
        const int textX   = left + kFileChipIconW + kFileChipPadX;
        const int textW   = chipW - kFileChipIconW - kFileChipPadX - 8;
        const int textTop = y + (kFileChipH - totalTextH) / 2;

        {
            QFont nameFont = QApplication::font();
            nameFont.setBold(true);
            p.setFont(nameFont);
            p.setPen(Th::c().text.primary);
            const QString elided = nameFm.elidedText(f.name, Qt::ElideRight, textW);
            p.drawText(
                QRect(textX, textTop, textW, nameFm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                elided
            );
        }
        {
            p.setFont(subFont);
            p.setPen(Th::c().text.secondary);
            QString       sub = f.prettyType;
            const QString sz  = MsgRender::formatFileSize(f.size);
            if (!sz.isEmpty())
                sub += (sub.isEmpty() ? "" : " · ") + sz;
            p.drawText(
                QRect(textX, textTop + nameFm.height() + 3, textW, subFm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                sub
            );
        }

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

        // Quick check: are there any non-image files?
        bool hasNonImg = false;
        for (const auto &f : item.msg.files)
            if (!f.isImage()) {
                hasNonImg = true;
                break;
            }
        if (!hasNonImg)
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
            if (!f.isImage())
                continue;
            anyImg            = true;
            const QString url = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
            auto          it  = _fileImages.constFind(url);
            if (it != _fileImages.constEnd() && !it->isNull()) {
                const auto  &px    = it.value();
                const double scale = std::min(
                    1.0, std::min((double)kImgMaxW / px.width(), (double)kImgMaxH / px.height())
                );
                chipY +=
                    (hasAbove0 ? kImgGap : 0) + kImgNameH + static_cast<int>(px.height() * scale);
            } else {
                chipY += (hasAbove0 ? kImgGap : 0) + kImgNameH + 24;
            }
        }

        const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
        bool       firstChip     = true;
        for (const auto &f : item.msg.files) {
            if (f.isImage())
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

// ── Reply bar ─────────────────────────────────────────────────────────────────

static void paintCircularAvatar(
    QPainter      &p,
    const QPixmap &px,
    const QRect   &rect,
    const QString &fallbackInitial,
    const QColor  &bgColor
) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(QRectF(rect));
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
        p.drawEllipse(rect);
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
    const QColor         ringColor = hovered ? Th::c().message.replyBarHover : QColor(Qt::white);

    // ── Avatars ───────────────────────────────────────────────────────────────
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
            x + i * (kThreadAvSize - kThreadAvOver), avY, kThreadAvSize, kThreadAvSize
        );

        // Ring separator between overlapping avatars matches the background
        if (i > 0) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(ringColor);
            p.drawEllipse(avRect.adjusted(-2, -2, 2, 2));
            p.restore();
        }
        paintCircularAvatar(p, px, avRect, initial, bg);
    }

    if (maxAv > 0)
        x += maxAv * (kThreadAvSize - kThreadAvOver) + kThreadAvOver + 6;

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
            const QString rel = relativeTime(*item.msg.latestReply);
            sub               = rel.isEmpty() ? tr("Last reply") : tr("Last reply %1").arg(rel);
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
        for (int ai = 0; ai < (int)_items[i].attachDocs.size(); ++ai)
            y += kAttachGap + attachTotalH(_items[i], ai);

        if (!_items[i].msg.reactions.empty())
            y += kReactH + 2;
        y += kReplyBarGap;

        if (QRect(textLeft, y, textWidth / 2, kReplyBarH).contains(viewportPos))
            return i;
    }
    return -1;
}

// ── Reaction hit-test ─────────────────────────────────────────────────────────

std::pair<int, int> MessageListWidget::reactionAt(const QPoint &viewportPos) const {
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

        // File images
        const bool hasAbove = item.docHeight > 0 || !item.attachDocs.empty();
        for (const auto &f : item.msg.files) {
            if (!f.isImage())
                continue;
            const QString url = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
            y += hasAbove ? kImgGap : 0;
            y += kImgNameH;
            auto it = _fileImages.constFind(url);
            if (it != _fileImages.constEnd() && !it->isNull()) {
                const double sc = std::min(
                    1.0, std::min((double)kImgMaxW / it->width(), (double)kImgMaxH / it->height())
                );
                y += (int)(it->height() * sc);
            } else {
                y += 24;
            }
        }

        // Non-image file chips
        bool anyImg = false;
        for (const auto &f : item.msg.files)
            if (f.isImage()) {
                anyImg = true;
                break;
            }
        const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImg;
        bool       firstChip     = true;
        for (const auto &f : item.msg.files) {
            if (f.isImage())
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
            if (viewportPos.x() >= x && viewportPos.x() < x + chipW)
                return {i, j};
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
    if (_hoveredRow < 0)
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
    // Soft drop shadow
    for (int i = 4; i >= 1; --i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 5 + (4 - i) * 3));
        p.drawRoundedRect(
            cardRect.adjusted(-i, -i, i, i + 1), kToolbarRadius + i, kToolbarRadius + i
        );
    }
    p.setBrush(Qt::white);
    p.setPen(QColor(0, 0, 0, 18));
    p.drawRoundedRect(cardRect, kToolbarRadius, kToolbarRadius);

    // SVG icons: 0=emoji (smile), 1=forward, 2=more-horizontal
    // NOTE: static — captures Th::c().icon.strong once at first paint (acceptable for V1).
    static const QSize    kIconSz(16, 16);
    static const QColor   kIconColor = Th::c().icon.strong;
    static const QPixmap  kPxSmile   = svgPixmap(":/ui/smile.svg", kIconSz, kIconColor);
    static const QPixmap  kPxForward = svgPixmap(":/ui/forward.svg", kIconSz, kIconColor);
    static const QPixmap  kPxMore    = svgPixmap(":/ui/more-horizontal.svg", kIconSz, kIconColor);
    static const QPixmap *kIcons[]   = {&kPxSmile, &kPxForward, &kPxMore};

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

void MessageListWidget::paintDateSep(QPainter &p, int top, int vw, const Ts &ts) const {
    const QString label = MsgRender::formatDateLabel(ts);
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
    p.drawRoundedRect(pill, pillH / 2, pillH / 2);

    p.setFont(font);
    p.setPen(Th::c().text.tertiary);
    p.drawText(pill, Qt::AlignCenter, label);
    p.restore();
}
