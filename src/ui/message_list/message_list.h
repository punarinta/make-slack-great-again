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
#include <QPixmap>
#include <QStringList>
#include <QTextDocument> // structs below hold unique_ptr<QTextDocument>; need the
                         // complete type so their (implicit) destructors are
                         // instantiable wherever this header is consumed.
#include <QTimer>

#include <memory>
#include <vector>

class QMovie;
class Session;
class ImageCache;
class PopupTooltip;
class EmojiPickerPopup;
class ImageViewerOverlay;
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

    // lastReadTs: the conversation's read cursor captured before the open marks
    // it read — when set, the list opens scrolled to the first message after it
    // (the first unread); when empty, it opens scrolled to the bottom.
    void openConversation(
        ConversationId conv,
        const QString &convName    = {},
        const QString &description = {},
        const Ts      &lastReadTs  = {}
    );
    // Update only the display name / description shown in the intro header without
    // reloading history (used when user data arrives after the conversation was opened).
    void updateConvName(const QString &convName, const QString &description);
    // Open a thread view: loads conversations.replies and filters events accordingly.
    void openThread(ConversationId conv, Ts rootTs);
    void clear();
    void setSession(Session *session);

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

signals:
    // Fired once when the first page of content for the current conversation is
    // ready to display — immediately if loaded from cache, otherwise when the
    // first network response arrives.
    void initialPageLoaded();
    // Emitted in channel mode when user clicks the "N replies" bar on a thread root.
    void threadClicked(ConversationId conv, Ts rootTs);
    // Emitted when "Edit message" is chosen; caller should call composer->enterEditMode().
    void editMessageRequested(Ts ts, QString rawText, std::vector<File> files);
    // Emitted when "Forward message" is chosen.
    void forwardMessageRequested(Message msg);
    // Emitted when "Message" is clicked on the mention-hover profile card.
    void openDmRequested(UserId user);

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
    bool tryHandleLinkPress(const QPoint &pos);
    bool tryHandleFileActionBarPress(const QPoint &pos);
    bool tryHandlePreviewPress(const QPoint &pos);
    bool tryHandleFileChipPress(const QPoint &pos);

    // Toolbar sub-actions called from tryHandleToolbarPress.
    void         openEmojiPickerForRow(int row, const QPoint &globalPos);
    void         showMessageContextMenu(const Message &msg, const QPoint &globalPos);
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
    void               appendMessage(const Message &msg);
    // Append without a relayout/repaint — for bulk loads that rebuild once at
    // the end (a per-message rebuildLayout is O(items), so a loop is O(n²)).
    void               appendMessageDeferred(const Message &msg);
    // Append every message then rebuild layout once. Used by the cache
    // pre-populate and first-page loads.
    void               appendMessages(const std::vector<Message> &msgs);
    int                findByTs(const Ts &ts) const; // linear scan, fine for <500 visible
    void               handleEvent(const Event &e);
    // Merge freshly-loaded network messages into the existing item list.
    void               mergeNetworkMessages(const std::vector<Message> &incoming);
    // Apply the pending open-scroll intent (saved position / first unread /
    // bottom) if set.
    void               applyPendingScroll();
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
    bool isCollapsed(int index) const; // true if same author within 5 min of previous
    void ensureDocLayout(const MessageItem &item) const;
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
    void paintReactions(
        QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
    ) const;
    void paintReplyBar(
        QPainter &p, const MessageItem &item, const PaintContext &ctx, int top, int index
    ) const;
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
    void  paintIntro(QPainter &p, int top) const;
    void  paintDateSep(QPainter &p, int top, int vw, qint64 dateMicros) const;

    int  introHeight() const;
    bool needsDateSep(int index) const;

    // Download any missing images for visible rows (called from doPaint).
    void triggerMissingDownloads();
    // Download any missing user avatars for visible rows (called from doPaint).
    void triggerMissingAvatarDownloads();

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
    int     attachImageH(const Attachment &att
        ) const; // preview image height (includes kImgGap), 0 if none
    int     attachTotalH(const MessageItem &item, int ai) const; // docH + imageH

    // Returns pointer to the non-image File chip under viewportPos, or nullptr.
    const File *fileChipAt(const QPoint &viewportPos) const;
    // Returns the hovered file whose inline preview (image or PDF first page)
    // is under viewportPos, or nullptr.
    const File *previewFileAt(const QPoint &viewportPos) const;
    // Displayed size of a file's inline preview: known file dimensions scaled to
    // fit, else the loaded pixmap scaled to fit, else maxW × 24.
    // Single source of truth for rowHeight / paint / hit-test geometry.
    QSize       filePreviewSize(const File &f, int maxW) const;
    // Preview source URLs matching this widget's screen density (DPR-aware).
    QString     filePreviewUrl(const File &f) const;
    QString     attachPreviewUrl(const Attachment &att) const;
    // Returns `src` smooth-scaled to `logical` × dpr physical pixels (with the
    // DPR set on the result), cached per key so paints don't rescale every frame.
    QPixmap scaledPreview(const QString &key, const QPixmap &src, QSize logical, qreal dpr) const;
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
    void    pullGifFrames(const MessageItem &item) const;
    // Start players painted this pass, pause the rest — called after each paint.
    void    syncGifPlayback() const;

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
    static constexpr int kPadH          = 16;  // horizontal margin on both sides
    static constexpr int kPadV          = 8;   // vertical padding above each full-header row
    static constexpr int kPadVBottom    = 4;   // vertical padding below each full-header row
    static constexpr int kAvSize        = 36;  // avatar square size
    static constexpr int kAvGap         = 10;  // gap between avatar and text column
    static constexpr int kHdrH          = 20;  // height of the name+timestamp header line
    static constexpr int kHdrGap        = 4;   // gap between header and message body
    static constexpr int kRowGap        = 0;   // no gap — spacing is entirely in kPadV
    static constexpr int kPadVCollapsed = 3;   // vertical padding for collapsed (same-author) rows
    static constexpr int kReactH        = 22;  // height of the reactions strip
    static constexpr int kReplyBarH     = 36;  // height of the thread-participants bar
    static constexpr int kReplyBarGap   = 6;   // gap above the reply bar
    static constexpr int kThreadAvSize  = 24;  // small rounded-square avatar size in reply bar
    static constexpr int kThreadAvGap   = 3;   // gap between consecutive avatars
    static constexpr int kAttachGap     = 4;   // gap above each attachment
    static constexpr int kAttachBarW    = 3;   // width of attachment color bar
    static constexpr int kAttachBarGap  = 8;   // gap between bar and attachment text
    static constexpr int kImgMaxW       = 400; // max inline image width
    static constexpr int kImgMaxH       = 300; // max inline image height
    static constexpr int kImgGap        = 6;   // gap above each inline image
    static constexpr int kImgNameH      = 14;  // height of the filename label above each image
    static constexpr int kFileChipH     = 52;  // height of each non-image file chip
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

    // Date separator
    static constexpr int kSepH = 32; // total height of date separator band

    // System/activity lines (joins, topic changes, …): centered single line.
    static constexpr int kSysRowPadV = 6; // vertical padding above and below the line

    // Conversation intro header (painted before first message)
    static constexpr int kIntroPadTop = 32; // space above the name line
    static constexpr int kIntroNameH  = 30; // height of the big name line
    static constexpr int kIntroGap    = 8;  // gap between name and description
    static constexpr int kIntroDescH  = 18; // height of description line
    static constexpr int kIntroPadBot = 24; // space below description before first message

    Session                 *_session;
    ConversationId           _currentConv;
    bool                     _isThreadMode = false;
    Ts                       _threadRootTs;
    QString                  _convName;
    QString                  _convDescription;
    bool                     _showIntro = false;
    std::vector<MessageItem> _items;
    std::vector<int>         _tops; // document-space top of each row
    // ts of each row at the time _tops was built — a consistent snapshot of the
    // previous layout used by rebuildLayout() to re-anchor the view (callers
    // mutate _items before calling it, so indexing into _items would misalign).
    std::vector<Ts>          _topsTs;
    int                      _totalH = 0;

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
    mutable qreal                   _previewDpr = 0.0;

    // New-message highlight: ts → elapsed ms since arrival (driven by _highlightTimer)
    QSet<QString>     _newMsgTs;
    QVariantAnimation _highlightAnim;

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
    std::pair<int, int> _hoveredReaction = {-1, -1}; // {row, reactionIdx} under the mouse

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

    PopupTooltip       *_tooltip = nullptr;
    QDeadlineTimer      _tooltipPin; // while running, hover logic leaves the tooltip alone
    EmojiPickerPopup   *_emojiPicker = nullptr;
    ImageViewerOverlay *_imageViewer = nullptr; // lazily created, parented to window()

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
