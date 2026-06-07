// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"
#include "ui/loading_indicator/loading_indicator.h"
#include "ui/virtual_list/virtual_list_widget.h"

#include <QElapsedTimer>
#include <QVariantAnimation>
#include <QSet>
#include <QHash>
#include <QPixmap>

#include <memory>
#include <vector>

class QTextDocument;
class Session;
class ImageCache;
class PopupTooltip;
class EmojiPickerPopup;

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
        false; // true once attachment image/favicon download triggered
};

// Aggregates the constant viewport geometry computed at the start of every paint/hit-test.
struct PaintContext {
    int vw;        // viewport()->width()
    int scrollY;   // verticalScrollBar()->value()
    int vh;        // viewport()->height()
    int textLeft;  // kPadH + kAvSize + kAvGap
    int textWidth; // vw - textLeft - kPadH
};

// Zero-widget virtual message list.
// Stores MessageItems, paints only visible rows in a single QPainter pass.
// No QLabel/QWidget per message — scales to thousands of rows without stutter.
class MessageListWidget : public VirtualListWidget {
    Q_OBJECT
public:
    explicit MessageListWidget(Session *session, ImageCache *imgCache, QWidget *parent = nullptr);

    void openConversation(
        ConversationId conv, const QString &convName = {}, const QString &description = {}
    );
    // Open a thread view: loads conversations.replies and filters events accordingly.
    void openThread(ConversationId conv, Ts rootTs);
    void clear();
    void setSession(Session *session);

    // Show/hide a full-area loading spinner independent of conversation state.
    // Used while the conversation list itself is loading (before any conv can be opened).
    void setWaiting(bool waiting);

    // Returns the most recent non-system message authored by `me`, or nullopt.
    std::optional<Message> lastOwnMessage(UserId me) const;

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

protected:
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // Viewport event handlers (called from eventFilter)
    void doPaint(QPaintEvent *event);
    void doMousePress(QMouseEvent *event);
    void doMouseMove(QMouseEvent *event);
    void doMouseRelease(QMouseEvent *event);
    void doMouseLeave();

    // doMousePress sub-handlers — each returns true if it consumed the event.
    bool tryHandleScrollbarPress(const QPoint &pos);
    bool tryHandleToolbarPress(const QPoint &pos);
    bool tryHandleReactionPress(const QPoint &pos);
    bool tryHandleDismissPress(const QPoint &pos);
    bool tryHandleReplyBarPress(const QPoint &pos);
    bool tryHandleLinkPress(const QPoint &pos);
    bool tryHandleFileChipPress(const QPoint &pos);

    // Toolbar sub-actions called from tryHandleToolbarPress.
    void         openEmojiPickerForRow(int row, const QPoint &globalPos);
    void         showMessageContextMenu(const Message &msg, const QPoint &globalPos);
    bool         isOnScrollThumb(int vpY) const; // delegates to VirtualListWidget with _totalH
    PaintContext makePaintContext() const;

    // Data model
    void appendMessage(const Message &msg);
    int  findByTs(const Ts &ts) const; // linear scan, fine for <500 visible
    void handleEvent(const Event &e);
    // Merge freshly-loaded network messages into the existing item list.
    void mergeNetworkMessages(const std::vector<Message> &incoming);
    // Apply _pendingRestorePos / _scrollToBottomPending if set.
    void applyPendingScroll();
    // Load older messages using the stored pagination cursor.
    void loadOlderMessages();

    // Layout
    void rebuildLayout();
    int  rowHeight(int index) const;
    bool isCollapsed(int index) const; // true if same author within 5 min of previous
    void ensureDocLayout(const MessageItem &item) const;
    int  textAreaWidth() const;

    // Painting
    void paintRow(QPainter &p, int index, int rowTop, const PaintContext &ctx) const;
    void paintAvatar(QPainter &p, const MessageItem &item, QRect rect) const;
    void
    paintReactions(QPainter &p, const MessageItem &item, const PaintContext &ctx, int top) const;
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
    void paintHoverToolbar(QPainter &p, int index, int rowTop, int rowH) const;
    void paintIntro(QPainter &p, int top) const;
    void paintDateSep(QPainter &p, int top, int vw, const Ts &ts) const;

    int  introHeight() const;
    bool needsDateSep(int index) const;

    // Download any missing images for visible rows (called from doPaint).
    void triggerMissingDownloads();
    // Download any missing user avatars for visible rows (called from doPaint).
    void triggerMissingAvatarDownloads();

    // Mouse: returns the href under the given viewport point, or empty.
    QString anchorAt(const QPoint &viewportPos) const;
    // Attachment height helpers
    int     attachImageH(const Attachment &att
        ) const; // preview image height (includes kImgGap), 0 if none
    int     attachTotalH(const MessageItem &item, int ai) const; // docH + imageH

    // Returns pointer to the non-image File chip under viewportPos, or nullptr.
    const File *fileChipAt(const QPoint &viewportPos) const;
    // Returns index of the message whose reply bar is at viewportPos, or -1.
    int         replyBarIndexAt(const QPoint &viewportPos) const;
    // Returns which toolbar button (0-2) is under pos for the hovered row, or -1.
    int         toolbarButtonAt(const QPoint &viewportPos) const;
    // Rect of toolbar button i for the given row top/height, in viewport coords.
    QRect       toolbarButtonRect(int btn, int rowTop, int rowH) const;

    // Returns {msgIdx, reactionIdx} of the reaction chip under viewportPos, else {-1,-1}.
    std::pair<int, int> reactionAt(const QPoint &viewportPos) const;

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
    static constexpr int kPadV          = 8;   // vertical padding top/bottom of each row
    static constexpr int kAvSize        = 36;  // avatar square size
    static constexpr int kAvGap         = 10;  // gap between avatar and text column
    static constexpr int kHdrH          = 20;  // height of the name+timestamp header line
    static constexpr int kHdrGap        = 4;   // gap between header and message body
    static constexpr int kRowGap        = 0;   // no gap — spacing is entirely in kPadV
    static constexpr int kPadVCollapsed = 3;   // vertical padding for collapsed (same-author) rows
    static constexpr int kReactH        = 22;  // height of the reactions strip
    static constexpr int kReplyBarH     = 36;  // height of the thread-participants bar
    static constexpr int kReplyBarGap   = 6;   // gap above the reply bar
    static constexpr int kThreadAvSize  = 22;  // small circular avatar size in reply bar
    static constexpr int kThreadAvOver  = 6;   // overlap between consecutive avatars
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
    int                      _totalH = 0;

    // Smooth scroll
    QVariantAnimation _scrollAnim;
    void              smoothScrollTo(int target);

    // Public-URL images (avatars, attachment previews, favicons) — owned by the caller,
    // shared across widgets. Emits loaded() when a download completes.
    ImageCache                     *_imgCache = nullptr;
    // Auth-required file image downloads (Slack CDN, private URLs via session token).
    mutable QHash<QString, QPixmap> _fileImages;

    // New-message highlight: ts → elapsed ms since arrival (driven by _highlightTimer)
    QSet<QString>     _newMsgTs;
    QVariantAnimation _highlightAnim;

    bool                _scrollToBottomPending = false;
    int                 _pendingRestorePos     = -1; // >= 0: restore this position after layout
    QHash<QString, int> _savedScrollPos;             // conv.value → last scroll position

    int                 _hoveredRow     = -1; // index of the row the mouse is over, or -1
    int                 _hoveredToolBtn = -1; // 0=emoji, 1=forward, 2=more; -1=none
    // {msgIdx, attachIdx} of the attachment preview the cursor is over, else {-1,-1}
    std::pair<int, int> _hoveredAttach  = {-1, -1};
    QString             _hoveredLinkUrl;       // URL of the link currently under the mouse cursor
    int                 _hoveredLinkRow  = -1; // row index owning that link (-1 if none)
    int                 _hoveredReplyRow = -1; // row index whose reply bar is hovered (-1 if none)

    // Client-side dismissed link previews: key is ts + "/" + attachIndex.
    QSet<QString> _dismissedAttachments;

    PopupTooltip     *_tooltip     = nullptr;
    EmojiPickerPopup *_emojiPicker = nullptr;

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
