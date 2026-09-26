// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "threads_page.h"
#include "backend/backend.h"
#include "session/session.h"
#include "ui/composer/composer_widget.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/message_list/message_render.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

using OverviewCard::kAvatarSize;
using OverviewCard::kTextLeft;

// ── ThreadMsgRow ──────────────────────────────────────────────────────────────
// One message inside a thread card: avatar + name + time header over the rich
// message body (a QTextDocument built with the shared MsgRender pipeline, so
// mrkdwn, mentions, emoji and code blocks render exactly like the chat).
// Clicking the row (outside a link) opens the real thread.
class ThreadMsgRow : public QWidget {
    Q_OBJECT
public:
    ThreadMsgRow(
        Message               msg,
        Session              *session,
        ImageCache           *imgCache,
        std::function<void()> onClicked,
        QWidget              *parent
    )
        : QWidget(parent), _msg(std::move(msg)), _session(session), _imgCache(imgCache),
          _onClicked(std::move(onClicked)) {
        setCursor(Qt::PointingHandCursor); // the whole row opens the thread
        QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);

        _avatarUrl = OverviewCard::avatarUrl(_session, _msg.author, _msg.botAvatarUrl);
        if (_imgCache) {
            if (!_avatarUrl.isEmpty())
                _imgCache->get(_avatarUrl); // kick off the download
            connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
                if (url == _avatarUrl) {
                    update();
                } else if (_emojiUrls.contains(url)) {
                    // Re-register the freshly downloaded emoji as a doc resource.
                    invalidateDoc();
                }
            });
        }
    }

    const Ts      &ts() const { return _msg.ts; }
    const Message &message() const { return _msg; }

    // Theme switch / emoji arrival: the doc bakes theme colors and image
    // resources, so it must be rebuilt from scratch.
    void invalidateDoc() {
        _doc.reset();
        _docWidth = -1;
        updateGeometry();
        update();
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int w) const override {
        ensureDoc(w);
        const QFontMetrics fm(nameFont());
        int                h = padV() + fm.height() + hdrGap() + _docHeight;
        if (!_msg.files.empty())
            h += QFontMetrics(QApplication::font()).height();
        return std::max(h, padV() + kAvatarSize) + padV();
    }

    QSize sizeHint() const override {
        const int w = width() > 0 ? width() : 400;
        return {w, heightForWidth(w)};
    }
    QSize minimumSizeHint() const override { return {kAvatarSize * 3, kAvatarSize}; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        ensureDoc(width());

        OverviewCard::paintRowHeader(
            p,
            width(),
            devicePixelRatioF(),
            _imgCache,
            _avatarUrl,
            displayName(),
            MsgRender::dateTimeLabel(_msg.date)
        );

        // Body document.
        if (_doc) {
            p.save();
            p.translate(textLeft(), docTop());
            MsgRender::paintCodeBlockChrome(p, _doc.get());
            MsgRender::paintDataTableChrome(p, _doc.get());
            MsgRender::paintBotButtonChrome(p, _doc.get());
            QAbstractTextDocumentLayout::PaintContext pCtx;
            pCtx.palette = QApplication::palette();
            // Base text color must come from the theme, not the app palette.
            pCtx.palette.setColor(QPalette::Text, Th::c().text.primary);
            pCtx.clip = QRectF(0, 0, docWidth(), _docHeight);
            _doc->documentLayout()->draw(&p, pCtx);
            p.restore();
        }

        // Attached files: a one-line summary (the full chips live in the thread).
        if (!_msg.files.empty()) {
            const QFont        af = QApplication::font();
            const QFontMetrics afm(af);
            const int          lineY = docTop() + _docHeight;
            const QPixmap      clip  = svgPixmapPhys(
                QStringLiteral(":/ui/paperclip.svg"),
                QSize(12, 12),
                Th::c().text.secondary,
                devicePixelRatioF()
            );
            p.drawPixmap(textLeft(), lineY + (afm.height() - 12) / 2, clip);
            p.setFont(af);
            p.setPen(Th::c().text.secondary);
            const QString label = _msg.files.size() == 1 ? _msg.files.front().name
                                                         : tr("%1 files").arg(_msg.files.size());
            p.drawText(textLeft() + 12 + Th::c().spacing.sm, lineY + afm.ascent(), label);
        }
    }

    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        ensureDoc(width());
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;
        const QString href = anchorAt(e->pos());
        if (href.startsWith(QLatin1String("http://")) ||
            href.startsWith(QLatin1String("https://"))) {
            QDesktopServices::openUrl(QUrl(href));
            return;
        }
        // Anything else (plain text, mentions, internal anchors): open the thread.
        if (_onClicked)
            _onClicked();
    }

private:
    static QFont nameFont() { return OverviewCard::nameFont(); }
    static int   padV() { return OverviewCard::rowPadV(); }
    static int   hdrGap() { return OverviewCard::rowHdrGap(); }
    static int   textLeft() { return kTextLeft; }
    int          docWidth() const { return std::max(50, width() - textLeft()); }
    int          docTop() const { return padV() + QFontMetrics(nameFont()).height() + hdrGap(); }

    QString displayName() const {
        if (!_msg.botName.isEmpty())
            return _msg.botName;
        return _session ? _session->userDisplayName(_msg.author) : _msg.author.value;
    }

    QString anchorAt(const QPoint &pos) const {
        if (!_doc)
            return {};
        const QPointF docPos(pos.x() - textLeft(), pos.y() - docTop());
        if (docPos.y() < 0)
            return {};
        return _doc->documentLayout()->anchorAt(docPos);
    }

    void ensureDoc(int forWidth) const {
        const int w = std::max(50, forWidth - textLeft());
        if (_doc && _docWidth == w)
            return;
        if (!_doc) {
            _doc = std::make_unique<QTextDocument>();
            _doc->setDefaultFont(QApplication::font());
            _doc->setDocumentMargin(0);
            _doc->setDefaultStyleSheet(MsgRender::docStyleSheet());
            if (!_emojiCollected) {
                _emojiUrls      = MsgRender::collectEmojiImageUrls(_msg, _session);
                _emojiCollected = true;
            }
            if (MsgRender::hasMessageLink(_msg))
                MsgRender::registerMessageLinkIcon(_doc.get(), devicePixelRatioF());
            if (_imgCache) {
                for (const auto &url : _emojiUrls) {
                    const QPixmap px = _imgCache->get(url);
                    if (px.isNull())
                        continue;
                    // Qt doesn't entity-decode &amp; inside an img src, so the
                    // resource must also be registered under the escaped key.
                    _doc->addResource(QTextDocument::ImageResource, QUrl(url), px);
                    const QString escaped = url.toHtmlEscaped();
                    if (escaped != url)
                        _doc->addResource(QTextDocument::ImageResource, QUrl(escaped), px);
                }
            }
            _doc->setHtml(MsgRender::buildMsgHtml(_msg, _session));
        }
        _doc->setTextWidth(w);
        _docWidth  = w;
        _docHeight = int(std::ceil(_doc->size().height()));
    }

    Message               _msg;
    Session              *_session  = nullptr;
    ImageCache           *_imgCache = nullptr;
    std::function<void()> _onClicked;
    QString               _avatarUrl;

    mutable QStringList                    _emojiUrls;
    mutable bool                           _emojiCollected = false;
    mutable std::unique_ptr<QTextDocument> _doc;
    mutable int                            _docWidth  = -1;
    mutable int                            _docHeight = 0;
};

// ── ThreadCard ────────────────────────────────────────────────────────────────
// One subscribed thread: channel header, root message, "Show N more replies",
// the latest replies, and an inline reply box (a real ComposerWidget, created
// lazily on first use — one per card up-front would be needlessly heavy).
class ThreadCard : public QWidget {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<void(ConversationId, Ts)> openThread;
        std::function<void(ConversationId)>     openChannel;
    };

    ThreadCard(
        ThreadOverview item, Session *session, ImageCache *imgCache, Callbacks cbs, QWidget *parent
    )
        : QWidget(parent), _item(std::move(item)), _session(session), _imgCache(imgCache),
          _cbs(std::move(cbs)) {
        // Like the official client: the channel header (name over a
        // participants line) sits OUTSIDE the white card, directly on the
        // page's grey background; only the messages and the reply box live
        // inside the bordered body, which spans the full content width.
        const auto &sp    = Th::c().spacing;
        auto       *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(sp.lg);

        // ── Header: "# channel" + participants + "new" pill ────────────
        auto *headerCol = new QVBoxLayout();
        headerCol->setContentsMargins(0, 0, 0, 0);
        headerCol->setSpacing(sp.xs);
        auto *nameRow = OverviewCard::makeChannelHeader(
            this,
            OverviewCard::conversationLabel(_session, _item.conv),
            [this] {
                if (_cbs.openChannel)
                    _cbs.openChannel(_item.conv);
            },
            &_chanIcon,
            &_chanBtn
        );
        _newPill = new QLabel(tr("New"), this);
        _newPill->setVisible(hasUnread());
        nameRow->addWidget(_newPill);
        headerCol->addLayout(nameRow);
        _participants = new QLabel(participantsLabel(), this);
        headerCol->addWidget(_participants);
        outer->addLayout(headerCol);

        // ── White card body ────────────────────────────────────────────
        _body = new QWidget(this);
        _body->setObjectName("threadCardBody");
        _body->setAttribute(Qt::WA_StyledBackground);
        _bodyLayout = new QVBoxLayout(_body);
        _bodyLayout->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
        _bodyLayout->setSpacing(sp.sm);
        outer->addWidget(_body);

        const auto openThread = [this] {
            markRead();
            if (_cbs.openThread)
                _cbs.openThread(_item.conv, _item.root.ts);
        };

        // ── Root message ───────────────────────────────────────────────
        _bodyLayout->addWidget(
            new ThreadMsgRow(_item.root, _session, _imgCache, openThread, _body)
        );

        // ── "Show N more replies" (the rest lives in the thread panel) ─
        const int hidden = _item.root.replyCount - int(_item.latestReplies.size());
        if (hidden > 0) {
            _moreReplies = new StyledButton(
                hidden == 1 ? tr("Show 1 more reply") : tr("Show %1 more replies").arg(hidden),
                StyledButton::Variant::Link,
                _body
            );
            connect(_moreReplies, &QPushButton::clicked, this, openThread);
            _bodyLayout->addWidget(_moreReplies, 0, Qt::AlignLeft);
        }

        // ── Latest replies ─────────────────────────────────────────────
        for (const auto &m : _item.latestReplies)
            addReplyRow(m, openThread);
        if (_item.root.latestReply)
            _latestTs = *_item.root.latestReply;

        // ── Reply affordance ───────────────────────────────────────────
        _replyBtn =
            new StyledButton(tr("Reply in thread…"), StyledButton::Variant::Secondary, _body);
        _replyBtn->setSize(StyledButton::Size::Small);
        connect(_replyBtn, &QPushButton::clicked, this, [this] { showComposer(); });
        _bodyLayout->addWidget(_replyBtn, 0, Qt::AlignLeft);

        applyTheme();
    }

    const ConversationId &conv() const { return _item.conv; }
    const Ts             &rootTs() const { return _item.root.ts; }

    // Live reply (realtime event or our own optimistic send).
    void addReply(const Message &msg) {
        for (auto *row : _rows)
            if (row->ts() == msg.ts)
                return; // Session dedups, but be safe
        _item.root.replyCount++;
        if (_latestTs.isEmpty() || msg.date >= decimalTsToMicros(_latestTs))
            _latestTs = msg.ts;
        addReplyRow(msg, [this] {
            markRead();
            if (_cbs.openThread)
                _cbs.openThread(_item.conv, _item.root.ts);
        });
    }

    // An optimistic copy was confirmed (EvMessageDeleted with its fake ts) or a
    // reply was deleted for real.
    void removeReply(const Ts &ts) {
        for (auto it = _rows.begin(); it != _rows.end(); ++it) {
            if ((*it)->ts() != ts)
                continue;
            (*it)->deleteLater();
            _rows.erase(it);
            _item.root.replyCount = std::max(0, _item.root.replyCount - 1);
            return;
        }
    }

    void applyTheme() {
        const auto &th = Th::c();
        // MainWindow cascades "QWidget { background: content }" over the whole
        // right panel, so every header widget here must pin an explicit
        // transparent background or it paints a white chip on the grey page.
        OverviewCard::styleCardBody(_body);
        OverviewCard::styleChannelHeader(
            _chanIcon,
            isPrivateChannel() ? QStringLiteral(":/ui/lock.svg") : QStringLiteral(":/ui/hash.svg"),
            _chanBtn
        );
        _participants->setStyleSheet(QString("background: transparent; color: %1; font-size: %2px;")
                                         .arg(Th::qss(th.text.secondary))
                                         .arg(th.fonts.caption));
        _newPill->setStyleSheet(
            QString(
                "background: %1; color: %2; border-radius: 8px; padding: 1px 8px; "
                "font-size: %3px; font-weight: 600;"
            )
                .arg(Th::qss(th.badge.mention), Th::qss(th.accent.text))
                .arg(th.fonts.sm)
        );
        // Covers the root row too (it's a child but not in _rows).
        for (auto *row : findChildren<ThreadMsgRow *>())
            row->invalidateDoc();
    }

private:
    bool isPrivateChannel() const {
        const Conversation *c = _session ? _session->findConversation(_item.conv) : nullptr;
        return c && c->kind == ConvKind::PrivateChannel;
    }

    bool hasUnread() const {
        if (!_item.root.latestReply)
            return false;
        const qint64 lastRead = _item.lastRead.isEmpty() ? 0 : decimalTsToMicros(_item.lastRead);
        return decimalTsToMicros(*_item.root.latestReply) > lastRead;
    }

    // "axelvonsydow and you" / "Adam, Patryk and 3 others" — thread
    // participants under the channel name, official-client style. Root author
    // first, then reply authors; the authed user renders as "you", last.
    QString participantsLabel() const {
        std::vector<UserId> ids;
        ids.push_back(_item.root.author);
        for (const auto &u : _item.root.replyUsers)
            if (std::find(ids.begin(), ids.end(), u) == ids.end())
                ids.push_back(u);
        const UserId me         = _session ? _session->meUserId() : UserId{};
        bool         includesMe = false;
        QStringList  names;
        for (const auto &id : ids) {
            if (!me.value.isEmpty() && id == me) {
                includesMe = true;
                continue;
            }
            if (id.value.isEmpty())
                continue;
            const QString name = _session ? _session->userDisplayName(id) : id.value;
            if (!name.isEmpty())
                names << name;
        }
        // Bot-authored roots have no author id — show the bot's name.
        if (names.isEmpty() && !includesMe && !_item.root.botName.isEmpty())
            names << _item.root.botName;
        constexpr int kMaxNames = 3;
        const int     extra     = int(names.size()) - kMaxNames;
        if (extra > 0) {
            names = names.mid(0, kMaxNames);
            return tr("%1 and %2 others").arg(names.join(QStringLiteral(", "))).arg(extra);
        }
        if (includesMe)
            return names.isEmpty() ? tr("you")
                                   : tr("%1 and you").arg(names.join(QStringLiteral(", ")));
        return names.join(QStringLiteral(", "));
    }

    void markRead() {
        if (!_session)
            return;
        const Ts upTo = _latestTs.isEmpty() ? _item.root.ts : _latestTs;
        _session->markThreadRead(_item.conv, _item.root.ts, upTo);
        _item.lastRead = upTo;
        _newPill->hide();
    }

    void addReplyRow(const Message &msg, std::function<void()> onClicked) {
        auto    *row = new ThreadMsgRow(msg, _session, _imgCache, std::move(onClicked), _body);
        // Replies go right above the reply affordance (button or composer).
        QWidget *anchor =
            _composer ? static_cast<QWidget *>(_composer) : static_cast<QWidget *>(_replyBtn);
        const int idx = anchor ? _bodyLayout->indexOf(anchor) : _bodyLayout->count();
        _bodyLayout->insertWidget(idx < 0 ? _bodyLayout->count() : idx, row);
        _rows.push_back(row);
    }

    void showComposer() {
        if (_composer) {
            _composer->focusInput();
            return;
        }
        _composer = new ComposerWidget(_body);
        _composer->setThreadMode(true);
        // Align the box with the message avatars — the card padding already
        // provides the gutter the chat footer's margins normally would.
        _composer->setFlushHorizontalMargins();
        _composer->setImageCache(_imgCache);
        _composer->setSession(_session);
        _composer->setEnabled(true);
        const Conversation *c = _session ? _session->findConversation(_item.conv) : nullptr;
        _composer->setConvKind(c ? c->kind : ConvKind::PublicChannel);
        _composer->setScheduleVisible(false);
        _composer->setPlaceholderText(tr("Reply in thread…"));
        connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &text) {
            if (!_session)
                return;
            const Ts ghost = _session->sendMessage(_item.conv, text, _item.root.ts);
            _composer->offerUndoSend(_item.conv, ghost);
            markRead();
        });
        connect(
            _composer,
            &ComposerWidget::uploadRequested,
            this,
            [this](const QStringList &filePaths, const QString &text) {
                if (!_session)
                    return;
                const Ts ghost = _session->uploadFiles(_item.conv, filePaths, text, _item.root.ts);
                _composer->offerUndoSend(_item.conv, ghost);
            }
        );
        connect(_composer, &ComposerWidget::typingStarted, this, [this] {
            if (_session)
                _session->sendTyping(_item.conv);
        });
        const int idx = _bodyLayout->indexOf(_replyBtn);
        _bodyLayout->insertWidget(idx < 0 ? _bodyLayout->count() : idx, _composer);
        _replyBtn->hide();
        _composer->focusInput();
    }

    ThreadOverview _item;
    Session       *_session  = nullptr;
    ImageCache    *_imgCache = nullptr;
    Callbacks      _cbs;
    Ts             _latestTs;

    QWidget        *_body         = nullptr; // white bordered card holding the rows
    QVBoxLayout    *_bodyLayout   = nullptr;
    QLabel         *_chanIcon     = nullptr;
    QPushButton    *_chanBtn      = nullptr;
    QLabel         *_participants = nullptr;
    QLabel         *_newPill      = nullptr;
    StyledButton   *_moreReplies  = nullptr;
    StyledButton   *_replyBtn     = nullptr;
    ComposerWidget *_composer     = nullptr;

    std::vector<ThreadMsgRow *> _rows; // reply rows only (root row excluded)
};

// ── ThreadsPage ───────────────────────────────────────────────────────────────

ThreadsPage::ThreadsPage(ImageCache *imgCache, QWidget *parent)
    : QWidget(parent), _imgCache(imgCache) {
    _page = OverviewCard::buildPage(this, QStringLiteral("threads"), tr("Threads"));

    _moreBtn = new StyledButton(tr("Show more threads"), StyledButton::Variant::Secondary, this);
    _moreBtn->setSize(StyledButton::Size::Small);
    _moreBtn->hide();
    connect(_moreBtn, &QPushButton::clicked, this, [this] { loadPage(_nextCursor); });
    // Below the status label, above the stretch.
    _page.listLayout->insertWidget(_page.listLayout->count() - 1, _moreBtn, 0, Qt::AlignLeft);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void ThreadsPage::setSession(Session *session) {
    if (_session == session)
        return;
    _session        = session;
    _eventsLifetime = rpl::lifetime();
    clear();
    if (!_session)
        return;
    // Keep visible cards live: replies arriving while the overview is open
    // (including our own optimistic sends, and the EvMessageDeleted that
    // retires an optimistic copy once the server confirms it).
    _session->events() |
        rpl::on_next(
            [this](Event e) {
                if (const auto *ev = std::get_if<EvMessageNew>(&e)) {
                    if (!ev->msg.threadRoot)
                        return;
                    for (auto *card : _cards) {
                        if (card->conv() == ev->conv && card->rootTs() == *ev->msg.threadRoot) {
                            card->addReply(ev->msg);
                            break;
                        }
                    }
                } else if (const auto *ev = std::get_if<EvMessageDeleted>(&e)) {
                    for (auto *card : _cards)
                        if (card->conv() == ev->conv)
                            card->removeReply(ev->ts);
                }
            },
            _eventsLifetime
        );
}

void ThreadsPage::open() {
    loadPage({});
}

void ThreadsPage::clear() {
    _loadLifetime = rpl::lifetime();
    _loading      = false;
    _hasMore      = false;
    _nextCursor.clear();
    for (auto *card : _cards)
        card->deleteLater();
    _cards.clear();
    _moreBtn->hide();
    setStatus({});
}

void ThreadsPage::loadPage(const QString &cursor) {
    if (!_session || _loading)
        return;
    _loading = true;
    _moreBtn->setEnabled(false);
    if (cursor.isEmpty())
        setStatus(tr("Loading threads…"));

    _loadLifetime = rpl::lifetime();
    auto got      = std::make_shared<bool>(false);
    _session->backend()->loadThreadsView(cursor) |
        rpl::on_next_done(
            [this, cursor, got](ThreadsViewPage page) {
                *got     = true;
                _loading = false;
                if (cursor.isEmpty()) {
                    for (auto *card : _cards)
                        card->deleteLater();
                    _cards.clear();
                }
                ThreadCard::Callbacks cbs;
                cbs.openThread = [this](ConversationId conv, Ts root) {
                    emit openThreadRequested(conv, root);
                };
                cbs.openChannel = [this](ConversationId conv) { emit openChannelRequested(conv); };
                for (const auto &t : page.threads) {
                    auto *card = new ThreadCard(t, _session, _imgCache, cbs, _page.listHost);
                    // Above the "Show more" button (which sits above the stretch).
                    _page.listLayout->insertWidget(_page.listLayout->indexOf(_moreBtn), card);
                    _cards.push_back(card);
                }
                _hasMore    = page.hasMore;
                _nextCursor = page.nextCursor;
                _moreBtn->setVisible(_hasMore);
                _moreBtn->setEnabled(true);
                setStatus(
                    _cards.empty() ? tr("Threads you're following will appear here.") : QString()
                );
            },
            [this, cursor, got] {
                if (*got)
                    return;
                // Completed without a value: unavailable or a failed request.
                _loading = false;
                _moreBtn->setEnabled(true);
                if (_cards.empty())
                    setStatus(tr("Couldn't load threads. Try again later."));
            },
            _loadLifetime
        );
}

void ThreadsPage::setStatus(const QString &text) {
    _page.statusLabel->setText(text);
    _page.statusLabel->setVisible(!text.isEmpty());
}

void ThreadsPage::applyTheme() {
    OverviewCard::stylePage(this, _page);
    for (auto *card : _cards)
        card->applyTheme();
}

#include "threads_page.moc"
