// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_list_widget.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/user_avatar.h"
#include "ui/message_list/message_render.h"
#include "util/emoji.h"
#include "util/emoji_font.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QFontMetrics>
#include <QApplication>
#include <QDateTime>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include "ui/context_menu/context_menu.h"
#include "ui/popup_tooltip/popup_tooltip.h"

ConvListWidget::ConvListWidget(ImageCache *imgCache, QWidget *parent)
    : VirtualListWidget(parent), _imgCache(imgCache) {
    loadVisitedAt();
    viewport()->setCursor(Qt::PointingHandCursor);
    _tooltip = new PopupTooltip(this);

    _selAnim.setDuration(140);
    _selAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&_selAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        _selT = v.toDouble();
        viewport()->update();
    });
    connect(&_selAnim, &QVariantAnimation::finished, this, [this] {
        _selT    = 1.0;
        _selFrom = -1;
    });

    if (_imgCache) {
        connect(_imgCache, &ImageCache::loaded, this, [this] { viewport()->update(); });
    }
    rebuildIconPixmaps();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        rebuildIconPixmaps();
        viewport()->update();
    });
}

void ConvListWidget::rebuildIconPixmaps() {
    const auto &th  = Th::c();
    const QSize big = QSize(kIconSize, kIconSize);
    const QSize sm  = QSize(14, 14);

    _iconPx.chevDown   = svgPixmap(":/ui/chevron-down.svg", big, th.text.onDarkDim);
    _iconPx.chevRight  = svgPixmap(":/ui/chevron-right.svg", big, th.text.onDarkDim);
    _iconPx.hash       = svgPixmap(":/ui/hash.svg", big, th.text.onDarkDim);
    _iconPx.msg        = svgPixmap(":/ui/messages-square.svg", big, th.text.onDarkDim);
    _iconPx.bot        = svgPixmap(":/ui/bot.svg", big, th.text.onDarkDim);
    _iconPx.plusDim    = svgPixmap(":/ui/plus.svg", big, th.text.onDarkDim);
    _iconPx.plusBright = svgPixmap(":/ui/plus.svg", big, th.text.onDark);

    _iconPx.lockDim        = svgPixmap(":/ui/lock.svg", sm, th.text.onDarkDim);
    _iconPx.lockBright     = svgPixmap(":/ui/lock.svg", sm, th.text.onDark);
    _iconPx.lockSelected   = svgPixmap(":/ui/lock.svg", sm, th.nav.primary);
    _iconPx.hashSmDim      = svgPixmap(":/ui/hash.svg", sm, th.text.onDarkDim);
    _iconPx.hashSmBright   = svgPixmap(":/ui/hash.svg", sm, th.text.onDark);
    _iconPx.hashSmSelected = svgPixmap(":/ui/hash.svg", sm, th.nav.primary);
}

void ConvListWidget::setRelevantDays(int days) {
    _relevantDays = std::max(1, days);
    rebuildRows();
}

void ConvListWidget::resetVisitedAt() {
    _visitedAt.clear();
    // rebuildRows() will auto-seed from current API data (latestTs / unread),
    // giving a clean first-launch experience without restarting.
    rebuildRows();
}

void ConvListWidget::loadVisitedAt() {
    const QByteArray  raw = QSettings("msga", "msga").value("conv/visitedAt").toString().toUtf8();
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        _visitedAt[it.key()] = it.value().toVariant().toLongLong();
}

void ConvListWidget::saveVisitedAt() {
    // Prune entries older than 2× the window so the store doesn't grow forever.
    const qint64 horizon = QDateTime::currentSecsSinceEpoch() - _relevantDays * qint64(86400) * 2;
    QJsonObject  obj;
    for (auto it = _visitedAt.begin(); it != _visitedAt.end(); ++it) {
        if (it.value() >= horizon)
            obj[it.key()] = it.value();
    }
    QSettings("msga", "msga")
        .setValue(
            "conv/visitedAt", QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact))
        );
}

// Returns true if s looks like a raw Slack user ID (e.g. "U0A1B2C3D").
static bool isRawSlackId(const QString &s) {
    if (s.length() < 9)
        return false;
    if (s[0] != 'U' && s[0] != 'W')
        return false;
    for (int i = 1; i < s.length(); ++i) {
        const QChar c = s[i];
        if (!c.isDigit() && !(c >= 'A' && c <= 'Z'))
            return false;
    }
    return true;
}

void ConvListWidget::setConversations(std::vector<Conversation> convs) {
    _allConvs = std::move(convs);
    rebuildFilteredConvs();
}

void ConvListWidget::selectRow(int row) {
    setSelected(row);
}

bool ConvListWidget::selectConversation(ConversationId id) {
    const auto it = std::find_if(_convs.begin(), _convs.end(), [&](const Conversation &c) {
        return c.id == id;
    });
    if (it == _convs.end())
        return false;
    const bool isDm = (it->kind == ConvKind::Im || it->kind == ConvKind::Mpim);
    (isAppConv(*it) ? _appsCollapsed : isDm ? _dmsCollapsed : _channelsCollapsed) = false;
    // Stamp before rebuilding so the relevance filter keeps the row visible.
    _visitedAt[id.value] = QDateTime::currentSecsSinceEpoch();
    saveVisitedAt();
    rebuildRows();
    const int row = rowForId(id);
    if (row < 0)
        return false;
    selectRow(row);
    return true;
}

void ConvListWidget::setUsers(const std::vector<User> &users) {
    _userInfos.clear();
    _usernameToId.clear();
    _userInfos.reserve(users.size());
    for (const auto &u : users) {
        _userInfos.insert(
            u.id.value,
            {
                .displayName = Emoji::expandCodes(u.displayName.isEmpty() ? u.name : u.displayName),
                .avatarUrl   = u.avatarUrl,
                .name        = u.name,
                .isDeactivated = u.isDeactivated,
                .isActive      = u.isActive,
                .dndEnabled    = u.dndEnabled,
                // Slackbot reports is_bot=false in the API; special-case its fixed ID.
                .isBot         = u.isBot || u.id.value == QLatin1String("USLACKBOT"),
                .statusEmoji   = u.statusEmoji,
            }
        );
        if (!u.name.isEmpty())
            _usernameToId.insert(u.name, u.id.value);
    }
    rebuildFilteredConvs();
}

bool ConvListWidget::isAppConv(const Conversation &c) const {
    if (c.kind != ConvKind::Im || !c.dmUser)
        return false;
    if (c.dmUser->value == QLatin1String("USLACKBOT"))
        return true;
    const auto it = _userInfos.constFind(c.dmUser->value);
    return it != _userInfos.constEnd() && it->isBot;
}

void ConvListWidget::rebuildFilteredConvs() {
    _convs.clear();
    for (const auto &conv : _allConvs) {
        if (!conv.isMember)
            continue;
        if ((conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim) && conv.dmUser) {
            const auto it = _userInfos.constFind(conv.dmUser->value);
            if (it != _userInfos.constEnd()) {
                if (it->isDeactivated)
                    continue;
                if (it->displayName == "deactivateduser")
                    continue;
                if (isRawSlackId(it->displayName))
                    continue;
            }
            // User info not yet loaded — let the DM through; it will re-filter
            // correctly once setUsers() is called.
        }
        _convs.push_back(conv);
    }
    rebuildRows();
}

void ConvListWidget::rebuildRows() {
    _rows.clear();

    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const qint64 cutoff = nowSec - _relevantDays * qint64(86400);

    // Stamp conversations with unread messages so they stay visible for the full window.
    bool seedChanged = false;
    for (const auto &c : _convs) {
        if (c.unread > 0 && _visitedAt.value(c.id.value, 0) < cutoff) {
            _visitedAt[c.id.value] = nowSec;
            seedChanged            = true;
        }
    }
    if (seedChanged)
        saveVisitedAt();

    // A conversation is relevant if:
    //   - it has unread messages, OR
    //   - it is currently selected, OR
    //   - the user visited it within the window (_visitedAt stamp >= cutoff), OR
    //   - it had server-side activity (latest message or read cursor — from
    //     Session's background conversations.info sweep, cache, or realtime
    //     events) within the window, OR
    //   - it is a channel nothing is known about yet (channels are not swept,
    //     so an unknown one is shown by default until it earns a stamp).
    // DMs and MPDMs with no data start hidden and pop in once the sweep
    // analyzes them and finds recent activity.
    auto isRelevant = [&](const Conversation &c) -> bool {
        if (c.unread > 0)
            return true;
        if (c.id == _selectedId)
            return true;
        const qint64 stamp = _visitedAt.value(c.id.value, -1);
        if (stamp >= cutoff)
            return true;
        // Slack ts strings are zero-padded fixed-width, so lexicographic max
        // picks the most recent of the two cursors.
        const QString &activity = std::max(c.latestTs, c.lastRead);
        if (!activity.isEmpty())
            return qint64(activity.toDouble()) >= cutoff;
        if (stamp >= 0)
            return false; // stale visit stamp and no newer activity
        return c.kind != ConvKind::Im && c.kind != ConvKind::Mpim;
    };

    std::vector<int> visCh, hidCh, visDm, hidDm, apps;
    for (int i = 0; i < (int)_convs.size(); ++i) {
        const auto &c = _convs[i];
        if (isAppConv(c)) {
            apps.push_back(i);
            continue;
        }
        const bool isDm = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
        (isDm ? (isRelevant(c) ? visDm : hidDm) : (isRelevant(c) ? visCh : hidCh)).push_back(i);
    }
    // ── Channels section ─────────────────────────────────────────────
    _rows.push_back({RowKind::SectionHeader, -1, 0});
    if (!_channelsCollapsed) {
        for (int i : visCh)
            _rows.push_back({RowKind::Conv, i, -1});
        if (!hidCh.empty()) {
            if (_showAllChannels)
                for (int i : hidCh)
                    _rows.push_back({RowKind::Conv, i, -1});
            else
                _rows.push_back({RowKind::ShowMore, -1, 0, (int)hidCh.size()});
        }
        _rows.push_back({RowKind::AddChannels, -1, 0});
    }

    // ── Direct messages section ───────────────────────────────────────
    // No "N more" expander here: hidden DMs/MPDMs can number in the hundreds,
    // so they are reached through the People tab of the browse dialog (the "+"
    // on the section header) instead of being dumped into the list.
    _rows.push_back({RowKind::SectionHeader, -1, 1});
    if (!_dmsCollapsed) {
        for (int i : visDm)
            _rows.push_back({RowKind::Conv, i, -1});
    }

    // ── Agents & apps section ─────────────────────────────────────────
    // Bot/app DMs live here, like in the official client. No relevance
    // filter: open app DMs are few, and unlike human DMs there is no
    // People-tab path to reopen one the filter would hide. The section
    // disappears entirely when there are no app DMs.
    if (!apps.empty()) {
        _rows.push_back({RowKind::SectionHeader, -1, 2});
        if (!_appsCollapsed) {
            for (int i : apps)
                _rows.push_back({RowKind::Conv, i, -1});
        }
    }

    // Re-map selection to new visual row indices.
    _selAnim.stop();
    _selT     = 1.0;
    _selFrom  = -1;
    _selected = -1;
    if (!_selectedId.value.isEmpty()) {
        for (int r = 0; r < (int)_rows.size(); ++r) {
            if (_rows[r].kind == RowKind::Conv && _convs[_rows[r].convIdx].id == _selectedId) {
                _selected = r;
                break;
            }
        }
    }
    _hovered = -1;
    updateScrollRange();
    viewport()->update();
}

ConversationId ConvListWidget::conversationId(int row) const {
    if (row < 0 || row >= (int)_rows.size())
        return {};
    const auto &ri = _rows[row];
    if (ri.kind != RowKind::Conv)
        return {};
    return _convs[ri.convIdx].id;
}

int ConvListWidget::rowForId(ConversationId id) const {
    for (int r = 0; r < (int)_rows.size(); ++r) {
        const auto &ri = _rows[r];
        if (ri.kind == RowKind::Conv && _convs[ri.convIdx].id == id)
            return r;
    }
    return -1;
}

// Parses "mpdm-alice.smith--bob.jones--3" → ["alice.smith", "bob.jones"]
static QStringList parseMpdmUsernames(const QString &name) {
    QString s = name;
    if (s.startsWith("mpdm-"))
        s = s.mid(5);
    // Strip trailing numeric suffix like "-1" or "-3"
    const int lastDash = s.lastIndexOf('-');
    if (lastDash > 0) {
        bool ok = false;
        s.mid(lastDash + 1).toInt(&ok);
        if (ok)
            s = s.left(lastDash);
    }
    return s.split("--", Qt::SkipEmptyParts);
}

QString ConvListWidget::resolvedName(int row) const {
    if (row < 0 || row >= (int)_rows.size())
        return {};
    const auto &ri = _rows[row];
    if (ri.kind != RowKind::Conv)
        return {};
    const auto &conv = _convs[ri.convIdx];
    if (conv.kind == ConvKind::Im && conv.dmUser) {
        const auto it = _userInfos.constFind(conv.dmUser->value);
        if (it != _userInfos.constEnd() && !it->displayName.isEmpty())
            return it->displayName;
    }
    if (conv.kind == ConvKind::Mpim) {
        QStringList names;
        if (!conv.members.empty()) {
            for (const auto &uid : conv.members) {
                if (!_meUserId.value.isEmpty() && uid == _meUserId)
                    continue;
                const auto it = _userInfos.constFind(uid.value);
                if (it != _userInfos.constEnd() && !it->displayName.isEmpty())
                    names.append(it->displayName);
            }
        } else {
            for (const QString &uname : parseMpdmUsernames(conv.name)) {
                const QString uid = _usernameToId.value(uname);
                if (!uid.isEmpty() && uid == _meUserId.value)
                    continue;
                const auto it = uid.isEmpty() ? _userInfos.constEnd() : _userInfos.constFind(uid);
                names.append(
                    (it != _userInfos.constEnd() && !it->displayName.isEmpty()) ? it->displayName
                                                                                : uname
                );
            }
        }
        if (!names.isEmpty())
            return names.join(", ");
    }
    return Emoji::expandCodes(conv.name);
}

// ── Events ────────────────────────────────────────────────────────────────────

void ConvListWidget::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRange();
}

void ConvListWidget::wheelEvent(QWheelEvent *event) {
    _tooltip->hide(); // rows shift under the cursor; reappears on next move
    auto        *vsb = verticalScrollBar();
    const QPoint px  = event->pixelDelta();
    if (!px.isNull()) {
        vsb->setValue(vsb->value() - px.y());
    } else {
        const int steps  = event->angleDelta().y();
        const int pixels = steps * QApplication::wheelScrollLines() * kRowH / 120;
        vsb->setValue(vsb->value() - pixels);
    }
    event->accept();
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

int ConvListWidget::rowAt(int viewportY) const {
    const int docY = viewportY + verticalScrollBar()->value();
    const int row  = docY / kRowH;
    if (row < 0 || row >= static_cast<int>(_rows.size()))
        return -1;
    return row;
}

void ConvListWidget::setHovered(int row) {
    if (row == _hovered)
        return;
    _hovered = row;
    viewport()->update();
}

void ConvListWidget::setSelected(int row) {
    if (row < 0 || row >= (int)_rows.size())
        return;
    if (_rows[row].kind != RowKind::Conv)
        return;
    if (row == _selected)
        return;
    _selFrom                      = _selected;
    _selT                         = 0.0;
    _selected                     = row;
    _selectedId                   = _convs[_rows[row].convIdx].id;
    // Record visit so this conversation stays visible in future sessions.
    _visitedAt[_selectedId.value] = QDateTime::currentSecsSinceEpoch();
    saveVisitedAt();
    _selAnim.stop();
    _selAnim.setStartValue(0.0);
    _selAnim.setEndValue(1.0);
    _selAnim.start();
    emit conversationSelected(row);
}

void ConvListWidget::doMouseMove(QMouseEvent *e) {
    const int total = static_cast<int>(_rows.size()) * kRowH;
    if (_sbDragging) {
        const int vh         = viewport()->height();
        const int thumbH     = std::max(20, vh * vh / total);
        const int trackRange = vh - thumbH;
        if (trackRange > 0) {
            const int newScroll =
                _sbDragStartScroll + (e->pos().y() - _sbDragStartY) * (total - vh) / trackRange;
            verticalScrollBar()->setValue(std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (e->pos().x() >= sbHitX && VirtualListWidget::isOnScrollThumb(e->pos().y(), total))
        viewport()->setCursor(Qt::SizeVerCursor);
    else
        viewport()->setCursor(Qt::PointingHandCursor);
    const int row = rowAt(e->pos().y());
    setHovered(row);

    // Tooltip for the "+" on the Direct messages header.
    bool onPlus = false;
    if (row >= 0 && _rows[row].kind == RowKind::SectionHeader && _rows[row].sectionId == 1) {
        const QRect r = dmPlusRect(row * kRowH - verticalScrollBar()->value());
        onPlus        = r.contains(e->pos());
        if (onPlus && !_tooltip->isVisible())
            _tooltip->showAbove(
                tr("Open a direct message"), QRect(viewport()->mapToGlobal(r.topLeft()), r.size())
            );
    }
    if (!onPlus)
        _tooltip->hide();
}

static void buildNotifySection(
    ContextMenu *menu, ConversationId id, NotificationLevel level, ConvListWidget *self
) {
    menu->addHeader(ConvListWidget::tr("Notify you about…"));
    menu->addItem(
        ConvListWidget::tr("All new posts"),
        [self, id] { emit self->setNotificationLevelRequested(id, NotificationLevel::All); },
        false,
        ":/ui/bell.svg",
        level == NotificationLevel::All
    );
    menu->addItem(
        ConvListWidget::tr("Just mentions"),
        [self, id] { emit self->setNotificationLevelRequested(id, NotificationLevel::Mentions); },
        false,
        ":/ui/bell.svg",
        level == NotificationLevel::Mentions || level == NotificationLevel::Default
    );
    menu->addItem(
        ConvListWidget::tr("Mute and hide"),
        [self, id] { emit self->setNotificationLevelRequested(id, NotificationLevel::Mute); },
        false,
        ":/ui/bell-off.svg",
        level == NotificationLevel::Mute
    );
}

void ConvListWidget::showChannelContextMenu(int row, QPoint globalPos) {
    const auto &conv    = _convs[_rows[row].convIdx];
    const bool  starred = conv.isStarred;
    auto       *menu    = new ContextMenu(viewport());

    menu->addItem(
        starred ? tr("Unstar channel") : tr("Star channel"),
        [this, id = conv.id, starred] { emit starConversationRequested(id, !starred); }
    );
    menu->addSeparator();
    buildNotifySection(menu, conv.id, conv.notifLevel, this);
    menu->addSeparator();
    menu->addItem(
        tr("Leave channel"),
        [this, id = conv.id] { emit leaveConversationRequested(id); },
        /*destructive=*/true
    );
    menu->popup(globalPos);
}

void ConvListWidget::showMpdmContextMenu(int row, QPoint globalPos) {
    const auto &conv    = _convs[_rows[row].convIdx];
    const bool  starred = conv.isStarred;
    auto       *menu    = new ContextMenu(viewport());

    menu->addItem(
        starred ? tr("Unstar conversation") : tr("Star conversation"),
        [this, id = conv.id, starred] { emit starConversationRequested(id, !starred); }
    );
    menu->addSeparator();
    buildNotifySection(menu, conv.id, conv.notifLevel, this);
    menu->addSeparator();
    menu->addItem(
        tr("Leave conversation"),
        [this, id = conv.id] { emit leaveConversationRequested(id); },
        /*destructive=*/true
    );
    menu->popup(globalPos);
}

void ConvListWidget::doMousePress(QMouseEvent *e) {
    _tooltip->hide();
    if (e->button() == Qt::RightButton) {
        const int row = rowAt(e->pos().y());
        if (row >= 0 && _rows[row].kind == RowKind::Conv) {
            const auto &conv = _convs[_rows[row].convIdx];
            if (conv.kind == ConvKind::Mpim) {
                showMpdmContextMenu(row, e->globalPosition().toPoint());
            } else if (conv.kind == ConvKind::PublicChannel ||
                       conv.kind == ConvKind::PrivateChannel) {
                showChannelContextMenu(row, e->globalPosition().toPoint());
            }
        }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const int total  = static_cast<int>(_rows.size()) * kRowH;
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (e->pos().x() >= sbHitX && VirtualListWidget::isOnScrollThumb(e->pos().y(), total)) {
        _sbDragging        = true;
        _sbDragStartY      = e->pos().y();
        _sbDragStartScroll = verticalScrollBar()->value();
        viewport()->setCursor(Qt::SizeVerCursor);
        return;
    }
    const int row = rowAt(e->pos().y());
    if (row < 0)
        return;
    const auto &ri = _rows[row];
    switch (ri.kind) {
    case RowKind::SectionHeader:
        if (ri.sectionId == 1 &&
            dmPlusRect(row * kRowH - verticalScrollBar()->value()).contains(e->pos())) {
            emit browsePeopleRequested();
            break;
        }
        if (ri.sectionId == 0)
            _channelsCollapsed = !_channelsCollapsed;
        else if (ri.sectionId == 1)
            _dmsCollapsed = !_dmsCollapsed;
        else
            _appsCollapsed = !_appsCollapsed;
        rebuildRows();
        break;
    case RowKind::Conv:
        setSelected(row);
        break;
    case RowKind::AddChannels: {
        auto *menu = new ContextMenu(viewport());
        menu->addItem(tr("Find a channel"), [this] { emit findChannelRequested(); });
        menu->addItem(tr("Create a channel"), [this] { emit createChannelRequested(); });
        menu->popup(e->globalPosition().toPoint());
        break;
    }
    case RowKind::ShowMore:
        _showAllChannels = true;
        rebuildRows();
        break;
    }
}

void ConvListWidget::doMouseRelease(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    if (_sbDragging) {
        _sbDragging = false;
        viewport()->setCursor(Qt::PointingHandCursor);
    }
}

void ConvListWidget::doMouseLeave() {
    setHovered(-1);
    _tooltip->hide();
}

// ── Layout ────────────────────────────────────────────────────────────────────

void ConvListWidget::updateScrollRange() {
    const int total = static_cast<int>(_rows.size()) * kRowH;
    const int vh    = viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, total - vh));
    verticalScrollBar()->setPageStep(vh);
}

// ── Avatar downloads ──────────────────────────────────────────────────────────

void ConvListWidget::triggerMissingAvatarDownloads() {
    if (!_imgCache)
        return;
    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();
    const int first   = scrollY / kRowH;
    const int last    = std::min((int)_rows.size() - 1, (scrollY + vh) / kRowH);

    for (int r = first; r <= last; ++r) {
        const auto &ri = _rows[r];
        if (ri.kind != RowKind::Conv)
            continue;
        const auto &conv = _convs[ri.convIdx];
        if ((conv.kind != ConvKind::Im && conv.kind != ConvKind::Mpim) || !conv.dmUser)
            continue;
        const auto infoIt = _userInfos.constFind(conv.dmUser->value);
        if (infoIt == _userInfos.constEnd())
            continue;
        const QString &url = infoIt->avatarUrl;
        if (!url.isEmpty())
            _imgCache->get(url);
    }
}

void ConvListWidget::drawUserAvatar(
    QPainter &p, QRect rect, const QString &userId, QColor bgColor, bool isSelected
) const {
    const auto        infoIt  = _userInfos.constFind(userId);
    const QString     url     = (infoIt != _userInfos.constEnd()) ? infoIt->avatarUrl : QString{};
    const QPixmap     pixmap  = (_imgCache && !url.isEmpty()) ? _imgCache->get(url) : QPixmap{};
    const QString     initial = (infoIt != _userInfos.constEnd() && !infoIt->displayName.isEmpty())
                                    ? infoIt->displayName.left(1)
                                    : QString{"?"};
    UserAvatar::State state =
        (infoIt != _userInfos.constEnd())
            ? UserAvatar::State{infoIt->isActive, infoIt->dndEnabled, isSelected}
            : UserAvatar::State{};
    if (_selfPhantomAway && !_meUserId.value.isEmpty() && userId == _meUserId.value)
        state.phantomAway = true;
    UserAvatar::paint(
        p, rect, pixmap, initial, state, kAvatarRadius, p.device()->devicePixelRatioF(), bgColor
    );
}

// ── Painting ──────────────────────────────────────────────────────────────────

void ConvListWidget::doPaint(QPaintEvent *event) {
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(event->rect(), Th::c().nav.primary);

    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    const int first = scrollY / kRowH;
    const int last  = std::min(static_cast<int>(_rows.size()) - 1, (scrollY + vh) / kRowH);

    for (int r = first; r <= last; ++r)
        paintRow(p, r, r * kRowH - scrollY);

    triggerMissingAvatarDownloads();

    paintScrollThumb(p, static_cast<int>(_rows.size()) * kRowH, Th::c().nav.scrollThumb);
}

void ConvListWidget::paintSectionHeader(QPainter &p, int row, int y, int sectionId) const {
    const bool hovered   = (row == _hovered);
    const bool collapsed = (sectionId == 0)   ? _channelsCollapsed
                           : (sectionId == 1) ? _dmsCollapsed
                                              : _appsCollapsed;

    if (hovered)
        p.fillRect(QRect(0, y, viewport()->width(), kRowH), Th::c().nav.itemHover);

    // Normally show the section icon; on hover replace it with the chevron that
    // previews what clicking will do (collapsed → down chevron, expanded → right chevron).
    const QColor color = Th::c().text.onDarkDim;

    const QPixmap *icon;
    if (hovered)
        icon = collapsed ? &_iconPx.chevRight : &_iconPx.chevDown;
    else
        icon = (sectionId == 0) ? &_iconPx.hash : (sectionId == 1) ? &_iconPx.msg : &_iconPx.bot;

    const int iconY = y + (kRowH - kIconSize) / 2;
    int       x     = kPadH;
    p.drawPixmap(x, iconY, *icon);
    x += kIconSize + 6;

    QFont font = QApplication::font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 0.82);
    p.setFont(font);
    p.setPen(color);
    const QFontMetrics fm(font);
    const QString      label = (sectionId == 0)   ? tr("Channels")
                               : (sectionId == 1) ? tr("Direct messages")
                                                  : tr("Agents & apps");
    p.drawText(x, y + (kRowH - fm.height()) / 2 + fm.ascent(), label);

    // DM section header: a "+" on hover opens the browse dialog on People.
    if (sectionId == 1 && hovered) {
        const QRect r = dmPlusRect(y);
        p.drawPixmap(r.topLeft(), _iconPx.plusDim);
    }
}

QRect ConvListWidget::dmPlusRect(int rowY) const {
    // Right-aligned inside the header row, vertically centered. Slightly inset
    // from the scroll thumb gutter.
    const int x = viewport()->width() - kPadH - kIconSize;
    return QRect(x, rowY + (kRowH - kIconSize) / 2, kIconSize, kIconSize);
}

void ConvListWidget::paintAddChannelsRow(QPainter &p, int row, int y) const {
    const bool hovered = (row == _hovered);
    if (hovered)
        p.fillRect(QRect(0, y, viewport()->width(), kRowH), Th::c().nav.itemHover);

    const QColor color = hovered ? Th::c().text.onDark : Th::c().text.onDarkDim;

    const QPixmap &plusPx = hovered ? _iconPx.plusBright : _iconPx.plusDim;
    p.drawPixmap(kPadH + kGroupIndent, y + (kRowH - kIconSize) / 2, plusPx);

    QFont font = QApplication::font();
    font.setWeight(QFont::Normal);
    p.setFont(font);
    p.setPen(color);

    const QFontMetrics fm(font);
    const int          textY = y + (kRowH - fm.height()) / 2 + fm.ascent();
    p.drawText(kPadH + kGroupIndent + kIconSize + 6, textY, tr("Add channels"));
}

void ConvListWidget::paintShowMoreRow(QPainter &p, int row, int y, int count) const {
    const bool hovered = (row == _hovered);
    if (hovered)
        p.fillRect(QRect(0, y, viewport()->width(), kRowH), Th::c().nav.itemHover);

    const QColor color = hovered ? Th::c().text.onDark : Th::c().text.onDarkDim;

    QFont font = QApplication::font();
    font.setWeight(QFont::Normal);
    p.setFont(font);
    p.setPen(color);

    const QFontMetrics fm(font);
    const int          textY = y + (kRowH - fm.height()) / 2 + fm.ascent();
    const int          leftX = kPadH + kGroupIndent;

    const QString label =
        tr("%1 more %2").arg(count).arg(count == 1 ? tr("channel") : tr("channels"));
    p.drawText(leftX, textY, label);
}

void ConvListWidget::paintRow(QPainter &p, int row, int y) const {
    const auto &ri = _rows[row];

    if (ri.kind == RowKind::SectionHeader) {
        paintSectionHeader(p, row, y, ri.sectionId);
        return;
    }
    if (ri.kind == RowKind::AddChannels) {
        paintAddChannelsRow(p, row, y);
        return;
    }
    if (ri.kind == RowKind::ShowMore) {
        paintShowMoreRow(p, row, y, ri.count);
        return;
    }

    // ── Conv row ──────────────────────────────────────────────────────
    const auto &conv = _convs[ri.convIdx];
    const QRect rowRect(0, y, viewport()->width(), kRowH);

    // Background
    QColor rowBg = Th::c().nav.primary;
    if (row == _selected) {
        const QColor base = (row == _hovered) ? Th::c().nav.itemHover : Th::c().nav.primary;
        const QColor sel  = Th::c().nav.itemSelected;
        auto lerp = [](int a, int b, double t) { return static_cast<int>(a + (b - a) * t); };
        rowBg     = QColor(
            lerp(base.red(), sel.red(), _selT),
            lerp(base.green(), sel.green(), _selT),
            lerp(base.blue(), sel.blue(), _selT)
        );
        // Rounded pill inset from edges, not full-width
        p.setPen(Qt::NoPen);
        p.setBrush(rowBg);
        p.drawRoundedRect(rowRect.adjusted(8, 0, -8, 0), 6, 6);
    } else if (row == _hovered) {
        rowBg = Th::c().nav.itemHover;
        p.setPen(Qt::NoPen);
        p.setBrush(rowBg);
        p.drawRoundedRect(rowRect.adjusted(8, 0, -8, 0), 6, 6);
    }

    QFont      font       = QApplication::font();
    const bool isUnread   = conv.unread > 0;
    const bool isSelected = (row == _selected);
    font.setWeight(isUnread ? QFont::DemiBold : QFont::Normal);
    p.setFont(font);

    const QColor textColor = isSelected ? Th::c().nav.primary
                             : isUnread ? Th::c().text.onDark
                                        : Th::c().text.onDarkDim;
    p.setPen(textColor);

    const QFontMetrics fm(font);
    const int          textY = y + (kRowH - fm.height()) / 2 + fm.ascent();

    const bool isDm           = (conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim);
    // DMs treat all unreads as high-priority; channels only when there are @mentions.
    const bool isHighPriority = isDm || conv.mentionCount > 0;
    const int  badgeW = conv.unread == 0 ? 0 : isHighPriority ? (conv.unread > 9 ? 28 : 20) : 14;

    const int leftX = kPadH + kGroupIndent;
    if (conv.kind == ConvKind::Mpim) {
        // Rounded square icon with participant count (excluding self)
        const int avY          = y + (kRowH - kAvatarSize) / 2;
        int       displayCount = 0;
        if (!conv.members.empty()) {
            for (const auto &uid : conv.members) {
                if (_meUserId.value.isEmpty() || uid != _meUserId)
                    ++displayCount;
            }
        } else {
            const QStringList unames = parseMpdmUsernames(conv.name);
            displayCount             = unames.size();
            for (const QString &uname : unames) {
                if (_usernameToId.value(uname) == _meUserId.value && !_meUserId.value.isEmpty())
                    --displayCount;
            }
        }
        if (displayCount <= 0)
            displayCount = 1;

        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().presence.away);
        p.drawRoundedRect(
            QRect(leftX, avY, kAvatarSize, kAvatarSize), kAvatarRadius, kAvatarRadius
        );

        QFont cf = font;
        cf.setPointSizeF(kAvatarSize * 0.38);
        cf.setBold(true);
        p.setFont(cf);
        p.setPen(Qt::white);
        p.drawText(
            QRect(leftX, avY, kAvatarSize, kAvatarSize),
            Qt::AlignCenter,
            displayCount > 0 ? QString::number(displayCount) : "+"
        );

        const int     nameX = leftX + kAvatarSize + kAvatarGap;
        const int     maxW  = viewport()->width() - nameX - badgeW - 8;
        const QString name  = fm.elidedText(resolvedName(row), Qt::ElideRight, maxW);
        p.setFont(font);
        p.setPen(textColor);
        p.drawText(nameX, textY, name);
    } else if (conv.kind == ConvKind::Im && conv.dmUser) {
        const int avY = y + (kRowH - kAvatarSize) / 2;
        drawUserAvatar(
            p, QRect(leftX, avY, kAvatarSize, kAvatarSize), conv.dmUser->value, rowBg, isSelected
        );

        const auto    infoIt = _userInfos.constFind(conv.dmUser->value);
        const QString emoji  = (infoIt != _userInfos.constEnd())
                                   ? MsgRender::resolveEmoji(infoIt->statusEmoji)
                                   : QString{};
        const bool    isMe   = !_meUserId.value.isEmpty() && conv.dmUser == _meUserId;

        const int nameX = leftX + kAvatarSize + kAvatarGap;

        int     suffixW = 0;
        QString emojiGlyph;
        if (!emoji.isEmpty() && !infoIt->statusEmoji.isEmpty()) {
            emojiGlyph = emoji;
            QFont ef   = emojiFont(
                static_cast<int>(
                    font.pixelSize() > 0 ? font.pixelSize() : QFontMetrics(font).height()
                )
            );
            suffixW += QFontMetrics(ef).horizontalAdvance(emojiGlyph) + 4;
        }
        int youW = 0;
        if (isMe) {
            QFont df = font;
            df.setWeight(QFont::Normal);
            df.setPointSizeF(df.pointSizeF() * 0.88);
            youW = QFontMetrics(df).horizontalAdvance(tr("you")) + 6;
            suffixW += youW;
        }

        const int     maxW = viewport()->width() - nameX - suffixW - badgeW - 8;
        const QString name = fm.elidedText(resolvedName(row), Qt::ElideRight, maxW);
        p.setPen(textColor);
        p.drawText(nameX, textY, name);
        int curX = nameX + fm.horizontalAdvance(name);

        if (!emojiGlyph.isEmpty()) {
            curX += 4;
            const int emojiPx = static_cast<int>(
                font.pixelSize() > 0 ? font.pixelSize() : QFontMetrics(font).height()
            );
            p.setFont(emojiFont(emojiPx));
            p.setPen(textColor);
            p.drawText(curX, textY, emojiGlyph);
            curX += QFontMetrics(p.font()).horizontalAdvance(emojiGlyph);
        }

        if (isMe) {
            curX += 4;
            QFont df = font;
            df.setWeight(QFont::Normal);
            df.setPointSizeF(df.pointSizeF() * 0.88);
            p.setFont(df);
            p.setPen(isSelected ? textColor : Th::c().text.onDarkDim);
            p.drawText(curX, textY, tr("you"));
        }
    } else {
        int prefixW = 0;
        if (conv.kind == ConvKind::PrivateChannel) {
            const QPixmap &lockPx = isSelected ? _iconPx.lockSelected
                                    : isUnread ? _iconPx.lockBright
                                               : _iconPx.lockDim;
            p.drawPixmap(leftX, y + (kRowH - 14) / 2, lockPx);
            prefixW = 14 + 6;
        } else {
            const QPixmap &hashPx = isSelected ? _iconPx.hashSmSelected
                                    : isUnread ? _iconPx.hashSmBright
                                               : _iconPx.hashSmDim;
            p.drawPixmap(leftX, y + (kRowH - 14) / 2, hashPx);
            prefixW = 14 + 6;
        }
        const int     maxW = viewport()->width() - leftX - prefixW - badgeW - 8;
        const QString name = fm.elidedText(conv.name, Qt::ElideRight, maxW);
        p.drawText(leftX + prefixW, textY, name);
    }

    // ── Unread indicator ──────────────────────────────────────────────
    if (conv.unread > 0) {
        if (isHighPriority) {
            // Red numbered badge for DMs and @mentions
            const QString badge = conv.unread > 99 ? "99+" : QString::number(conv.unread);
            QFont         bf    = font;
            bf.setPointSizeF(bf.pointSizeF() * 0.78);
            bf.setBold(true);
            p.setFont(bf);
            const QFontMetrics bfm(bf);
            const int          bw = bfm.horizontalAdvance(badge) + 10;
            const int          bh = bfm.height() + 4;
            const int          bx = viewport()->width() - bw - 8;
            const int          by = y + (kRowH - bh) / 2;
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().badge.mention);
            p.drawRoundedRect(QRect(bx, by, bw, bh), bh / 2, bh / 2);
            p.setPen(Qt::white);
            p.drawText(QRect(bx, by, bw, bh), Qt::AlignCenter, badge);
        } else {
            // Small dim dot for regular channel unreads (bold text already signals activity)
            const int dotD = 8;
            const int bx   = viewport()->width() - dotD - 10;
            const int by   = y + (kRowH - dotD) / 2;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 160));
            p.drawEllipse(bx, by, dotD, dotD);
        }
    }
}
