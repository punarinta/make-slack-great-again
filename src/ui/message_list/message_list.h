// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"
#include "ui/loading_indicator/loading_indicator.h"
#include "ui/virtual_list/virtual_list_widget.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QVariantAnimation>
#include <QSet>
#include <QHash>
#include <QStaticText>
#include <QPixmap>
#include <QStringList>
#include <QTextDocument> // structs below hold unique_ptr<QTextDocument>; need the
                         // complete type so their (implicit) destructors are
                         // instantiable wherever this header is consumed.
#include <QTimer>

#include <map>
#include <memory>
#include <vector>

class QMovie;
class Session;
class ImageCache;
class PopupTooltip;
class EmojiPickerPopup;
class ImageViewerOverlay;
class TableViewerOverlay;
class UserProfileCard;

// Per-attachment rendered doc (lazy, like the main textDoc).
struct AttachDoc {
    mutable std::unique_ptr<QTextDocument> textDoc;
    mutable int                            docWidth  = 0;
    mutable int                            docHeight = 0;
};

// Data + lazily-computed layout for a single message.
// textDoc is mutable so ensureDocLayout() can work from a const context.
struct MessageItem {
    Message                                msg;
    mutable std::unique_ptr<QTextDocument> textDoc;
    mutable int                    docWidth  = 0; // viewport width at which textDoc was laid out
    mutable int                    docHeight = 0; // cached pixel height of textDoc
    mutable std::vector<AttachDoc> attachDocs;    // one per msg.attachments entry
    mutable bool fileImgsRequested = false; // true once file image download has been triggered
    mutable bool attachImgsRequested =
        false;                     // true once attachment image/favicon download triggered
    mutable QStringList emojiUrls; // custom-emoji image URLs referenced by this message
    mutable bool        emojiUrlsCollected = false;
    // Memoized layoutFileImages(…, hasAbove=false).height for rowHeight(),
    // which runs O(n) per rebuildLayout — computing the full layout (two heap
    // allocations) each call is measurable. Valid while fileImgGen matches the
    // widget's _fileImagesGen; reset to -1 when msg is replaced in place.
    mutable int         fileImgBaseH       = -1;
    mutable quint32     fileImgGen         = 0;
    // Shaped-text caches for the header line (author name, timestamp) —
    // QPainter::drawText re-shapes its string on every call, and
    // paintMessageHeader runs per visible row per frame. Keyed by the source
    // string so a display-name change or time-format switch rebuilds in place.
    mutable QStaticText stName, stTs;
    mutable QString     stNameSrc, stTsSrc;
    mutable int         stNameW = 0;
};

// Aggregates the constant viewport geometry computed at the start of every paint/hit-test.
struct PaintContext {
    int vw;        // viewport()->width()
    int scrollY;   // verticalScrollBar()->value()
    int vh;        // viewport()->height()
    int textLeft;  // kPadH + kAvSize + kAvGap
    int textWidth; // vw - textLeft - kPadH
};

// Character position inside a painted message (used for text-selection hit testing).
struct TextPos {
    int  row    = -1; // index into _items (-1 = invalid)
    int  offset = 0;  // character offset within that message's QTextDocument
    bool operator==(const TextPos &o) const { return row == o.row && offset == o.offset; }
    bool operator!=(const TextPos &o) const { return !(*this == o); }
};

// Zero-widget virtual message list.
// Stores MessageItems, paints only visible rows in a single QPainter pass.
// No QLabel/QWidget per message — scales to thousands of rows without stutter.
class MessageListWidget : public VirtualListWidget {
    Q_OBJECT
public:
    explicit MessageListWidget(Session *session, ImageCache *imgCache, QWidget *parent = nullptr);
    // Hands cache-owned animated players back to ImageCache (releaseGifMovies)
    // so their entries don't stay pinned past the widget's life.
    ~MessageListWidget() override;

    // lastReadTs: the conversation's read cursor captured before the open marks
    // it read — when set, the list opens scrolled to the first message after it
    // (the first unread); when empty, it opens scrolled to the bottom.
    void openConversation(ConversationId conv, const Ts &lastReadTs = {});
    // Open a thread view: loads conversations.replies and filters events accordingly.
    void openThread(ConversationId conv, Ts rootTs);
    void clear();
    void setSession(Session *session);

    // Threads display mode (Settings → Appearance). When inline, clicking the
    // reply bar expands the replies underneath the message instead of opening
    // the standalone panel. Switching modes collapses any inline expansions.
    void setThreadsInline(bool on);
    // The thread root currently shown in the standalone panel ({} when none).
    // Drives the reply-bar "Close thread" copy in standalone mode. Pass {} when
    // the panel is closed — this only clears the open-root, it never collapses an
    // inline expansion (the panel may have been opened only to reply inline).
    void setOpenThreadRoot(const Ts &root);

    // Remember the current conversation's reading position so reopening it (after
    // a chat or workspace switch) restores exactly where the user was. Must be
    // called while the list is still laid out normally — before any sibling
    // widget (composer/header) is hidden, since that grows the viewport and would
    // clamp a slightly-scrolled-up position to the bottom.
    void saveScrollAnchor();

    // Show/hide a full-area loading spinner independent of conversation state.
    // Used while the conversation list itself is loading (before any conv can be opened).
    void setWaiting(bool waiting);

    // Returns the most recent non-system message authored by `me`, or nullopt.
    std::optional<Message> lastOwnMessage(UserId me) const;

    // Smooth-scroll to the message with this timestamp if it is currently loaded.
    void scrollToTs(const Ts &ts);

    // Stop animated-image (GIF) decoding while nothing is on screen. Called by
    // the host window on minimize — children get no hideEvent/paint then, so the
    // widget can't notice on its own. Playback resumes on the next paint pass.
    void pauseGifPlayback();

signals:
    // Fired once when the first page of content for the current conversation is
    // ready to display — immediately if loaded from cache, otherwise when the
    // first network response arrives.
    void initialPageLoaded();
    // Emitted in channel mode when user clicks the "N replies" bar on a thread root.
    void threadClicked(ConversationId conv, Ts rootTs);
    // Emitted when the user closes a thread from the message list — either by
    // clicking "Close thread" on an open reply bar (standalone) or by collapsing
    // an inline expansion whose panel is also open. The host should hide the panel.
    void threadCloseRequested();
    // Emitted when "Edit message" is chosen; caller should call composer->enterEditMode().
    void editMessageRequested(Ts ts, QString rawText, std::vector<File> files);
    // Emitted when "Forward message" is chosen.
    void forwardMessageRequested(Message msg);
    // Emitted when "Message" is clicked on the mention-hover profile card.
    void openDmRequested(UserId user);
    // Emitted when a #channel mention chip is clicked; the host should
    // navigate to that conversation (joining it first if not a member).
    void openChannelRequested(ConversationId conv);
    // Emitted when "Open settings" is clicked on the summarize no-provider
    // notice; the host should open Settings → AI assistance.
    void aiSettingsRequested();

protected:
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    // Viewport event handlers (called from eventFilter)
    void doPaint(QPaintEvent *event);
    void doMousePress(QMouseEvent *event);
    void doMouseDoubleClick(QMouseEvent *event) override;
    void doMouseMove(QMouseEvent *event);
    void doMouseRelease(QMouseEvent *event);
    void doMouseLeave();

    // doMousePress sub-handlers — each returns true if it consumed the event.
    bool tryHandleTripleClick(const QPoint &pos);
    bool tryHandleScrollbarPress(const QPoint &pos);
    bool tryHandleToolbarPress(const QPoint &pos);
    bool tryHandleReactionPress(const QPoint &pos);
    bool tryHandleDismissPress(const QPoint &pos);
    bool tryHandleReplyBarPress(const QPoint &pos);
    bool tryHandleInlineThreadPress(const QPoint &pos);
    bool tryHandleLinkPress(const QPoint &pos);
    bool tryHandleFileActionBarPress(const QPoint &pos);
    bool tryHandlePreviewPress(const QPoint &pos);
    bool tryHandleFileChipPress(const QPoint &pos);
    bool tryHandleTablePillPress(const QPoint &pos);

    // A data table under the cursor (message doc or an attachment doc).
    struct TableHit {
        int   row       = -1;
        int   attachIdx = -1; // -1 = the message's own doc
        int   tableIdx  = -1; // n-th data table within that doc
        QRect vpRect;         // table rect in viewport coords
        bool  valid() const { return row >= 0; }
        bool  operator==(const TableHit &) const = default;
    };
    TableHit     tableHitAt(const QPoint &viewportPos) const;
    // "Open full table" pill rect for a hovered table, in viewport coords.
    QRect        tablePillRect(const TableHit &hit) const;
    // The Block a table hit refers to, or nullptr if the model changed.
    const Block *tableBlockFor(const TableHit &hit) const;
    void         paintTablePill(QPainter &p) const;

    // Toolbar sub-actions called from tryHandleToolbarPress.
    void         openEmojiPickerForRow(int row, const QPoint &globalPos);
    void         showMessageContextMenu(const Message &msg, const QPoint &globalPos);
    // "Summarize down": AI-summarize everything from `fromTs` (inclusive) to
    // the newest loaded message. Runs as a background task (SummarizeJob); the
    // report appears in a SummaryDialog when ready.
    void         startSummarizeDown(const Ts &fromTs);
    void         downloadFileToUser(const File &file);
    // Copy the full-resolution image (not the preview thumbnail) to the clipboard,
    // fetching it from disk / cache / network as needed.
    void         copyFullImageToClipboard(const File &file);
    void         showFileContextMenu(const File &file, const Message &msg, const QPoint &globalPos);
    // Open the full-window in-app viewer for a file preview (image / PDF page),
    // then fetch the full-resolution image for real images.
    void         openPreviewViewer(const File &file, const Message &msg);
    bool         isOnScrollThumb(int vpY) const; // delegates to VirtualListWidget with _totalH
    PaintContext makePaintContext() const;

    // Data model
    void appendMessage(const Message &msg);
    // Append without a relayout/repaint — for bulk loads that rebuild once at
    // the end (a per-message rebuildLayout is O(items), so a loop is O(n²)).
    void appendMessageDeferred(const Message &msg);
    // Append every message then rebuild layout once. Used by the cache
    // pre-populate and first-page loads.
    void appendMessages(const std::vector<Message> &msgs);
    int  findByTs(const Ts &ts) const; // linear scan, fine for <500 visible
    void handleEvent(const Event &e);
    // Merge an authoritative server page into the existing item list: insert new
    // rows, update changed ones, and drop rows the server no longer has (deleted
    // from another client). fromHeadPage=true means `incoming` is the latest
    // (no-cursor) page and therefore the channel head, so a local row newer than
    // its newest message was deleted, not merely not-yet-fetched; a cursored
    // (older) page is a middle slice, so deletion reconciliation is capped at its
    // newest message.
    void mergeNetworkMessages(const std::vector<Message> &incoming, bool fromHeadPage);
    // Re-fetch the newest page of the open conversation/thread and merge it,
    // recovering messages that arrived while the realtime socket was down (Slack
    // doesn't replay them). Driven by EvRealtimeReconnected.
    void backfillAfterReconnect();
    // Apply the pending open-scroll intent (saved position / first unread /
    // bottom) if set.
    void applyPendingScroll();
    // {ts of the message anchoring the viewport top, its offset from the
    // viewport top}, or empty ts when nothing is laid out. Reads the
    // _tops/_topsTs snapshot, so it stays valid when _items was mutated after
    // the last rebuild.
    std::pair<Ts, int> viewportAnchor() const;
    // Load older messages using the stored pagination cursor.
    void               loadOlderMessages();
    // Keep loading older pages while the content is shorter than the viewport —
    // the scroll-position trigger can't fire when there is nothing to scroll.
    void               maybeFillViewport();
    // Distance from the top (px) at which scrolling starts fetching the next
    // older page — one viewport height (floored) so the page is already in
    // flight before the very top is reached.
    int                loadOlderMargin() const;

    // Layout
    void rebuildLayout();
    int  rowHeight(int index) const;
    // Index of the first row whose bottom edge can be at/below document-space y
    // `docY` — binary search over the monotonic _tops, so paint and hit-test
    // walks start at the viewport instead of scanning the whole scrollback.
    // Returns 0 when _tops is stale (callers' skip/break guards still apply).
    int  firstVisibleRow(int docY) const;
    // Lazy layout: the expensive QTextDocument::size() runs only for rows that
    // become visible; off-screen rows use a cheap text-length estimate until a
    // background pass measures them. rowMeasured() == laid out at the current width.
    bool rowMeasured(const MessageItem &item) const;
    int  estimatedTextHeight(const QString &text) const;
    int  estimatedDocHeight(const MessageItem &item) const;
    int  estimatedAttachHeight(const Attachment &att) const;
    // Lay out the on-screen rows (called from doPaint). Returns true when a
    // height changed (i.e. rebuildLayout ran and _tops shifted).
    bool measureVisibleRows();
    void scheduleProgressiveLayout();  // kick the background measurement pass
    void measureLayoutChunk();         // measure one chunk of off-screen rows, then reschedule
    bool isCollapsed(int index) const; // true if same author within 5 min of previous
    // Lay out (and lazily build) a message's docs at `forWidth` logical pixels of
    // text column; forWidth < 0 means the full row width (textAreaWidth()).
    // Inline thread replies pass the narrower indented width.
    void ensureDocLayout(const MessageItem &item, int forWidth = -1) const;
    // Trigger any missing image/attachment downloads for one item (factored out
    // of triggerMissingDownloads so inline-reply items get fetched too).
    void requestItemImages(MessageItem &item);
    // Drop every item's rendered docs (and collected emoji URLs) so the next
    // paint rebuilds them — used when emoji resolution inputs change.
    void invalidateAllDocs();
    int  textAreaWidth() const;

    // Painting
    void paintRow(QPainter &p, int index, int rowTop, const PaintContext &ctx) const;
    // Centered single-line rendering for system/activity messages (joins, topic
    // changes, …). msgTop is the row top after any date separator.
    void paintSystemRow(QPainter &p, int index, int msgTop, const PaintContext &ctx) const;
    // Height of a system line's content (no separator), from the active font.
    int  systemRowHeight() const;
    // True when the row is a system/activity line rather than a real message.
    bool isSystemRow(int index) const { return isSystemEvent(_items[index].msg); }
    void paintAvatar(QPainter &p, const MessageItem &item, QRect rect) const;
    // Draw the name + APP badge + timestamp header line at the given text column
    // left and content top. Shared by full message rows and inline thread replies.
    void paintMessageHeader(QPainter &p, const MessageItem &item, int textLeft, int contTop) const;
    void paintReactions(
        QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
    ) const;
    void paintReplyBar(
        QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
    ) const;
    // ── Inline threads ──
    // Geometry of the indented reply column for an expanded inline thread.
    struct InlineMetrics {
        int avatarLeft; // x of each reply's avatar
        int textLeft;   // x of each reply's text column
        int textWidth;  // available width of that text column
    };
    InlineMetrics inlineReplyMetrics() const;
    // True if reply `i` collapses into the previous reply (same author, <5 min).
    bool          inlineReplyCollapsed(const std::vector<MessageItem> &replies, int i) const;
    // Height of a single inline reply laid out at `width`.
    int           replyItemHeight(const MessageItem &item, int width, bool collapsed) const;
    // Total height of the expanded inline region for the thread rooted at `ts`
    // (loading placeholder or replies, plus the "Reply to thread" footer).
    int           inlineThreadHeight(const Ts &rootTs) const;
    // Paint the expanded inline region; `top` is the viewport y just below the
    // reply bar.
    void paintInlineThread(QPainter &p, const Ts &rootTs, const PaintContext &ctx, int top) const;
    // Paint a single inline reply at the indented coordinates; returns nothing,
    // caller advances by replyItemHeight().
    void paintReplyItem(
        QPainter            &p,
        const MessageItem   &item,
        const InlineMetrics &m,
        const PaintContext  &subCtx,
        int                  top,
        bool                 collapsed
    ) const;
    // Viewport y of the reply bar top for row `i` (mirrors the content walk).
    int  replyBarVpTop(int i, const PaintContext &ctx) const;
    // Pixel width of the "Reply to thread" footer label (for hit-test/hover).
    int  inlineFooterTextWidth() const;
    // Root ts whose "Reply to thread" footer is under `pos`, or {} if none.
    Ts   inlineFooterAt(const QPoint &pos) const;
    // Begin loading + showing the inline replies for a thread root.
    void expandInlineThread(ConversationId conv, const Ts &rootTs);
    // Collapse an inline expansion; if its panel is also open, request its close.
    void collapseInlineThread(const Ts &rootTs);
    // Resolve a (already-fetched) anchor href: mentions, bot buttons, mailto,
    // URLs. Returns true if it handled it. Does NOT handle the GIF-collapse
    // anchor (that needs the owning doc, handled at the call site).
    bool openAnchorTarget(const QString &anchor, const QPoint &pos);
    void paintAttachments(
        QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
    ) const;
    void
    paintFileImages(QPainter &p, const MessageItem &item, const PaintContext &ctx, int top) const;
    void
    paintFileChips(QPainter &p, const MessageItem &item, const PaintContext &ctx, int top) const;
    void  paintHoverToolbar(QPainter &p, int index, int rowTop, int rowH) const;
    void  paintFileActionBar(QPainter &p, const QRect &fileRect) const;
    QRect fileViewportRect(int msgIdx, int fileIdx) const;
    QRect fileActionBarButtonRect(int btn, const QRect &fileRect) const;
    int   fileActionBarButtonAt(const QPoint &viewportPos) const;
    void  paintDateSep(QPainter &p, int top, int vw, qint64 dateMicros) const;

    bool needsDateSep(int index) const;

    // Download any missing images for visible rows (called from doPaint).
    void triggerMissingDownloads();
    // Download any missing user avatars for visible rows (called from doPaint).
    void triggerMissingAvatarDownloads();

    // A user that users.list omitted just resolved (Session::userInfoLoaded):
    // re-render any rows whose author or @mention is this user so the raw id
    // becomes a name + avatar.
    void onUserResolved(UserId id);

    // Mouse: returns the href under the given viewport point, or empty.
    QString anchorAt(const QPoint &viewportPos) const;

    // Mention hover profile card
    // Viewport rect of the mention-anchor fragment under viewportPos; falls
    // back to a 1px rect at viewportPos when the fragment can't be located.
    QRect   userAnchorVpRect(const QPoint &viewportPos, const QString &href) const;
    // Look up the user and show the profile card anchored to anchorVpRect.
    void    showProfileCardFor(const QString &userIdStr, const QRect &anchorVpRect);
    void    hideProfileCard();
    // If viewportPos is over a message-row avatar of a known user, returns that
    // user's id and (when outVpRect is non-null) the avatar's viewport rect;
    // otherwise returns an empty string.
    QString avatarUserAt(const QPoint &viewportPos, QRect *outVpRect = nullptr) const;
    // Attachment height helpers
    int
    attachImageH(const Attachment &att) const; // preview image height (includes kImgGap), 0 if none
    int attachTotalH(const MessageItem &item, int ai) const; // docH + imageH

    // Returns pointer to the non-image File chip under viewportPos, or nullptr.
    const File *fileChipAt(const QPoint &viewportPos) const;
    // Returns the hovered file whose inline preview (image or PDF first page)
    // is under viewportPos, or nullptr.
    const File *previewFileAt(const QPoint &viewportPos) const;
    // Displayed size of a file's inline preview: known file dimensions scaled to
    // fit, else the loaded pixmap scaled to fit, else maxW × 24.
    // Single source of truth for rowHeight / paint / hit-test geometry.
    QSize       filePreviewSize(const File &f, int maxW) const;

    // Geometry of a message's inline image/preview region. The single source of
    // truth shared by rowHeight / paintFileImages / hit-test, so the three never
    // drift. One preview file → the classic name-label-above-image layout; 2+ →
    // an equal-tile cover-cropped gallery in wrapping rows (no name labels).
    struct FileImageLayout {
        std::vector<QRect> rects;           // per files() index; null for non-preview files
        int                height  = 0;     // total region height (incl. leading gap)
        bool               gallery = false; // true when tiles are cover-cropped
    };
    // Coordinates are local to the region: x relative to the text-column left,
    // y relative to the region top. `hasAbove` = content (text/attachments)
    // precedes the region (adds a leading gap, matching the old per-image gap).
    FileImageLayout layoutFileImages(const MessageItem &item, int width, bool hasAbove) const;
    // Preview source URLs matching this widget's screen density (DPR-aware).
    QString         filePreviewUrl(const File &f) const;
    QString         attachPreviewUrl(const Attachment &att) const;
    // Returns `src` smooth-scaled to `logical` × dpr physical pixels (with the
    // DPR set on the result), cached per key so paints don't rescale every frame.
    QPixmap scaledPreview(const QString &key, const QPixmap &src, QSize logical, qreal dpr) const;
    // Like scaledPreview, but fills `tile` by scaling `src` to cover and centre-
    // cropping the overflow (CSS object-fit: cover) — used by the image gallery.
    QPixmap coverPreview(const QString &key, const QPixmap &src, QSize tile, qreal dpr) const;
    // Returns index of the message whose reply bar is at viewportPos, or -1.
    int     replyBarIndexAt(const QPoint &viewportPos) const;
    // Returns which toolbar button (0-2) is under pos for the hovered row, or -1.
    int     toolbarButtonAt(const QPoint &viewportPos) const;
    // Rect of toolbar button i for the given row top/height, in viewport coords.
    QRect   toolbarButtonRect(int btn, int rowTop, int rowH) const;

    // Returns {msgIdx, reactionIdx} of the reaction chip under viewportPos, else {-1,-1}.
    // When a chip is hit and outChipRect is non-null, it receives the chip's viewport rect.
    std::pair<int, int> reactionAt(const QPoint &viewportPos, QRect *outChipRect = nullptr) const;

    // Shows the hover preview (emoji + reactor names) for reaction `ri` on row `mi`.
    void showReactionTooltip(int mi, int ri, const QRect &chipVpRect);

    // ── Animated images (GIF / animated WebP) ──
    // Shared player for a public-URL image, or nullptr while loading / static.
    // First sighting wires frameChanged → viewport repaint (gated on visibility).
    QMovie *gifMovieFor(const QString &url) const;
    void    watchGifMovie(const QString &url, QMovie *movie) const;
    // Create a widget-owned player for an auth-downloaded file when its bytes
    // decode to an animation (public-URL ones are owned by ImageCache).
    void    maybeCreateFileGifMovie(const QString &url, const QByteArray &bytes) const;
    // Swap the current movie frame into the item's doc image resources and mark
    // the url visible; called per visible row right before the docs are drawn.
    // vpRect is the row's viewport rect — the dirty region a frame change of
    // any of the item's animated emoji must repaint.
    void    pullGifFrames(const MessageItem &item, const QRect &vpRect) const;
    // Record an animated url as painted this pass: adds it to _visibleGifs and
    // grows its per-frame dirty rect (_gifRects) by vpRect.
    void    markGifVisible(const QString &url, const QRect &vpRect) const;
    // Start players painted this pass, pause the rest — called after each paint.
    void    syncGifPlayback() const;
    // Release one url's player: a widget-owned movie is deleted together with
    // its _fileImages pixmap (so a reload recreates both from the disk image
    // cache); a cache-owned one is handed back via ImageCache::releaseMovie.
    void    dropGifMovie(const QString &url) const;
    // Release every player — conversation switch (clear) and widget teardown.
    // Reopening recreates them from cached bytes; playback restarts at frame 0.
    void    releaseGifMovies() const;

    // ── Cache bounds (see docs/PERF_AUDIT_2026_07.md §1.3) ──
    // Rows this many viewport heights above/below the visible area keep their
    // rendered docs and file images; farther ones are released and rebuilt on
    // demand (doc rebuild and disk-cache image decode are both cheap).
    static constexpr int    kKeepViewports         = 4;
    static constexpr qint64 kFileImageCapBytes     = 32LL * 1024 * 1024;
    static constexpr qint64 kScaledPreviewCapBytes = 32LL * 1024 * 1024;
    // Purge _fileImages back to the near-viewport working set once its decoded
    // bytes exceed the cap; evicted urls reload from the session's disk image
    // cache when their rows scroll back in. Call after inserting a pixmap.
    void                    enforceFileImageCap() const;
    // Same idea for the display-scaled copies: when over the cap, drop all but
    // the pixmap just produced (keepKey) — the rest re-scale on their next paint.
    void                    enforceScaledPreviewCap(const QString &keepKey) const;
    // Release the rendered QTextDocuments of rows far outside the viewport,
    // keeping their measured heights (docWidth/docHeight) so the layout and
    // scrollbar stay exact. ensureDocLayout rebuilds a released doc on demand.
    void                    trimOffscreenDocs();

    // Text selection
    TextPos textHitTest(const QPoint &viewportPos) const;
    void    clearSelection();
    bool    hasSelection() const;
    QString selectedText() const;

    // Dismiss button for link-preview attachments.
    // Returns {msgIdx, attachIdx} if pos is on a dismiss "×" button, else {-1,-1}.
    std::pair<int, int> dismissButtonAt(const QPoint &viewportPos) const;
    // Returns the viewport rect of dismiss button (msgIdx, attachIdx), or null rect.
    QRect               dismissButtonVpRect(int msgIdx, int attachIdx) const;
    bool                isDismissed(const Ts &ts, int ai) const {
        return _dismissedAttachments.contains(ts + "/" + QString::number(ai));
    }

    // Layout constants (all in logical pixels)
    static constexpr int kPadH            = 16; // horizontal margin on both sides
    static constexpr int kPadV            = 8;  // vertical padding above each full-header row
    static constexpr int kPadVBottom      = 4;  // vertical padding below each full-header row
    static constexpr int kAvSize          = 36; // avatar square size
    static constexpr int kAvGap           = 10; // gap between avatar and text column
    static constexpr int kHdrH            = 20; // height of the name+timestamp header line
    static constexpr int kHdrGap          = 4;  // gap between header and message body
    static constexpr int kRowGap          = 0;  // no gap — spacing is entirely in kPadV
    static constexpr int kPadVCollapsed   = 3;  // vertical padding for collapsed (same-author) rows
    static constexpr int kReactH          = 22; // height of the reactions strip
    static constexpr int kReplyBarH       = 36; // height of the thread-participants bar
    static constexpr int kReplyBarGap     = 6;  // gap above the reply bar
    static constexpr int kThreadAvSize    = 24; // small rounded-square avatar size in reply bar
    static constexpr int kThreadAvGap     = 3;  // gap between consecutive avatars
    // Inline-thread expanded region
    static constexpr int kInlineTopGap    = 8;   // gap between reply bar and first reply
    static constexpr int kInlineLoadingH  = 28;  // height of the "Loading replies…" placeholder
    static constexpr int kInlineFooterGap = 6;   // gap above the "Reply to thread" footer
    static constexpr int kInlineFooterH   = 24;  // height of the "Reply to thread" footer row
    static constexpr int kInlineBottomGap = 6;   // gap below the footer
    static constexpr int kAttachGap       = 4;   // gap above each attachment
    static constexpr int kAttachBarW      = 3;   // width of attachment color bar
    static constexpr int kAttachBarGap    = 8;   // gap between bar and attachment text
    static constexpr int kImgMaxW         = 400; // max inline image width
    static constexpr int kImgMaxH         = 300; // max inline image height
    static constexpr int kImgGap          = 6;   // gap above each inline image
    static constexpr int kImgNameH        = 14;  // height of the filename label above each image
    // Multi-image gallery (2+ inline previews): equal cover-cropped tiles laid out
    // in wrapping rows, like the official Slack client. No per-image filename label.
    static constexpr int kGalleryTileH    = 180; // fixed tile height (the "max height")
    static constexpr int kGalleryGap      = 8;   // gap between gallery tiles (h & v)
    static constexpr int kGalleryMaxW     = 520; // max gallery width (wider than single-image cap)
    static constexpr int kGalleryRadius   = 8;   // rounded-corner radius of gallery tiles
    static constexpr int kFileChipH       = 52;  // height of each non-image file chip
    static constexpr int kFileChipGap   = 6; // gap before each chip (between chips, or above first)
    static constexpr int kFileChipIconW = 48;  // width of the colored type-icon area
    static constexpr int kFileChipMaxW  = 380; // max chip width (won't span full viewport)
    static constexpr int kFileChipPadX  = 12;  // gap between icon right edge and text

    // Hover toolbar
    static constexpr int kToolbarBtnSize = 28; // icon button square size
    static constexpr int kToolbarPadH    = 8;  // horizontal inner padding of toolbar card
    static constexpr int kToolbarPadV    = 6;  // vertical inner padding
    static constexpr int kToolbarGap     = 4;  // gap between buttons
    static constexpr int kToolbarRadius  = 8;  // card corner radius
    static constexpr int kToolbarRight   = 12; // right margin from viewport edge

    // Dismiss button for attachments (link previews)
    static constexpr int kDismissW   = 18; // dismiss button size (square)
    static constexpr int kDismissGap = 4;  // gap between dismiss button and attachment left edge
    static constexpr int kTablePillH = 28; // "Open full table" hover pill height
    // Pill width beyond the label text: side paddings + expand icon + icon gap.
    static constexpr int kTablePillIconPad = 40;

    // Date separator
    static constexpr int kSepH = 32; // total height of date separator band

    // System/activity lines (joins, topic changes, …): centered single line.
    static constexpr int kSysRowPadV = 6; // vertical padding above and below the line

    Session       *_session;
    ConversationId _currentConv;
    bool           _isThreadMode = false;
    Ts             _threadRootTs;

    // ── Inline threads (Appearance → Threads = Inline) ──
    // Loaded replies + load state for one inline-expanded thread.
    struct InlineThread {
        std::vector<MessageItem> replies; // root excluded
        bool                     loading = false;
        bool                     loaded  = false;
        rpl::lifetime            lifetime;
    };
    bool                       _threadsInline = false;
    // Keyed by root ts; presence == expanded. std::map (not QHash) because the
    // value is move-only (holds rpl::lifetime + move-only MessageItems).
    std::map<Ts, InlineThread> _inlineThreads;
    // Root ts currently shown in the standalone panel ({} when none).
    Ts                         _openThreadRoot;
    std::vector<MessageItem>   _items;
    std::vector<int>           _tops; // document-space top of each row
    // ts of each row at the time _tops was built — a consistent snapshot of the
    // previous layout used by rebuildLayout() to re-anchor the view (callers
    // mutate _items before calling it, so indexing into _items would misalign).
    std::vector<Ts>            _topsTs;
    int                        _totalH = 0;

    // Smooth scroll
    QVariantAnimation _scrollAnim;
    void              smoothScrollTo(int target);

    // Public-URL images (avatars, attachment previews, favicons) — owned by the caller,
    // shared across widgets. Emits loaded() when a download completes.
    ImageCache                     *_imgCache = nullptr;
    // Auth-required file image downloads (Slack CDN, private URLs via session token).
    mutable QHash<QString, QPixmap> _fileImages;
    // Preview pixmaps pre-scaled to physical pixels for the current DPR (see scaledPreview).
    mutable QHash<QString, QPixmap> _scaledPreviews;
    // DPR the visible previews were requested for; a change (window moved to a
    // screen with a different density) re-triggers downloads at the new density.
    mutable qreal                   _previewDpr    = 0.0;
    // Bumped on every _fileImages mutation; invalidates the per-item
    // fileImgBaseH memo (a loaded pixmap can change a preview's size).
    mutable quint32                 _fileImagesGen = 0;

    // New-message highlight: ts → elapsed ms since arrival (driven by _highlightTimer)
    QSet<QString>     _newMsgTs;
    QVariantAnimation _highlightAnim;

    // True while a measureLayoutChunk() tick is queued, so we never stack timers.
    bool _progressiveLayoutScheduled = false;

    // Debounced doc trim: restarted on every scroll, fires once scrolling has
    // stopped so rows scrolled far away release their QTextDocuments.
    QTimer _docTrimTimer;

    bool _scrollToBottomPending = false;
    // Read cursor of the conversation being opened: while _scrollToBottomPending
    // is set, the initial scroll targets the first message after this ts instead
    // of the bottom. Empty = no unreads, plain scroll-to-bottom.
    Ts   _pendingLastReadTs;
    // Saved reading position of the conversation being opened (takes precedence
    // over the unread target): message ts + its offset from the viewport top.
    Ts   _pendingAnchorTs;
    int  _pendingAnchorDelta = 0;
    // conv.value → saved reading position. An entry is written for every chat the
    // user leaves, so returning to it always restores where they were — including
    // "at the bottom" (atBottom), which is sticky and overrides the first-unread
    // placement. Survives conversation and workspace switches (clear() leaves it).
    struct SavedAnchor {
        bool atBottom = false; // left at the bottom — restore to the bottom
        Ts   ts;               // otherwise anchor to this message ts...
        int  delta = 0;        // ...at this pixel offset from the viewport top
    };
    QHash<QString, SavedAnchor> _savedAnchors;

    // Text selection state
    TextPos       _selAnchor;              // where the drag started
    TextPos       _selFocus;               // current drag end
    bool          _selDragging    = false; // true while LMB is held and dragging a selection
    // Triple-click detection: Qt delivers the third click as a plain press after the
    // double-click, so we track the last double-click to recognise it.
    unsigned long _lastDblClickTs = 0;
    QPoint        _lastDblClickPos;

    int                 _hoveredRow     = -1; // index of the row the mouse is over, or -1
    int                 _hoveredToolBtn = -1; // 0=emoji, 1=forward, 2=more; -1=none
    // {msgIdx, attachIdx} of the attachment preview the cursor is over, else {-1,-1}
    std::pair<int, int> _hoveredAttach  = {-1, -1};
    // {msgIdx, fileIdx} of the file chip/image the cursor is over, else {-1,-1}
    std::pair<int, int> _hoveredFile    = {-1, -1};
    int                 _hoveredFileBtn = -1;  // 0=download, 1=share, 2=more; -1=none
    QString             _hoveredLinkUrl;       // URL of the link currently under the mouse cursor
    int                 _hoveredLinkRow  = -1; // row index owning that link (-1 if none)
    int                 _hoveredReplyRow = -1; // row index whose reply bar is hovered (-1 if none)
    Ts                  _hoveredThreadFooter;  // root ts whose inline "Reply to thread" is hovered
    std::pair<int, int> _hoveredReaction = {-1, -1}; // {row, reactionIdx} under the mouse
    TableHit            _hoveredTable;               // data table under the mouse (row -1 = none)

    // Header labels shaped once on first paint (a language switch needs an app
    // restart, so the strings are constants for the run). Members, not
    // paint-path statics — those must never hold tr() results.
    mutable QStaticText _stEdited, _stApp, _stExt;
    mutable int         _appBadgeW = 0, _extBadgeW = 0; // horizontalAdvance of APP / EXT

    // Client-side dismissed link previews: key is ts + "/" + attachIndex.
    QSet<QString> _dismissedAttachments;

    // Image blocks the user collapsed via their "GIF ▾" title line.
    // Key: ts [+ "/a" + attachIndex] + "/b" + blockIndex (see GifRenderContext).
    QSet<QString> _collapsedGifs;

    // Animated image players by url — ImageCache-owned for public URLs,
    // widget-owned (parented to this) for auth-downloaded files.
    mutable QHash<QString, QMovie *> _gifMovies;
    // Urls whose frames were drawn during the current/last paint pass.
    mutable QSet<QString>            _visibleGifs;
    // Viewport rect each visible animated url was painted into (rebuilt with
    // _visibleGifs on every full paint) — the frameChanged handler repaints just
    // this region instead of the whole viewport.
    mutable QHash<QString, QRect>    _gifRects;

    PopupTooltip       *_tooltip = nullptr;
    QDeadlineTimer      _tooltipPin; // while running, hover logic leaves the tooltip alone
    EmojiPickerPopup   *_emojiPicker = nullptr;
    ImageViewerOverlay *_imageViewer = nullptr; // lazily created, parented to window()
    TableViewerOverlay *_tableViewer = nullptr; // lazily created, parented to window()

    // Mention hover profile card
    UserProfileCard *_profileCard = nullptr;
    QTimer           _profileShowTimer;         // hover delay before the card appears
    QString          _pendingProfileAnchor;     // mention href waiting on the show timer
    UserId           _pendingProfileAvatarUser; // avatar user waiting on the show timer

    std::optional<QString> _olderCursor; // set when more pages exist above
    bool                   _loadingOlder = false;
    bool                   _loading      = false; // true while waiting for the initial page
    bool                   _waiting      = false; // true while the conv list itself hasn't loaded
    LoadingIndicator       _loadingAnim;
    QElapsedTimer          _loadingElapsedTimer;

    rpl::lifetime _loadLifetime;
    rpl::lifetime _olderLoadLifetime;
    rpl::lifetime _eventLifetime;
};
