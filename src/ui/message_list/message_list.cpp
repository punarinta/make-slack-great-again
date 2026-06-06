// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_list.h"
#include "message_render.h"
#include "session/session.h"
#include "backend/backend.h"
#include "ui/theme.h"
#include "ui/context_menu/context_menu.h"
#include "ui/popup_tooltip/popup_tooltip.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDesktopServices>
#include <QClipboard>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QTimer>
#include <QCursor>

#include <algorithm>
#include <cmath>

// ── MessageListWidget ─────────────────────────────────────────────────────────

MessageListWidget::MessageListWidget(Session *session, QWidget *parent)
    : QAbstractScrollArea(parent)
    , _session(session)
    , _avatarNam(new QNetworkAccessManager(this))
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalScrollBar()->setSingleStep(20);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->installEventFilter(this);

    _tooltip = new PopupTooltip(this);

    // Smooth scroll
    _scrollAnim.setDuration(220);
    _scrollAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&_scrollAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        verticalScrollBar()->setValue(v.toInt());
    });

    // New-message highlight: fades from accent → transparent over 1.2s
    _highlightAnim.setDuration(1200);
    _highlightAnim.setEasingCurve(QEasingCurve::OutQuart);
    _highlightAnim.setStartValue(1.0);
    _highlightAnim.setEndValue(0.0);
    connect(&_highlightAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &) {
        viewport()->update();
    });
    connect(&_highlightAnim, &QVariantAnimation::finished, this, [this] {
        _newMsgTs.clear();
        viewport()->update();
    });
}

void MessageListWidget::smoothScrollTo(int target) {
    if (target == verticalScrollBar()->value()) return;
    _scrollAnim.stop();
    _scrollAnim.setStartValue(verticalScrollBar()->value());
    _scrollAnim.setEndValue(target);
    _scrollAnim.start();
}

bool MessageListWidget::eventFilter(QObject *obj, QEvent *event) {
    if (obj == viewport()) {
        switch (event->type()) {
        case QEvent::Paint:
            doPaint(static_cast<QPaintEvent *>(event));
            return true;
        case QEvent::MouseButtonPress:
            doMousePress(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::MouseButtonRelease:
            doMouseRelease(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::MouseMove:
            doMouseMove(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::Leave:
            doMouseLeave();
            return false;
        default:
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(obj, event);
}

void MessageListWidget::scrollContentsBy(int /*dx*/, int /*dy*/) {
    viewport()->update();
    if (verticalScrollBar()->value() <= 200)
        loadOlderMessages();
}

void MessageListWidget::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    for (auto &item : _items) {
        item.docWidth = 0;  // invalidate so docs re-layout at new width
        for (auto &ad : item.attachDocs)
            ad.docWidth = 0;
    }
    rebuildLayout();
    if (_items.empty()) return;
    if (_pendingRestorePos >= 0) {
        verticalScrollBar()->setValue(
            std::min(_pendingRestorePos, verticalScrollBar()->maximum()));
        _pendingRestorePos = -1;
    } else if (_scrollToBottomPending) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        _scrollToBottomPending = false;
    }
}

// ── Data model ────────────────────────────────────────────────────────────────

std::optional<Message> MessageListWidget::lastOwnMessage(UserId me) const {
    for (int i = static_cast<int>(_items.size()) - 1; i >= 0; --i) {
        const auto &msg = _items[i].msg;
        if (msg.author == me && !msg.subtype)
            return msg;
    }
    return {};
}

void MessageListWidget::clear() {
    _loadLifetime       = rpl::lifetime();
    _olderLoadLifetime  = rpl::lifetime();
    _eventLifetime      = rpl::lifetime();
    _olderCursor        = std::nullopt;
    _loadingOlder       = false;
    _items.clear();
    _tops.clear();
    _totalH = 0;
    _showIntro = false;
    _convName.clear();
    _convDescription.clear();
    verticalScrollBar()->setRange(0, 0);
    viewport()->update();
}

void MessageListWidget::setSession(Session *session) {
    clear();
    _currentConv = {};
    _session = session;
}

void MessageListWidget::openConversation(ConversationId conv, const QString &convName, const QString &description) {
    // Persist messages of the conversation we're leaving before discarding them.
    if (!_currentConv.value.isEmpty() && _session && !_items.empty()) {
        std::vector<Message> msgs;
        msgs.reserve(_items.size());
        for (const auto &item : _items) msgs.push_back(item.msg);
        _session->cacheMessages(_currentConv, msgs);
    }

    // Save scroll position of the conversation we're leaving.
    if (!_currentConv.value.isEmpty())
        _savedScrollPos[_currentConv.value] = verticalScrollBar()->value();

    clear();
    _currentConv      = conv;
    _isThreadMode     = false;
    _threadRootTs     = {};
    _convName         = convName;
    _convDescription  = description;
    _showIntro        = true;

    _session->events()
        | rpl::on_next([this](Event e) { handleEvent(e); }, _eventLifetime);

    // Reset scroll intent — stale state from a previous conversation must not leak.
    _scrollToBottomPending = false;
    _pendingRestorePos     = -1;

    if (_savedScrollPos.contains(conv.value))
        _pendingRestorePos = _savedScrollPos[conv.value];
    else
        _scrollToBottomPending = true;

    // Pre-populate from cache for instant display while network loads.
    const bool hasCached = [&] {
        if (!_session) return false;
        const auto cached = _session->cachedMessages(conv);
        if (cached.empty()) return false;
        for (const auto &msg : cached) appendMessage(msg);
        // Pre-warm the image pixel cache from disk so images appear without download.
        for (const auto &item : _items) {
            for (const auto &f : item.msg.files) {
                if (!f.isImage()) continue;
                const QString url = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
                if (_imageCache.contains(url)) continue;
                const auto data = _session->cachedImage(url);
                if (data.isEmpty()) continue;
                QPixmap px;
                if (px.loadFromData(data) && !px.isNull())
                    _imageCache[url] = px;
            }
        }
        return true;
    }();

    if (hasCached) {
        // Apply scroll position after layout settles (next event-loop tick).
        QTimer::singleShot(0, this, [this, conv] {
            if (_currentConv != conv) return;
            applyPendingScroll();
        });
    }

    // Fetch fresh data from the network; merge it into whatever is already shown.
    _session->backend()->loadHistory(conv, std::nullopt)
        | rpl::on_next([this, conv, hasCached](MessagePage page) {
            if (_currentConv != conv) return;

            _olderCursor = page.olderCursor;

            // Always cache the authoritative network result.
            if (_session) {
                std::vector<Message> msgs(page.messages.begin(), page.messages.end());
                _session->cacheMessages(conv, msgs);
            }

            if (hasCached && !page.messages.empty()) {
                // Merge: network data may differ slightly (edits, reactions) but is
                // mostly the same as the cached view, so the update is imperceptible.
                const bool wasAtBottom =
                    verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
                mergeNetworkMessages(page.messages);
                if (wasAtBottom || _scrollToBottomPending) {
                    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
                    _scrollToBottomPending = false;
                }
                if (_pendingRestorePos >= 0) {
                    verticalScrollBar()->setValue(
                        std::min(_pendingRestorePos, verticalScrollBar()->maximum()));
                    _pendingRestorePos = -1;
                }
            } else {
                // No cached data was shown — normal first-load path.
                for (const auto &msg : page.messages) appendMessage(msg);
                QTimer::singleShot(0, this, [this, conv] {
                    if (_currentConv != conv) return;
                    applyPendingScroll();
                });
            }
        }, _loadLifetime);

}

void MessageListWidget::applyPendingScroll() {
    if (textAreaWidth() <= 0 || _items.empty()) return;
    if (_pendingRestorePos >= 0) {
        verticalScrollBar()->setValue(
            std::min(_pendingRestorePos, verticalScrollBar()->maximum()));
        _pendingRestorePos     = -1;
        _scrollToBottomPending = false;
    } else if (_scrollToBottomPending) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        _scrollToBottomPending = false;
    }
}

void MessageListWidget::loadOlderMessages() {
    if (_loadingOlder || !_olderCursor.has_value() || !_session || _currentConv.value.isEmpty())
        return;

    _loadingOlder = true;
    _olderLoadLifetime = rpl::lifetime();

    const auto conv   = _currentConv;
    const QString cur = *_olderCursor;
    _olderCursor = std::nullopt;

    auto producer = _isThreadMode
        ? _session->backend()->loadThread(conv, _threadRootTs, cur)
        : _session->backend()->loadHistory(conv, cur);

    std::move(producer)
        | rpl::on_next([this, conv](MessagePage page) {
            if (_currentConv != conv) {
                _loadingOlder = false;
                return;
            }

            _olderCursor  = page.olderCursor;
            _loadingOlder = false;

            if (page.messages.empty()) return;

            // Record the pre-insert total height and current scroll so we can
            // shift the scrollbar down by exactly the height added at the top,
            // keeping the visible content from jumping.
            const int prevTotalH = _totalH;
            const int scrollY    = verticalScrollBar()->value();

            mergeNetworkMessages(page.messages);

            const int delta = _totalH - prevTotalH;
            if (delta > 0) {
                _scrollAnim.stop();
                verticalScrollBar()->setValue(scrollY + delta);
            }
        }, _olderLoadLifetime);
}

void MessageListWidget::openThread(ConversationId conv, Ts rootTs) {
    _loadLifetime  = rpl::lifetime();
    _eventLifetime = rpl::lifetime();
    _items.clear();
    _tops.clear();
    _totalH = 0;
    verticalScrollBar()->setRange(0, 0);
    viewport()->update();

    _currentConv      = conv;
    _isThreadMode     = true;
    _threadRootTs     = rootTs;
    _showIntro        = false;
    _scrollToBottomPending = true;
    _pendingRestorePos     = -1;

    if (!_session) return;

    _session->events()
        | rpl::on_next([this](Event e) { handleEvent(e); }, _eventLifetime);

    _session->backend()->loadThread(conv, rootTs, std::nullopt)
        | rpl::on_next([this](MessagePage page) {
            _olderCursor = page.olderCursor;
            for (const auto &msg : page.messages)
                appendMessage(msg);
            QTimer::singleShot(0, this, [this] { applyPendingScroll(); });
        }, _loadLifetime);
}

void MessageListWidget::mergeNetworkMessages(const std::vector<Message> &incoming) {
    // Build ts → index map for the items already displayed.
    QHash<QString, int> tsIdx;
    tsIdx.reserve(static_cast<int>(_items.size()));
    for (int i = 0; i < static_cast<int>(_items.size()); ++i)
        tsIdx[_items[i].msg.ts] = i;

    bool changed = false;
    std::vector<Message> toInsert;

    for (const auto &msg : incoming) {
        const auto it = tsIdx.constFind(msg.ts);
        if (it != tsIdx.constEnd()) {
            auto &item = _items[*it];
            if (item.msg != msg) {
                item.msg = msg;
                item.textDoc.reset();
                item.docWidth = 0;
                item.attachDocs.clear();
                item.fileImgsRequested = false;
                changed = true;
            }
        } else {
            toInsert.push_back(msg);
            changed = true;
        }
    }

    for (const auto &msg : toInsert) {
        const double ts = msg.ts.toDouble();
        int insertAt = static_cast<int>(_items.size());
        for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
            if (_items[i].msg.ts.toDouble() > ts) { insertAt = i; break; }
        }
        MessageItem item;
        item.msg = msg;
        _items.insert(_items.begin() + insertAt, std::move(item));
    }

    if (changed) {
        rebuildLayout();
        viewport()->update();
    }
}

void MessageListWidget::appendMessage(const Message &msg) {
    MessageItem item;
    item.msg = msg;
    _items.push_back(std::move(item));
    rebuildLayout();
    viewport()->update();
}

int MessageListWidget::findByTs(const Ts &ts) const {
    for (int i = 0; i < static_cast<int>(_items.size()); ++i)
        if (_items[i].msg.ts == ts) return i;
    return -1;
}

// ── Layout ────────────────────────────────────────────────────────────────────

int MessageListWidget::introHeight() const {
    if (!_showIntro) return 0;
    int h = kIntroPadTop + kIntroNameH + kIntroPadBot;
    if (!_convDescription.isEmpty()) h += kIntroGap + kIntroDescH;
    return h;
}

bool MessageListWidget::needsDateSep(int index) const {
    if (index < 0 || index >= (int)_items.size()) return false;
    if (index == 0) return true;
    const QDate d0 = MsgRender::tsToDate(_items[index - 1].msg.ts);
    const QDate d1 = MsgRender::tsToDate(_items[index].msg.ts);
    return d0 != d1;
}

int MessageListWidget::textAreaWidth() const {
    return viewport()->width() - kPadH - kAvSize - kAvGap - kPadH;
}

void MessageListWidget::ensureDocLayout(const MessageItem &item) const {
    const int w = textAreaWidth();
    if (w <= 0) return;

    // Main text doc
    if (!item.textDoc) {
        item.textDoc = std::make_unique<QTextDocument>();
        item.textDoc->setDefaultFont(QApplication::font());
        item.textDoc->setDocumentMargin(0);
        item.textDoc->setDefaultStyleSheet("p { line-height: 135%; margin: 0; }");
        const auto html = MsgRender::buildMsgHtml(item.msg, _session);
        if (!html.isEmpty())
            item.textDoc->setHtml(html);
    }
    if (item.docWidth != w) {
        item.textDoc->setTextWidth(w);
        item.docWidth  = w;
        item.docHeight = item.textDoc->isEmpty()
            ? 0
            : static_cast<int>(std::ceil(item.textDoc->size().height()));
    }

    // Attachment docs (one per attachment)
    const auto &attachments = item.msg.attachments;
    if (item.attachDocs.size() != attachments.size())
        item.attachDocs.resize(attachments.size());

    const int attW = w - kAttachBarW - kAttachBarGap;
    for (int ai = 0; ai < (int)attachments.size(); ++ai) {
        auto &ad = item.attachDocs[ai];
        if (!ad.textDoc) {
            ad.textDoc = std::make_unique<QTextDocument>();
            ad.textDoc->setDefaultFont(QApplication::font());
            ad.textDoc->setDocumentMargin(0);
            const auto html = MsgRender::buildAttachHtml(attachments[ai], _session);
            if (!html.isEmpty()) ad.textDoc->setHtml(html);
        }
        if (ad.docWidth != attW) {
            ad.textDoc->setTextWidth(attW > 0 ? attW : 1);
            ad.docWidth  = attW;
            ad.docHeight = static_cast<int>(std::ceil(ad.textDoc->size().height()));
        }
    }
}

bool MessageListWidget::isCollapsed(int index) const {
    if (index <= 0) return false;
    const auto &prev = _items[index-1].msg;
    const auto &curr = _items[index].msg;
    if (prev.author != curr.author) return false;
    if (prev.replyCount > 0) return false; // thread roots break the run
    if (curr.replyCount > 0) return false;
    bool ok1, ok2;
    double t1 = prev.ts.toDouble(&ok1);
    double t2 = curr.ts.toDouble(&ok2);
    if (!ok1 || !ok2) return false;
    return (t2 - t1) < 300.0; // collapse if within 5 minutes
}

int MessageListWidget::rowHeight(int index) const {
    ensureDocLayout(_items[index]);
    const auto &item = _items[index];
    const bool collapsed = isCollapsed(index);

    int extraH = 0;

    // Attachment heights (skip client-dismissed ones)
    for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
        if (isDismissed(item.msg.ts, ai)) continue;
        extraH += kAttachGap + std::max(item.attachDocs[ai].docHeight, 0);
    }

    // Inline file image heights
    const bool hasContentAboveImages = item.docHeight > 0 || !item.attachDocs.empty();
    bool anyImgFiles = false;
    for (const auto &f : item.msg.files) {
        if (!f.isImage()) continue;
        anyImgFiles = true;
        const QString imgUrl = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
        const int imgGap = hasContentAboveImages ? kImgGap : 0;
        auto it = _imageCache.find(imgUrl);
        if (it != _imageCache.end() && !it->isNull()) {
            const auto &px = it.value();
            const double scale = std::min(1.0,
                std::min((double)kImgMaxW / px.width(),
                         (double)kImgMaxH / px.height()));
            extraH += imgGap + kImgNameH + static_cast<int>(px.height() * scale);
        } else if (f.imageWidth > 0 && f.imageHeight > 0) {
            // Use known file dimensions so layout doesn't jump when the image loads.
            const double scale = std::min(1.0,
                std::min((double)kImgMaxW / f.imageWidth,
                         (double)kImgMaxH / f.imageHeight));
            extraH += imgGap + kImgNameH + static_cast<int>(f.imageHeight * scale);
        } else {
            extraH += imgGap + kImgNameH + 24; // unknown size — small placeholder
        }
    }

    // Non-image file chips
    const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImgFiles;
    bool firstChip = true;
    for (const auto &f : item.msg.files) {
        if (f.isImage()) continue;
        if (!firstChip || hasAboveChips) extraH += kFileChipGap;
        firstChip = false;
        extraH += kFileChipH;
    }

    const int reactionH  = item.msg.reactions.empty() ? 0 : (kReactH + 2);
    const int replyBarH  = (!_isThreadMode && item.msg.replyCount > 0)
                           ? (kReplyBarGap + kReplyBarH) : 0;
    const int headerH    = collapsed ? 0 : (kHdrH + kHdrGap);
    const int contentH   = headerH + item.docHeight + extraH + reactionH;
    const int sepH       = needsDateSep(index) ? kSepH : 0;
    if (collapsed)
        return sepH + kPadVCollapsed + contentH + kPadVCollapsed + replyBarH;
    return sepH + kPadV + std::max(kAvSize, contentH) + kPadV + replyBarH;
}

void MessageListWidget::rebuildLayout() {
    _tops.resize(_items.size());
    const int ih = introHeight();
    int y = ih + kPadV;
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        _tops[i] = y;
        y += rowHeight(i) + kRowGap;
    }
    _totalH = std::max(y + kPadV, ih + kPadV * 2);

    const int vh = viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, _totalH - vh));
    verticalScrollBar()->setPageStep(vh);
}

// ── Mouse handling ────────────────────────────────────────────────────────────

QString MessageListWidget::anchorAt(const QPoint &viewportPos) const {
    const int scrollY = verticalScrollBar()->value();
    const int docY    = viewportPos.y() + scrollY;
    const int textLeft = kPadH + kAvSize + kAvGap;

    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        const int rowTop = _tops[i];
        const int rh     = rowHeight(i);
        if (docY < rowTop) break;
        if (docY > rowTop + rh) continue;

        const auto &item = _items[i];
        ensureDocLayout(item);

        const int sepH2 = needsDateSep(i) ? kSepH : 0;
        const int textTop = isCollapsed(i)
            ? rowTop + sepH2 + kPadVCollapsed
            : rowTop + sepH2 + kPadV + kHdrH;
        const QPointF local(viewportPos.x() - textLeft, docY - textTop);
        if (local.x() < 0 || local.y() < 0 || local.y() > item.docHeight) return {};

        return item.textDoc->documentLayout()->anchorAt(local);
    }
    return {};
}

std::pair<int,int> MessageListWidget::dismissButtonAt(const QPoint &viewportPos) const {
    const int scrollY = verticalScrollBar()->value();
    const int textLeft = kPadH + kAvSize + kAvGap;
    const int btnX = textLeft - kDismissGap - kDismissW;

    for (int i = 0; i < (int)_items.size(); ++i) {
        const int rowTop = _tops[i] - scrollY;
        if (rowTop > viewportPos.y()) break;
        if (rowTop + rowHeight(i) <= viewportPos.y()) continue;

        const auto &item = _items[i];
        if (item.msg.attachments.empty()) continue;
        ensureDocLayout(item);

        const bool collapsed = isCollapsed(i);
        const int padV = collapsed ? kPadVCollapsed : kPadV;
        const int sep = needsDateSep(i) ? kSepH : 0;
        int y = rowTop + sep + padV + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;

        for (int ai = 0; ai < (int)item.msg.attachments.size(); ++ai) {
            y += kAttachGap;
            if (!isDismissed(item.msg.ts, ai)) {
                if (QRect(btnX, y, kDismissW, kDismissW).contains(viewportPos))
                    return {i, ai};
                y += item.attachDocs[ai].docHeight;
            }
        }
    }
    return {-1, -1};
}

QRect MessageListWidget::dismissButtonVpRect(int msgIdx, int attachIdx) const {
    if (msgIdx < 0 || msgIdx >= (int)_items.size()) return {};
    const auto &item = _items[msgIdx];
    ensureDocLayout(item);

    const int scrollY = verticalScrollBar()->value();
    const int textLeft = kPadH + kAvSize + kAvGap;
    const int btnX = textLeft - kDismissGap - kDismissW;
    const bool collapsed = isCollapsed(msgIdx);
    const int padV = collapsed ? kPadVCollapsed : kPadV;
    const int sep = needsDateSep(msgIdx) ? kSepH : 0;
    const int rowTop = _tops[msgIdx] - scrollY;
    int y = rowTop + sep + padV + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;

    for (int ai = 0; ai < (int)item.msg.attachments.size(); ++ai) {
        y += kAttachGap;
        if (!isDismissed(item.msg.ts, ai)) {
            if (ai == attachIdx)
                return QRect(btnX, y, kDismissW, kDismissW);
            y += item.attachDocs[ai].docHeight;
        }
    }
    return {};
}

void MessageListWidget::doMousePress(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;

    // Scrollbar thumb drag
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (event->pos().x() >= sbHitX && isOnScrollThumb(event->pos().y())) {
        _sbDragging        = true;
        _sbDragStartY      = event->pos().y();
        _sbDragStartScroll = verticalScrollBar()->value();
        viewport()->setCursor(Qt::SizeVerCursor);
        return;
    }

    // Toolbar button clicks
    const int btn = toolbarButtonAt(event->pos());
    if (btn == 0) {
        QMessageBox::information(this, tr("Add reaction"), tr("Not implemented"));
        return;
    } else if (btn == 1) {
        QMessageBox::information(this, tr("Forward message"), tr("Not implemented"));
        return;
    } else if (btn == 2 && _hoveredRow >= 0) {
        // "More actions" — show the styled context menu
        const auto &msg = _items[_hoveredRow].msg;
        const int scrollY = verticalScrollBar()->value();
        const int rowTop  = _tops[_hoveredRow] - scrollY;
        const int rh      = rowHeight(_hoveredRow);
        const QRect moreRect = toolbarButtonRect(2, rowTop, rh);
        const QPoint globalPos = viewport()->mapToGlobal(moreRect.bottomLeft());

        auto *menu = new ContextMenu(this);
        menu->addItem(tr("Edit message"),   "E",      [this, ts = msg.ts] {
            Q_UNUSED(ts)
            QMessageBox::information(this, tr("Edit message"), tr("Not implemented"));
        });
        menu->addSeparator();
        menu->addItem(tr("Mark unread"),    "U",      [this] {
            QMessageBox::information(this, tr("Mark unread"), tr("Not implemented"));
        });
        menu->addItem(tr("Remind me"),      {},       [this] {
            QMessageBox::information(this, tr("Remind me"), tr("Not implemented"));
        }, false, /*submenu=*/true);
        menu->addItem(tr("Turn off notifications for replies"), {}, [this] {
            QMessageBox::information(this, tr("Turn off notifications"), tr("Not implemented"));
        });
        menu->addSeparator();
        menu->addItem(tr("Copy link"),      "L",      [this] {
            QMessageBox::information(this, tr("Copy link"), tr("Not implemented"));
        });
        menu->addItem(tr("Copy message"),   "Ctrl+C", [this, text = msg.text.text] {
            QApplication::clipboard()->setText(text);
        });
        menu->addSeparator();
        menu->addItem(tr("Pin to channel"), "P",      [this] {
            QMessageBox::information(this, tr("Pin to channel"), tr("Not implemented"));
        });
        menu->addSeparator();
        menu->addItem(tr("Delete message…"), "delete", [this] {
            QMessageBox::information(this, tr("Delete message"), tr("Not implemented"));
        }, /*destructive=*/true);
        menu->popup(globalPos);
        return;
    }

    // Dismiss attachment click
    const auto [dMsgIdx, dAi] = dismissButtonAt(event->pos());
    if (dMsgIdx >= 0) {
        const auto &ts = _items[dMsgIdx].msg.ts;
        _dismissedAttachments.insert(ts + "/" + QString::number(dAi));
        rebuildLayout();
        viewport()->update();
        return;
    }

    // Reply bar click → open thread panel
    const int replyIdx = replyBarIndexAt(event->pos());
    if (replyIdx >= 0) {
        emit threadClicked(_currentConv, _items[replyIdx].msg.ts);
        return;
    }

    const QString anchor = anchorAt(event->pos());
    if (!anchor.isEmpty()) {
        QDesktopServices::openUrl(QUrl(anchor));
        return;
    }

    const File *f = fileChipAt(event->pos());
    if (f) {
        const QString url = f->permalink.isEmpty() ? f->urlPrivate : f->permalink;
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    }
}

void MessageListWidget::doMouseLeave() {
    // Showing the tooltip (a separate window) fires a spurious Leave event on
    // the viewport on X11 even though the cursor never left.  Ignore it.
    if (viewport()->rect().contains(viewport()->mapFromGlobal(QCursor::pos())))
        return;

    _tooltip->hide();
    if (_hoveredRow != -1 || _hoveredToolBtn != -1 || _hoveredAttach.first != -1) {
        _hoveredRow     = -1;
        _hoveredToolBtn = -1;
        _hoveredAttach  = {-1, -1};
        viewport()->update();
    }
}

bool MessageListWidget::isOnScrollThumb(int vpY) const {
    const int vh = viewport()->height();
    if (_totalH <= vh) return false;
    const int scrollY = verticalScrollBar()->value();
    const int thumbH  = std::max(20, vh * vh / _totalH);
    const int thumbY  = scrollY * (vh - thumbH) / (_totalH - vh);
    return vpY >= thumbY && vpY < thumbY + thumbH;
}

void MessageListWidget::doMouseRelease(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    if (_sbDragging) {
        _sbDragging = false;
        viewport()->setCursor(Qt::ArrowCursor);
    }
}

void MessageListWidget::doMouseMove(QMouseEvent *event) {
    if (_sbDragging) {
        const int vh         = viewport()->height();
        const int thumbH     = std::max(20, vh * vh / _totalH);
        const int trackRange = vh - thumbH;
        if (trackRange > 0) {
            const int newScroll = _sbDragStartScroll
                + (event->pos().y() - _sbDragStartY) * (_totalH - vh) / trackRange;
            verticalScrollBar()->setValue(
                std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }

    const QPoint pos = event->pos();
    const int scrollY = verticalScrollBar()->value();

    const int vw    = viewport()->width();
    const int cardW = kToolbarPadH * 2 + 3 * kToolbarBtnSize + 2 * kToolbarGap;
    const int cardH = kToolbarPadV * 2 + kToolbarBtnSize;

    // If a row is already hovered, keep it as long as the mouse remains inside
    // that row's rect (any X) OR its toolbar card rect.  The card straddles the
    // row's top edge, so without this the hover would flip to the row above
    // the moment the cursor enters the card's upper half.
    int newHoveredRow = -1;
    if (_hoveredRow >= 0) {
        const int rowTop = _tops[_hoveredRow] - scrollY;
        const int rh     = rowHeight(_hoveredRow);
        const bool inRow  = pos.y() >= rowTop && pos.y() < rowTop + rh;
        const int sep4   = needsDateSep(_hoveredRow) ? kSepH : 0;
        const QRect card(vw - kToolbarRight - cardW, (rowTop + sep4) - cardH / 2, cardW, cardH);
        if (inRow || card.contains(pos))
            newHoveredRow = _hoveredRow;
    }

    // Mouse has left the current row's combined zone — geometric hit-test.
    if (newHoveredRow < 0) {
        for (int i = 0; i < (int)_items.size(); ++i) {
            const int rowTop = _tops[i] - scrollY;
            const int rh     = rowHeight(i);
            if (rowTop > pos.y()) break;
            if (pos.y() < rowTop + rh) { newHoveredRow = i; break; }
        }
    }

    // Detect which toolbar button (if any) is under the cursor — computed
    // directly so we don't depend on the stale _hoveredRow value.
    int newHoveredBtn = -1;
    if (newHoveredRow >= 0) {
        const int rowTop = _tops[newHoveredRow] - scrollY;
        const int rh     = rowHeight(newHoveredRow);
        const int sep5   = needsDateSep(newHoveredRow) ? kSepH : 0;
        for (int b = 0; b < 3; ++b) {
            if (toolbarButtonRect(b, rowTop + sep5, rh - sep5).contains(pos)) {
                newHoveredBtn = b;
                break;
            }
        }
    }

    // Detect which attachment preview (if any) the cursor is over.
    std::pair<int,int> newHoveredAttach = {-1, -1};
    if (newHoveredRow >= 0) {
        const auto &item = _items[newHoveredRow];
        if (!item.msg.attachments.empty()) {
            ensureDocLayout(item);
            const bool collA = isCollapsed(newHoveredRow);
            const int padVA  = collA ? kPadVCollapsed : kPadV;
            const int sepA   = needsDateSep(newHoveredRow) ? kSepH : 0;
            const int rtA    = _tops[newHoveredRow] - scrollY;
            int ay = rtA + sepA + padVA + (collA ? 0 : kHdrH + kHdrGap) + item.docHeight;
            for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
                ay += kAttachGap;
                if (!isDismissed(item.msg.ts, ai)) {
                    const int ah = item.attachDocs[ai].docHeight;
                    if (pos.y() >= ay && pos.y() < ay + ah) {
                        newHoveredAttach = {newHoveredRow, ai};
                        break;
                    }
                    ay += ah;
                }
            }
        }
    }

    if (newHoveredRow != _hoveredRow || newHoveredBtn != _hoveredToolBtn
            || newHoveredAttach != _hoveredAttach) {
        _hoveredRow     = newHoveredRow;
        _hoveredToolBtn = newHoveredBtn;
        _hoveredAttach  = newHoveredAttach;
        viewport()->update();
    }

    // Tooltip for the hovered toolbar button or dismiss button
    if (newHoveredBtn >= 0) {
        static const QString kTips[] = { tr("Add reaction"), tr("Forward message"), tr("More actions") };
        const int rowTop  = _tops[_hoveredRow] - scrollY;
        const int rh      = rowHeight(_hoveredRow);
        const int sep6    = needsDateSep(_hoveredRow) ? kSepH : 0;
        const QRect btnLocal = toolbarButtonRect(newHoveredBtn, rowTop + sep6, rh - sep6);
        const QRect btnGlobal(viewport()->mapToGlobal(btnLocal.topLeft()), btnLocal.size());
        _tooltip->showAbove(kTips[newHoveredBtn], btnGlobal);
    } else {
        const auto [dMsgIdx, dAi] = dismissButtonAt(pos);
        const bool attachHovered = _hoveredAttach.first == dMsgIdx && _hoveredAttach.second == dAi
                                   && dMsgIdx >= 0;
        if (attachHovered) {
            const QRect btnLocal = dismissButtonVpRect(dMsgIdx, dAi);
            const QRect btnGlobal(viewport()->mapToGlobal(btnLocal.topLeft()), btnLocal.size());
            _tooltip->showAbove(tr("Remove preview"), btnGlobal);
        } else {
            _tooltip->hide();
        }
    }

    // Cursor
    const QString anchor = anchorAt(pos);
    const auto [dMI, dAI] = dismissButtonAt(pos);
    const bool overDismiss = dMI >= 0 && _hoveredAttach.first == dMI && _hoveredAttach.second == dAI;
    const bool overLink  = !anchor.isEmpty() || fileChipAt(pos) || replyBarIndexAt(pos) >= 0
                           || overDismiss;
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    const bool overScroll = pos.x() >= sbHitX && isOnScrollThumb(pos.y());
    if (overScroll)
        viewport()->setCursor(Qt::SizeVerCursor);
    else
        viewport()->setCursor(overLink ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

// ── Live event handling ───────────────────────────────────────────────────────

void MessageListWidget::handleEvent(const Event &e) {
    const bool wasAtBottom =
        verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;

    if (auto *ev = std::get_if<EvMessageNew>(&e)) {
        if (ev->conv != _currentConv) return;
        if (_isThreadMode) {
            // In thread mode, only show messages belonging to this thread.
            const bool isRoot  = ev->msg.ts == _threadRootTs;
            const bool isReply = ev->msg.threadRoot.has_value()
                                 && *ev->msg.threadRoot == _threadRootTs;
            if (!isRoot && !isReply) return;
        } else if (ev->msg.threadRoot.has_value()) {
            // In channel mode, thread replies don't appear in the main list —
            // bump the reply count on the root message instead.
            const int rootIdx = findByTs(*ev->msg.threadRoot);
            if (rootIdx >= 0) {
                _items[rootIdx].msg.replyCount++;
                rebuildLayout();
                viewport()->update();
            }
            return;
        }
        appendMessage(ev->msg);
        // Highlight the new row
        _newMsgTs.insert(ev->msg.ts);
        _highlightAnim.stop();
        _highlightAnim.setStartValue(1.0);
        _highlightAnim.setEndValue(0.0);
        _highlightAnim.start();
        // Scroll to reveal the new message only if we were already at the bottom
        if (wasAtBottom)
            QTimer::singleShot(0, this, [this] {
                verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            });

    } else if (auto *ev = std::get_if<EvMessageChanged>(&e)) {
        if (ev->conv != _currentConv) return;
        const int i = findByTs(ev->msg.ts);
        if (i < 0) return;
        _items[i].msg      = ev->msg;
        _items[i].textDoc.reset();  // invalidate rendered docs
        _items[i].docWidth = 0;
        _items[i].attachDocs.clear();
        _items[i].fileImgsRequested = false;
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvMessageDeleted>(&e)) {
        if (ev->conv != _currentConv) return;
        const int i = findByTs(ev->ts);
        if (i < 0) return;
        _items.erase(_items.begin() + i);
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvReactionAdded>(&e)) {
        if (ev->conv != _currentConv) return;
        const int i = findByTs(ev->ts);
        if (i < 0) return;
        auto &reactions = _items[i].msg.reactions;
        bool found = false;
        for (auto &r : reactions) {
            if (r.name == ev->name) {
                r.count++;
                r.users.push_back(ev->user);
                found = true;
                break;
            }
        }
        if (!found) reactions.push_back({ev->name, 1, {ev->user}});
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvReactionRemoved>(&e)) {
        if (ev->conv != _currentConv) return;
        const int i = findByTs(ev->ts);
        if (i < 0) return;
        auto &reactions = _items[i].msg.reactions;
        for (auto rit = reactions.begin(); rit != reactions.end(); ++rit) {
            if (rit->name == ev->name) {
                rit->count = std::max(0, rit->count - 1);
                auto &users = rit->users;
                users.erase(
                    std::remove(users.begin(), users.end(), ev->user),
                    users.end());
                if (rit->count == 0) reactions.erase(rit);
                break;
            }
        }
        rebuildLayout();
        viewport()->update();
    }
}
