// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_list_widget.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/paint_utils.h"
#include "ui/user_avatar.h"
#include "ui/message_list/message_render.h"
#include "session/session.h"
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
        // The cache is shared app-wide and fires for every image loaded
        // anywhere (message previews, emoji, favicons) — repaint only when the
        // url is the avatar of a row currently on screen.
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
            const int scrollY = verticalScrollBar()->value();
            const int vh      = viewport()->height();
            const int first   = scrollY / kRowH;
            const int last    = std::min((int)_rows.size() - 1, (scrollY + vh) / kRowH);
            for (int r = first; r <= last; ++r) {
                const auto &ri = _rows[r];
                if (ri.kind != RowKind::Conv)
                    continue;
                const auto &conv = _convs[ri.convIdx];
                if (!conv.dmUser)
                    continue;
                const auto infoIt = _userInfos.constFind(conv.dmUser->value);
                if (infoIt == _userInfos.constEnd() || infoIt->avatarUrl != url)
                    continue;
                viewport()->update(QRect(0, r * kRowH - scrollY, viewport()->width(), kRowH));
            }
        });
    }
    rebuildIconPixmaps();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        rebuildIconPixmaps();
        viewport()->update();
    });

    _saveVisitedTimer.setSingleShot(true);
    connect(&_saveVisitedTimer, &QTimer::timeout, this, [this] { saveVisitedAt(); });
}

ConvListWidget::~ConvListWidget() {
    // Flush a debounced recency write that hasn't fired yet.
    if (_saveVisitedTimer.isActive())
        saveVisitedAt();
}

void ConvListWidget::rebuildIconPixmaps() {
    const auto &th  = Th::c();
    const QSize big = QSize(kIconSize, kIconSize);
    const QSize sm  = QSize(14, 14);

    // Rasterise at the live painted DPR (set in doPaint); fall back to the
    // app-global ratio before the first paint. See the doPaint comment.
    const qreal dpr = _iconDpr > 0 ? _iconDpr : (qGuiApp ? qGuiApp->devicePixelRatio() : 1.0);
    const auto  px  = [&](const QString &path, const QSize &sz, const QColor &c) {
        return svgPixmapPhys(path, sz, c, dpr);
    };

    _iconPx.chevDown   = px(":/ui/chevron-down.svg", big, th.text.onDarkDim);
    _iconPx.chevRight  = px(":/ui/chevron-right.svg", big, th.text.onDarkDim);
    _iconPx.hash       = px(":/ui/hash.svg", big, th.text.onDarkDim);
    _iconPx.msg        = px(":/ui/messages-square.svg", big, th.text.onDarkDim);
    _iconPx.bot        = px(":/ui/bot.svg", big, th.text.onDarkDim);
    _iconPx.plusDim    = px(":/ui/plus.svg", big, th.text.onDarkDim);
    _iconPx.plusBright = px(":/ui/plus.svg", big, th.text.onDark);

    _iconPx.lockDim        = px(":/ui/lock.svg", sm, th.text.onDarkDim);
    _iconPx.lockBright     = px(":/ui/lock.svg", sm, th.text.onDark);
    _iconPx.lockSelected   = px(":/ui/lock.svg", sm, th.nav.itemSelectedText);
    _iconPx.hashSmDim      = px(":/ui/hash.svg", sm, th.text.onDarkDim);
    _iconPx.hashSmBright   = px(":/ui/hash.svg", sm, th.text.onDark);
    _iconPx.hashSmSelected = px(":/ui/hash.svg", sm, th.nav.itemSelectedText);

    _iconPx.huddle = px(":/ui/headphones.svg", QSize(13, 13), th.accent.text);
}

void ConvListWidget::setRelevantDays(int days) {
    _relevantDays = std::max(1, days);
    rebuildRows();
}

void ConvListWidget::setDefaultNotifyLevel(NotificationLevel level) {
    if (_defaultNotify == level)
        return;
    _defaultNotify = level;
    viewport()->update(); // badge visibility/color for Default-level convs may change
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
    _saveVisitedTimer.stop(); // an explicit write subsumes any pending debounce
}

void ConvListWidget::scheduleSaveVisitedAt() {
    // The recency store is serialized over the whole hash; keep it off the
    // rebuild/selection path and coalesce bursts. Durability isn't critical —
    // a lost update just means a slightly stale relevance window, which
    // self-heals, and the destructor flushes anything still pending.
    _saveVisitedTimer.start(1500);
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
    scheduleSaveVisitedAt();
    rebuildRows();
    const int row = rowForId(id);
    if (row < 0)
        return false;
    selectRow(row);
    return true;
}

void ConvListWidget::setUsers(const std::vector<User> &users) {
    // No-op re-emissions are common (the network refresh hands back an
    // identical list, workspace switches re-emit the current value). Skip
    // the full hash rebuild + rebuildRows when nothing actually changed.
    // (Presence/DND flips arrive via setUserPresence/setUserDnd, not here.)
    if (users == _lastUsers)
        return;
    _lastUsers = users;

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
                // System accounts may report is_bot=false; the backend knows them.
                .isBot         = u.isBot || (_session && _session->isSyntheticUser(u.id)),
                .isExternal    = u.isExternal,
                .statusEmoji   = u.statusEmoji,
            }
        );
        if (!u.name.isEmpty())
            _usernameToId.insert(u.name, u.id.value);
    }
    rebuildFilteredConvs();
}

void ConvListWidget::setUserPresence(const UserId &id, bool active) {
    const auto it = _userInfos.find(id.value);
    if (it == _userInfos.end() || it->isActive == active)
        return;
    it->isActive = active;
    // Keep the setUsers() no-op guard truthful, or the next identical roster
    // snapshot would compare unequal and force a full rebuild.
    for (auto &u : _lastUsers)
        if (u.id == id) {
            u.isActive = active;
            break;
        }
    updateRowsForUser(id.value);
}

void ConvListWidget::setUserDnd(const UserId &id, bool dnd) {
    const auto it = _userInfos.find(id.value);
    if (it == _userInfos.end() || it->dndEnabled == dnd)
        return;
    it->dndEnabled = dnd;
    for (auto &u : _lastUsers)
        if (u.id == id) {
            u.dndEnabled = dnd;
            break;
        }
    updateRowsForUser(id.value);
}

void ConvListWidget::updateRowsForUser(const QString &userId) {
    const int scrollY = verticalScrollBar()->value();
    const int vw      = viewport()->width();
    const int vh      = viewport()->height();
    for (int r = 0; r < static_cast<int>(_rows.size()); ++r) {
        const auto &ri = _rows[r];
        if (ri.kind != RowKind::Conv)
            continue;
        const auto &conv = _convs[ri.convIdx];
        if (!conv.dmUser || conv.dmUser->value != userId)
            continue;
        const int y = r * kRowH - scrollY;
        if (y + kRowH < 0 || y > vh)
            continue;
        viewport()->update(QRect(0, y, vw, kRowH));
    }
}

bool ConvListWidget::isAppConv(const Conversation &c) const {
    if (c.kind != ConvKind::Im || !c.dmUser)
        return false;
    if (_session && _session->isSyntheticUser(*c.dmUser))
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
                if (_session && _session->isUnresolvedUserId(it->displayName))
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
        scheduleSaveVisitedAt();

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
        // Most recent of the two activity cursors, compared on epoch micros (the
        // orderable time) rather than the raw ts — the UI never lexically
        // compares ids. (Conversation has no dedicated date field like
        // Message::date yet, so we derive it here via the shared helper.)
        const qint64 latest   = c.latestTs.isEmpty() ? 0 : decimalTsToMicros(c.latestTs);
        const qint64 read     = c.lastRead.isEmpty() ? 0 : decimalTsToMicros(c.lastRead);
        const qint64 activity = std::max(latest, read);
        if (activity != 0)
            return activity / 1000000 >= cutoff;
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
        if (it != _userInfos.constEnd() && !it->displayName.isEmpty() &&
            !(_session && _session->isUnresolvedUserId(it->displayName)))
            return it->displayName;
        // conv.name for an IM is the raw peer id; resolve it (and kick off a
        // users.info fetch) so the title is never a cryptic "U…/W…".
        if (_session)
            return _session->userDisplayName(*conv.dmUser);
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
    _tooltipRow      = -1;
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
    scheduleSaveVisitedAt();
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
    const int sbHitX = scrollThumbHitX();
    if (e->pos().x() >= sbHitX && VirtualListWidget::isOnScrollThumb(e->pos().y(), total))
        viewport()->setCursor(Qt::SizeVerCursor);
    else
        viewport()->setCursor(Qt::PointingHandCursor);
    const int row = rowAt(e->pos().y());
    setHovered(row);

    // Tooltips: the "+" on the Direct messages header, and the full name over a
    // truncated (elided) chat name. _tooltipRow tracks what's showing so we only
    // re-issue showAbove when the target changes (-2 = the "+", else the row).
    bool tooltipShown = false;
    if (row >= 0 && _rows[row].kind == RowKind::SectionHeader && _rows[row].sectionId == 1) {
        const QRect r = dmPlusRect(row * kRowH - verticalScrollBar()->value());
        if (r.contains(e->pos())) {
            if (_tooltipRow != -2) {
                _tooltip->showAbove(
                    tr("Open a direct message"),
                    QRect(viewport()->mapToGlobal(r.topLeft()), r.size())
                );
                _tooltipRow = -2;
            }
            tooltipShown = true;
        }
    } else if (row >= 0 && _rows[row].kind == RowKind::Conv) {
        const auto it = _truncNameRects.constFind(row);
        if (it != _truncNameRects.constEnd()) {
            if (_tooltipRow != row) {
                const QRect r = it.value();
                _tooltip->showRightOf(
                    resolvedName(row), QRect(viewport()->mapToGlobal(r.topLeft()), r.size())
                );
                _tooltipRow = row;
            }
            tooltipShown = true;
        }
    }
    if (!tooltipShown) {
        _tooltip->hide();
        _tooltipRow = -1;
    }
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
        level == NotificationLevel::Mentions
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
    buildNotifySection(menu, conv.id, effectiveNotifLevel(conv, _defaultNotify), this);
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
    buildNotifySection(menu, conv.id, effectiveNotifLevel(conv, _defaultNotify), this);
    menu->addSeparator();
    menu->addItem(
        tr("Leave conversation"),
        [this, id = conv.id] { emit leaveConversationRequested(id); },
        /*destructive=*/true
    );
    menu->popup(globalPos);
}

void ConvListWidget::showDmContextMenu(int row, QPoint globalPos) {
    const auto &conv  = _convs[_rows[row].convIdx];
    auto       *menu  = new ContextMenu(viewport());
    const bool  muted = conv.locallyMuted;
    menu->addItem(muted ? tr("Unmute") : tr("Mute"), [this, id = conv.id, muted] {
        emit muteConversationRequested(id, !muted);
    });
    menu->popup(globalPos);
}

void ConvListWidget::doMousePress(QMouseEvent *e) {
    _tooltip->hide();
    _tooltipRow = -1;
    if (e->button() == Qt::RightButton) {
        const int row = rowAt(e->pos().y());
        if (row >= 0 && _rows[row].kind == RowKind::Conv) {
            const auto &conv = _convs[_rows[row].convIdx];
            if (conv.kind == ConvKind::Mpim) {
                showMpdmContextMenu(row, e->globalPosition().toPoint());
            } else if (conv.kind == ConvKind::PublicChannel ||
                       conv.kind == ConvKind::PrivateChannel) {
                showChannelContextMenu(row, e->globalPosition().toPoint());
            } else if (conv.kind == ConvKind::Im) {
                showDmContextMenu(row, e->globalPosition().toPoint());
            }
        }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const int total  = static_cast<int>(_rows.size()) * kRowH;
    const int sbHitX = scrollThumbHitX();
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
    case RowKind::Conv: {
        // Clicking the live-huddle indicator joins the huddle rather than just
        // opening the conversation.
        const auto &conv = _convs[ri.convIdx];
        const auto  hit  = _huddleHitRects.constFind(conv.id.value);
        if (hit != _huddleHitRects.constEnd() && hit->contains(e->pos())) {
            emit joinHuddleRequested(conv.id);
            break;
        }
        setSelected(row);
        break;
    }
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
    _tooltipRow = -1;
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
    // Apps/bots (incl. Slackbot/system accounts) can't go offline — no presence dot.
    if (infoIt != _userInfos.constEnd() && infoIt->isBot)
        state.showPresence = false;
    // Services without a presence concept (email/IMAP) draw no dot at all.
    if (!_session || !_session->capabilities().presence)
        state.showPresence = false;
    UserAvatar::paint(
        p, rect, pixmap, initial, state, kAvatarRadius, p.device()->devicePixelRatioF(), bgColor
    );
}

// ── Painting ──────────────────────────────────────────────────────────────────

void ConvListWidget::doPaint(QPaintEvent *event) {
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);

    // Keep the pre-baked nav icons matched to the surface's live device pixel
    // ratio. They're baked once at construction, but on Wayland the fractional
    // scale isn't known until after the first show, so a construction-time bake
    // would leave them upscaled (pixelated) on a fractional display. Re-bake
    // when the painted DPR changes (also covers moving the window between
    // monitors of different scale).
    const qreal dpr = p.device()->devicePixelRatioF();
    if (!qFuzzyCompare(dpr, _iconDpr)) {
        _iconDpr = dpr;
        rebuildIconPixmaps();
    }
    p.fillRect(
        event->rect(),
        Th::navGradient(viewport(), Th::c().nav.primaryGradTop, Th::c().nav.primaryGradBottom)
    );

    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    const int first = scrollY / kRowH;
    const int last  = std::min(static_cast<int>(_rows.size()) - 1, (scrollY + vh) / kRowH);

    _huddleHitRects.clear(); // repopulated by paintRow for visible huddle rows
    _truncNameRects.clear(); // repopulated by paintRow for elided names
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

    QFont                   font       = QApplication::font();
    // A muted conversation reads as fully silent — no bold/bright "unread"
    // emphasis even when it holds an @mention.
    const NotificationLevel lvl        = effectiveNotifLevel(conv, _defaultNotify);
    const bool              isUnread   = conv.unread > 0 && lvl != NotificationLevel::Mute;
    const bool              isSelected = (row == _selected);
    font.setWeight(isUnread ? QFont::DemiBold : QFont::Normal);
    p.setFont(font);

    const QColor textColor = isSelected ? Th::c().nav.itemSelectedText
                             : isUnread ? Th::c().text.onDark
                                        : Th::c().text.onDarkDim;
    p.setPen(textColor);

    const QFontMetrics fm(font);
    const int          textY = y + (kRowH - fm.height()) / 2 + fm.ascent();

    const bool isDm     = (conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim);
    // Red badge = @mentions / DM unreads (only when not muted). Its number is the
    // count of things you were actually notified about: DM unreads, or channel
    // @mentions.
    const int  redCount = isDm ? conv.unread : conv.mentionCount;
    // A locally-muted person keeps the bold "unread" emphasis above but shows no
    // counter badge — the mute kills every outward count, red or blue.
    const bool showRed  = !conv.locallyMuted && lvl != NotificationLevel::Mute && redCount > 0;
    // Blue dot = other *allowed* activity: non-@mention unreads, but only in
    // channels set to "All new posts" (a "Just mentions" or muted channel stays
    // quiet for regular messages — no badge at all).
    const bool showBlue = !conv.locallyMuted && lvl == NotificationLevel::All && !isDm &&
                          conv.mentionCount == 0 && conv.unread > 0;
    const int badgeW = showRed ? (redCount > 9 ? 28 : 20) : showBlue ? 14 : 0;

    // Live-huddle indicator (host avatar + accent pill with headphones + count),
    // right-aligned like Slack. Compute its width up front so the name doesn't
    // run under it. Drawn at the tail of this function.
    const QString huddleCount = conv.huddleParticipants.empty()
                                    ? QString()
                                    : QString::number(int(conv.huddleParticipants.size()));
    int           huddlePillW = 0, huddleW = 0;
    if (conv.huddleActive) {
        QFont cf = font;
        cf.setPointSizeF(cf.pointSizeF() * 0.78);
        cf.setBold(true);
        const int countW =
            huddleCount.isEmpty() ? 0 : QFontMetrics(cf).horizontalAdvance(huddleCount) + 4;
        huddlePillW = kHuddlePad + kHuddleIcon + countW + kHuddlePad;
        huddleW = huddlePillW + (conv.huddleParticipants.empty() ? 0 : (kAvatarSize + kHuddleGap));
    }

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
        const int     maxW  = viewport()->width() - nameX - badgeW - huddleW - 14;
        const QString full  = resolvedName(row);
        const QString name  = fm.elidedText(full, Qt::ElideRight, maxW);
        p.setFont(font);
        p.setPen(textColor);
        p.drawText(nameX, textY, name);
        if (name != full)
            _truncNameRects.insert(row, QRect(nameX, y, fm.horizontalAdvance(name), kRowH));
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

        const bool isExternal = (infoIt != _userInfos.constEnd()) && infoIt->isExternal;
        QFont      extFont    = font;
        extFont.setPointSizeF(extFont.pointSizeF() * 0.62);
        extFont.setBold(true);
        const QFontMetrics extFm(extFont);
        const QString      extLabel = tr("EXT");
        const int          extPillW = extFm.horizontalAdvance(extLabel) + 8;

        int suffixW = 0;
        if (isExternal)
            suffixW += extPillW + 6;
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

        const int     maxW = viewport()->width() - nameX - suffixW - badgeW - huddleW - 14;
        const QString full = resolvedName(row);
        const QString name = fm.elidedText(full, Qt::ElideRight, maxW);
        p.setPen(textColor);
        p.drawText(nameX, textY, name);
        if (name != full)
            _truncNameRects.insert(row, QRect(nameX, y, fm.horizontalAdvance(name), kRowH));
        int curX = nameX + fm.horizontalAdvance(name);

        if (isExternal) {
            curX += 6;
            const int   bH = 14;
            const QRect bRect(curX, y + (kRowH - bH) / 2, extPillW, bH);
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().nav.extBadgeBg);
            p.drawRoundedRect(bRect, 2, 2);
            p.setFont(extFont);
            p.setPen(Th::c().nav.extBadgeText);
            p.drawText(bRect, Qt::AlignCenter, extLabel);
            p.restore();
            p.setFont(font);
            curX = bRect.right();
        }

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
        const int     maxW = viewport()->width() - leftX - prefixW - badgeW - huddleW - 14;
        const QString name = fm.elidedText(conv.name, Qt::ElideRight, maxW);
        p.drawText(leftX + prefixW, textY, name);
        if (name != conv.name)
            _truncNameRects.insert(
                row, QRect(leftX + prefixW, y, fm.horizontalAdvance(name), kRowH)
            );
    }

    // ── Right-side indicators (live huddle, then unread) ──────────────
    int rightEdge = viewport()->width() - 14; // inner right padding

    // Live huddle: host avatar + accent pill (headphones + participant count),
    // mirroring Slack's sidebar indicator. Anchored to the right edge.
    if (conv.huddleActive) {
        const int pillH = 18;
        const int pillX = rightEdge - huddlePillW;
        const int pillY = y + (kRowH - pillH) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().accent.def);
        Paint::pill(p, QRect(pillX, pillY, huddlePillW, pillH));
        p.drawPixmap(pillX + kHuddlePad, pillY + (pillH - kHuddleIcon) / 2, _iconPx.huddle);
        if (!huddleCount.isEmpty()) {
            QFont cf = font;
            cf.setPointSizeF(cf.pointSizeF() * 0.78);
            cf.setBold(true);
            p.setFont(cf);
            p.setPen(Th::c().accent.text);
            p.drawText(
                QRect(pillX + kHuddlePad + kHuddleIcon, pillY, huddlePillW, pillH),
                Qt::AlignVCenter | Qt::AlignLeft,
                huddleCount
            );
        }
        rightEdge = pillX - kHuddleGap;

        if (!conv.huddleParticipants.empty()) {
            const int avX = rightEdge - kAvatarSize;
            const int avY = y + (kRowH - kAvatarSize) / 2;
            drawUserAvatar(
                p,
                QRect(avX, avY, kAvatarSize, kAvatarSize),
                conv.huddleParticipants.front().value,
                Th::c().accent.def,
                isSelected
            );
            rightEdge = avX - kHuddleGap;
        }

        // Whole avatar+pill group is a click target → join the huddle.
        const int groupLeft = rightEdge + kHuddleGap;
        _huddleHitRects.insert(
            conv.id.value, QRect(groupLeft, y, viewport()->width() - 14 - groupLeft, kRowH)
        );
    }

    // ── Unread indicator (left of the huddle indicator if both present) ──
    if (showRed) {
        // Red numbered badge for DM unreads and channel @mentions.
        const QString badge = redCount > 99 ? "99+" : QString::number(redCount);
        QFont         bf    = font;
        bf.setPointSizeF(bf.pointSizeF() * 0.78);
        bf.setBold(true);
        p.setFont(bf);
        const QFontMetrics bfm(bf);
        const int          bh = bfm.height() + 4;
        // Never narrower than tall: a single-digit badge is a circle (w == h),
        // multi-digit grows into a pill — instead of a squeezed oval.
        const int          bw = qMax(bfm.horizontalAdvance(badge) + 10, bh);
        const int          bx = rightEdge - bw;
        const int          by = y + (kRowH - bh) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().badge.mention);
        Paint::pill(p, QRect(bx, by, bw, bh));
        p.setPen(Qt::white);
        p.drawText(QRect(bx, by, bw, bh), Qt::AlignCenter, badge);
    } else if (showBlue) {
        // Blue dot for other allowed activity in "All new posts" channels.
        const int dotD = 8;
        const int bx   = rightEdge - dotD;
        const int by   = y + (kRowH - dotD) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().badge.activity);
        p.drawEllipse(bx, by, dotD, dotD);
    }
}
