// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_list.h"
#include "message_render.h"
#include "session/session.h"
#include "backend/backend.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/icon_utils.h"
#include "ui/file_dialog_utils.h"
#include "ui/image_cache.h"
#include "ui/context_menu/context_menu.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/emoji_picker/emoji_picker_popup.h"
#include "ui/user_profile_card/user_profile_card.h"
#include "ui/image_viewer/image_viewer.h"
#include "ui/table_viewer/table_viewer.h"
#include "ui/delete_message_dialog/delete_message_dialog.h"
#include "ui/summary_dialog/summarize_job.h"
#include "ui/summary_dialog/summary_dialog.h"
#include "llm/llm_service.h"
#include "util/background_tasks.h"
#include "util/clipboard.h"
#include "util/mailto_link.h"

#include <QBuffer>
#include <QImage>
#include <QMovie>
#include <QThreadPool>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFontMetrics>
#include <QDesktopServices>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QUrl>
#include <QTimer>
#include <QCursor>

#include <algorithm>
#include <cmath>
#include <limits>

// ── MessageListWidget ─────────────────────────────────────────────────────────

MessageListWidget::MessageListWidget(Session *session, ImageCache *imgCache, QWidget *parent)
    : VirtualListWidget(parent), _session(session), _imgCache(imgCache) {
    verticalScrollBar()->setSingleStep(20);
    setFocusPolicy(Qt::ClickFocus);

    _tooltip     = new PopupTooltip(this);
    _emojiPicker = new EmojiPickerPopup(this);
    _emojiPicker->setImageCache(_imgCache);

    _profileCard = new UserProfileCard(this);
    connect(_profileCard, &UserProfileCard::messageRequested, this, [this](UserId user) {
        hideProfileCard();
        emit openDmRequested(user);
    });
    _profileShowTimer.setSingleShot(true);
    _profileShowTimer.setInterval(300);
    connect(&_profileShowTimer, &QTimer::timeout, this, [this] {
        const QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
        if (!_pendingProfileAnchor.isEmpty()) {
            // Show only if the cursor is still on the same mention.
            if (anchorAt(vpPos) != _pendingProfileAnchor)
                return;
            const QString uid = MsgRender::userIdFromAnchor(_pendingProfileAnchor);
            if (!uid.isEmpty())
                showProfileCardFor(uid, userAnchorVpRect(vpPos, _pendingProfileAnchor));
        } else if (!_pendingProfileAvatarUser.value.isEmpty()) {
            // Show only if the cursor is still on the same avatar.
            QRect         avRect;
            const QString uid = avatarUserAt(vpPos, &avRect);
            if (uid == _pendingProfileAvatarUser.value)
                showProfileCardFor(uid, avRect);
        }
    });

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

    // Once scrolling settles, release the rendered docs of rows now far from
    // the viewport (deep scroll-back would otherwise keep thousands alive).
    _docTrimTimer.setSingleShot(true);
    _docTrimTimer.setInterval(1000);
    connect(&_docTrimTimer, &QTimer::timeout, this, [this] { trimOffscreenDocs(); });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        // Theme colors are baked into the cached docs as inline HTML spans at
        // build time — a switch to a theme that retints content (dark) must
        // rebuild them, not just repaint.
        for (auto &item : _items) {
            item.textDoc.reset();
            item.attachDocs.clear();
            item.docWidth = -1;
        }
        rebuildLayout();
        viewport()->update();
    });

    if (_imgCache) {
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
            // Custom-emoji image arrived: docs referencing it were laid out
            // without the resource, so rebuild them from scratch.
            for (auto &item : _items) {
                if (item.emojiUrlsCollected && item.emojiUrls.contains(url)) {
                    item.textDoc.reset();
                    item.attachDocs.clear();
                    item.docWidth = -1;
                }
            }
            const bool wasAtBottom =
                verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
            rebuildLayout();
            if (wasAtBottom)
                verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            viewport()->update();
        });
        // Deliver the avatar to the profile card if it arrives while shown.
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
            if (_profileCard->isVisible() && url == _profileCard->avatarUrl())
                _profileCard->updateAvatar(_imgCache->get(url));
        });
    }
}

MessageListWidget::~MessageListWidget() {
    // The shared ImageCache outlives this widget — un-pin our movie holds so
    // its entries return to normal LRU eviction.
    releaseGifMovies();
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
    hideProfileCard(); // anchor moved away from under the card
    viewport()->update();
    _docTrimTimer.start(); // (re)arm: trim docs once scrolling settles
    if (verticalScrollBar()->value() <= loadOlderMargin())
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
    maybeFillViewport();
    applyPendingScroll();
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
    _topsTs.clear();
    _totalH = 0;
    // Animated players belong to the conversation being left; keeping them
    // would pin their ImageCache entries (bytes + decoded frames) forever.
    // _fileImages/_scaledPreviews stay — they're byte-capped (enforce*Cap) and
    // keeping them makes switching back to a recent conversation instant.
    releaseGifMovies();
    // Inline thread expansions belong to the conversation being left.
    _inlineThreads.clear();
    _openThreadRoot.clear();
    hideProfileCard();
    _hoveredLinkUrl.clear();
    _hoveredLinkRow  = -1;
    _hoveredRow      = -1;
    _hoveredToolBtn  = -1;
    _hoveredAttach   = {-1, -1};
    _hoveredTable    = {};
    _hoveredReplyRow = -1;
    _hoveredThreadFooter.clear();
    _hoveredFile           = {-1, -1};
    _hoveredFileBtn        = -1;
    _selAnchor             = {};
    _selFocus              = {};
    _selDragging           = false;
    _scrollToBottomPending = false;
    _pendingLastReadTs.clear();
    _pendingAnchorTs.clear();
    _pendingAnchorDelta = 0;
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
    // Workspace switch arrives here (not openConversation). The reading position
    // of the chat we're leaving is saved by activateWorkspace *before* it hides
    // the composer/header — doing it here would read a viewport that already grew
    // (composer hidden), clamping a scrolled-up position to a false "at bottom".
    //
    // Snapshot the loaded messages first (same as openConversation): if the user
    // scrolled up, the view holds paginated *older* messages that aren't in the
    // plain cache. Without this, returning to the workspace can't find the saved
    // anchor (it's older than the no-cursor history page) and falls to the bottom.
    if (!_currentConv.value.isEmpty() && _session && !_isThreadMode && !_items.empty()) {
        std::vector<Message> msgs;
        msgs.reserve(_items.size());
        for (const auto &item : _items)
            msgs.push_back(item.msg);
        _session->cacheMessages(_currentConv, msgs);
    }
    clear();
    _currentConv = {};
    _session     = session;
    if (_emojiPicker)
        _emojiPicker->setSession(session);
}

void MessageListWidget::openConversation(ConversationId conv, const Ts &lastReadTs) {
    // Persist messages of the conversation we're leaving before discarding them.
    if (!_currentConv.value.isEmpty() && _session && !_items.empty()) {
        std::vector<Message> msgs;
        msgs.reserve(_items.size());
        for (const auto &item : _items)
            msgs.push_back(item.msg);
        _session->cacheMessages(_currentConv, msgs);
    }

    saveScrollAnchor();
    clear();
    _currentConv  = conv;
    _isThreadMode = false;
    _threadRootTs = {};

    _session->events() | rpl::on_next([this](Event e) { handleEvent(e); }, _eventLifetime);

    _session->userInfoLoaded() |
        rpl::on_next([this](UserId id) { onUserResolved(id); }, _eventLifetime);

    _session->botInfoLoaded() | rpl::on_next(
                                    [this](UserId) {
                                        rebuildLayout();
                                        viewport()->update();
                                    },
                                    _eventLifetime
                                );

    // Fresh emoji.list arrived: :codes: that resolved to nothing (or to stale
    // URLs) while the map was empty must be re-rendered.
    _session->emojiMapLoaded() | rpl::on_next([this] { invalidateAllDocs(); }, _eventLifetime);

    // If we've shown this chat before, restore exactly where the user left it —
    // ignoring the unread target so switching chats/workspaces never jumps the
    // view. Only the first open of a chat lands on the first unread message
    // (or the bottom if all read).
    _scrollToBottomPending = true;
    if (const auto it = _savedAnchors.constFind(conv.value); it != _savedAnchors.constEnd()) {
        if (!it->atBottom) {
            _pendingAnchorTs    = it->ts;
            _pendingAnchorDelta = it->delta;
        }
        // atBottom: leave both targets empty → plain scroll-to-bottom.
    } else {
        _pendingLastReadTs = lastReadTs;
    }

    // Pre-populate from cache for instant display while network loads.
    const bool hasCached = [&] {
        if (!_session)
            return false;
        const auto cached = _session->cachedMessages(conv);
        if (cached.empty())
            return false;
        appendMessages(cached);
        // Pre-warm the image pixel cache from disk so images appear without download.
        for (const auto &item : _items) {
            for (const auto &f : item.msg.files) {
                if (!f.hasPreview())
                    continue;
                const QString url = filePreviewUrl(f);
                if (_fileImages.contains(url))
                    continue;
                const auto data = _session->cachedImage(url);
                if (data.isEmpty())
                    continue;
                QPixmap px;
                if (px.loadFromData(data) && !px.isNull()) {
                    _fileImages[url] = px;
                    ++_fileImagesGen;
                    // clear() released this conversation's players — an animated
                    // file needs its player back or it repaints as a still.
                    maybeCreateFileGifMovie(url, data);
                }
            }
        }
        enforceFileImageCap();
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

                if (hasCached) {
                    // Merge: network data may differ slightly (edits, reactions) but is
                    // mostly the same as the cached view, so the update is imperceptible.
                    // Runs even for an empty page so a conversation whose only/last
                    // message was deleted elsewhere clears instead of showing the stale
                    // cached copy.
                    const bool wasAtBottom =
                        verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
                    mergeNetworkMessages(page.messages, /*fromHeadPage=*/true);
                    if (_scrollToBottomPending) {
                        applyPendingScroll();
                        // The network page is authoritative — if the saved or
                        // unread anchor wasn't found in it, stop retargeting.
                        _scrollToBottomPending = false;
                        _pendingLastReadTs.clear();
                        _pendingAnchorTs.clear();
                    } else if (wasAtBottom) {
                        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
                    }
                } else {
                    // No cached data was shown — normal first-load path.
                    appendMessages(page.messages);
                    emit initialPageLoaded();
                    QTimer::singleShot(0, this, [this, conv] {
                        if (_currentConv != conv)
                            return;
                        applyPendingScroll();
                    });
                }
                maybeFillViewport();
            },
            _loadLifetime
        );
}

std::pair<Ts, int> MessageListWidget::viewportAnchor() const {
    const int scrollY = verticalScrollBar()->value();
    for (int i = 0; i < static_cast<int>(_topsTs.size()); ++i)
        if (_tops[i] >= scrollY)
            return {_topsTs[i], _tops[i] - scrollY};
    // Viewport top is inside the last row (taller than the viewport) — anchor
    // to it with a negative offset.
    if (!_topsTs.empty())
        return {_topsTs.back(), _tops.back() - scrollY};
    return {};
}

void MessageListWidget::saveScrollAnchor() {
    if (_currentConv.value.isEmpty() || _isThreadMode)
        return;
    auto *sb = verticalScrollBar();
    if (sb->value() >= sb->maximum() - 4) {
        // At the bottom — remember that, so returning sticks to the bottom
        // (and new messages) rather than jumping up to the first-unread marker.
        _savedAnchors[_currentConv.value] = {.atBottom = true};
        return;
    }
    const auto anchor = viewportAnchor();
    if (anchor.first.isEmpty())
        _savedAnchors.remove(_currentConv.value);
    else
        _savedAnchors[_currentConv.value] = {.ts = anchor.first, .delta = anchor.second};
}

void MessageListWidget::applyPendingScroll() {
    if (textAreaWidth() <= 0 || _items.empty() || !_scrollToBottomPending)
        return;
    if (!_pendingAnchorTs.isEmpty()) {
        // The user deliberately left this chat scrolled-up — restore that
        // reading position, anchored to a message ts (pixel offsets don't
        // survive relayout).
        const int idx = findByTs(_pendingAnchorTs);
        if (idx >= 0) {
            verticalScrollBar()->setValue(
                std::clamp(_tops[idx] - _pendingAnchorDelta, 0, verticalScrollBar()->maximum())
            );
            _scrollToBottomPending = false;
            _pendingAnchorTs.clear();
            _pendingLastReadTs.clear();
            return;
        }
        // The anchor message isn't in the loaded window (stale cache) — show
        // the bottom for now, but keep the intent so the network merge retargets.
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return;
    }
    if (!_pendingLastReadTs.isEmpty()) {
        // First message strictly newer than the read cursor = first unread.
        // Compare on the dedicated time field (epoch micros), never the id.
        const qint64 lastReadMicros = decimalTsToMicros(_pendingLastReadTs);
        for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
            if (_items[i].msg.date > lastReadMicros) {
                verticalScrollBar()->setValue(qMax(0, _tops[i] - viewport()->height() / 3));
                _scrollToBottomPending = false;
                _pendingLastReadTs.clear();
                return;
            }
        }
        // The unreads aren't in the loaded window (stale cache) — show the
        // bottom for now, but keep the intent so the network merge retargets.
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return;
    }
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    _scrollToBottomPending = false;
}

int MessageListWidget::loadOlderMargin() const {
    // One viewport height, floored so a tiny window still prefetches a screenful
    // ahead of the top. A small fixed margin (the old 200px) let a fast scroll or
    // a coarse wheel step reach the very top before the trigger crossed it, so the
    // fetch sometimes never fired; prefetching a full screen early hides the load.
    return std::max(400, viewport()->height());
}

void MessageListWidget::maybeFillViewport() {
    if (_loading || _loadingOlder || !_olderCursor.has_value())
        return;
    // Keep a screenful of headroom above the viewport so a scroll-up always has
    // somewhere to go; matches the scrollContentsBy trigger threshold.
    if (_totalH >= viewport()->height() + loadOlderMargin())
        return;
    loadOlderMessages();
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

    // A failed history fetch completes the producer with done() but no page (the
    // backend swallows the error). Without a done handler that would leave
    // _loadingOlder stuck true and the cursor lost (cleared above), permanently
    // killing pagination for this conversation. Track delivery so done() can
    // restore the cursor and let the next scroll retry.
    auto gotPage = std::make_shared<bool>(false);

    std::move(producer) | rpl::on_next_done(
                              [this, conv, gotPage](MessagePage page) {
                                  *gotPage = true;
                                  if (_currentConv != conv) {
                                      _loadingOlder = false;
                                      return;
                                  }

                                  _olderCursor  = page.olderCursor;
                                  _loadingOlder = false;

                                  if (page.messages.empty()) {
                                      maybeFillViewport();
                                      return;
                                  }

                                  // Inserting older messages at the top shifts row indices —
                                  // drop any in-progress selection to avoid stale positions.
                                  _selAnchor   = {};
                                  _selFocus    = {};
                                  _selDragging = false;

                                  // rebuildLayout (inside the merge) keeps the topmost visible
                                  // message anchored while rows are inserted above; just stop
                                  // any running scroll animation, whose absolute target the
                                  // insert invalidated.
                                  const int prevTotalH = _totalH;
                                  // Cursored older page: a middle slice, not the head,
                                  // so deletion reconciliation is capped at its window.
                                  mergeNetworkMessages(page.messages, /*fromHeadPage=*/false);
                                  if (_totalH != prevTotalH)
                                      _scrollAnim.stop();
                                  maybeFillViewport();
                              },
                              [this, conv, cur, gotPage] {
                                  if (*gotPage)
                                      return;
                                  // Fetch failed without delivering a page — restore the cursor so
                                  // a later scroll-up retries instead of pagination dying for good.
                                  _loadingOlder = false;
                                  if (_currentConv == conv)
                                      _olderCursor = cur;
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
    _totalH          = 0;
    _hoveredRow      = -1;
    _hoveredToolBtn  = -1;
    _hoveredAttach   = {-1, -1};
    _hoveredTable    = {};
    _hoveredReplyRow = -1;
    _hoveredFile     = {-1, -1};
    _hoveredFileBtn  = -1;
    verticalScrollBar()->setRange(0, 0);
    viewport()->update();

    _currentConv           = conv;
    _isThreadMode          = true;
    _threadRootTs          = rootTs;
    _scrollToBottomPending = true;
    _pendingLastReadTs.clear(); // threads always open at the bottom
    _pendingAnchorTs.clear();

    if (!_session)
        return;

    _loading = true;
    _loadingElapsedTimer.start();
    _loadingAnim.start();

    _session->events() | rpl::on_next([this](Event e) { handleEvent(e); }, _eventLifetime);

    _session->userInfoLoaded() |
        rpl::on_next([this](UserId id) { onUserResolved(id); }, _eventLifetime);

    _session->emojiMapLoaded() | rpl::on_next([this] { invalidateAllDocs(); }, _eventLifetime);

    _session->backend()->loadThread(conv, rootTs, std::nullopt) |
        rpl::on_next(
            [this](MessagePage page) {
                _loading = false;
                _loadingAnim.stop();
                _olderCursor = page.olderCursor;
                appendMessages(page.messages);
                QTimer::singleShot(0, this, [this] { applyPendingScroll(); });
            },
            _loadLifetime
        );
}

void MessageListWidget::setThreadsInline(bool on) {
    if (_threadsInline == on)
        return;
    _threadsInline = on;
    // Switching modes drops any inline expansions (the panel, if open, stays).
    _inlineThreads.clear();
    rebuildLayout();
    viewport()->update();
}

void MessageListWidget::setOpenThreadRoot(const Ts &root) {
    if (_openThreadRoot == root)
        return;
    _openThreadRoot = root;
    viewport()->update(); // reply-bar copy ("View thread" ⇄ "Close thread")
}

void MessageListWidget::expandInlineThread(ConversationId conv, const Ts &rootTs) {
    if (!_session)
        return;
    auto &th   = _inlineThreads[rootTs]; // inserts → expanded
    th.loading = true;
    th.loaded  = false;
    th.replies.clear();
    th.lifetime = rpl::lifetime();
    rebuildLayout();
    viewport()->update();

    _session->backend()->loadThread(conv, rootTs, std::nullopt) |
        rpl::on_next(
            [this, rootTs](MessagePage page) {
                const auto it = _inlineThreads.find(rootTs);
                if (it == _inlineThreads.end())
                    return; // collapsed before the replies arrived
                auto &th   = it->second;
                th.loading = false;
                th.loaded  = true;
                th.replies.clear();
                for (auto &m : page.messages) {
                    if (m.ts == rootTs)
                        continue; // the root itself is rendered above the bar
                    if (_session) {
                        _session->fetchBotIfNeeded(m.author);
                        // Inline replies aren't swept by triggerMissingAvatarDownloads
                        // (it walks top-level rows only), so resolve their external
                        // authors + @mentions here.
                        _session->fetchUserIfNeeded(m.author);
                        for (const auto &e : m.text.entities)
                            if (e.type == EntityType::UserMention)
                                _session->fetchUserIfNeeded(UserId{e.data});
                    }
                    MessageItem item;
                    item.msg = m;
                    th.replies.push_back(std::move(item));
                }
                rebuildLayout();
                viewport()->update();
            },
            th.lifetime
        );
}

void MessageListWidget::collapseInlineThread(const Ts &rootTs) {
    if (_inlineThreads.erase(rootTs) == 0)
        return;
    rebuildLayout();
    viewport()->update();
    // If this thread is also open in the standalone panel, close it too.
    if (!_openThreadRoot.isEmpty() && _openThreadRoot == rootTs)
        emit threadCloseRequested();
}

void MessageListWidget::mergeNetworkMessages(
    const std::vector<Message> &incoming, bool fromHeadPage
) {
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
                item.fileImgBaseH      = -1; // files may differ
                changed                = true;
            }
        } else {
            toInsert.push_back(msg);
            changed = true;
        }
    }

    for (const auto &msg : toInsert) {
        const qint64 ts       = msg.date;
        int          insertAt = static_cast<int>(_items.size());
        for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
            if (_items[i].msg.date > ts) {
                insertAt = i;
                break;
            }
        }
        MessageItem item;
        item.msg = msg;
        _items.insert(_items.begin() + insertAt, std::move(item));
    }

    // Reconcile deletions. A message deleted from another client won't appear in
    // this authoritative page, yet it's still sitting in our list (pre-populated
    // from cache, or left behind by a realtime message_deleted the socket never
    // delivered). Drop any local non-pending message that falls inside the span
    // this page authoritatively covers but is absent from it.
    //
    // Lower bound is always the page's oldest message, so paginated-older history
    // below it is never touched. Upper bound depends on the page kind: a HEAD
    // page (latest, no cursor) IS the channel head — nothing on the server is
    // newer than its newest message, so a local row above it was deleted (the
    // common case: you delete the most recent message). A cursored OLDER page is
    // only a middle slice, so cap removal at its newest to leave genuinely-newer
    // rows alone. Optimistic pending ghosts are always spared.
    // A non-empty page gives a concrete [oldest, newest] span. An EMPTY head page
    // means the channel head itself is empty — every message there was deleted —
    // so the reconcile window is fully open and clears the stale rows. (This
    // lambda only runs on a successful fetch; a failed history load completes
    // without firing, so an empty page is a real "no messages", not an error.)
    // An empty cursored (older) page covers no span, so there's nothing to do.
    const bool haveRange = !incoming.empty();
    const bool reconcile = haveRange || fromHeadPage;
    if (reconcile) {
        QSet<QString> incomingTs;
        incomingTs.reserve(static_cast<int>(incoming.size()));
        qint64 oldest = haveRange ? incoming.front().date : 0;
        qint64 newest = haveRange ? incoming.front().date : 0;
        for (const auto &msg : incoming) {
            incomingTs.insert(msg.ts);
            oldest = std::min(oldest, msg.date);
            newest = std::max(newest, msg.date);
        }
        const qint64 lower   = haveRange ? oldest : std::numeric_limits<qint64>::min();
        const qint64 upper   = fromHeadPage ? std::numeric_limits<qint64>::max() : newest;
        bool         removed = false;
        for (int i = static_cast<int>(_items.size()) - 1; i >= 0; --i) {
            const auto &m = _items[i].msg;
            if (m.pending)
                continue; // optimistic send not yet on the server
            if (m.date < lower || m.date > upper)
                continue; // below this page, or above a non-head page's window
            if (incomingTs.contains(m.ts))
                continue; // still on the server
            _items.erase(_items.begin() + i);
            removed = true;
            changed = true;
        }
        if (removed) {
            // Row indices shifted — drop hover state; the next mouse move
            // recomputes it (mirrors the EvMessageDeleted path).
            _hoveredRow      = -1;
            _hoveredToolBtn  = -1;
            _hoveredAttach   = {-1, -1};
            _hoveredTable    = {};
            _hoveredReplyRow = -1;
            _hoveredFile     = {-1, -1};
            _hoveredFileBtn  = -1;
        }
    }

    if (changed) {
        rebuildLayout();
        viewport()->update();
    }
}

void MessageListWidget::appendMessageDeferred(const Message &msg) {
    // fetchBotIfNeeded no-ops for non-bot ids (it asks the backend), so we don't
    // inspect the id's shape here — the UI treats it as opaque.
    if (_session)
        _session->fetchBotIfNeeded(msg.author);
    MessageItem item;
    item.msg = msg;
    _items.push_back(std::move(item));
}

void MessageListWidget::appendMessage(const Message &msg) {
    appendMessageDeferred(msg);
    rebuildLayout();
    viewport()->update();
}

void MessageListWidget::appendMessages(const std::vector<Message> &msgs) {
    if (msgs.empty())
        return;
    for (const auto &msg : msgs)
        appendMessageDeferred(msg);
    // appendMessageDeferred preserves input order; keep the list chronological
    // regardless of the source's order (e.g. a backend that returns newest-first,
    // or a stale cache written by an older build). Stable so equal-date messages
    // keep their arrival order. No-op when the input is already sorted (Slack).
    std::stable_sort(_items.begin(), _items.end(), [](const MessageItem &a, const MessageItem &b) {
        return a.msg.date < b.msg.date;
    });
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

bool MessageListWidget::needsDateSep(int index) const {
    if (index < 0 || index >= (int)_items.size())
        return false;
    if (index == 0)
        return true;
    const QDate d0 = MsgRender::tsToDate(_items[index - 1].msg.date);
    const QDate d1 = MsgRender::tsToDate(_items[index].msg.date);
    return d0 != d1;
}

int MessageListWidget::textAreaWidth() const {
    return viewport()->width() - kPadH - kAvSize - kAvGap - kPadH;
}

void MessageListWidget::onUserResolved(UserId id) {
    // True if this message's author or one of its @mentions is `id` — those are
    // the rows whose rendered text/header can change now that the user is known.
    const auto references = [&id](const Message &m) {
        if (m.author == id)
            return true;
        for (const auto &e : m.text.entities)
            if (e.type == EntityType::UserMention && e.data == id.value)
                return true;
        return false;
    };
    // Mentions are baked into the cached doc, so the doc must be rebuilt; the
    // author name + avatar are painted live, so a repaint alone refreshes them.
    const auto refresh = [&references](MessageItem &item) {
        if (!references(item.msg))
            return;
        item.textDoc.reset();
        item.docWidth = -1;
    };
    for (auto &item : _items)
        refresh(item);
    for (auto &entry : _inlineThreads)
        for (auto &reply : entry.second.replies)
            refresh(reply);
    rebuildLayout();
    viewport()->update();
}

void MessageListWidget::invalidateAllDocs() {
    for (auto &item : _items) {
        item.textDoc.reset();
        item.attachDocs.clear();
        item.docWidth = -1;
        item.emojiUrls.clear();
        item.emojiUrlsCollected = false;
        item.fileImgBaseH       = -1;
    }
    rebuildLayout();
    viewport()->update();
}

void MessageListWidget::ensureDocLayout(const MessageItem &item, int forWidth) const {
    const int w = forWidth < 0 ? textAreaWidth() : forWidth;
    if (w <= 0)
        return;

    // System/activity lines paint their own centered text directly — they carry
    // no message document, so text hit-testing and selection skip them.
    if (isSystemEvent(item.msg)) {
        item.docHeight = 0;
        return;
    }

    // Custom-emoji images referenced by this message. Registering the cached
    // pixmaps as document resources lets the `<img>` tags emitted by
    // MsgRender::toHtml render; ImageCache::get() also kicks off the download
    // for anything missing (the loaded() handler resets the docs to re-render).
    if (!item.emojiUrlsCollected) {
        item.emojiUrls          = MsgRender::collectEmojiImageUrls(item.msg, _session);
        item.emojiUrlsCollected = true;
    }
    // Chevron pixmaps for image-block title lines ("GIF ▾") — only rendered
    // when the message actually carries an image block.
    auto hasImageBlock = [](const std::vector<Block> &blocks) {
        return std::any_of(blocks.begin(), blocks.end(), [](const Block &b) {
            return b.typeStr == "image" && !b.imageUrl.isEmpty();
        });
    };
    bool anyImageBlock = hasImageBlock(item.msg.blocks);
    for (const auto &att : item.msg.attachments)
        anyImageBlock = anyImageBlock || hasImageBlock(att.blocks);

    // Rasterization lives inside the lambda: it only runs when a doc is
    // actually (re)built below, while ensureDocLayout itself runs on every
    // paint of the row — rendering the SVGs up here would redo it each frame.
    auto addImageResources = [&](QTextDocument *doc) {
        if (anyImageBlock) {
            const qreal dpr = devicePixelRatioF();
            doc->addResource(
                QTextDocument::ImageResource,
                QUrl(MsgRender::kGifChevronExpandedRes),
                svgPixmapPhys(":/ui/chevron-down.svg", QSize(10, 10), Th::c().text.secondary, dpr)
            );
            doc->addResource(
                QTextDocument::ImageResource,
                QUrl(MsgRender::kGifChevronCollapsedRes),
                svgPixmapPhys(":/ui/chevron-right.svg", QSize(10, 10), Th::c().text.secondary, dpr)
            );
        }
        if (!_imgCache)
            return;
        for (const auto &url : item.emojiUrls) {
            const QPixmap px = _imgCache->get(url);
            if (!px.isNull()) {
                doc->addResource(QTextDocument::ImageResource, QUrl(url), px);
            } else if (anyImageBlock) {
                // Still downloading: flat placeholder so the reserved image area
                // doesn't show Qt's broken-image icon (loaded() rebuilds the doc).
                QPixmap ph(4, 3);
                ph.fill(Th::c().message.imagePlaceholderBg);
                doc->addResource(QTextDocument::ImageResource, QUrl(url), ph);
            }
        }
    };

    // Main text doc. `created` matters for docs released by trimOffscreenDocs:
    // they come back with docWidth already == w, but the fresh QTextDocument
    // still needs its text width applied.
    bool created = false;
    if (!item.textDoc) {
        item.textDoc = std::make_unique<QTextDocument>();
        item.textDoc->setDefaultFont(QApplication::font());
        item.textDoc->setDocumentMargin(0);
        item.textDoc->setDefaultStyleSheet(MsgRender::docStyleSheet());
        addImageResources(item.textDoc.get());
        const MsgRender::GifRenderContext gifCtx{item.msg.ts, &_collapsedGifs};
        const bool collapseQuotes = _session && _session->capabilities().collapseQuotedReplies;
        const auto html = MsgRender::buildMsgHtml(item.msg, _session, &gifCtx, collapseQuotes);
        if (!html.isEmpty())
            item.textDoc->setHtml(html);
        created = true;
    }
    if (created || item.docWidth != w) {
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
        auto &ad        = item.attachDocs[ai];
        bool  adCreated = false;
        if (!ad.textDoc) {
            ad.textDoc = std::make_unique<QTextDocument>();
            ad.textDoc->setDefaultFont(QApplication::font());
            ad.textDoc->setDocumentMargin(0);
            ad.textDoc->setDefaultStyleSheet(MsgRender::docStyleSheet());
            addImageResources(ad.textDoc.get());
            const MsgRender::GifRenderContext gifCtx{
                item.msg.ts + "/a" + QString::number(ai), &_collapsedGifs
            };
            const auto html = MsgRender::buildAttachHtml(attachments[ai], _session, &gifCtx);
            if (!html.isEmpty())
                ad.textDoc->setHtml(html);
            adCreated = true;
        }
        if (adCreated || ad.docWidth != attW) {
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
    // A system line has its own layout and must never merge with a neighbour,
    // even when its author id happens to match the adjacent message.
    if (isSystemEvent(prev) || isSystemEvent(curr))
        return false;
    if (prev.author != curr.author)
        return false;
    if (prev.replyCount > 0)
        return false; // thread roots break the run
    if (curr.replyCount > 0)
        return false;
    return (curr.date - prev.date) < 300LL * 1000000; // collapse if within 5 minutes
}

int MessageListWidget::rowHeight(int index) const {
    const auto &item = _items[index];

    if (isSystemEvent(item.msg)) {
        const int sepH = needsDateSep(index) ? kSepH : 0;
        return sepH + systemRowHeight();
    }

    // Lazy layout: a visible row is laid out for real (the expensive
    // QTextDocument::size()); an off-screen row uses a cheap text-length estimate
    // until the background pass measures it. The arithmetic below is identical
    // either way — only the text/attachment heights come from a measurement vs an
    // estimate, so a measured row's height is exactly what it always was.
    const bool measured = rowMeasured(item);
    const int  docH     = measured ? item.docHeight : estimatedDocHeight(item);

    const bool collapsed = isCollapsed(index);

    int extraH = 0;

    // Attachment heights (skip client-dismissed ones). Iterate msg.attachments
    // (not attachDocs, which only exist once measured) so the count is right for
    // estimated rows too; for measured rows the two are 1:1.
    const int nAtt = (int)item.msg.attachments.size();
    for (int ai = 0; ai < nAtt; ++ai) {
        if (isDismissed(item.msg.ts, ai))
            continue;
        const int ah =
            measured ? attachTotalH(item, ai) : estimatedAttachHeight(item.msg.attachments[ai]);
        extraH += kAttachGap + std::max(ah, 0);
    }

    // Inline file preview heights (images + prerendered docs). The region height
    // is width-independent (single-image height comes from capped original dims;
    // gallery height from tile count), so passing kImgMaxW matches paint exactly.
    // Memoized: rowHeight runs O(n) per rebuildLayout and the full layout heap-
    // allocates; the base (no-lead) height only changes when the message or a
    // loaded preview pixmap does (_fileImagesGen tracks the latter).
    if (item.fileImgBaseH < 0 || item.fileImgGen != _fileImagesGen) {
        item.fileImgBaseH = layoutFileImages(item, kImgMaxW, false).height;
        item.fileImgGen   = _fileImagesGen;
    }
    const bool hasContentAboveImages = docH > 0 || nAtt > 0;
    const int  imgRegionH            = (item.fileImgBaseH > 0 && hasContentAboveImages)
                                           ? item.fileImgBaseH + kImgGap
                                           : item.fileImgBaseH;
    extraH += imgRegionH;

    // File chips (files without a preview)
    const bool hasAboveChips = docH > 0 || nAtt > 0 || imgRegionH > 0;
    bool       firstChip     = true;
    for (const auto &f : item.msg.files) {
        if (f.hasPreview())
            continue;
        if (!firstChip || hasAboveChips)
            extraH += kFileChipGap;
        firstChip = false;
        extraH += kFileChipH;
    }

    const int  reactionH   = item.msg.reactions.empty() ? 0 : (kReactH + 2);
    const bool hasReplyBar = !_isThreadMode && item.msg.replyCount > 0;
    const int  replyBarH   = hasReplyBar ? (kReplyBarGap + kReplyBarH) : 0;
    // Expanded inline thread region grows the row below the reply bar.
    const int  inlineH  = (hasReplyBar && _threadsInline && _inlineThreads.count(item.msg.ts) > 0)
                              ? inlineThreadHeight(item.msg.ts)
                              : 0;
    const int  headerH  = collapsed ? 0 : (kHdrH + kHdrGap);
    const int  pinnedH  = item.msg.pinned ? 18 : 0;
    // pinnedH is a banner drawn before padV — kept separate from contentH.
    const int  contentH = headerH + docH + extraH + reactionH;
    const int  sepH     = needsDateSep(index) ? kSepH : 0;
    if (collapsed)
        return sepH + pinnedH + kPadVCollapsed + contentH + kPadVCollapsed + replyBarH + inlineH;
    return sepH + pinnedH + kPadV + std::max(kAvSize, contentH) + kPadVBottom + replyBarH + inlineH;
}

// "Laid out at the current width" — the cheap proxy for "measured". ensureDocLayout
// sets docWidth to the layout width; invalidation resets it to -1/0. System rows
// carry a fixed height and never need a QTextDocument, so treat them as measured.
bool MessageListWidget::rowMeasured(const MessageItem &item) const {
    if (isSystemEvent(item.msg))
        return true;
    const int w = textAreaWidth();
    return w > 0 && item.docWidth == w;
}

// Rough pixel height of `text` at the current column width, without laying out a
// QTextDocument: wrap each hard line to the average character count. Only used for
// off-screen rows, where being approximate is fine — the value is replaced by the
// real height the moment the row is measured.
int MessageListWidget::estimatedTextHeight(const QString &text) const {
    if (text.isEmpty())
        return 0;
    const int          w = std::max(1, textAreaWidth());
    const QFontMetrics fm(QApplication::font());
    const int          lineH = std::max(1, qRound(fm.height() * 1.35));
    const int          cpl   = std::max(1, w / std::max(1, fm.averageCharWidth()));
    int                lines = 0, lineStart = 0;
    const int          n = (int)text.size();
    for (int i = 0; i <= n; ++i)
        if (i == n || text[i] == '\n') {
            lines += std::max(1, (i - lineStart + cpl - 1) / cpl);
            lineStart = i + 1;
        }
    return lines * lineH;
}

int MessageListWidget::estimatedDocHeight(const MessageItem &item) const {
    if (!item.msg.text.text.isEmpty())
        return estimatedTextHeight(item.msg.text.text);
    // Rich blocks with no plain text (rare) — assume a couple of lines so the row
    // isn't estimated as empty and then jump tall when measured.
    if (!item.msg.blocks.empty())
        return 2 * std::max(1, qRound(QFontMetrics(QApplication::font()).height() * 1.35));
    return 0;
}

int MessageListWidget::estimatedAttachHeight(const Attachment &att) const {
    // Known image height (from cached-pixmap metadata) + a body-text estimate;
    // refined to the exact height as soon as the row is measured.
    return attachImageH(att) + estimatedTextHeight(att.text.text);
}

void MessageListWidget::rebuildLayout() {
    // View stability lives here so EVERY height change keeps the reading
    // position: heights settle asynchronously long after any scroll was applied
    // (image downloads, emoji-map doc invalidation, bot info, older-page
    // inserts). At the bottom the view stays pinned to the newest message;
    // anywhere else the topmost visible message keeps its viewport offset.
    // The anchor comes from the _tops/_topsTs snapshot of the PREVIOUS layout —
    // callers mutate _items before calling us, so it is matched back by ts.
    auto      *sb                      = verticalScrollBar();
    const bool atBottom                = sb->value() >= sb->maximum() - 4;
    const auto [anchorTs, anchorDelta] = atBottom ? std::pair<Ts, int>{} : viewportAnchor();

    _tops.resize(_items.size());
    _topsTs.resize(_items.size());
    int y = kPadV;
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        _tops[i]   = y;
        _topsTs[i] = _items[i].msg.ts;
        y += rowHeight(i) + kRowGap;
    }
    const int contentH = y + kPadV;

    // When the whole conversation is shorter than the viewport, sit the messages
    // at the bottom (against the composer) like the official client instead of
    // pinning them to the top. Baking the offset into _tops keeps every hit-test
    // and paint path (which all read _tops) consistent for free.
    const int vh        = viewport()->height();
    const int topOffset = std::max(0, vh - contentH);
    if (topOffset > 0)
        for (int &t : _tops)
            t += topOffset;
    _totalH = contentH + topOffset;

    sb->setRange(0, std::max(0, _totalH - vh));
    sb->setPageStep(vh);

    if (atBottom) {
        sb->setValue(sb->maximum());
    } else if (!anchorTs.isEmpty()) {
        const int idx = findByTs(anchorTs);
        if (idx >= 0)
            sb->setValue(std::clamp(_tops[idx] - anchorDelta, 0, sb->maximum()));
    }

    // Any rows still carrying an estimate get laid out for real in the
    // background, a chunk per event-loop tick, so the scrollbar range and
    // off-screen positions converge without ever blocking the open.
    scheduleProgressiveLayout();
}

// Rows are contiguous (kRowGap = 0), so the row containing document-space y is
// the last one whose top is <= y; everything before it is fully above.
int MessageListWidget::firstVisibleRow(int docY) const {
    if (_tops.empty() || _tops.size() != _items.size())
        return 0;
    const auto it = std::upper_bound(_tops.begin(), _tops.end(), docY);
    return std::max(0, static_cast<int>(it - _tops.begin()) - 1);
}

// Lay out the rows currently on screen for real before they're painted. Called at
// the top of doPaint: off-screen rows reach paint with only an estimate, and the
// visible ones must be exact. If measuring corrects any height, rebuildLayout()
// fixes _tops and re-pins the anchor so the visible content doesn't jump.
bool MessageListWidget::measureVisibleRows() {
    const int w = textAreaWidth();
    if (w <= 0 || _items.empty() || _tops.size() != _items.size())
        return false;
    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();
    bool      changed = false;
    for (int i = firstVisibleRow(scrollY); i < static_cast<int>(_items.size()); ++i) {
        const int top = _tops[i] - scrollY;
        if (top > vh)
            break; // _tops is monotonic — nothing below is on screen
        if (top + rowHeight(i) < 0)
            continue; // entirely above the viewport
        if (!rowMeasured(_items[i])) {
            ensureDocLayout(_items[i]);
            changed = true;
        }
    }
    if (changed)
        rebuildLayout();
    return changed;
}

// Off-screen rows measured a chunk at a time on the event loop. Each chunk is
// bounded so a tick stays short even for layout-heavy messages; rebuildLayout()
// re-pins the anchor and (via its tail) schedules the next chunk until all rows
// are measured.
static constexpr int kLayoutChunk = 8;

void MessageListWidget::scheduleProgressiveLayout() {
    if (_progressiveLayoutScheduled || textAreaWidth() <= 0)
        return;
    bool anyUnmeasured = false;
    for (const auto &it : _items)
        if (!rowMeasured(it)) {
            anyUnmeasured = true;
            break;
        }
    if (!anyUnmeasured)
        return;
    _progressiveLayoutScheduled = true;
    QTimer::singleShot(0, this, [this] { measureLayoutChunk(); });
}

void MessageListWidget::measureLayoutChunk() {
    _progressiveLayoutScheduled = false;
    if (textAreaWidth() <= 0 || _items.empty())
        return;
    int done = 0;
    for (int i = 0; i < static_cast<int>(_items.size()) && done < kLayoutChunk; ++i)
        if (!rowMeasured(_items[i])) {
            ensureDocLayout(_items[i]);
            ++done;
        }
    if (done > 0) {
        rebuildLayout(); // re-pins the anchor; reschedules the next chunk via its tail
        // The docs were built only to take exact heights — rows far from the
        // viewport don't need them for painting, so release them right away
        // instead of accumulating one live QTextDocument per row ever measured.
        trimOffscreenDocs();
    }
}

void MessageListWidget::trimOffscreenDocs() {
    if (_items.empty() || _tops.size() != _items.size())
        return;
    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();
    if (vh <= 0)
        return;
    const int keepTop = scrollY - kKeepViewports * vh;
    const int keepBot = scrollY + (kKeepViewports + 1) * vh;
    const int n       = static_cast<int>(_items.size());
    for (int i = 0; i < n; ++i) {
        const int top    = _tops[i];
        const int bottom = (i + 1 < n) ? _tops[i + 1] : _totalH;
        if (bottom >= keepTop && top <= keepBot)
            continue; // inside the keep band
        // Release the rendered docs but keep docWidth/docHeight (and the
        // collected emoji urls): the row stays "measured", so heights, _tops
        // and the scrollbar are untouched; ensureDocLayout rebuilds the doc
        // if the row scrolls back in.
        auto &item = _items[i];
        item.textDoc.reset();
        for (auto &ad : item.attachDocs)
            ad.textDoc.reset();
    }
}

// ── Attachment height helpers ─────────────────────────────────────────────────

int MessageListWidget::attachImageH(const Attachment &att) const {
    const QString imgUrl = attachPreviewUrl(att);
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

// ── Animated images (GIF / animated WebP) ─────────────────────────────────────

QMovie *MessageListWidget::gifMovieFor(const QString &url) const {
    const auto it = _gifMovies.constFind(url);
    if (it != _gifMovies.constEnd())
        return it.value();
    QMovie *m = _imgCache ? _imgCache->movie(url) : nullptr;
    if (m)
        watchGifMovie(url, m);
    return m;
}

void MessageListWidget::watchGifMovie(const QString &url, QMovie *movie) const {
    _gifMovies.insert(url, movie);
    // Repaint per frame, but only the region the gif was painted into — a
    // whole-viewport update would re-rasterize every visible row at the gif's
    // frame rate. Offscreen movies (no _gifRects entry) get paused by
    // syncGifPlayback on the next paint anyway.
    connect(movie, &QMovie::frameChanged, this, [this, url](int) {
        const auto it = _gifRects.constFind(url);
        if (it != _gifRects.constEnd() && !it->isEmpty())
            viewport()->update(*it);
    });
}

void MessageListWidget::maybeCreateFileGifMovie(const QString &url, const QByteArray &bytes) const {
    if (_gifMovies.contains(url) || !ImageCache::isAnimatedImage(bytes))
        return;
    auto *buf = new QBuffer;
    buf->setData(bytes);
    buf->open(QIODevice::ReadOnly);
    auto *movie = new QMovie(buf);
    buf->setParent(movie);
    if (!movie->isValid()) {
        delete movie;
        return;
    }
    movie->setParent(const_cast<MessageListWidget *>(this));
    watchGifMovie(url, movie);
}

void MessageListWidget::dropGifMovie(const QString &url) const {
    const auto it = _gifMovies.constFind(url);
    if (it == _gifMovies.constEnd())
        return;
    QMovie *m = it.value();
    _gifMovies.remove(url);
    _visibleGifs.remove(url);
    _gifRects.remove(url);
    if (m->parent() == this) {
        // Widget-owned (auth-downloaded file). Drop the decoded pixmap too:
        // the reload path is what recreates the player (maybeCreateFileGifMovie),
        // so a kept pixmap would repaint as a frozen frame.
        delete m;
        _fileImages.remove(url);
        ++_fileImagesGen;
    } else {
        // Cache-owned: sever our frameChanged connection and hand the hold back.
        disconnect(m, nullptr, this, nullptr);
        if (_imgCache)
            _imgCache->releaseMovie(url);
    }
}

void MessageListWidget::releaseGifMovies() const {
    const auto urls = _gifMovies.keys();
    for (const auto &url : urls)
        dropGifMovie(url);
}

void MessageListWidget::markGifVisible(const QString &url, const QRect &vpRect) const {
    _visibleGifs.insert(url);
    QRect &r = _gifRects[url];
    r        = r.isNull() ? vpRect : r.united(vpRect);
}

void MessageListWidget::pullGifFrames(const MessageItem &item, const QRect &vpRect) const {
    for (const auto &url : item.emojiUrls) {
        QMovie *m = gifMovieFor(url);
        if (!m)
            continue;
        markGifVisible(url, vpRect);
        const QPixmap frame = m->currentPixmap();
        if (frame.isNull())
            continue;
        if (item.textDoc)
            item.textDoc->addResource(QTextDocument::ImageResource, QUrl(url), frame);
        for (auto &ad : item.attachDocs)
            if (ad.textDoc)
                ad.textDoc->addResource(QTextDocument::ImageResource, QUrl(url), frame);
    }
}

void MessageListWidget::syncGifPlayback() const {
    for (auto it = _gifMovies.cbegin(); it != _gifMovies.cend(); ++it) {
        QMovie    *m       = it.value();
        const bool visible = _visibleGifs.contains(it.key());
        if (visible) {
            if (m->state() == QMovie::NotRunning)
                m->start();
            else if (m->state() == QMovie::Paused)
                m->setPaused(false);
        } else if (m->state() == QMovie::Running) {
            m->setPaused(true);
        }
    }
}

void MessageListWidget::hideEvent(QHideEvent *event) {
    pauseGifPlayback();
    VirtualListWidget::hideEvent(event);
}

void MessageListWidget::pauseGifPlayback() {
    // Don't burn CPU decoding frames nobody can see. The next paint pass
    // re-marks whatever is actually on screen and restarts those players.
    _visibleGifs.clear();
    _gifRects.clear();
    syncGifPlayback();
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

    for (int i = firstVisibleRow(docY); i < static_cast<int>(_items.size()); ++i) {
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
        const int attW = textAreaWidth() - kAttachBarW - kAttachBarGap;
        int       ay   = textTop + item.docHeight;
        for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
            if (isDismissed(item.msg.ts, ai))
                continue;
            ay += kAttachGap;
            const auto &ad         = item.attachDocs[ai];
            // Image-only and table-only attachments paint un-indented (no quote
            // bar) — keep the hit-test x in sync with paintAttachments.
            const int   attTextX   = (MsgRender::attachIsImageOnly(item.msg.attachments[ai]) ||
                                  MsgRender::attachIsTableOnly(item.msg.attachments[ai]))
                                         ? textLeft
                                         : textLeft + kAttachBarW + kAttachBarGap;
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

QRect MessageListWidget::userAnchorVpRect(const QPoint &viewportPos, const QString &href) const {
    // Fallback anchors the card to the cursor (e.g. mentions inside attachment docs).
    const QRect fallback(viewportPos - QPoint(8, 8), QSize(16, 16));
    if (href.isEmpty())
        return fallback;

    const PaintContext ctx  = makePaintContext();
    const int          docY = viewportPos.y() + ctx.scrollY;

    for (int i = firstVisibleRow(docY); i < (int)_items.size(); ++i) {
        const int rowTop = _tops[i];
        const int rh     = rowHeight(i);
        if (docY < rowTop)
            break;
        if (docY > rowTop + rh)
            continue;

        const auto &item = _items[i];
        ensureDocLayout(item);
        if (!item.textDoc)
            return fallback;

        const bool coll    = isCollapsed(i);
        const int  padV    = coll ? kPadVCollapsed : kPadV;
        const int  sepH    = needsDateSep(i) ? kSepH : 0;
        const int  pinnedH = item.msg.pinned ? 18 : 0;
        const int  textTop = rowTop + sepH + pinnedH + padV + (coll ? 0 : kHdrH + kHdrGap);

        const QPointF local(viewportPos.x() - ctx.textLeft, docY - textTop);
        const int     hit = item.textDoc->documentLayout()->hitTest(local, Qt::ExactHit);
        if (hit < 0)
            return fallback;

        // Find the anchor fragment under the cursor and compute its line rect.
        const QTextBlock block = item.textDoc->findBlock(hit);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.charFormat().anchorHref() != href)
                continue;
            if (hit < frag.position() || hit >= frag.position() + frag.length())
                continue;
            QTextLayout    *lay  = block.layout();
            const int       rel  = frag.position() - block.position();
            const QTextLine line = lay->lineForTextPosition(rel);
            if (!line.isValid())
                break;
            const qreal x1 = line.cursorToX(rel);
            const qreal x2 = line.cursorToX(rel + frag.length());
            return QRect(
                ctx.textLeft + qRound(lay->position().x() + std::min(x1, x2)),
                textTop - ctx.scrollY + qRound(lay->position().y() + line.y()),
                qRound(std::abs(x2 - x1)),
                qRound(line.height())
            );
        }
        return fallback;
    }
    return fallback;
}

QString MessageListWidget::avatarUserAt(const QPoint &viewportPos, QRect *outVpRect) const {
    const int scrollY = verticalScrollBar()->value();
    const int docY    = viewportPos.y() + scrollY;

    for (int i = firstVisibleRow(docY); i < (int)_items.size(); ++i) {
        const int rowTop = _tops[i];
        const int rh     = rowHeight(i);
        if (docY < rowTop)
            break;
        if (docY > rowTop + rh)
            continue;
        // Collapsed (consecutive same-author) rows paint no avatar.
        if (isCollapsed(i))
            return {};

        // Mirror the avatar geometry from paintRow(): the square sits at
        // (kPadH, contTop + 2) where contTop = rowTop + sepH + pinnedBannerH + kPadV.
        const auto &item    = _items[i];
        const int   sepH    = needsDateSep(i) ? kSepH : 0;
        const int   pinnedH = item.msg.pinned ? 18 : 0;
        const int   contTop = rowTop + sepH + pinnedH + kPadV;
        const QRect avVp(kPadH, contTop + 2 - scrollY, kAvSize, kAvSize);
        if (!avVp.contains(viewportPos))
            return {};
        // Only real users get a profile card (bots/apps have no User entry).
        if (!_session || !_session->findUser(item.msg.author))
            return {};
        if (outVpRect)
            *outVpRect = avVp;
        return item.msg.author.value;
    }
    return {};
}

void MessageListWidget::showProfileCardFor(const QString &userIdStr, const QRect &anchorVpRect) {
    if (!_session)
        return;
    const User *user = _session->findUser(UserId{userIdStr});
    if (!user)
        return;

    QPixmap avatar;
    if (_imgCache && !user->avatarUrl.isEmpty())
        avatar = _imgCache->get(user->avatarUrl);

    const QRect globalRect(viewport()->mapToGlobal(anchorVpRect.topLeft()), anchorVpRect.size());
    const bool  hasPresence = _session->capabilities().presence;
    _profileCard->showFor(*user, avatar, globalRect, hasPresence);
    // Refresh the presence dot; the result arrives as EvPresenceChanged in handleEvent.
    if (hasPresence)
        _session->requestPresence(user->id);
}

void MessageListWidget::hideProfileCard() {
    _profileShowTimer.stop();
    _pendingProfileAnchor.clear();
    if (_profileCard)
        _profileCard->hideNow();
}

std::pair<int, int> MessageListWidget::dismissButtonAt(const QPoint &viewportPos) const {
    const PaintContext ctx      = makePaintContext();
    const int          scrollY  = ctx.scrollY;
    const int          textLeft = ctx.textLeft;
    const int          btnX     = textLeft - kDismissGap - kDismissW;

    for (int i = firstVisibleRow(viewportPos.y() + scrollY); i < (int)_items.size(); ++i) {
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
                // Table attachments are message content — not dismissable.
                if (!MsgRender::attachIsTableOnly(item.msg.attachments[ai]) &&
                    QRect(btnX, y, kDismissW, kDismissW).contains(viewportPos))
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

MessageListWidget::TableHit MessageListWidget::tableHitAt(const QPoint &viewportPos) const {
    const PaintContext ctx      = makePaintContext();
    const int          scrollY  = ctx.scrollY;
    const int          docY     = viewportPos.y() + scrollY;
    const int          textLeft = ctx.textLeft;

    for (int i = firstVisibleRow(docY); i < static_cast<int>(_items.size()); ++i) {
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

        // Tables in a laid-out doc at origin (originX, originY): return the
        // hit table's viewport rect and its index within the doc.
        const auto hitIn =
            [&](QTextDocument *doc, int originX, int originY, int attachIdx) -> TableHit {
            if (!doc)
                return {};
            const auto rects = MsgRender::dataTableRects(doc);
            for (int ti = 0; ti < rects.size(); ++ti) {
                const QRect vp = rects[ti].translated(originX, originY - scrollY).toAlignedRect();
                if (vp.contains(viewportPos))
                    return TableHit{i, attachIdx, ti, vp};
            }
            return {};
        };

        // Main message doc
        if (TableHit hit = hitIn(item.textDoc.get(), textLeft, textTop, -1); hit.valid())
            return hit;

        // Attachment docs
        int ay = textTop + item.docHeight;
        for (int ai = 0; ai < (int)item.attachDocs.size(); ++ai) {
            if (isDismissed(item.msg.ts, ai))
                continue;
            ay += kAttachGap;
            const auto &att = item.msg.attachments[ai];
            const int   attTextX =
                (MsgRender::attachIsImageOnly(att) || MsgRender::attachIsTableOnly(att))
                      ? textLeft
                      : textLeft + kAttachBarW + kAttachBarGap;
            if (TableHit hit = hitIn(item.attachDocs[ai].textDoc.get(), attTextX, ay, ai);
                hit.valid())
                return hit;
            ay += attachTotalH(item, ai);
        }
        return {};
    }
    return {};
}

QRect MessageListWidget::tablePillRect(const TableHit &hit) const {
    if (!hit.valid())
        return {};
    // Vertically centred on the VISIBLE part of the table (tables can be taller
    // than the viewport), inset from the right edge — like the official client.
    const QRect visible = hit.vpRect.intersected(viewport()->rect());
    if (visible.isEmpty())
        return {};
    const QFontMetrics fm(QApplication::font());
    const int          w = fm.horizontalAdvance(tr("Open full table")) + kTablePillIconPad;
    const int          x = hit.vpRect.right() - w - Th::c().spacing.md;
    return {std::max(hit.vpRect.left(), x), visible.center().y() - kTablePillH / 2, w, kTablePillH};
}

const Block *MessageListWidget::tableBlockFor(const TableHit &hit) const {
    if (!hit.valid() || hit.row >= (int)_items.size())
        return nullptr;
    const auto               &msg    = _items[hit.row].msg;
    const std::vector<Block> *blocks = nullptr;
    if (hit.attachIdx < 0)
        blocks = &msg.blocks;
    else if (hit.attachIdx < (int)msg.attachments.size())
        blocks = &msg.attachments[hit.attachIdx].blocks;
    if (!blocks)
        return nullptr;
    // The n-th data table in the doc is the n-th block with table rows.
    int ti = 0;
    for (const auto &b : *blocks)
        if (!b.tableRows.empty() && ti++ == hit.tableIdx)
            return &b;
    return nullptr;
}

bool MessageListWidget::tryHandleTablePillPress(const QPoint &pos) {
    if (!_hoveredTable.valid() || !tablePillRect(_hoveredTable).contains(pos))
        return false;
    const Block *blk = tableBlockFor(_hoveredTable);
    if (!blk)
        return false;
    if (!_tableViewer)
        _tableViewer = new TableViewerOverlay(window());
    _tableViewer->open(*blk, _session);
    return true;
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

    // Any press dismisses the profile card; a press on a mention re-shows it
    // immediately via tryHandleLinkPress below.
    hideProfileCard();

    // Triple-click: Qt reports the third click as a plain press shortly after the
    // double-click, so detect it by closeness in time and space and select the whole line.
    const bool maybeTriple = _lastDblClickTs != 0 &&
                             event->timestamp() - _lastDblClickTs <=
                                 (unsigned long)QApplication::doubleClickInterval() &&
                             (event->pos() - _lastDblClickPos).manhattanLength() <= 4;
    _lastDblClickTs = 0;
    if (maybeTriple && tryHandleTripleClick(event->pos()))
        return;

    // Clear any existing selection; it may be re-established below if the press lands on text.
    clearSelection();

    if (tryHandleScrollbarPress(event->pos()))
        return;
    if (tryHandleToolbarPress(event->pos()))
        return;
    if (tryHandleReactionPress(event->pos()))
        return;
    if (tryHandleTablePillPress(event->pos()))
        return;
    if (tryHandleDismissPress(event->pos()))
        return;
    if (tryHandleReplyBarPress(event->pos()))
        return;
    if (tryHandleInlineThreadPress(event->pos()))
        return;
    if (tryHandleLinkPress(event->pos()))
        return;
    if (tryHandleFileActionBarPress(event->pos()))
        return;
    if (tryHandlePreviewPress(event->pos()))
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

void MessageListWidget::doMouseDoubleClick(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        VirtualListWidget::doMouseDoubleClick(event);
        return;
    }

    // Double-click on message text selects the word under the cursor.
    const TextPos tp = textHitTest(event->pos());
    if (tp.row >= 0) {
        const auto &item = _items[tp.row];
        ensureDocLayout(item);
        if (item.textDoc) {
            QTextCursor cur(item.textDoc.get());
            cur.setPosition(tp.offset);
            cur.select(QTextCursor::WordUnderCursor);
            if (cur.hasSelection()) {
                _selAnchor   = {tp.row, cur.selectionStart()};
                _selFocus    = {tp.row, cur.selectionEnd()};
                _selDragging = false;
                viewport()->update();
            }
        }
        // Remember this double-click so a follow-up press is recognised as a triple-click.
        _lastDblClickTs  = event->timestamp();
        _lastDblClickPos = event->pos();
        return;
    }

    // Off-text: treat as another press so rapid clicks (reactions, etc.) register.
    VirtualListWidget::doMouseDoubleClick(event);
}

bool MessageListWidget::tryHandleTripleClick(const QPoint &pos) {
    const TextPos tp = textHitTest(pos);
    if (tp.row < 0)
        return false;
    auto &item = _items[tp.row];
    ensureDocLayout(item);
    if (!item.textDoc)
        return false;

    // Select the visual line under the cursor (the "row" of text), matching the
    // common triple-click behaviour of text editors.
    QTextCursor cur(item.textDoc.get());
    cur.setPosition(tp.offset);
    cur.select(QTextCursor::LineUnderCursor);
    if (!cur.hasSelection())
        return false;
    _selAnchor   = {tp.row, cur.selectionStart()};
    _selFocus    = {tp.row, cur.selectionEnd()};
    _selDragging = false;
    viewport()->update();
    return true;
}

bool MessageListWidget::tryHandleScrollbarPress(const QPoint &pos) {
    const int sbHitX = scrollThumbHitX();
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
    if (btn < 0 || _hoveredRow < 0 || _hoveredRow >= (int)_tops.size())
        return false;

    const auto &msg = _items[_hoveredRow].msg;
    if (msg.pending) // not on the server yet — no actions available
        return false;
    if (isSystemRow(_hoveredRow)) // system lines have no actions or toolbar
        return false;
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
    if (msg.pending)
        return;
    const Capabilities caps         = _session ? _session->capabilities() : Capabilities{};
    const bool         isOwnMessage = _session && (msg.author == _session->meUserId());
    // Edit is own-only and only where the backend supports editing (email can't).
    const bool         canEdit      = isOwnMessage && caps.editMessage;
    // Delete needs backend support, then: any message if the backend says so
    // (email — it's your mailbox), else your own messages or the admin path.
    const bool         canDelete = caps.deleteMessage && (caps.deleteAnyMessage || isOwnMessage ||
                                                  (_session && _session->meIsAdmin()));
    const QString      linkUrl   = firstLinkInMessage(msg);

    auto *menu = new ContextMenu(this);

    bool addedThreadSection = false;
    if (!_isThreadMode && canHostThread(msg)) {
        menu->addItem(
            tr("Reply in thread"),
            "T",
            [this, conv = _currentConv, rootTs = msg.threadRoot.value_or(msg.ts)] {
                emit threadClicked(conv, rootTs);
            },
            false,
            false,
            ":/ui/message-square-reply.svg"
        );
        addedThreadSection = true;
    }
    // Mute/unmute the thread this message belongs to (root with replies, or a
    // reply). Threads notify by default; muting silences their replies.
    if (_session) {
        if (const auto threadRoot = threadRootOf(msg)) {
            const bool muted = _session->isThreadMuted(_currentConv, *threadRoot);
            menu->addItem(
                muted ? tr("Unmute thread") : tr("Mute thread"),
                {},
                [this, conv = _currentConv, root = *threadRoot, muted] {
                    if (_session)
                        _session->setThreadMuted(conv, root, !muted);
                },
                false,
                false,
                muted ? ":/ui/bell.svg" : ":/ui/bell-off.svg"
            );
            addedThreadSection = true;
        }
    }
    if (addedThreadSection)
        menu->addSeparator();

    if (canEdit) {
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

    menu->addItem(
        tr("Summarize down"),
        {},
        [this, ts = msg.ts] { startSummarizeDown(ts); },
        false,
        false,
        ":/ui/sparkles.svg"
    );

    if (canDelete) {
        menu->addSeparator();
        menu->addItem(
            tr("Delete message…"),
            "Del",
            [this, msg] {
                auto *dlg = new DeleteMessageDialog(msg, _session, window());
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &AppDialog::accepted, this, [this, ts = msg.ts, conv = _currentConv] {
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

void MessageListWidget::startSummarizeDown(const Ts &fromTs) {
    if (!_session)
        return;
    // Fail fast when no AI provider is connected: no spinner, no thread
    // fetches — just the notice with a deep link to Settings → AI assistance.
    if (!LlmService::instance().isAvailable()) {
        auto *dlg = new SummaryDialog(
            tr("Summaries need an AI provider. Connect one in Settings → AI assistance."),
            SummaryDialog::Kind::NoProvider,
            window()
        );
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &SummaryDialog::openSettingsRequested, this, [this] {
            emit aiSettingsRequested();
        });
        dlg->open();
        return;
    }
    const int start = findByTs(fromTs);
    if (start < 0)
        return;
    // Everything from the chosen message down to the newest loaded one — the
    // pages below a visible message are always already loaded (history arrives
    // newest-first and paginates upward).
    std::vector<Message> span;
    for (int i = start; i < (int)_items.size(); ++i) {
        const Message &m = _items[i].msg;
        if (m.pending || isSystemEvent(m))
            continue;
        span.push_back(m);
    }
    // In a thread view the replies are the span itself; in a channel the roots'
    // threads are fetched and inlined by the job.
    SummarizeJob::start(_session, _currentConv, std::move(span), !_isThreadMode, window());
}

void MessageListWidget::showReactionTooltip(int mi, int ri, const QRect &chipVpRect) {
    if (mi < 0 || mi >= (int)_items.size())
        return;
    const auto &reactions = _items[mi].msg.reactions;
    if (ri < 0 || ri >= (int)reactions.size())
        return;
    const auto &r = reactions[ri];

    // Resolve reactor display names; our own reaction reads as "You".
    const UserId me = _session->meUserId();
    QStringList  names;
    names.reserve((int)r.users.size());
    for (const UserId &uid : r.users) {
        if (uid == me) {
            names.push_front(tr("You"));
            continue;
        }
        names.push_back(_session->userDisplayName(uid));
    }
    if (names.isEmpty())
        return;

    const auto emoji = MsgRender::resolveEmojiRich(r.name, _session);
    QPixmap    img =
        (!emoji.imageUrl.isEmpty() && _imgCache) ? _imgCache->get(emoji.imageUrl) : QPixmap();
    // For an animated custom emoji, prefer the live animation frame over the still
    // first frame (which is often an odd pose — see paintReactions). The pill's movie
    // is already running, so this shows the current clinked/mid-motion frame.
    if (!emoji.imageUrl.isEmpty()) {
        if (QMovie *mv = gifMovieFor(emoji.imageUrl)) {
            const QPixmap frame = mv->currentPixmap();
            if (!frame.isNull())
                img = frame;
        }
    }

    const QRect chipGlobal(viewport()->mapToGlobal(chipVpRect.topLeft()), chipVpRect.size());
    _tooltip->showReaction(emoji.unicode, img, names, chipGlobal);
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
    const Ts ts = _items[replyIdx].msg.ts;
    if (_threadsInline) {
        // Inline mode: the bar toggles the expanded replies under the message.
        if (_inlineThreads.count(ts) > 0)
            collapseInlineThread(ts);
        else
            expandInlineThread(_currentConv, ts);
    } else if (!_openThreadRoot.isEmpty() && _openThreadRoot == ts) {
        // Standalone mode: clicking an already-open thread closes the panel.
        emit threadCloseRequested();
    } else {
        emit threadClicked(_currentConv, ts);
    }
    return true;
}

bool MessageListWidget::tryHandleInlineThreadPress(const QPoint &pos) {
    if (!_threadsInline || _inlineThreads.empty())
        return false;

    const PaintContext ctx = makePaintContext();
    const auto         m   = inlineReplyMetrics();

    for (int i = 0; i < (int)_items.size(); ++i) {
        if (_items[i].msg.replyCount <= 0)
            continue;
        const Ts   ts  = _items[i].msg.ts;
        const auto itt = _inlineThreads.find(ts);
        if (itt == _inlineThreads.end())
            continue;

        const int rowTop = _tops[i] - ctx.scrollY;
        const int rh     = rowHeight(i);
        if (rowTop > pos.y())
            break;
        if (rowTop + rh <= pos.y())
            continue;

        const int regionTop = replyBarVpTop(i, ctx) + kReplyBarH;
        const int regionH   = inlineThreadHeight(ts);
        if (pos.y() < regionTop || pos.y() >= regionTop + regionH)
            continue; // click is elsewhere in this row (e.g. the reply bar itself)

        const auto &th = itt->second;
        int         y  = regionTop + kInlineTopGap;

        if (th.loading && th.replies.empty()) {
            y += kInlineLoadingH;
        } else {
            for (int j = 0; j < (int)th.replies.size(); ++j) {
                const MessageItem &reply = th.replies[j];
                const bool         coll  = inlineReplyCollapsed(th.replies, j);
                const int          hRep  = replyItemHeight(reply, m.textWidth, coll);
                if (pos.y() >= y && pos.y() < y + hRep) {
                    // Resolve a link inside this reply's body document.
                    ensureDocLayout(reply, m.textWidth);
                    const int contentY = (coll ? y + kPadVCollapsed : y + kPadV + kHdrH + kHdrGap);
                    if (reply.textDoc && reply.docHeight > 0) {
                        const QPointF local(pos.x() - m.textLeft, pos.y() - contentY);
                        if (local.x() >= 0 && local.y() >= 0 && local.y() <= reply.docHeight) {
                            const QString anchor = reply.textDoc->documentLayout()->anchorAt(local);
                            if (!anchor.isEmpty()) {
                                const QString gifKey = MsgRender::gifKeyFromAnchor(anchor);
                                if (!gifKey.isEmpty()) {
                                    if (!_collapsedGifs.remove(gifKey))
                                        _collapsedGifs.insert(gifKey);
                                    reply.textDoc.reset();
                                    reply.attachDocs.clear();
                                    reply.docWidth = -1;
                                    rebuildLayout();
                                    viewport()->update();
                                    return true;
                                }
                                openAnchorTarget(anchor, pos);
                                return true;
                            }
                        }
                    }
                    return true; // consumed inside the reply (no text-selection inline)
                }
                y += hRep;
            }
        }

        // "Reply to thread" footer → open the standalone panel.
        y += kInlineFooterGap;
        if (QRect(m.textLeft, y, inlineFooterTextWidth(), kInlineFooterH).contains(pos)) {
            emit threadClicked(_currentConv, ts);
            return true;
        }
        return true; // click landed in the region but not on anything actionable
    }
    return false;
}

bool MessageListWidget::tryHandleLinkPress(const QPoint &pos) {
    const QString anchor = anchorAt(pos);
    if (anchor.isEmpty())
        return false;
    const QString gifKey = MsgRender::gifKeyFromAnchor(anchor);
    if (!gifKey.isEmpty()) {
        // "GIF ▾" title line: collapse/expand the image block below it.
        if (!_collapsedGifs.remove(gifKey))
            _collapsedGifs.insert(gifKey);
        const int idx = findByTs(gifKey.section('/', 0, 0));
        if (idx >= 0) {
            auto &item = _items[idx];
            item.textDoc.reset();
            item.attachDocs.clear();
            item.docWidth = -1;
            rebuildLayout();
            viewport()->update();
        }
        return true;
    }
    return openAnchorTarget(anchor, pos);
}

bool MessageListWidget::openAnchorTarget(const QString &anchor, const QPoint &pos) {
    const QString uid = MsgRender::userIdFromAnchor(anchor);
    if (!uid.isEmpty()) {
        // Clicking a mention opens the profile card without the hover delay.
        _profileShowTimer.stop();
        showProfileCardFor(uid, userAnchorVpRect(pos, anchor));
        return true;
    }
    const QString cid = MsgRender::channelIdFromAnchor(anchor);
    if (!cid.isEmpty()) {
        emit openChannelRequested(ConversationId{cid});
        return true;
    }
    if (MsgRender::isBotButtonAnchor(anchor)) {
        const QString btnUrl = MsgRender::botButtonUrlFromAnchor(anchor);
        if (!btnUrl.isEmpty()) {
            QDesktopServices::openUrl(QUrl(btnUrl));
            return true;
        }
        // Interactive bot buttons can't be triggered from here: Slack delivers
        // button callbacks to the bot only from its own clients (the endpoints
        // are closed to third-party API tokens) — explain instead of ignoring.
        constexpr int kToastMs = 2600;
        const QPoint  gPos     = viewport()->mapToGlobal(pos);
        _tooltip->showAbove(
            tr("Slack doesn't let third-party apps press bot buttons, we are working on a "
               "workaround"),
            QRect(gPos - QPoint(0, 2), QSize(1, 4))
        );
        _tooltipPin.setRemainingTime(kToastMs);
        QTimer::singleShot(kToastMs, _tooltip, &QWidget::hide);
        return true;
    }
    const QUrl url(anchor);
    if (MailtoLink::isMailto(url)) {
        if (MailtoLink::openOrCopy(url)) {
            // No mail client registered — tell the user where the address went.
            constexpr int kToastMs = 1800;
            const QPoint  gPos     = viewport()->mapToGlobal(pos);
            _tooltip->showAbove(
                tr("No email app — address copied"), QRect(gPos - QPoint(0, 2), QSize(1, 4))
            );
            _tooltipPin.setRemainingTime(kToastMs);
            QTimer::singleShot(kToastMs, _tooltip, &QWidget::hide);
        }
        return true;
    }
    QDesktopServices::openUrl(url);
    return true;
}

bool MessageListWidget::tryHandleFileActionBarPress(const QPoint &pos) {
    const int btn = fileActionBarButtonAt(pos);
    if (btn < 0 || _hoveredFile.first < 0)
        return false;

    const auto &msg = _items[_hoveredFile.first].msg;
    if (msg.pending) // not on the server yet — no actions available
        return false;
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
        Ui::getSaveFileName(this, tr("Save file"), QDir::homePath() + "/" + defaultName);
    if (savePath.isEmpty())
        return;
    const QString url  = file.urlPrivate;
    // Spinner runs from the click until the bytes are on disk (or the download
    // fails) — same background-task indication used by the image-copy path.
    const int     task = BackgroundTasks::instance().begin(tr("Downloading %1").arg(defaultName));
    _session->downloadFile(
        url,
        [savePath, task](QByteArray data) {
            QFile f(savePath);
            if (f.open(QIODevice::WriteOnly))
                f.write(data);
            BackgroundTasks::instance().end(task);
        },
        [task](QString err) {
            qWarning() << "File download failed:" << err;
            BackgroundTasks::instance().end(task);
        }
    );
}

namespace {
// Decode the image bytes on a thread-pool worker — a multi-megapixel decode can
// take tens of ms and would otherwise hitch the GUI thread — then hop back to the
// GUI thread for the clipboard set (QClipboard is GUI-thread-only) and to clear
// the background task. Routed through qApp, not the widget, so the copy still
// completes and the spinner still clears even if the message list is gone by then.
void decodeImageToClipboardAsync(QByteArray data, int task) {
    QThreadPool::globalInstance()->start([data = std::move(data), task]() mutable {
        QImage     img;
        const bool ok = img.loadFromData(data) && !img.isNull();
        QMetaObject::invokeMethod(qApp, [img = std::move(img), ok, task]() mutable {
            if (ok)
                Clipboard::setImage(img);
            BackgroundTasks::instance().end(task);
        });
    });
}
} // namespace

void MessageListWidget::copyFullImageToClipboard(const File &file) {
    if (file.urlPrivate.isEmpty())
        return;

    // Spinner runs from the click until the image lands on the clipboard. Every
    // path funnels through decodeImageToClipboardAsync, which decodes off the GUI
    // thread and ends the task (even on empty/invalid bytes).
    const QString copyName = file.name.isEmpty() ? tr("image") : file.name;
    const int     task     = BackgroundTasks::instance().begin(tr("Copying %1").arg(copyName));

    // Pending upload — the original bytes live on disk.
    if (file.urlPrivate.startsWith("file://")) {
        QFile      f(QUrl(file.urlPrivate).toLocalFile());
        QByteArray data;
        if (f.open(QIODevice::ReadOnly))
            data = f.readAll();
        decodeImageToClipboardAsync(std::move(data), task);
        return;
    }

    // Already downloaded in full once (e.g. opened in the viewer) — use the cache.
    if (_session) {
        const auto cached = _session->cachedImage(file.urlPrivate);
        if (!cached.isEmpty()) {
            decodeImageToClipboardAsync(cached, task);
            return;
        }
    }

    if (!_session) {
        BackgroundTasks::instance().end(task);
        return;
    }
    // Slow path: the bytes must come over the network.
    _session->downloadFile(
        file.urlPrivate,
        [this, url = file.urlPrivate, task](QByteArray data) {
            if (_session)
                _session->cacheImage(url, data);
            decodeImageToClipboardAsync(std::move(data), task);
        },
        [task](QString err) {
            qWarning() << "Copy full image failed:" << err;
            BackgroundTasks::instance().end(task);
        }
    );
}

void MessageListWidget::showFileContextMenu(
    const File &file, const Message &msg, const QPoint &globalPos
) {
    if (msg.pending)
        return;
    const bool isImage = file.isImage();
    auto      *menu    = new ContextMenu(this);

    if (file.isCsv() && !file.urlPrivate.isEmpty()) {
        menu->addItem(
            tr("Preview"), {}, [this, file] { openCsvPreview(file); }, false, false, ":/ui/eye.svg"
        );
    }

    const QString linkUrl = file.permalink.isEmpty() ? file.urlPrivate : file.permalink;
    menu->addItem(
        isImage ? tr("Copy link to image") : tr("Copy link to file"),
        {},
        [linkUrl] { Clipboard::setText(linkUrl); },
        false,
        false,
        ":/ui/link.svg"
    );

    if (isImage && !file.urlPrivate.isEmpty()) {
        menu->addItem(
            tr("Copy full image"),
            {},
            [this, file] { copyFullImageToClipboard(file); },
            false,
            false,
            ":/ui/copy.svg"
        );
    }

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

void MessageListWidget::openCsvPreview(const File &file) {
    if (!_session || file.urlPrivate.isEmpty())
        return;
    const QString name = file.name.isEmpty() ? tr("file") : file.name;
    // Spinner runs from the click until the viewer opens (or the download fails).
    const int     task = BackgroundTasks::instance().begin(tr("Downloading %1").arg(name));
    _session->downloadFile(
        file.urlPrivate,
        [this, task](QByteArray data) {
            BackgroundTasks::instance().end(task);
            Block blk = MsgRender::csvToTableBlock(data);
            if (blk.tableRows.empty())
                blk.tableRows.push_back({TextWithEntities{tr("This file is empty"), {}}});
            if ((int)blk.tableRows.size() > MsgRender::kMaxCsvViewerRows) {
                const int all = (int)blk.tableRows.size();
                blk.tableRows.resize(MsgRender::kMaxCsvViewerRows);
                blk.tableRows.push_back({TextWithEntities{
                    tr("Showing the first %1 of %2 rows")
                        .arg(MsgRender::kMaxCsvViewerRows)
                        .arg(all),
                    {}
                }});
            }
            if (!_tableViewer)
                _tableViewer = new TableViewerOverlay(window());
            _tableViewer->open(blk, _session);
        },
        [task](QString err) {
            qWarning() << "CSV preview download failed:" << err;
            BackgroundTasks::instance().end(task);
        }
    );
}

bool MessageListWidget::tryHandlePreviewPress(const QPoint &pos) {
    const File *f = previewFileAt(pos);
    if (!f)
        return false;
    openPreviewViewer(*f, _items[_hoveredFile.first].msg);
    return true;
}

void MessageListWidget::openPreviewViewer(const File &file, const Message &msg) {
    if (!_imageViewer) {
        _imageViewer = new ImageViewerOverlay(window());
        connect(_imageViewer, &ImageViewerOverlay::downloadRequested, this, [this](const File &f) {
            downloadFileToUser(f);
        });
        connect(
            _imageViewer, &ImageViewerOverlay::forwardRequested, this, [this](const Message &m) {
                emit forwardMessageRequested(m);
            }
        );
        connect(
            _imageViewer,
            &ImageViewerOverlay::moreRequested,
            this,
            [this](const File &f, const Message &m, const QPoint &globalPos) {
                showFileContextMenu(f, m, globalPos);
            }
        );
    }

    const QString thumbKey = filePreviewUrl(file);
    _imageViewer->open(file, msg, _fileImages.value(thumbKey));

    // Full resolution only makes sense for real images — a PDF's url_private is
    // the document itself, and its thumb already IS the prerendered page.
    if (!file.isImage() || file.urlPrivate.isEmpty())
        return;
    if (file.urlPrivate.startsWith("file://")) { // pending upload — read from disk
        const QPixmap px(QUrl(file.urlPrivate).toLocalFile());
        _imageViewer->updatePixmap(file.id, px);
        return;
    }
    if (file.urlPrivate == thumbKey || !_session || msg.pending)
        return;
    const auto cached = _session->cachedImage(file.urlPrivate);
    if (!cached.isEmpty()) {
        QPixmap px;
        if (px.loadFromData(cached) && !px.isNull()) {
            _imageViewer->updatePixmap(file.id, px);
            return;
        }
    }
    _session->downloadFile(
        file.urlPrivate, [this, id = file.id, url = file.urlPrivate](QByteArray data) {
            if (_session)
                _session->cacheImage(url, data);
            QPixmap px;
            if (px.loadFromData(data) && _imageViewer)
                _imageViewer->updatePixmap(id, px);
        }
    );
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
    // Grace-period hide so the cursor can travel from the mention into the
    // card (entering the card cancels the timer).
    _profileShowTimer.stop();
    _pendingProfileAnchor.clear();
    _profileCard->scheduleHide();

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
        _hoveredReplyRow != -1 || _hoveredFile.first != -1 || _hoveredReaction.first != -1 ||
        !_hoveredThreadFooter.isEmpty() || _hoveredTable.valid()) {
        _hoveredRow          = -1;
        _hoveredToolBtn      = -1;
        _hoveredAttach       = {-1, -1};
        _hoveredReplyRow     = -1;
        _hoveredFile         = {-1, -1};
        _hoveredFileBtn      = -1;
        _hoveredReaction     = {-1, -1};
        _hoveredThreadFooter = Ts{};
        _hoveredTable        = {};
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

    for (int i = firstVisibleRow(docY); i < (int)_items.size(); ++i) {
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
    if (_hoveredRow >= 0 && _hoveredRow < (int)_tops.size()) {
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
        for (int i = firstVisibleRow(pos.y() + scrollY); i < (int)_items.size(); ++i) {
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
    if (newHoveredRow >= 0 && !isSystemRow(newHoveredRow)) {
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

    const int newHoveredReplyRow   = replyBarIndexAt(pos);
    const Ts  newHoveredThreadFoot = inlineFooterAt(pos);

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

    QRect                     reactionChipRect;
    const std::pair<int, int> newHoveredReaction = reactionAt(pos, &reactionChipRect);
    const TableHit            newHoveredTable    = tableHitAt(pos);

    if (newHoveredRow != _hoveredRow || newHoveredBtn != _hoveredToolBtn ||
        newHoveredAttach != _hoveredAttach || newHoveredReplyRow != _hoveredReplyRow ||
        newHoveredFile != _hoveredFile || newHoveredFileBtn != _hoveredFileBtn ||
        newHoveredReaction != _hoveredReaction || newHoveredThreadFoot != _hoveredThreadFooter ||
        newHoveredTable != _hoveredTable) {
        _hoveredRow          = newHoveredRow;
        _hoveredToolBtn      = newHoveredBtn;
        _hoveredAttach       = newHoveredAttach;
        _hoveredReplyRow     = newHoveredReplyRow;
        _hoveredFile         = newHoveredFile;
        _hoveredFileBtn      = newHoveredFileBtn;
        _hoveredReaction     = newHoveredReaction;
        _hoveredThreadFooter = newHoveredThreadFoot;
        _hoveredTable        = newHoveredTable;
        viewport()->update();
    }

    // Compute anchor once and reuse for link hover, tooltip, and cursor
    const QString anchor       = anchorAt(pos);
    const bool    isUserAnchor = !MsgRender::userIdFromAnchor(anchor).isEmpty();
    const bool    isChanAnchor = !MsgRender::channelIdFromAnchor(anchor).isEmpty();
    const bool    isGifAnchor  = !MsgRender::gifKeyFromAnchor(anchor).isEmpty();
    const bool    isBotBtn     = MsgRender::isBotButtonAnchor(anchor);

    // Update link hover underline (mention chips, "GIF ▾" toggles and bot
    // buttons don't get underlined — buttons aren't links)
    if (anchor != _hoveredLinkUrl) {
        if (!_hoveredLinkUrl.isEmpty() && _hoveredLinkRow >= 0 &&
            _hoveredLinkRow < (int)_items.size()) {
            setDocLinkUnderline(_items[_hoveredLinkRow].textDoc.get(), _hoveredLinkUrl, false);
            for (auto &ad : _items[_hoveredLinkRow].attachDocs)
                setDocLinkUnderline(ad.textDoc.get(), _hoveredLinkUrl, false);
        }
        _hoveredLinkUrl = anchor;
        _hoveredLinkRow = newHoveredRow;
        if (!anchor.isEmpty() && !isUserAnchor && !isChanAnchor && !isGifAnchor && !isBotBtn &&
            newHoveredRow >= 0) {
            setDocLinkUnderline(_items[newHoveredRow].textDoc.get(), anchor, true);
            for (auto &ad : _items[newHoveredRow].attachDocs)
                setDocLinkUnderline(ad.textDoc.get(), anchor, true);
        }
        viewport()->update();
    }

    // Mention / avatar hover → profile card (after a short delay); leaving → grace hide
    QRect         avatarVpRect;
    const QString avatarUid = isUserAnchor ? QString() : avatarUserAt(pos, &avatarVpRect);
    if (isUserAnchor || !avatarUid.isEmpty()) {
        const QString uid = isUserAnchor ? MsgRender::userIdFromAnchor(anchor) : avatarUid;
        if (_profileCard->isVisible() && _profileCard->userId().value == uid) {
            _profileCard->cancelHide();
        } else if (isUserAnchor) {
            if (anchor != _pendingProfileAnchor) {
                _pendingProfileAnchor     = anchor;
                _pendingProfileAvatarUser = UserId{};
                _profileShowTimer.start();
            }
        } else if (_pendingProfileAvatarUser.value != uid) {
            _pendingProfileAnchor.clear();
            _pendingProfileAvatarUser = UserId{uid};
            _profileShowTimer.start();
        }
    } else {
        _profileShowTimer.stop();
        _pendingProfileAnchor.clear();
        _pendingProfileAvatarUser = UserId{};
        _profileCard->scheduleHide();
    }

    // Tooltip
    if (!_tooltipPin.hasExpired()) {
        // A click toast (e.g. "address copied") is showing — leave it up.
    } else if (newHoveredReaction.first >= 0 && _session) {
        showReactionTooltip(newHoveredReaction.first, newHoveredReaction.second, reactionChipRect);
    } else if (newHoveredBtn >= 0) {
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
    } else if (!anchor.isEmpty() && !isUserAnchor && !isChanAnchor && !isGifAnchor) {
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
    const bool overTablePill = _hoveredTable.valid() && tablePillRect(_hoveredTable).contains(pos);
    // File action bar buttons take priority — keep arrow cursor over them even if a chip is below.
    const bool overFileBar   = newHoveredFileBtn >= 0;
    const bool overLink =
        !overFileBar && (!anchor.isEmpty() || fileChipAt(pos) || previewFileAt(pos) ||
                         replyBarIndexAt(pos) >= 0 || overDismiss || overTablePill ||
                         newHoveredReaction.first >= 0 || !newHoveredThreadFoot.isEmpty());
    const int  sbHitX     = scrollThumbHitX();
    const bool overScroll = pos.x() >= sbHitX && isOnScrollThumb(pos.y());
    const bool overText   = !overLink && textHitTest(pos).row >= 0;
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
            // bump the reply count on the root message instead. Skip the
            // optimistic ghost: our own reply arrives twice (the pending ghost
            // with a fake ts, then the confirmed echo with the real ts), and
            // since the two carry different ts they can't be deduped against
            // each other — counting both double-bumps the badge until a
            // reload heals it. Counting only the confirmed copy keeps it exact.
            if (!ev->msg.pending) {
                const int rootIdx = findByTs(*ev->msg.threadRoot);
                if (rootIdx >= 0) {
                    _items[rootIdx].msg.replyCount++;
                    rebuildLayout();
                    viewport()->update();
                }
            }
            return;
        }
        // Backstop dedup: the ts can already be on screen when a history
        // load raced the realtime echo. Refresh that row in place instead of
        // appending a twin.
        const int existing = findByTs(ev->msg.ts);
        if (existing >= 0) {
            _items[existing].msg = ev->msg;
            _items[existing].textDoc.reset();
            _items[existing].docWidth = 0;
            _items[existing].attachDocs.clear();
            _items[existing].fileImgsRequested = false;
            _items[existing].fileImgBaseH      = -1;
            rebuildLayout();
            viewport()->update();
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
        if (ev->textOnly) {
            // chat.update response echo — carries only the new text; keep the
            // row's files/reactions/thread state. Blocks must be taken from the
            // echo too (i.e. cleared): the doc renders from blocks when present,
            // so the stale rich_text block would keep showing the old text. A
            // text-only chat.update replaces the blocks server-side as well; the
            // realtime echo later restores the server-regenerated ones.
            _items[i].msg.text    = ev->msg.text;
            _items[i].msg.rawText = ev->msg.rawText;
            _items[i].msg.blocks  = ev->msg.blocks;
            _items[i].msg.edited  = true;
        } else {
            _items[i].msg = ev->msg;
        }
        _items[i].textDoc.reset(); // invalidate rendered docs
        _items[i].docWidth = 0;
        _items[i].attachDocs.clear();
        _items[i].fileImgsRequested = false;
        _items[i].fileImgBaseH      = -1;
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvMessageDeleted>(&e)) {
        if (ev->conv != _currentConv)
            return;
        bool changed = false;
        // A deleted thread reply contributes to the root's reply count, so drop
        // it (mirrors the EvMessageNew bump). Most replies have no row of their
        // own in the channel list — but a "also send to channel" broadcast does,
        // so don't stop here: still remove its row below if present. Thread mode
        // shows the reply itself and no count, so it skips this and just removes.
        if (!_isThreadMode && ev->threadRoot && *ev->threadRoot != ev->ts) {
            const int rootIdx = findByTs(*ev->threadRoot);
            if (rootIdx >= 0 && _items[rootIdx].msg.replyCount > 0) {
                _items[rootIdx].msg.replyCount--;
                changed = true;
            }
        }
        const int i = findByTs(ev->ts);
        if (i >= 0) {
            _items.erase(_items.begin() + i);
            // Row indices shifted — drop hover state; the next mouse move recomputes it.
            _hoveredRow      = -1;
            _hoveredToolBtn  = -1;
            _hoveredAttach   = {-1, -1};
            _hoveredTable    = {};
            _hoveredReplyRow = -1;
            _hoveredFile     = {-1, -1};
            _hoveredFileBtn  = -1;
            changed          = true;
        }
        if (changed) {
            rebuildLayout();
            viewport()->update();
        }

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
                // Idempotent vs the optimistic local add: our own reaction echoes
                // back via Socket Mode after we've already counted it.
                if (std::find(r.users.begin(), r.users.end(), ev->user) == r.users.end()) {
                    r.count++;
                    r.users.push_back(ev->user);
                }
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
                auto      &users = rit->users;
                const auto sz    = users.size();
                users.erase(std::remove(users.begin(), users.end(), ev->user), users.end());
                // Idempotent vs the optimistic local remove (own un-react already
                // decremented). Still decrement when the user list is truncated
                // (history reactions can carry count > users.size()).
                if (users.size() != sz || (int)sz < rit->count)
                    rit->count = std::max(0, rit->count - 1);
                if (rit->count == 0)
                    reactions.erase(rit);
                break;
            }
        }
        rebuildLayout();
        viewport()->update();

    } else if (auto *ev = std::get_if<EvPresenceChanged>(&e)) {
        // Keep the profile card's presence dot live while it is shown.
        if (_profileCard->isVisible() && ev->user == _profileCard->userId())
            _profileCard->setActive(ev->active);

    } else if (auto *ev = std::get_if<EvUserChanged>(&e)) {
        // A member changed their profile/avatar. Author info is resolved live
        // via Session::findUser on paint, so a repaint is enough to pick up the
        // new name + avatar URL (ImageCache fetches the new image on demand).
        const bool authoredHere = std::any_of(_items.begin(), _items.end(), [&](const auto &it) {
            return it.msg.author == ev->user.id;
        });
        if (authoredHere)
            viewport()->update();

    } else if (auto *ev = std::get_if<EvUsersChanged>(&e)) {
        // Bulk profile/avatar refresh — one repaint if any of them wrote here.
        QSet<QString> changed;
        for (const User &u : ev->users)
            changed.insert(u.id.value);
        const bool authoredHere = std::any_of(_items.begin(), _items.end(), [&](const auto &it) {
            return changed.contains(it.msg.author.value);
        });
        if (authoredHere)
            viewport()->update();

    } else if (auto *ev = std::get_if<EvHeadRefresh>(&e)) {
        // The safety poll re-fetched this conversation's head page. Merge it to
        // fill any middle gap the socket's round-robin steal left buried under a
        // later message — the per-message realtime path can't recover that.
        mergeHeadPage(ev->conv, ev->messages);
    } else if (std::get_if<EvRealtimeReconnected>(&e)) {
        backfillAfterReconnect();
    }
}

void MessageListWidget::backfillAfterReconnect() {
    if (!_session || _currentConv.value.isEmpty())
        return;
    // Collapse a reconnect flap into one head fetch per window (see the member
    // comment): conversations.history is rate-limited to ~1 req/min, and an
    // un-gated backfill on every EvRealtimeReconnected was a 429 source.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - _lastReconnectBackfillMs < kReconnectBackfillGapMs)
        return;
    _lastReconnectBackfillMs = now;
    const auto conv          = _currentConv;
    auto       producer      = _isThreadMode
                                   ? _session->backend()->loadThread(conv, _threadRootTs, std::nullopt)
                                   : _session->backend()->loadHistory(conv, std::nullopt);
    std::move(producer) | rpl::on_next(
                              [this, conv](MessagePage page) {
                                  // (mergeNetworkMessages dedups by ts, so racing the initial open
                                  // load can't produce twins.) mergeHeadPage guards _currentConv.
                                  mergeHeadPage(conv, page.messages);
                              },
                              _eventLifetime
                          );
}

void MessageListWidget::mergeHeadPage(
    const ConversationId &conv, const std::vector<Message> &messages
) {
    // Conversation changed out from under an in-flight fetch, or the page is for a
    // thread we're not in (the poll always fetches channel history, never thread
    // replies — merging it into a thread view would be wrong).
    if (_currentConv != conv || _isThreadMode)
        return;
    const bool wasAtBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
    // No-cursor (head) fetch — authoritative for the channel head, so it also
    // reconciles deletions missed during the socket gap.
    mergeNetworkMessages(messages, /*fromHeadPage=*/true);
    if (_session) {
        std::vector<Message> msgs(messages.begin(), messages.end());
        _session->cacheMessages(conv, msgs);
    }
    // Reveal anything that landed during the gap, but only if the user was already
    // pinned to the bottom (don't yank them out of scrollback they're reading).
    if (wasAtBottom)
        QTimer::singleShot(0, this, [this, conv] {
            if (_currentConv == conv)
                verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        });
}
