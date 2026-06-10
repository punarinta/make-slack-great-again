// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_list.h"
#include "message_render.h"
#include "session/session.h"
#include "backend/backend.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/image_cache.h"
#include "ui/context_menu/context_menu.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/emoji_picker/emoji_picker_popup.h"
#include "ui/delete_message_dialog/delete_message_dialog.h"
#include "util/clipboard.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDesktopServices>
#include <QClipboard>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include <QUrl>
#include <QTimer>
#include <QCursor>

#include <algorithm>
#include <cmath>

// ── MessageListWidget ─────────────────────────────────────────────────────────

MessageListWidget::MessageListWidget(Session *session, ImageCache *imgCache, QWidget *parent)
    : VirtualListWidget(parent), _session(session), _imgCache(imgCache) {
    verticalScrollBar()->setSingleStep(20);
    setFocusPolicy(Qt::ClickFocus);

    _tooltip     = new PopupTooltip(this);
    _emojiPicker = new EmojiPickerPopup(this);

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

    _loadingAnim.setUpdateCallback([this] { viewport()->update(); });

    connect(
        &ThemeManager::instance(),
        &ThemeManager::themeChanged,
        viewport(),
        QOverload<>::of(&QWidget::update)
    );

    if (_imgCache) {
        connect(_imgCache, &ImageCache::loaded, this, [this] {
            const bool wasAtBottom =
                verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
            rebuildLayout();
            if (wasAtBottom)
                verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            viewport()->update();
        });
    }
}

void MessageListWidget::smoothScrollTo(int target) {
    if (target == verticalScrollBar()->value())
        return;
    _scrollAnim.stop();
    _scrollAnim.setStartValue(verticalScrollBar()->value());
    _scrollAnim.setEndValue(target);
    _scrollAnim.start();
}

void MessageListWidget::scrollContentsBy(int /*dx*/, int /*dy*/) {
    viewport()->update();
    if (verticalScrollBar()->value() <= 200)
        loadOlderMessages();
}

void MessageListWidget::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    for (auto &item : _items) {
        item.docWidth = 0; // invalidate so docs re-layout at new width
        for (auto &ad : item.attachDocs)
            ad.docWidth = 0;
    }
    rebuildLayout();
    if (_items.empty())
        return;
    if (_pendingRestorePos >= 0) {
        verticalScrollBar()->setValue(std::min(_pendingRestorePos, verticalScrollBar()->maximum()));
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
    _loading = false;
    _loadingAnim.stop();
    _loadLifetime      = rpl::lifetime();
    _olderLoadLifetime = rpl::lifetime();
    _eventLifetime     = rpl::lifetime();
    _olderCursor       = std::nullopt;
    _loadingOlder      = false;
    _items.clear();
    _tops.clear();
    _totalH    = 0;
    _showIntro = false;
    _hoveredLinkUrl.clear();
    _hoveredLinkRow = -1;
    _convName.clear();
    _convDescription.clear();
    _selAnchor   = {};
    _selFocus    = {};
    _selDragging = false;
    verticalScrollBar()->setRange(0, 0);
    viewport()->update();
}

void MessageListWidget::setWaiting(bool waiting) {
    _waiting = waiting;
    if (waiting) {
        if (!_loadingAnim.isRunning()) {
            _loadingElapsedTimer.start();
            _loadingAnim.start();
        }
    } else {
        if (!_loading)
            _loadingAnim.stop();
    }
    viewport()->update();
}

void MessageListWidget::setSession(Session *session) {
    clear();
    _currentConv = {};
    _session     = session;
    if (_emojiPicker)
        _emojiPicker->setSession(session);
}

void MessageListWidget::openConversation(
    ConversationId conv, const QString &convName, const QString &description
) {
    // Persist messages of the conversation we're leaving before discarding them.
    if (!_currentConv.value.isEmpty() && _session && !_items.empty()) {
        std::vector<Message> msgs;
        msgs.reserve(_items.size());
        for (const auto &item : _items)
            msgs.push_back(item.msg);
        _session->cacheMessages(_currentConv, msgs);
    }

    // Save scroll position of the conversation we're leaving.
    if (!_currentConv.value.isEmpty())
        _savedScrollPos[_currentConv.value] = verticalScrollBar()->value();

    clear();
    _currentConv     = conv;
    _isThreadMode    = false;
    _threadRootTs    = {};
    _convName        = convName;
    _convDescription = description;
    _showIntro       = true;

    _session->events() | rpl::on_next([this](Event e) { handleEvent(e); }, _eventLifetime);

    _session->botInfoLoaded() | rpl::on_next(
                                    [this](UserId) {
                                        rebuildLayout();
                                        viewport()->update();
                                    },
                                    _eventLifetime
                                );

    // Reset scroll intent — stale state from a previous conversation must not leak.
    _scrollToBottomPending = false;
    _pendingRestorePos     = -1;

    if (_savedScrollPos.contains(conv.value))
        _pendingRestorePos = _savedScrollPos[conv.value];
    else
        _scrollToBottomPending = true;

    // Pre-populate from cache for instant display while network loads.
    const bool hasCached = [&] {
        if (!_session)
            return false;
        const auto cached = _session->cachedMessages(conv);
        if (cached.empty())
            return false;
        for (const auto &msg : cached)
            appendMessage(msg);
        // Pre-warm the image pixel cache from disk so images appear without download.
        for (const auto &item : _items) {
            for (const auto &f : item.msg.files) {
                if (!f.isImage())
                    continue;
                const QString url = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
                if (_fileImages.contains(url))
                    continue;
                const auto data = _session->cachedImage(url);
                if (data.isEmpty())
                    continue;
                QPixmap px;
                if (px.loadFromData(data) && !px.isNull())
                    _fileImages[url] = px;
            }
        }
        return true;
    }();

    if (hasCached) {
        emit initialPageLoaded();
        // Apply scroll position after layout settles (next event-loop tick).
        QTimer::singleShot(0, this, [this, conv] {
            if (_currentConv != conv)
                return;
            applyPendingScroll();
        });
    } else {
        _loading = true;
        _loadingElapsedTimer.start();
        _loadingAnim.start();
    }

    // Fetch fresh data from the network; merge it into whatever is already shown.
    _session->backend()->loadHistory(conv, std::nullopt) |
        rpl::on_next(
            [this, conv, hasCached](MessagePage page) {
                if (_currentConv != conv)
                    return;

                _loading = false;
                _loadingAnim.stop();
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
                            std::min(_pendingRestorePos, verticalScrollBar()->maximum())
                        );
                        _pendingRestorePos = -1;
                    }
                } else {
                    // No cached data was shown — normal first-load path.
                    for (const auto &msg : page.messages)
                        appendMessage(msg);
                    emit initialPageLoaded();
                    QTimer::singleShot(0, this, [this, conv] {
                        if (_currentConv != conv)
                            return;
                        applyPendingScroll();
                    });
                }
            },
            _loadLifetime
        );
}

void MessageListWidget::updateConvName(const QString &convName, const QString &description) {
    _convName        = convName;
    _convDescription = description;
    rebuildLayout();
    viewport()->update();
}

void MessageListWidget::applyPendingScroll() {
    if (textAreaWidth() <= 0 || _items.empty())
        return;
    if (_pendingRestorePos >= 0) {
        verticalScrollBar()->setValue(std::min(_pendingRestorePos, verticalScrollBar()->maximum()));
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

    _loadingOlder      = true;
    _olderLoadLifetime = rpl::lifetime();

    const auto    conv = _currentConv;
    const QString cur  = *_olderCursor;
    _olderCursor       = std::nullopt;

    auto producer = _isThreadMode ? _session->backend()->loadThread(conv, _threadRootTs, cur)
                                  : _session->backend()->loadHistory(conv, cur);

    std::move(producer) | rpl::on_next(
                              [this, conv](MessagePage page) {
                                  if (_currentConv != conv) {
                                      _loadingOlder = false;
                                      return;
                                  }

                                  _olderCursor  = page.olderCursor;
                                  _loadingOlder = false;

                                  if (page.messages.empty())
                                      return;

                                  // Inserting older messages at the top shifts row indices —
                                  // drop any in-progress selection to avoid stale positions.
                                  _selAnchor   = {};
                                  _selFocus    = {};
                                  _selDragging = false;

                                  // Record the pre-insert total height and current scroll so we can
                                  // shift the scrollbar down by exactly the height added at the
                                  // top, keeping the visible content from jumping.
                                  const int prevTotalH = _totalH;
                                  const int scrollY    = verticalScrollBar()->value();

                                  mergeNetworkMessages(page.messages);

                                  const int delta = _totalH - prevTotalH;
                                  if (delta > 0) {
                                      _scrollAnim.stop();
                                      verticalScrollBar()->setValue(scrollY + delta);
                                  }
                              },
                              _olderLoadLifetime
                          );
}

void MessageListWidget::openThread(ConversationId conv, Ts rootTs) {
    _loading = false;
    _loadingAnim.stop();
    _loadLifetime  = rpl::lifetime();
    _eventLifetime = rpl::lifetime();
    _items.clear();
    _tops.clear();
    _totalH = 0;
    verticalScrollBar()->setRange(0, 0);
    viewport()->update();

    _currentConv           = conv;
    _isThreadMode          = true;
    _threadRootTs          = rootTs;
    _showIntro             = false;
    _scrollToBottomPending = true;
    _pendingRestorePos     = -1;

    if (!_session)
        return;

    _loading = true;
    _loadingElapsedTimer.start();
    _loadingAnim.start();

    _session->events() | rpl::on_next([this](Event e) { handleEvent(e); }, _eventLifetime);

    _session->backend()->loadThread(conv, rootTs, std::nullopt) |
        rpl::on_next(
            [this](MessagePage page) {
                _loading = false;
                _loadingAnim.stop();
                _olderCursor = page.olderCursor;
                for (const auto &msg : page.messages)
                    appendMessage(msg);
                QTimer::singleShot(0, this, [this] { applyPendingScroll(); });
            },
            _loadLifetime
        );
}

void MessageListWidget::mergeNetworkMessages(const std::vector<Message> &incoming) {
    // Build ts → index map for the items already displayed.
    QHash<QString, int> tsIdx;
    tsIdx.reserve(static_cast<int>(_items.size()));
    for (int i = 0; i < static_cast<int>(_items.size()); ++i)
        tsIdx[_items[i].msg.ts] = i;

    bool                 changed = false;
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
                changed                = true;
            }
        } else {
            toInsert.push_back(msg);
            changed = true;
        }
    }

    for (const auto &msg : toInsert) {
        const double ts       = msg.ts.toDouble();
        int          insertAt = static_cast<int>(_items.size());
        for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
            if (_items[i].msg.ts.toDouble() > ts) {
                insertAt = i;
                break;
            }
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
    if (_session && msg.author.value.startsWith('B'))
        _session->fetchBotIfNeeded(msg.author);
    MessageItem item;
    item.msg = msg;
    _items.push_back(std::move(item));
    rebuildLayout();
    viewport()->update();
}

int MessageListWidget::findByTs(const Ts &ts) const {
    for (int i = 0; i < static_cast<int>(_items.size()); ++i)
        if (_items[i].msg.ts == ts)
            return i;
    return -1;
}

void MessageListWidget::scrollToTs(const Ts &ts) {
    const int idx = findByTs(ts);
    if (idx < 0 || idx >= (int)_tops.size())
        return;
    smoothScrollTo(qMax(0, _tops[idx] - viewport()->height() / 3));
}

// ── Layout ────────────────────────────────────────────────────────────────────

int MessageListWidget::introHeight() const {
    if (!_showIntro)
        return 0;
    int h = kIntroPadTop + kIntroNameH + kIntroPadBot;
    if (!_convDescription.isEmpty())
        h += kIntroGap + kIntroDescH;
    return h;
}

bool MessageListWidget::needsDateSep(int index) const {
    if (index < 0 || index >= (int)_items.size())
        return false;
    if (index == 0)
        return true;
    const QDate d0 = MsgRender::tsToDate(_items[index - 1].msg.ts);
    const QDate d1 = MsgRender::tsToDate(_items[index].msg.ts);
    return d0 != d1;
}

int MessageListWidget::textAreaWidth() const {
    return viewport()->width() - kPadH - kAvSize - kAvGap - kPadH;
}

void MessageListWidget::ensureDocLayout(const MessageItem &item) const {
    const int w = textAreaWidth();
    if (w <= 0)
        return;

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
            if (!html.isEmpty())
                ad.textDoc->setHtml(html);
        }
        if (ad.docWidth != attW) {
            ad.textDoc->setTextWidth(attW > 0 ? attW : 1);
            ad.docWidth  = attW;
            ad.docHeight = static_cast<int>(std::ceil(ad.textDoc->size().height()));
        }
    }
}

bool MessageListWidget::isCollapsed(int index) const {
    if (index <= 0)
        return false;
    const auto &prev = _items[index - 1].msg;
    const auto &curr = _items[index].msg;
    if (prev.author != curr.author)
        return false;
    if (prev.replyCount > 0)
        return false; // thread roots break the run
    if (curr.replyCount > 0)
        return false;
    bool   ok1, ok2;
    double t1 = prev.ts.toDouble(&ok1);
    double t2 = curr.ts.toDouble(&ok2);
    if (!ok1 || !ok2)
        return false;
    return (t2 - t1) < 300.0; // collapse if within 5 minutes
}

int MessageListWidget::rowHeight(int index) const {
    ensureDocLayout(_items[index]);
    const auto &item      = _items[index];
    const bool  collapsed = isCollapsed(index);

    int extraH = 0;

    // Attachment heights (skip client-dismissed ones)
    for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
        if (isDismissed(item.msg.ts, ai))
            continue;
        extraH += kAttachGap + std::max(attachTotalH(item, ai), 0);
    }

    // Inline file image heights
    const bool hasContentAboveImages = item.docHeight > 0 || !item.attachDocs.empty();
    bool       anyImgFiles           = false;
    for (const auto &f : item.msg.files) {
        if (!f.isImage())
            continue;
        anyImgFiles          = true;
        const QString imgUrl = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
        const int     imgGap = hasContentAboveImages ? kImgGap : 0;
        auto          it     = _fileImages.constFind(imgUrl);
        if (it != _fileImages.constEnd() && !it->isNull()) {
            const auto  &px    = it.value();
            const double scale = std::min(
                1.0, std::min((double)kImgMaxW / px.width(), (double)kImgMaxH / px.height())
            );
            extraH += imgGap + kImgNameH + static_cast<int>(px.height() * scale);
        } else if (f.imageWidth > 0 && f.imageHeight > 0) {
            // Use known file dimensions so layout doesn't jump when the image loads.
            const double scale = std::min(
                1.0, std::min((double)kImgMaxW / f.imageWidth, (double)kImgMaxH / f.imageHeight)
            );
            extraH += imgGap + kImgNameH + static_cast<int>(f.imageHeight * scale);
        } else {
            extraH += imgGap + kImgNameH + 24; // unknown size — small placeholder
        }
    }

    // Non-image file chips
    const bool hasAboveChips = item.docHeight > 0 || !item.attachDocs.empty() || anyImgFiles;
    bool       firstChip     = true;
    for (const auto &f : item.msg.files) {
        if (f.isImage())
            continue;
        if (!firstChip || hasAboveChips)
            extraH += kFileChipGap;
        firstChip = false;
        extraH += kFileChipH;
    }

    const int reactionH = item.msg.reactions.empty() ? 0 : (kReactH + 2);
    const int replyBarH =
        (!_isThreadMode && item.msg.replyCount > 0) ? (kReplyBarGap + kReplyBarH) : 0;
    const int headerH  = collapsed ? 0 : (kHdrH + kHdrGap);
    const int pinnedH  = item.msg.pinned ? 18 : 0;
    // pinnedH is a banner drawn before padV — kept separate from contentH.
    const int contentH = headerH + item.docHeight + extraH + reactionH;
    const int sepH     = needsDateSep(index) ? kSepH : 0;
    if (collapsed)
        return sepH + pinnedH + kPadVCollapsed + contentH + kPadVCollapsed + replyBarH;
    return sepH + pinnedH + kPadV + std::max(kAvSize, contentH) + kPadVBottom + replyBarH;
}

void MessageListWidget::rebuildLayout() {
    _tops.resize(_items.size());
    const int ih = introHeight();
    int       y  = ih + kPadV;
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        _tops[i] = y;
        y += rowHeight(i) + kRowGap;
    }
    _totalH = std::max(y + kPadV, ih + kPadV * 2);

    const int vh = viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, _totalH - vh));
    verticalScrollBar()->setPageStep(vh);
}

// ── Attachment height helpers ─────────────────────────────────────────────────

int MessageListWidget::attachImageH(const Attachment &att) const {
    const QString imgUrl = att.thumbUrl.isEmpty() ? att.imageUrl : att.thumbUrl;
    if (imgUrl.isEmpty() || !_imgCache)
        return 0;
    const QPixmap px = _imgCache->get(imgUrl);
    if (px.isNull())
        return 0;
    const double scale =
        std::min(1.0, std::min((double)kImgMaxW / px.width(), (double)kImgMaxH / px.height()));
    return kImgGap + (int)(px.height() * scale);
}

int MessageListWidget::attachTotalH(const MessageItem &item, int ai) const {
    return item.attachDocs[ai].docHeight + attachImageH(item.msg.attachments[ai]);
}

// Walk all text fragments in a QTextDocument and set underline on those whose
// anchor href matches url.
static void setDocLinkUnderline(QTextDocument *doc, const QString &url, bool underline) {
    if (!doc || url.isEmpty())
        return;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            QTextCharFormat fmt = frag.charFormat();
            if (!fmt.isAnchor() || fmt.anchorHref() != url)
                continue;
            QTextCursor cur(doc);
            cur.setPosition(frag.position());
            cur.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            QTextCharFormat newFmt = fmt;
            newFmt.setFontUnderline(underline);
            cur.setCharFormat(newFmt);
        }
    }
}

// Collect the plain text of all fragments in doc whose href matches url.
static QString collectLinkText(QTextDocument *doc, const QString &url) {
    if (!doc || url.isEmpty())
        return {};
    QString text;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (frag.isValid() && frag.charFormat().anchorHref() == url)
                text += frag.text();
        }
    }
    return text;
}

// ── Mouse handling ────────────────────────────────────────────────────────────

QString MessageListWidget::anchorAt(const QPoint &viewportPos) const {
    const PaintContext ctx      = makePaintContext();
    const int          scrollY  = ctx.scrollY;
    const int          docY     = viewportPos.y() + scrollY;
    const int          textLeft = ctx.textLeft;

    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        const int rowTop = _tops[i];
        const int rh     = rowHeight(i);
        if (docY < rowTop)
            break;
        if (docY > rowTop + rh)
            continue;

        const auto &item = _items[i];
        ensureDocLayout(item);

        const bool coll    = isCollapsed(i);
        const int  padV    = coll ? kPadVCollapsed : kPadV;
        const int  sepH2   = needsDateSep(i) ? kSepH : 0;
        const int  pinnedH = item.msg.pinned ? 18 : 0;
        const int  textTop = rowTop + sepH2 + pinnedH + padV + (coll ? 0 : kHdrH + kHdrGap);

        // Check main message text doc
        {
            const QPointF local(viewportPos.x() - textLeft, docY - textTop);
            if (local.x() >= 0 && local.y() >= 0 && local.y() <= item.docHeight && item.textDoc) {
                const QString href = item.textDoc->documentLayout()->anchorAt(local);
                if (!href.isEmpty())
                    return href;
            }
        }

        // Check attachment text docs
        const int attTextX = textLeft + kAttachBarW + kAttachBarGap;
        const int attW     = textAreaWidth() - kAttachBarW - kAttachBarGap;
        int       ay       = textTop + item.docHeight;
        for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
            if (isDismissed(item.msg.ts, ai))
                continue;
            ay += kAttachGap;
            const auto &ad         = item.attachDocs[ai];
            const int   attTextTop = ay;
            if (docY >= attTextTop && docY < attTextTop + ad.docHeight && ad.textDoc) {
                const QPointF local(viewportPos.x() - attTextX, docY - attTextTop);
                if (local.x() >= 0 && local.x() < attW) {
                    const QString href = ad.textDoc->documentLayout()->anchorAt(local);
                    if (!href.isEmpty())
                        return href;
                }
            }
            ay += attachTotalH(item, ai);
        }
        return {};
    }
    return {};
}

std::pair<int, int> MessageListWidget::dismissButtonAt(const QPoint &viewportPos) const {
    const PaintContext ctx      = makePaintContext();
    const int          scrollY  = ctx.scrollY;
    const int          textLeft = ctx.textLeft;
    const int          btnX     = textLeft - kDismissGap - kDismissW;

    for (int i = 0; i < (int)_items.size(); ++i) {
        const int rowTop = _tops[i] - scrollY;
        if (rowTop > viewportPos.y())
            break;
        if (rowTop + rowHeight(i) <= viewportPos.y())
            continue;

        const auto &item = _items[i];
        if (item.msg.attachments.empty())
            continue;
        ensureDocLayout(item);

        const bool collapsed = isCollapsed(i);
        const int  padV      = collapsed ? kPadVCollapsed : kPadV;
        const int  sep       = needsDateSep(i) ? kSepH : 0;
        const int  pinnedH   = item.msg.pinned ? 18 : 0;
        int y = rowTop + sep + padV + pinnedH + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;

        for (int ai = 0; ai < (int)item.msg.attachments.size(); ++ai) {
            y += kAttachGap;
            if (!isDismissed(item.msg.ts, ai)) {
                if (QRect(btnX, y, kDismissW, kDismissW).contains(viewportPos))
                    return {i, ai};
                y += attachTotalH(item, ai);
            }
        }
    }
    return {-1, -1};
}

QRect MessageListWidget::dismissButtonVpRect(int msgIdx, int attachIdx) const {
    if (msgIdx < 0 || msgIdx >= (int)_items.size())
        return {};
    const auto &item = _items[msgIdx];
    ensureDocLayout(item);

    const PaintContext ctx       = makePaintContext();
    const int          scrollY   = ctx.scrollY;
    const int          textLeft  = ctx.textLeft;
    const int          btnX      = textLeft - kDismissGap - kDismissW;
    const bool         collapsed = isCollapsed(msgIdx);
    const int          padV      = collapsed ? kPadVCollapsed : kPadV;
    const int          sep       = needsDateSep(msgIdx) ? kSepH : 0;
    const int          pinnedH   = item.msg.pinned ? 18 : 0;
    const int          rowTop    = _tops[msgIdx] - scrollY;
    int y = rowTop + sep + padV + pinnedH + (collapsed ? 0 : kHdrH + kHdrGap) + item.docHeight;

    for (int ai = 0; ai < (int)item.msg.attachments.size(); ++ai) {
        y += kAttachGap;
        if (!isDismissed(item.msg.ts, ai)) {
            if (ai == attachIdx)
                return QRect(btnX, y, kDismissW, kDismissW);
            y += attachTotalH(item, ai);
        }
    }
    return {};
}

// Returns the first link URL in the message text entities, or empty string.
static QString firstLinkInMessage(const Message &msg) {
    for (const auto &ent : msg.text.entities) {
        if (ent.type == EntityType::Link && !ent.data.isEmpty())
            return ent.data;
    }
    return {};
}

void MessageListWidget::doMousePress(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return;

    // Clear any existing selection; it may be re-established below if the press lands on text.
    clearSelection();

    if (tryHandleScrollbarPress(event->pos()))
        return;
    if (tryHandleToolbarPress(event->pos()))
        return;
    if (tryHandleReactionPress(event->pos()))
        return;
    if (tryHandleDismissPress(event->pos()))
        return;
    if (tryHandleReplyBarPress(event->pos()))
        return;
    if (tryHandleLinkPress(event->pos()))
        return;
    if (tryHandleFileActionBarPress(event->pos()))
        return;

    // Start text-selection drag if the click lands inside a message body.
    const TextPos tp = textHitTest(event->pos());
    if (tp.row >= 0) {
        _selAnchor   = tp;
        _selFocus    = tp;
        _selDragging = true;
        viewport()->update();
        return;
    }

    tryHandleFileChipPress(event->pos());
}

bool MessageListWidget::tryHandleScrollbarPress(const QPoint &pos) {
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (pos.x() < sbHitX || !isOnScrollThumb(pos.y()))
        return false;
    _sbDragging        = true;
    _sbDragStartY      = pos.y();
    _sbDragStartScroll = verticalScrollBar()->value();
    viewport()->setCursor(Qt::SizeVerCursor);
    return true;
}

bool MessageListWidget::tryHandleToolbarPress(const QPoint &pos) {
    const int btn = toolbarButtonAt(pos);
    if (btn < 0 || _hoveredRow < 0)
        return false;

    const auto  &msg       = _items[_hoveredRow].msg;
    const int    scrollY   = verticalScrollBar()->value();
    const int    rowTop    = _tops[_hoveredRow] - scrollY;
    const int    rh        = rowHeight(_hoveredRow);
    const int    sep       = needsDateSep(_hoveredRow) ? kSepH : 0;
    const QRect  btnRect   = toolbarButtonRect(btn, rowTop + sep, rh - sep);
    const QPoint globalPos = viewport()->mapToGlobal(btnRect.bottomLeft());

    if (btn == 0)
        openEmojiPickerForRow(_hoveredRow, globalPos);
    else if (btn == 1)
        emit forwardMessageRequested(msg);
    else if (btn == 2)
        showMessageContextMenu(msg, globalPos);
    return true;
}

void MessageListWidget::openEmojiPickerForRow(int row, const QPoint &globalPos) {
    const Ts             ts   = _items[row].msg.ts;
    const ConversationId conv = _currentConv;
    _emojiPicker->open(globalPos);
    connect(
        _emojiPicker,
        &EmojiPickerPopup::emojiSelected,
        this,
        [this, ts, conv](const QString &name) {
            if (!_session)
                return;
            _session->backend()->addReaction(conv, ts, name);
            // Optimistic: add reaction locally so it appears immediately.
            const int idx = findByTs(ts);
            if (idx >= 0) {
                const UserId me        = _session->meUserId();
                auto        &reactions = _items[idx].msg.reactions;
                bool         found     = false;
                for (auto &r : reactions) {
                    if (r.name == name) {
                        if (std::find(r.users.begin(), r.users.end(), me) == r.users.end()) {
                            r.count++;
                            r.users.push_back(me);
                        }
                        found = true;
                        break;
                    }
                }
                if (!found)
                    reactions.push_back({name, 1, {me}});
                rebuildLayout();
                viewport()->update();
            }
        },
        Qt::SingleShotConnection
    );
}

void MessageListWidget::showMessageContextMenu(const Message &msg, const QPoint &globalPos) {
    const bool    isOwnMessage = _session && (msg.author == _session->meUserId());
    const bool    canDelete    = isOwnMessage || (_session && _session->meIsAdmin());
    const QString linkUrl      = firstLinkInMessage(msg);

    auto *menu = new ContextMenu(this);

    if (isOwnMessage) {
        menu->addItem(
            tr("Edit message"),
            "E",
            [this, ts = msg.ts, raw = msg.rawText, files = msg.files] {
                emit editMessageRequested(ts, raw, files);
            },
            false,
            false,
            ":/ui/edit-3.svg"
        );
        menu->addSeparator();
    }

    if (!linkUrl.isEmpty()) {
        menu->addItem(
            tr("Copy link"),
            "L",
            [linkUrl] { Clipboard::setText(linkUrl); },
            false,
            false,
            ":/ui/link.svg"
        );
    }

    menu->addItem(
        tr("Copy message"),
        "Ctrl+C",
        [text = msg.text.text] { Clipboard::setText(text); },
        false,
        false,
        ":/ui/copy.svg"
    );

    menu->addSeparator();

    if (msg.pinned) {
        menu->addItem(
            tr("Unpin from channel"),
            "P",
            [this, ts = msg.ts, conv = _currentConv] {
                if (_session)
                    _session->backend()->unpinMessage(conv, ts);
                const int idx = findByTs(ts);
                if (idx >= 0) {
                    _items[idx].msg.pinned = false;
                    viewport()->update();
                }
            },
            false,
            false,
            ":/ui/pin-off.svg"
        );
    } else {
        menu->addItem(
            tr("Pin to channel"),
            "P",
            [this, ts = msg.ts, conv = _currentConv] {
                if (_session)
                    _session->backend()->pinMessage(conv, ts);
                const int idx = findByTs(ts);
                if (idx >= 0) {
                    _items[idx].msg.pinned   = true;
                    _items[idx].msg.pinnedBy = _session->meUserId();
                    viewport()->update();
                }
            },
            false,
            false,
            ":/ui/pin.svg"
        );
    }

    menu->addSeparator();

    menu->addItem(
        tr("Forward message"),
        {},
        [this, msg] { emit forwardMessageRequested(msg); },
        false,
        false,
        ":/ui/share-2.svg"
    );

    if (canDelete) {
        menu->addSeparator();
        menu->addItem(
            tr("Delete message…"),
            "Del",
            [this, msg] {
                auto *dlg = new DeleteMessageDialog(msg, _session, window());
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &QDialog::accepted, this, [this, ts = msg.ts, conv = _currentConv] {
                    if (_session)
                        _session->backend()->deleteMessage(conv, ts);
                });
                dlg->exec();
            },
            /*destructive=*/true,
            false,
            ":/ui/trash-2.svg"
        );
    }

    menu->popup(globalPos);
}

bool MessageListWidget::tryHandleReactionPress(const QPoint &pos) {
    const auto [reactMsgIdx, reactIdx] = reactionAt(pos);
    if (reactMsgIdx < 0 || !_session)
        return false;

    const QString emojiName = _items[reactMsgIdx].msg.reactions[reactIdx].name;
    const Ts      reactTs   = _items[reactMsgIdx].msg.ts;
    auto         &reactions = _items[reactMsgIdx].msg.reactions;
    const UserId  me        = _session->meUserId();
    const bool    already   = std::any_of(
        reactions[reactIdx].users.begin(), reactions[reactIdx].users.end(), [&me](const UserId &u) {
            return u == me;
        }
    );
    if (already) {
        _session->backend()->removeReaction(_currentConv, reactTs, emojiName);
        for (auto it = reactions.begin(); it != reactions.end(); ++it) {
            if (it->name == emojiName) {
                it->count = std::max(0, it->count - 1);
                it->users.erase(
                    std::remove(it->users.begin(), it->users.end(), me), it->users.end()
                );
                if (it->count == 0)
                    reactions.erase(it);
                break;
            }
        }
    } else {
        _session->backend()->addReaction(_currentConv, reactTs, emojiName);
        bool found = false;
        for (auto &rx : reactions) {
            if (rx.name == emojiName) {
                rx.count++;
                rx.users.push_back(me);
                found = true;
                break;
            }
        }
        if (!found)
            reactions.push_back({emojiName, 1, {me}});
    }
    rebuildLayout();
    viewport()->update();
    return true;
}

bool MessageListWidget::tryHandleDismissPress(const QPoint &pos) {
    const auto [dMsgIdx, dAi] = dismissButtonAt(pos);
    if (dMsgIdx < 0)
        return false;
    const auto &ts = _items[dMsgIdx].msg.ts;
    _dismissedAttachments.insert(ts + "/" + QString::number(dAi));
    rebuildLayout();
    viewport()->update();
    return true;
}

bool MessageListWidget::tryHandleReplyBarPress(const QPoint &pos) {
    const int replyIdx = replyBarIndexAt(pos);
    if (replyIdx < 0)
        return false;
    emit threadClicked(_currentConv, _items[replyIdx].msg.ts);
    return true;
}

bool MessageListWidget::tryHandleLinkPress(const QPoint &pos) {
    const QString anchor = anchorAt(pos);
    if (anchor.isEmpty())
        return false;
    QDesktopServices::openUrl(QUrl(anchor));
    return true;
}

bool MessageListWidget::tryHandleFileActionBarPress(const QPoint &pos) {
    const int btn = fileActionBarButtonAt(pos);
    if (btn < 0 || _hoveredFile.first < 0)
        return false;

    const auto  &msg  = _items[_hoveredFile.first].msg;
    const auto  &file = msg.files[_hoveredFile.second];
    const QRect  fr   = fileViewportRect(_hoveredFile.first, _hoveredFile.second);
    const QRect  btnR = fileActionBarButtonRect(btn, fr);
    const QPoint gPos = viewport()->mapToGlobal(btnR.bottomLeft());

    if (btn == 0)
        downloadFileToUser(file);
    else if (btn == 1)
        emit forwardMessageRequested(msg);
    else if (btn == 2)
        showFileContextMenu(file, msg, gPos);
    return true;
}

void MessageListWidget::downloadFileToUser(const File &file) {
    if (!_session)
        return;
    const QString defaultName = file.name.isEmpty() ? tr("file") : file.name;
    const QString savePath =
        QFileDialog::getSaveFileName(this, tr("Save file"), QDir::homePath() + "/" + defaultName);
    if (savePath.isEmpty())
        return;
    const QString url = file.urlPrivate;
    _session->downloadFile(
        url,
        [savePath](QByteArray data) {
            QFile f(savePath);
            if (f.open(QIODevice::WriteOnly))
                f.write(data);
        },
        [](QString err) { qWarning() << "File download failed:" << err; }
    );
}

void MessageListWidget::showFileContextMenu(
    const File &file, const Message &msg, const QPoint &globalPos
) {
    const bool isImage = file.isImage();
    auto      *menu    = new ContextMenu(this);

    const QString linkUrl = file.permalink.isEmpty() ? file.urlPrivate : file.permalink;
    menu->addItem(
        isImage ? tr("Copy link to image") : tr("Copy link to file"),
        {},
        [linkUrl] { Clipboard::setText(linkUrl); },
        false,
        false,
        ":/ui/link.svg"
    );

    const bool canDelete =
        _session && (msg.author == _session->meUserId() || _session->meIsAdmin());
    if (canDelete && !file.id.isEmpty()) {
        menu->addSeparator();
        menu->addItem(
            isImage ? tr("Delete image…") : tr("Delete file…"),
            {},
            [this, fileId = file.id] {
                if (_session)
                    _session->backend()->deleteFile(fileId);
            },
            /*destructive=*/true,
            false,
            ":/ui/trash-2.svg"
        );
    }

    menu->popup(globalPos);
}

bool MessageListWidget::tryHandleFileChipPress(const QPoint &pos) {
    const File *f = fileChipAt(pos);
    if (!f)
        return false;
    const QString url = f->permalink.isEmpty() ? f->urlPrivate : f->permalink;
    if (!url.isEmpty())
        QDesktopServices::openUrl(QUrl(url));
    return true;
}

void MessageListWidget::doMouseLeave() {
    // Showing the tooltip (a separate window) fires a spurious Leave event on
    // the viewport on X11 even though the cursor never left.  Ignore it.
    if (viewport()->rect().contains(viewport()->mapFromGlobal(QCursor::pos())))
        return;

    _tooltip->hide();

    if (!_hoveredLinkUrl.isEmpty()) {
        if (_hoveredLinkRow >= 0 && _hoveredLinkRow < (int)_items.size()) {
            setDocLinkUnderline(_items[_hoveredLinkRow].textDoc.get(), _hoveredLinkUrl, false);
            for (auto &ad : _items[_hoveredLinkRow].attachDocs)
                setDocLinkUnderline(ad.textDoc.get(), _hoveredLinkUrl, false);
        }
        _hoveredLinkUrl.clear();
        _hoveredLinkRow = -1;
    }

    if (_hoveredRow != -1 || _hoveredToolBtn != -1 || _hoveredAttach.first != -1 ||
        _hoveredReplyRow != -1 || _hoveredFile.first != -1) {
        _hoveredRow      = -1;
        _hoveredToolBtn  = -1;
        _hoveredAttach   = {-1, -1};
        _hoveredReplyRow = -1;
        _hoveredFile     = {-1, -1};
        _hoveredFileBtn  = -1;
        viewport()->update();
    }
}

bool MessageListWidget::isOnScrollThumb(int vpY) const {
    return VirtualListWidget::isOnScrollThumb(vpY, _totalH);
}

// ── Text selection ────────────────────────────────────────────────────────────

TextPos MessageListWidget::textHitTest(const QPoint &viewportPos) const {
    if (_items.empty() || _tops.empty())
        return {};
    const PaintContext ctx      = makePaintContext();
    const int          scrollY  = ctx.scrollY;
    const int          docY     = viewportPos.y() + scrollY;
    const int          textLeft = ctx.textLeft;

    for (int i = 0; i < (int)_items.size(); ++i) {
        const int rowTop = _tops[i];
        const int rh     = rowHeight(i);
        if (docY < rowTop)
            break;
        if (docY > rowTop + rh)
            continue;

        const auto &item = _items[i];
        ensureDocLayout(item);
        if (!item.textDoc || item.docHeight <= 0)
            continue;

        const bool coll    = isCollapsed(i);
        const int  padV    = coll ? kPadVCollapsed : kPadV;
        const int  sepH    = needsDateSep(i) ? kSepH : 0;
        const int  pinnedH = item.msg.pinned ? 18 : 0;
        const int  textTop = rowTop + sepH + pinnedH + padV + (coll ? 0 : kHdrH + kHdrGap);

        const QPointF local(viewportPos.x() - textLeft, docY - textTop);
        if (local.y() < 0 || local.y() > item.docHeight)
            continue;
        const QPointF clamped(std::max(0.0, local.x()), local.y());
        const int     hit = item.textDoc->documentLayout()->hitTest(clamped, Qt::FuzzyHit);
        if (hit >= 0)
            return {i, hit};
    }
    return {};
}

void MessageListWidget::clearSelection() {
    if (_selAnchor.row != -1 || _selFocus.row != -1 || _selDragging) {
        _selAnchor   = {};
        _selFocus    = {};
        _selDragging = false;
        viewport()->update();
    }
}

bool MessageListWidget::hasSelection() const {
    return _selAnchor.row >= 0 && _selFocus.row >= 0 && _selAnchor != _selFocus;
}

QString MessageListWidget::selectedText() const {
    if (!hasSelection())
        return {};

    int aRow = _selAnchor.row, aOff = _selAnchor.offset;
    int fRow = _selFocus.row, fOff = _selFocus.offset;
    if (aRow > fRow || (aRow == fRow && aOff > fOff)) {
        std::swap(aRow, fRow);
        std::swap(aOff, fOff);
    }

    QStringList parts;
    for (int i = aRow; i <= fRow && i < (int)_items.size(); ++i) {
        const auto &item = _items[i];
        ensureDocLayout(item);
        if (!item.textDoc)
            continue;
        const int from = (i == aRow) ? aOff : 0;
        const int to   = (i == fRow) ? fOff : item.textDoc->characterCount();
        if (from >= to)
            continue;
        QTextCursor cur(item.textDoc.get());
        cur.setPosition(from);
        cur.setPosition(to, QTextCursor::KeepAnchor);
        QString text = cur.selectedText();
        text.replace(QChar::ParagraphSeparator, '\n');
        if (!text.isEmpty())
            parts.append(text);
    }
    return parts.join('\n');
}

void MessageListWidget::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Copy) && hasSelection()) {
        Clipboard::setText(selectedText());
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && hasSelection()) {
        clearSelection();
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void MessageListWidget::doMouseRelease(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (_sbDragging) {
        _sbDragging = false;
        viewport()->setCursor(Qt::ArrowCursor);
        return;
    }
    if (_selDragging) {
        _selDragging = false;
        // If the cursor never moved off the anchor position, treat as a plain click — no selection.
        if (_selAnchor == _selFocus) {
            _selAnchor = {};
            _selFocus  = {};
        }
        viewport()->update();
    }
}

void MessageListWidget::doMouseMove(QMouseEvent *event) {
    if (_sbDragging) {
        const int vh         = viewport()->height();
        const int thumbH     = std::max(20, vh * vh / _totalH);
        const int trackRange = vh - thumbH;
        if (trackRange > 0) {
            const int newScroll = _sbDragStartScroll +
                                  (event->pos().y() - _sbDragStartY) * (_totalH - vh) / trackRange;
            verticalScrollBar()->setValue(std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }

    if (_selDragging) {
        const TextPos tp = textHitTest(event->pos());
        if (tp.row >= 0 && tp != _selFocus) {
            _selFocus = tp;
            viewport()->update();
        }
        viewport()->setCursor(Qt::IBeamCursor);
        return;
    }

    const QPoint pos     = event->pos();
    const int    scrollY = verticalScrollBar()->value();

    const int vw    = viewport()->width();
    const int cardW = kToolbarPadH * 2 + 3 * kToolbarBtnSize + 2 * kToolbarGap;
    const int cardH = kToolbarPadV * 2 + kToolbarBtnSize;

    // If a row is already hovered, keep it as long as the mouse remains inside
    // that row's rect (any X) OR its toolbar card rect.  The card straddles the
    // row's top edge, so without this the hover would flip to the row above
    // the moment the cursor enters the card's upper half.
    int newHoveredRow = -1;
    if (_hoveredRow >= 0) {
        const int   rowTop = _tops[_hoveredRow] - scrollY;
        const int   rh     = rowHeight(_hoveredRow);
        const bool  inRow  = pos.y() >= rowTop && pos.y() < rowTop + rh;
        const int   sep4   = needsDateSep(_hoveredRow) ? kSepH : 0;
        const QRect card(vw - kToolbarRight - cardW, (rowTop + sep4) - cardH / 2, cardW, cardH);
        if (inRow || card.contains(pos))
            newHoveredRow = _hoveredRow;
    }

    // Mouse has left the current row's combined zone — geometric hit-test.
    if (newHoveredRow < 0) {
        for (int i = 0; i < (int)_items.size(); ++i) {
            const int rowTop = _tops[i] - scrollY;
            const int rh     = rowHeight(i);
            if (rowTop > pos.y())
                break;
            if (pos.y() < rowTop + rh) {
                newHoveredRow = i;
                break;
            }
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
    std::pair<int, int> newHoveredAttach = {-1, -1};
    if (newHoveredRow >= 0) {
        const auto &item = _items[newHoveredRow];
        if (!item.msg.attachments.empty()) {
            ensureDocLayout(item);
            const bool collA = isCollapsed(newHoveredRow);
            const int  padVA = collA ? kPadVCollapsed : kPadV;
            const int  sepA  = needsDateSep(newHoveredRow) ? kSepH : 0;
            const int  pinHA = item.msg.pinned ? 18 : 0;
            const int  rtA   = _tops[newHoveredRow] - scrollY;
            int ay = rtA + sepA + pinHA + padVA + (collA ? 0 : kHdrH + kHdrGap) + item.docHeight;
            for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
                ay += kAttachGap;
                if (!isDismissed(item.msg.ts, ai)) {
                    const int ah = attachTotalH(item, ai);
                    if (pos.y() >= ay && pos.y() < ay + ah) {
                        newHoveredAttach = {newHoveredRow, ai};
                        break;
                    }
                    ay += ah;
                }
            }
        }
    }

    const int newHoveredReplyRow = replyBarIndexAt(pos);

    // Detect which file chip or image (if any) the cursor is over, and which action bar button.
    std::pair<int, int> newHoveredFile    = {-1, -1};
    int                 newHoveredFileBtn = -1;
    if (newHoveredRow >= 0) {
        const auto &fitem    = _items[newHoveredRow];
        const int   nButtons = 3;
        const int   fCardW =
            kToolbarPadH * 2 + nButtons * kToolbarBtnSize + (nButtons - 1) * kToolbarGap;
        const int fCardH = kToolbarPadV * 2 + kToolbarBtnSize;
        for (int fi = 0; fi < (int)fitem.msg.files.size(); ++fi) {
            const QRect fr = fileViewportRect(newHoveredRow, fi);
            if (fr.isNull())
                continue;
            const QRect cardArea(fr.right() - fCardW, fr.top() - fCardH / 2, fCardW, fCardH);
            if (!fr.contains(pos) && !cardArea.contains(pos))
                continue;
            newHoveredFile = {newHoveredRow, fi};
            for (int b = 0; b < nButtons; ++b) {
                if (fileActionBarButtonRect(b, fr).contains(pos)) {
                    newHoveredFileBtn = b;
                    break;
                }
            }
            break;
        }
    }

    if (newHoveredRow != _hoveredRow || newHoveredBtn != _hoveredToolBtn ||
        newHoveredAttach != _hoveredAttach || newHoveredReplyRow != _hoveredReplyRow ||
        newHoveredFile != _hoveredFile || newHoveredFileBtn != _hoveredFileBtn) {
        _hoveredRow      = newHoveredRow;
        _hoveredToolBtn  = newHoveredBtn;
        _hoveredAttach   = newHoveredAttach;
        _hoveredReplyRow = newHoveredReplyRow;
        _hoveredFile     = newHoveredFile;
        _hoveredFileBtn  = newHoveredFileBtn;
        viewport()->update();
    }

    // Compute anchor once and reuse for link hover, tooltip, and cursor
    const QString anchor = anchorAt(pos);

    // Update link hover underline
    if (anchor != _hoveredLinkUrl) {
        if (!_hoveredLinkUrl.isEmpty() && _hoveredLinkRow >= 0 &&
            _hoveredLinkRow < (int)_items.size()) {
            setDocLinkUnderline(_items[_hoveredLinkRow].textDoc.get(), _hoveredLinkUrl, false);
            for (auto &ad : _items[_hoveredLinkRow].attachDocs)
                setDocLinkUnderline(ad.textDoc.get(), _hoveredLinkUrl, false);
        }
        _hoveredLinkUrl = anchor;
        _hoveredLinkRow = newHoveredRow;
        if (!anchor.isEmpty() && newHoveredRow >= 0) {
            setDocLinkUnderline(_items[newHoveredRow].textDoc.get(), anchor, true);
            for (auto &ad : _items[newHoveredRow].attachDocs)
                setDocLinkUnderline(ad.textDoc.get(), anchor, true);
        }
        viewport()->update();
    }

    // Tooltip
    if (newHoveredBtn >= 0) {
        static const QString kTips[] = {
            tr("Add reaction"), tr("Forward message"), tr("More actions")
        };
        const int   rowTop   = _tops[_hoveredRow] - scrollY;
        const int   rh       = rowHeight(_hoveredRow);
        const int   sep6     = needsDateSep(_hoveredRow) ? kSepH : 0;
        const QRect btnLocal = toolbarButtonRect(newHoveredBtn, rowTop + sep6, rh - sep6);
        const QRect btnGlobal(viewport()->mapToGlobal(btnLocal.topLeft()), btnLocal.size());
        _tooltip->showAbove(kTips[newHoveredBtn], btnGlobal);
    } else if (newHoveredFileBtn >= 0 && newHoveredFile.first >= 0) {
        static const QString kFileTips[] = {tr("Download"), tr("Share"), tr("More actions")};
        const QRect          fr = fileViewportRect(newHoveredFile.first, newHoveredFile.second);
        const QRect          btnLocal = fileActionBarButtonRect(newHoveredFileBtn, fr);
        const QRect btnGlobal(viewport()->mapToGlobal(btnLocal.topLeft()), btnLocal.size());
        _tooltip->showAbove(kFileTips[newHoveredFileBtn], btnGlobal);
    } else if (!anchor.isEmpty()) {
        // Collect link display text; skip tooltip when it is identical to the URL.
        QString linkText;
        if (_hoveredLinkRow >= 0 && _hoveredLinkRow < (int)_items.size()) {
            const auto &item = _items[_hoveredLinkRow];
            linkText         = collectLinkText(item.textDoc.get(), anchor);
            if (linkText.isEmpty()) {
                for (const auto &ad : item.attachDocs) {
                    linkText = collectLinkText(ad.textDoc.get(), anchor);
                    if (!linkText.isEmpty())
                        break;
                }
            }
        }
        if (linkText != anchor) {
            const QPoint gPos = viewport()->mapToGlobal(pos);
            _tooltip->showAbove(anchor, QRect(gPos - QPoint(0, 2), QSize(1, 4)));
        } else {
            _tooltip->hide();
        }
    } else {
        const auto [dMsgIdx, dAi] = dismissButtonAt(pos);
        const bool attachHovered =
            _hoveredAttach.first == dMsgIdx && _hoveredAttach.second == dAi && dMsgIdx >= 0;
        if (attachHovered) {
            const QRect btnLocal = dismissButtonVpRect(dMsgIdx, dAi);
            const QRect btnGlobal(viewport()->mapToGlobal(btnLocal.topLeft()), btnLocal.size());
            _tooltip->showAbove(tr("Remove preview"), btnGlobal);
        } else {
            _tooltip->hide();
        }
    }

    // Cursor
    const auto [dMI, dAI] = dismissButtonAt(pos);
    const bool overDismiss =
        dMI >= 0 && _hoveredAttach.first == dMI && _hoveredAttach.second == dAI;
    const auto [rMI, rRI]  = reactionAt(pos);
    // File action bar buttons take priority — keep arrow cursor over them even if a chip is below.
    const bool overFileBar = newHoveredFileBtn >= 0;
    const bool overLink    = !overFileBar && (!anchor.isEmpty() || fileChipAt(pos) ||
                                           replyBarIndexAt(pos) >= 0 || overDismiss || rMI >= 0);
    const int  sbHitX      = viewport()->width() - kScrollW - 2 - 6;
    const bool overScroll  = pos.x() >= sbHitX && isOnScrollThumb(pos.y());
    const bool overText    = !overLink && textHitTest(pos).row >= 0;
    if (overScroll)
        viewport()->setCursor(Qt::SizeVerCursor);
    else if (overLink)
        viewport()->setCursor(Qt::PointingHandCursor);
    else if (overText)
        viewport()->setCursor(Qt::IBeamCursor);
    else
        viewport()->setCursor(Qt::ArrowCursor);
}

// ── Live event handling ───────────────────────────────────────────────────────

void MessageListWidget::handleEvent(const Event &e) {
    const bool wasAtBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;

    if (auto *ev = std::get_if<EvMessageNew>(&e)) {
        if (ev->conv != _currentConv)
            return;
        if (_isThreadMode) {
            // In thread mode, only show messages belonging to this thread.
            const bool isRoot = ev->msg.ts == _threadRootTs;
            const bool isReply =
                ev->msg.threadRoot.has_value() && *ev->msg.threadRoot == _threadRootTs;
            if (!isRoot && !isReply)
                return;
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
        if (ev->conv != _currentConv)
            return;
        const int i = findByTs(ev->msg.ts);
        if (i < 0)
            return;
        _items[i].msg = ev->msg;
        _items[i].textDoc.reset(); // invalidate rendered docs
        _items[i].docWidth = 0;
        _items[i].attachDocs.clear();
        _items[i].fileImgsRequested = false;
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvMessageDeleted>(&e)) {
        if (ev->conv != _currentConv)
            return;
        const int i = findByTs(ev->ts);
        if (i < 0)
            return;
        _items.erase(_items.begin() + i);
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvReactionAdded>(&e)) {
        if (ev->conv != _currentConv)
            return;
        const int i = findByTs(ev->ts);
        if (i < 0)
            return;
        auto &reactions = _items[i].msg.reactions;
        bool  found     = false;
        for (auto &r : reactions) {
            if (r.name == ev->name) {
                r.count++;
                r.users.push_back(ev->user);
                found = true;
                break;
            }
        }
        if (!found)
            reactions.push_back({ev->name, 1, {ev->user}});
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvReactionRemoved>(&e)) {
        if (ev->conv != _currentConv)
            return;
        const int i = findByTs(ev->ts);
        if (i < 0)
            return;
        auto &reactions = _items[i].msg.reactions;
        for (auto rit = reactions.begin(); rit != reactions.end(); ++rit) {
            if (rit->name == ev->name) {
                rit->count  = std::max(0, rit->count - 1);
                auto &users = rit->users;
                users.erase(std::remove(users.begin(), users.end(), ev->user), users.end());
                if (rit->count == 0)
                    reactions.erase(rit);
                break;
            }
        }
        rebuildLayout();
        viewport()->update();
    }
}
