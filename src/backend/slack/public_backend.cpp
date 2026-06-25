// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "public_backend.h"
#include "json_mappers.h"
#include "slack_auth.h"
#include "socket_mode_realtime.h"
#include "auth/token_store.h"
#include "backend/common_commands.h"

#include <QUrlQuery>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QTimer>
#include <QDebug>

namespace slack {

PublicBackend::PublicBackend(
    const Credentials &creds, const AppConfig &appCfg, const QString &refreshUrl
)
    : _teamId(creds.teamId), _refreshToken(creds.refreshToken), _refreshUrl(refreshUrl),
      _api(new WebApiClient(nullptr)), _historyApi(new WebApiClient(nullptr)),
      _infoApi(new WebApiClient(nullptr)) {
    // The shared app-level Socket Mode socket (null if no xapp token is set).
    _sharedRealtime = _realtimeHandle.socket();

    _api->setToken(creds.xoxp);
    _historyApi->setToken(creds.xoxp);
    _infoApi->setToken(creds.xoxp);
    // Pre-warm TLS so the first API calls skip the handshake latency. The info
    // client is not pre-warmed: its background sweep starts well after launch.
    _api->preWarm("slack.com");
    _historyApi->preWarm("slack.com");

    // Surface HTTP 429s to the UI (transient notice). All three clients can be
    // throttled; the background-sweep _infoApi is the usual culprit. The sending
    // client is the QObject context, so the connection dies with it.
    for (WebApiClient *client : {_api, _historyApi, _infoApi})
        QObject::connect(
            client, &WebApiClient::rateLimited, client, [this](const QString &method, int secs) {
                _events.fire(EvRateLimited{method, secs});
            }
        );

    // Always install the handler so token_expired triggers logout/refresh even
    // when the stored token has no companion refresh token yet.
    setupTokenRefresh(creds, appCfg);
}

void PublicBackend::setupTokenRefresh(const Credentials &creds, const AppConfig &appCfg) {
    _appCfg         = appCfg;
    _tokenExpiresAt = creds.expiresAt;

    // Reactive: WebApiClient calls this on token_expired API error
    auto handler = [this](std::function<void(bool)> done) {
        qDebug() << "[TokenRefresh] token_expired received";
        triggerRefresh(std::move(done));
    };
    _api->setOnTokenExpired(handler);
    _historyApi->setOnTokenExpired(handler);
    _infoApi->setOnTokenExpired(handler);

    // Proactive: refresh before expiry so users never see a token_expired error.
    // A periodic wall-clock check rather than one long single-shot timer: Qt
    // timers run on the monotonic clock, which pauses during system suspend, so
    // a multi-hour timer fires hours late after a night of sleep. The periodic
    // check also retries transient refresh failures automatically.
    if (!_refreshToken.isEmpty()) {
        _proactiveRefreshTimer = new QTimer(_api);
        _proactiveRefreshTimer->setInterval(60 * 1000);
        QObject::connect(_proactiveRefreshTimer, &QTimer::timeout, _api, [this]() {
            maybeProactiveRefresh();
        });
        _proactiveRefreshTimer->start();
        if (_tokenExpiresAt > 0) {
            const qint64 secsLeft = _tokenExpiresAt - QDateTime::currentSecsSinceEpoch();
            qDebug() << "[TokenRefresh] token healthy, valid for" << secsLeft
                     << "s more; will auto-refresh in" << std::max<qint64>(secsLeft - 3600, 0)
                     << "s";
        }
        // First check is deferred: doRefresh is virtual and must not be
        // dispatched from within the constructor.
        QTimer::singleShot(0, _api, [this]() { maybeProactiveRefresh(); });
    }
}

void PublicBackend::triggerRefresh(std::function<void(bool)> done) {
    qDebug() << "[TokenRefresh] triggerRefresh; inProgress=" << _refreshInProgress
             << "refreshToken present=" << !_refreshToken.isEmpty();
    _refreshWaiters.push_back(std::move(done));
    if (_refreshInProgress) {
        qDebug() << "[TokenRefresh] refresh already in flight, queuing";
        return;
    }
    _refreshInProgress = true;
    doRefresh([this](RefreshResult result) {
        qDebug() << "[TokenRefresh] doRefresh completed, result="
                 << (result == RefreshResult::Success          ? "success"
                     : result == RefreshResult::TransientError ? "transient error"
                                                               : "auth error");
        _refreshInProgress = false;
        auto waiters       = std::move(_refreshWaiters);
        for (auto &w : waiters)
            w(result == RefreshResult::Success);
        if (result == RefreshResult::AuthError) {
            // Credentials definitively rejected — stop retrying, show login.
            if (_proactiveRefreshTimer)
                _proactiveRefreshTimer->stop();
            // Drain every client's pending queue before tearing anything down.
            // The token_expired waiters above only drain the client that
            // actually got the expiry; a refresh kicked off proactively (no-op
            // waiter) leaves queued calls — and their self-referential paginate
            // Ctx — orphaned forever. Done before force_assign so the queues are
            // emptied while the backend is still alive (force_assign may notify
            // subscribers that tear the session down synchronously).
            _api->failAllPending("token_expired");
            _historyApi->failAllPending("token_expired");
            _infoApi->failAllPending("token_expired");
            _authState.force_assign(AuthState::NotLoggedIn);
        }
        // TransientError: stay logged in — the periodic check retries within 60 s.
    });
}

void PublicBackend::maybeProactiveRefresh() {
    if (_refreshToken.isEmpty() || _tokenExpiresAt == 0 || _refreshInProgress)
        return;

    const qint64 secsLeft = _tokenExpiresAt - QDateTime::currentSecsSinceEpoch();
    if (secsLeft > 3600)
        return;

    qDebug() << "[TokenRefresh] proactive: token expires in" << secsLeft << "s, refreshing now";
    triggerRefresh([](bool) {});
}

void PublicBackend::doRefresh(std::function<void(RefreshResult)> done) {
    if (_refreshToken.isEmpty()) {
        qDebug() << "[TokenRefresh] no refresh token stored — forcing logout";
        done(RefreshResult::AuthError);
        return;
    }
    qDebug() << "[TokenRefresh] posting oauth.v2.access for team" << _teamId;

    // oauth.v2.access with grant_type=refresh_token is the rotation refresh
    // call; oauth.v2.exchange only migrates a legacy token and rejects this
    // grant.
    const QUrl endpoint{
        _refreshUrl.isEmpty() ? QStringLiteral("https://slack.com/api/oauth.v2.access")
                              : _refreshUrl
    };
    auto           *nam = new QNetworkAccessManager(_api); // _api owns it → cleaned up with backend
    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QUrlQuery body;
    body.addQueryItem("grant_type", "refresh_token");
    body.addQueryItem("client_id", _appCfg.clientId);
    body.addQueryItem("client_secret", _appCfg.clientSecret);
    body.addQueryItem("refresh_token", _refreshToken);
    auto *reply = nam->post(req, body.toString(QUrl::FullyEncoded).toUtf8());

    QObject::connect(reply, &QNetworkReply::finished, _api, [this, reply, nam, done]() mutable {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[TokenRefresh] network error:" << reply->errorString();
            done(RefreshResult::TransientError);
            return;
        }
        const auto raw = reply->readAll();
        auto       obj = QJsonDocument::fromJson(raw).object();
        qDebug() << "[TokenRefresh] refresh response:" << raw;
        if (!obj.value("ok").toBool()) {
            const QString err = obj.value("error").toString();
            qWarning() << "[TokenRefresh] Slack error:" << err;
            // Server-side hiccups are retried later; anything else (e.g.
            // invalid_refresh_token) means the credentials are dead.
            const bool transient = err == QLatin1String("internal_error") ||
                                   err == QLatin1String("service_unavailable") ||
                                   err == QLatin1String("fatal_error") ||
                                   err == QLatin1String("ratelimited");
            done(transient ? RefreshResult::TransientError : RefreshResult::AuthError);
            return;
        }
        const QString newToken   = obj.value("access_token").toString();
        const QString newRefresh = obj.value("refresh_token").toString();
        qDebug() << "[TokenRefresh] new access_token prefix:" << newToken.left(20)
                 << "... refresh_token present=" << !newRefresh.isEmpty();
        if (newToken.isEmpty()) {
            qWarning() << "[TokenRefresh] empty access_token in successful response";
            done(RefreshResult::TransientError);
            return;
        }

        // Update in-memory state
        _api->setToken(newToken);
        _historyApi->setToken(newToken);
        _infoApi->setToken(newToken);
        _refreshToken          = newRefresh.isEmpty() ? _refreshToken : newRefresh;
        const qint64 expiresIn = obj.value("expires_in").toInteger(0);
        if (expiresIn > 0)
            _tokenExpiresAt = QDateTime::currentSecsSinceEpoch() + expiresIn;

        // Persist atomically. Decode the existing record's Slack auth blob,
        // update only the rotated token fields, re-encode — displayName/iconUrl
        // are preserved.
        const WorkspaceKey key{Service::Slack, _teamId};
        Credentials        saved = fromRecord(
            TokenStore::loadWorkspace(key).value_or(TokenStore::WorkspaceRecord{key, {}, {}, {}})
        );
        saved.xoxp         = newToken;
        saved.refreshToken = _refreshToken;
        saved.expiresAt    = _tokenExpiresAt;
        TokenStore::saveWorkspace(toRecord(saved));

        qDebug() << "[TokenRefresh] token refreshed successfully for team" << _teamId
                 << "next expiry in" << expiresIn << "s";
        done(RefreshResult::Success);
    });
}

PublicBackend::~PublicBackend() {
    // Drop our sink before _realtimeHandle releases (and possibly destroys) the
    // shared socket.
    if (_sharedRealtime)
        _sharedRealtime->removeSink(&_events);
    delete _infoApi;
    delete _historyApi;
    delete _api;
}

void PublicBackend::setApiBaseUrlForTests(const QString &url) {
    _api->setBaseUrl(url);
    _historyApi->setBaseUrl(url);
    _infoApi->setBaseUrl(url);
}

rpl::producer<AuthState> PublicBackend::authState() const {
    return _authState.value();
}

Capabilities PublicBackend::capabilities() const {
    // What the public API path supports today. typing + livePresence stay false:
    // live "is typing" and realtime presence_change need the internal path
    // (Phase 5) — the public path only polls presence. Everything else listed
    // here is already live in the UI, so reporting it true keeps behavior
    // unchanged now that the UI gates on these flags.
    Capabilities c;
    c.presence      = true; // polled presence (users.getPresence) → online/away dots
    c.huddles       = true;
    c.canvases      = true;
    c.slashCommands = true;
    c.reactions     = true;
    c.editMessage   = true;
    c.deleteMessage = true;
    c.threads       = true;
    c.fileUpload    = true;
    return c;
}

bool PublicBackend::isSyntheticUser(UserId id) const {
    // Slack's two built-in pseudo-accounts: USLACKBOT (Slackbot) and USLACK (the
    // "Slack" workspace/billing notifier). Both are absent from users.list and
    // both report is_bot=false, so only the fixed ids identify them.
    return id.value == QLatin1String("USLACKBOT") || id.value == QLatin1String("USLACK");
}

bool PublicBackend::isBotId(UserId id) const {
    return id.value.startsWith('B');
}

bool PublicBackend::isUserId(UserId id) const {
    // Human users are "U…" on a normal workspace and "W…" on Enterprise Grid;
    // external Slack Connect collaborators surface with either prefix. Accept
    // both so they're resolved via the user-info path (mirrors the U/W test in
    // isUnresolvedUserId) — a W-only omission left Connect partners stuck as a
    // raw id with no name or avatar.
    return id.value.startsWith('U') || id.value.startsWith('W');
}

bool PublicBackend::isUnresolvedUserId(const QString &s) const {
    // Looks like a raw Slack user id ("U0A1B2C3D" / "W…" for enterprise) rather
    // than a human name — long enough, leading U/W, and otherwise [A-Z0-9].
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

void PublicBackend::connectRealtime() {
    if (!_sharedRealtime)
        return; // no xapp token configured → no realtime, same as before
    _sharedRealtime->addSink(&_events);
    _sharedRealtime->start();
}

void PublicBackend::disconnectRealtime() {
    if (_sharedRealtime)
        _sharedRealtime->removeSink(&_events);
}

void PublicBackend::verifyRealtime() {
    if (_sharedRealtime)
        _sharedRealtime->ensureConnected();
}

void PublicBackend::reestablishRealtime() {
    if (_sharedRealtime)
        _sharedRealtime->reconnectNow();
}

// ── Snapshot loads ────────────────────────────────────────────────

rpl::producer<UserId> PublicBackend::loadMe() {
    return [this](auto consumer) mutable {
        _api->call(
            "auth.test",
            QUrlQuery{},
            [this, consumer](QJsonObject resp) mutable {
                _meUserId = UserId{resp.value("user_id").toString()};
                _teamUrl  = resp.value("url").toString();
                consumer.put_next(UserId{resp.value("user_id").toString()});
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadMe error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<Conversation>> PublicBackend::loadConversations() {
    return [this](auto consumer) mutable {
        auto      accum = std::make_shared<std::vector<Conversation>>();
        QUrlQuery params;
        params.addQueryItem("types", "public_channel,private_channel,im,mpim");
        params.addQueryItem("exclude_archived", "true");
        // Slack's default page is 100; conversations.list is heavily rate-limited
        // (Tier 2), so pull the max 1000 per page to minimise the number of calls
        // a full reload costs (fewer pages = fewer chances to trip a 429).
        params.addQueryItem("limit", "1000");

        _api->paginate(
            "conversations.list",
            "channels",
            params,
            [accum](QJsonArray page) {
                auto batch = JsonMappers::toConversations(page);
                accum->insert(accum->end(), batch.begin(), batch.end());
            },
            [consumer, accum]() mutable {
                consumer.put_next(std::move(*accum));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadConversations error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<Conversation> PublicBackend::loadConversationInfo(ConversationId id) {
    return [this, id](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("channel", id.value);
        _infoApi->call(
            "conversations.info",
            params,
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toConversation(resp.value("channel").toObject()));
                consumer.put_done();
            },
            [consumer, id](QString err) mutable {
                qWarning() << "loadConversationInfo error:" << id.value << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<User>> PublicBackend::loadUsers() {
    return [this](auto consumer) mutable {
        auto accum = std::make_shared<std::vector<User>>();
        _historyApi->paginate(
            "users.list",
            "members",
            QUrlQuery{},
            [accum, myTeam = _teamId](QJsonArray page) {
                auto batch = JsonMappers::toUsers(page);
                for (auto &u : batch)
                    u.isExternal = u.isExternal ||
                                   (!u.teamId.isEmpty() && !myTeam.isEmpty() && u.teamId != myTeam);
                accum->insert(accum->end(), batch.begin(), batch.end());
            },
            [consumer, accum]() mutable {
                consumer.put_next(std::move(*accum));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadUsers error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<bool> PublicBackend::loadPresence(UserId userId) {
    return [this, userId](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("user", userId.value);
        _api->call(
            "users.getPresence",
            params,
            [consumer](QJsonObject resp) mutable {
                bool active = resp.value("presence").toString() == "active";
                consumer.put_next(std::move(active));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadPresence error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<SelfPresence> PublicBackend::loadSelfPresence() {
    return [this](auto consumer) mutable {
        // No "user" param → Slack returns the rich self snapshot
        // (online / auto_away / manual_away / connection_count).
        _api->call(
            "users.getPresence",
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toSelfPresence(resp));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadSelfPresence error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<User> PublicBackend::loadBotInfo(UserId botId) {
    return [this, botId](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("bot", botId.value);
        _api->call(
            "bots.info",
            params,
            [consumer](QJsonObject resp) mutable {
                const auto bot   = resp.value("bot").toObject();
                const auto icons = bot.value("icons").toObject();
                User       u;
                u.id          = UserId{bot.value("id").toString()};
                u.name        = bot.value("name").toString();
                u.displayName = bot.value("name").toString();
                u.avatarUrl =
                    icons.value("image_72")
                        .toString(
                            icons.value("image_48").toString(icons.value("image_36").toString())
                        );
                u.isBot = true;
                consumer.put_next(std::move(u));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadBotInfo error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<User> PublicBackend::loadUser(UserId userId) {
    return [this, userId](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("user", userId.value);
        _api->call(
            "users.info",
            params,
            [consumer, myTeam = _teamId](QJsonObject resp) mutable {
                auto u       = JsonMappers::toUser(resp.value("user").toObject());
                u.isExternal = u.isExternal ||
                               (!u.teamId.isEmpty() && !myTeam.isEmpty() && u.teamId != myTeam);
                consumer.put_next(std::move(u));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadUser error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

void PublicBackend::reconcileHuddleFromHistory(
    const ConversationId &conv, const QJsonArray &messages
) {
    // Find the newest huddle_thread message in this page (a busy channel can
    // hold several over time; only the latest reflects "now").
    QJsonObject newest;
    QString     newestTs;
    for (const auto &v : messages) {
        const auto m = v.toObject();
        if (m.value("subtype").toString() != QLatin1String("huddle_thread"))
            continue;
        const auto ts = m.value("ts").toString();
        if (newestTs.isEmpty() || ts.toDouble() > newestTs.toDouble()) {
            newest   = m;
            newestTs = ts;
        }
    }
    // Only act when the message carries a usable huddle `room`. If it's absent
    // (no huddle_thread in the window, or the token can't read `room`), stay
    // silent rather than emitting active=false and wiping a live huddle.
    const auto room = newest.value("room").toObject();
    if (room.isEmpty() || room.value("call_family").toString() != QLatin1String("huddle"))
        return;
    const auto h = JsonMappers::readHuddleRoom(room);
    _events.fire(EvHuddleChanged{conv, h.active, h.link, h.participants});
}

rpl::producer<MessagePage>
PublicBackend::loadHistory(ConversationId conv, std::optional<QString> cursor) {
    return [this, conv, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("channel", conv.value);
        params.addQueryItem("limit", "50");
        if (cursor)
            params.addQueryItem("cursor", *cursor);

        const bool firstPage = !cursor;
        _historyApi->call(
            "conversations.history",
            params,
            [this, conv, firstPage, consumer](QJsonObject resp) mutable {
                const auto messages = resp.value("messages").toArray();
                // Self-heal huddle state from the authoritative huddle_thread
                // message. Only on the newest page — older (scroll-up) pages may
                // hold a long-ended huddle that must not clobber a live one.
                if (firstPage)
                    reconcileHuddleFromHistory(conv, messages);
                MessagePage page;
                page.messages = JsonMappers::toMessages(messages);
                auto meta     = resp.value("response_metadata").toObject();
                auto next     = meta.value("next_cursor").toString();
                if (!next.isEmpty())
                    page.olderCursor = next;
                consumer.put_next(std::move(page));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadHistory error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<MessagePage>
PublicBackend::loadThread(ConversationId conv, Ts root, std::optional<QString> cursor) {
    return [this, conv, root, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("channel", conv.value);
        params.addQueryItem("ts", root);
        params.addQueryItem("limit", "50");
        if (cursor)
            params.addQueryItem("cursor", *cursor);

        _historyApi->call(
            "conversations.replies",
            params,
            [consumer](QJsonObject resp) mutable {
                MessagePage page;
                // conversations.replies returns oldest-first; no reversal needed.
                page.messages = JsonMappers::toMessages(resp.value("messages").toArray(), false);
                auto meta     = resp.value("response_metadata").toObject();
                auto next     = meta.value("next_cursor").toString();
                if (!next.isEmpty())
                    page.olderCursor = next;
                consumer.put_next(std::move(page));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadThread error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

// ── Self presence / status ────────────────────────────────────────

void PublicBackend::setPresence(bool away, std::function<void(bool, QString)> done) {
    QUrlQuery params;
    params.addQueryItem("presence", away ? "away" : "auto");
    _api->call(
        "users.setPresence",
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "setPresence error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::setStatus(
    const QString                     &emoji,
    const QString                     &text,
    qint64                             expirationTs,
    std::function<void(bool, QString)> done
) {
    const QJsonObject profile{
        {"status_text", text},
        {"status_emoji", emoji},
        {"status_expiration", expirationTs},
    };
    QUrlQuery params;
    params.addQueryItem(
        "profile", QString::fromUtf8(QJsonDocument(profile).toJson(QJsonDocument::Compact))
    );
    _api->call(
        "users.profile.set",
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "setStatus error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::setDndSnooze(int minutes, std::function<void(bool, QString)> done) {
    QUrlQuery params;
    if (minutes > 0)
        params.addQueryItem("num_minutes", QString::number(minutes));
    _api->call(
        minutes > 0 ? "dnd.setSnooze" : "dnd.endSnooze",
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "setDndSnooze error:" << e;
            if (done)
                done(false, e);
        }
    );
}

// ── Own profile ───────────────────────────────────────────────────

void PublicBackend::loadMyProfile(std::function<void(MyProfile)> done) {
    _api->call(
        "users.profile.get",
        QUrlQuery{},
        [done](QJsonObject resp) {
            const auto p = resp.value("profile").toObject();
            MyProfile  mp;
            mp.realName    = p.value("real_name").toString();
            mp.displayName = p.value("display_name").toString();
            mp.email       = p.value("email").toString();
            mp.phone       = p.value("phone").toString();
            mp.avatarUrl   = p.value("image_512").toString();
            if (mp.avatarUrl.isEmpty())
                mp.avatarUrl = p.value("image_192").toString();
            if (done)
                done(mp);
        },
        [done](QString e) {
            qWarning() << "loadMyProfile error:" << e;
            if (done)
                done({});
        }
    );
}

void PublicBackend::updateProfile(
    const QHash<QString, QString> &fields, std::function<void(bool, QString)> done
) {
    QJsonObject profile;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        profile.insert(it.key(), it.value());
    QUrlQuery params;
    params.addQueryItem(
        "profile", QString::fromUtf8(QJsonDocument(profile).toJson(QJsonDocument::Compact))
    );
    _api->call(
        "users.profile.set",
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "updateProfile error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::setPhoto(
    const QString &filePath, std::function<void(bool, QString, QString)> done
) {
    auto *file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        qWarning() << "setPhoto: cannot open" << filePath;
        delete file;
        if (done)
            done(false, QStringLiteral("cannot_open_file"), {});
        return;
    }

    auto         *mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart     imagePart;
    const QString fileName = QFileInfo(filePath).fileName();
    imagePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"image\"; filename=\"%1\"").arg(fileName))
    );
    file->setParent(mp); // closed/destroyed with the multipart
    imagePart.setBodyDevice(file);
    mp->append(imagePart);

    _api->postMultipart(
        "users.setPhoto",
        mp,
        [done](QJsonObject resp) {
            const auto profile = resp.value("profile").toObject();
            QString    url     = profile.value("image_512").toString();
            if (url.isEmpty())
                url = profile.value("image_192").toString();
            if (done)
                done(true, {}, url);
        },
        [done](QString e) {
            qWarning() << "setPhoto error:" << e;
            if (done)
                done(false, e, {});
        }
    );
}

// ── Slash commands ────────────────────────────────────────────────
// Both endpoints are undocumented official-client API (commands.list /
// chat.command) and may be rejected for OAuth tokens (missing legacy `post`
// scope). listCommands degrades to "produce nothing" — the Session merges in
// its built-in command set; runCommand reports the server error to the caller.

std::vector<SlashCommand> PublicBackend::nativeCommands() const {
    // The Slack-flavoured commands Session::runCommand executes natively. These are
    // Slack conventions (incl. /shrug, /mute), so they live here — not app-level —
    // and never appear in another service's composer.
    return CommonCommands::select(
        {"shrug", "mute", "active", "away", "dnd", "status", "msg", "dm", "leave"}
    );
}

rpl::producer<std::vector<SlashCommand>> PublicBackend::listCommands() {
    return [this](auto consumer) mutable {
        _api->call(
            "commands.list",
            {},
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toSlashCommands(resp.value("commands")));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                // not_allowed_token_type is the norm for OAuth tokens (see the
                // comment above) — built-ins take over, nothing to report.
                if (err != QLatin1String("not_allowed_token_type"))
                    qWarning() << "listCommands error:" << err;
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

void PublicBackend::runCommand(
    ConversationId                                conv,
    const QString                                &command,
    const QString                                &text,
    std::function<void(bool ok, QString message)> done
) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("command", command);
    if (!text.isEmpty())
        params.addQueryItem("text", text);
    _api->callNonIdempotent(
        "chat.command",
        params,
        [done](QJsonObject resp) {
            if (done)
                done(true, resp.value("response").toString());
        },
        [done](QString err) {
            qWarning() << "runCommand error:" << err;
            if (done)
                done(false, err);
        }
    );
}

// ── Commands ──────────────────────────────────────────────────────

// A chat.postMessage whose connection died mid-flight may or may not have
// reached Slack, and resending blindly is exactly what duplicates messages.
// Slack offers no idempotency key for API callers (client_msg_id is internal
// to first-party clients and ignored here), so the loop is:
//   post → ambiguous failure → wait with backoff → scan recent history for
//   the message (own author + same text, window anchored on the last server
//   ts seen before the send) → found: emit its echo / absent: post again.
// It repeats until delivered or Slack returns a definitive error, which
// surfaces as EvSendFailed.
struct PublicBackend::SendState {
    ConversationId  conv;
    OutgoingMessage msg;
    QString         wireText; // exactly what chat.postMessage was given
    QString         oldestTs; // exclusive lower bound of the reconcile scan
    int             attempts = 0;
};

namespace {
// Slack entity-escapes bare & < > in stored message text; unescape both
// sides so a sent text compares equal to its stored form.
QString unescapedText(QString t) {
    t.replace(QLatin1String("&lt;"), QLatin1String("<"))
        .replace(QLatin1String("&gt;"), QLatin1String(">"))
        .replace(QLatin1String("&amp;"), QLatin1String("&"));
    return t.trimmed();
}
} // namespace

void PublicBackend::sendMessage(ConversationId conv, OutgoingMessage msg) {
    auto st      = std::make_shared<SendState>();
    st->conv     = conv;
    st->wireText = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
    // Prefer the server-assigned anchor (immune to local clock skew); fall
    // back to the local clock minus a minute of slack for empty convs.
    st->oldestTs = msg.sinceTs.isEmpty() ? QString::number(QDateTime::currentSecsSinceEpoch() - 60)
                                         : msg.sinceTs;
    st->msg      = std::move(msg);
    postMessageAttempt(std::move(st));
}

void PublicBackend::postMessageAttempt(std::shared_ptr<SendState> st) {
    QUrlQuery params;
    params.addQueryItem("channel", st->conv.value);
    params.addQueryItem("text", st->wireText);
    if (st->msg.threadRoot)
        params.addQueryItem("thread_ts", *st->msg.threadRoot);
    _api->callNonIdempotent(
        "chat.postMessage",
        params,
        [this, st](QJsonObject resp) {
            // Confirm from the HTTP response instead of waiting for the
            // realtime echo — the websocket may be down while HTTP works.
            // Session drops the second copy when the echo arrives anyway.
            Message m = JsonMappers::toMessage(resp.value("message").toObject());
            if (m.ts.isEmpty())
                m.ts = resp.value("ts").toString();
            _events.fire(EvMessageNew{st->conv, std::move(m)});
        },
        [this, st](QString err) {
            if (err == WebApiClient::kConnectionLost) {
                st->attempts++;
                const int delay = qMin(_sendRetryDelayMs << qMin(st->attempts - 1, 6), 60'000);
                qDebug() << "sendMessage: connection lost mid-flight, reconciling in" << delay
                         << "ms";
                QTimer::singleShot(delay, _api, [this, st] { reconcileSend(st); });
                return;
            }
            qWarning() << "sendMessage error:" << err;
            _events.fire(EvSendFailed{st->conv, err});
        }
    );
}

void PublicBackend::reconcileSend(std::shared_ptr<SendState> st) {
    const bool inThread = st->msg.threadRoot.has_value();
    QUrlQuery  params;
    params.addQueryItem("channel", st->conv.value);
    params.addQueryItem("oldest", st->oldestTs);
    params.addQueryItem("limit", "100");
    if (inThread)
        params.addQueryItem("ts", *st->msg.threadRoot);
    _api->call(
        inThread ? "conversations.replies" : "conversations.history",
        params,
        [this, st, inThread](QJsonObject resp) {
            const QString want = unescapedText(st->wireText);
            for (const auto v : resp.value("messages").toArray()) {
                const auto o = v.toObject();
                if (inThread && o.value("ts").toString() == *st->msg.threadRoot)
                    continue; // the thread root itself, not a reply
                if (!_meUserId.value.isEmpty() && o.value("user").toString() != _meUserId.value)
                    continue;
                if (unescapedText(o.value("text").toString()) != want)
                    continue;
                qDebug() << "sendMessage: message" << o.value("ts").toString()
                         << "was delivered after all — not resending";
                _events.fire(EvMessageNew{st->conv, JsonMappers::toMessage(o)});
                return;
            }
            postMessageAttempt(st); // genuinely missing — safe to post again
        },
        [this, st](QString err) {
            // Transport failures retry inside WebApiClient and never reach
            // here; a Slack-level error means the conversation itself is
            // unusable (gone, kicked, …) — resending would fail the same way.
            qWarning() << "sendMessage reconcile error:" << err;
            _events.fire(EvSendFailed{st->conv, err});
        }
    );
}

void PublicBackend::reconcileUpload(
    const ConversationId &conv, const QSet<QString> &fileIds, int attempt
) {
    if (fileIds.isEmpty())
        return;
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    // The share lands at "now"; a small window absorbs clock skew and history
    // lag without trawling the whole channel.
    params.addQueryItem("oldest", QString::number(QDateTime::currentSecsSinceEpoch() - 120));
    params.addQueryItem("limit", "30");
    _api->call(
        "conversations.history",
        params,
        [this, conv, fileIds, attempt](QJsonObject resp) {
            for (const auto v : resp.value("messages").toArray()) {
                const auto o = v.toObject();
                if (!_meUserId.value.isEmpty() && o.value("user").toString() != _meUserId.value)
                    continue;
                bool match = false;
                for (const auto fv : o.value("files").toArray()) {
                    if (fileIds.contains(fv.toObject().value("id").toString())) {
                        match = true;
                        break;
                    }
                }
                if (!match)
                    continue;
                // Found the shared message — emit its echo. Session de-ghosts
                // the optimistic copy and dedups the later realtime echo by ts.
                _events.fire(EvMessageNew{conv, JsonMappers::toMessage(o)});
                return;
            }
            // Not yet visible — a heavy share routinely lags conversations.history
            // by a beat. Retry with backoff so de-ghosting doesn't hinge on the
            // realtime echo (which may be delayed or dropped). The later echo —
            // from a retry here or from the websocket — is deduped by ts in
            // Session, so an extra scan is harmless once one path succeeds.
            constexpr int kMaxUploadReconcileRetries = 6;
            if (attempt < kMaxUploadReconcileRetries) {
                const int delay = qMin(_sendRetryDelayMs << attempt, 60'000);
                QTimer::singleShot(delay, _api, [this, conv, fileIds, attempt] {
                    reconcileUpload(conv, fileIds, attempt + 1);
                });
            }
        },
        [conv](QString err) { qWarning() << "reconcileUpload error:" << conv.value << err; }
    );
}

void PublicBackend::editMessage(ConversationId conv, Ts ts, TextWithEntities text) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts", ts);
    params.addQueryItem("text", text.text);
    _api->call("chat.update", params, {}, [](QString e) {
        qWarning() << "editMessage error:" << e;
    });
}

void PublicBackend::deleteMessage(ConversationId conv, Ts ts) {
    deleteMessageAttempt(conv, ts, 0);
}

void PublicBackend::deleteMessageAttempt(ConversationId conv, Ts ts, int attempts) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts", ts);
    // POST (non-idempotent): Qt silently retransmits a GET whose connection
    // died mid-flight, which would fire a second chat.delete that comes back
    // message_not_found. Deletion is naturally idempotent, but routing it as a
    // write method keeps it off the auto-retransmit path and consistent with
    // the other chat.* writes.
    _api->callNonIdempotent(
        "chat.delete",
        params,
        [this, conv, ts](QJsonObject) {
            // Confirm the deletion from the response itself rather than waiting
            // for the realtime message_deleted echo, which may never arrive if
            // the socket is recycling. Session dedups EvMessageDeleted, so the
            // later realtime frame collapses into this one harmlessly.
            _events.fire(EvMessageDeleted{conv, ts});
        },
        [this, conv, ts, attempts](QString e) {
            if (e == QLatin1String("message_not_found")) {
                // The message is already gone server-side (deleted elsewhere, or
                // a stale local copy such as an unreconciled optimistic send) —
                // the user's intent is satisfied either way, so drop it locally
                // as if the delete succeeded.
                _events.fire(EvMessageDeleted{conv, ts});
                return;
            }
            if (e == WebApiClient::kConnectionLost) {
                // Ambiguous mid-flight failure of a POST. The delete may or may
                // not have applied, but it is idempotent (a re-delete just yields
                // message_not_found, handled above), so resending is always safe.
                // Bounded backoff so a persistent outage doesn't loop forever.
                constexpr int kMaxDeleteRetries = 6;
                if (attempts < kMaxDeleteRetries) {
                    const int delay = qMin(_sendRetryDelayMs << attempts, 60'000);
                    QTimer::singleShot(delay, _api, [this, conv, ts, attempts] {
                        deleteMessageAttempt(conv, ts, attempts + 1);
                    });
                    return;
                }
            }
            qWarning() << "deleteMessage error:" << e;
        }
    );
}

void PublicBackend::deleteFile(const QString &fileId) {
    QUrlQuery params;
    params.addQueryItem("file", fileId);
    _api->call("files.delete", params, {}, [](QString e) {
        qWarning() << "deleteFile error:" << e;
    });
}

void PublicBackend::addReaction(ConversationId conv, Ts ts, QString emoji) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("timestamp", ts);
    params.addQueryItem("name", emoji);
    _api->call("reactions.add", params, {}, [](QString e) {
        qWarning() << "addReaction error:" << e;
    });
}

void PublicBackend::removeReaction(ConversationId conv, Ts ts, QString emoji) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("timestamp", ts);
    params.addQueryItem("name", emoji);
    _api->call("reactions.remove", params, {}, [](QString e) {
        qWarning() << "removeReaction error:" << e;
    });
}

void PublicBackend::markRead(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts", ts);
    _api->call("conversations.mark", params, {}, [](QString e) {
        qWarning() << "markRead error:" << e;
    });
}

void PublicBackend::sendTyping(ConversationId) {
    // users.typing was removed from the Slack Web API; no-op.
}

void PublicBackend::scheduleMessage(ConversationId conv, OutgoingMessage msg, qint64 postAt) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("text", msg.text.text);
    params.addQueryItem("post_at", QString::number(postAt));
    if (msg.threadRoot)
        params.addQueryItem("thread_ts", *msg.threadRoot);
    _api->callNonIdempotent("chat.scheduleMessage", params, {}, [](QString e) {
        qWarning() << "scheduleMessage error:" << e;
    });
}

void PublicBackend::pinMessage(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("timestamp", ts);
    _api->call("pins.add", params, {}, [](QString e) { qWarning() << "pinMessage error:" << e; });
}

void PublicBackend::unpinMessage(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("timestamp", ts);
    _api->call("pins.remove", params, {}, [](QString e) {
        qWarning() << "unpinMessage error:" << e;
    });
}

void PublicBackend::starConversation(ConversationId conv, bool star) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    _api->call(star ? "stars.add" : "stars.remove", params, {}, [star](QString e) {
        qWarning() << (star ? "starConversation" : "unstarConversation") << "error:" << e;
    });
}

void PublicBackend::leaveConversation(ConversationId conv) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    _api->call("conversations.leave", params, {}, [](QString e) {
        qWarning() << "leaveConversation error:" << e;
    });
}

void PublicBackend::createChannel(
    const QString                      &name,
    bool                                isPrivate,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    QJsonObject body;
    body["name"]       = name;
    body["is_private"] = isPrivate;
    _api->postJson(
        "conversations.create",
        body,
        [onSuccess](QJsonObject resp) {
            const QString id = resp.value("channel").toObject().value("id").toString();
            if (!id.isEmpty() && onSuccess)
                onSuccess(ConversationId{id});
        },
        [onError](QString e) {
            qWarning() << "createChannel error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::joinChannel(
    ConversationId                      id,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    QJsonObject body;
    body["channel"] = id.value;
    _api->postJson(
        "conversations.join",
        body,
        [id, onSuccess](QJsonObject) {
            if (onSuccess)
                onSuccess(id);
        },
        [onError](QString e) {
            qWarning() << "joinChannel error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::openDm(
    UserId user, std::function<void(ConversationId)> onSuccess, std::function<void(QString)> onError
) {
    QJsonObject body;
    body["users"] = user.value;
    _api->postJson(
        "conversations.open",
        body,
        [onSuccess](QJsonObject resp) {
            if (onSuccess)
                onSuccess(ConversationId{resp.value("channel").toObject().value("id").toString()});
        },
        [onError](QString e) {
            qWarning() << "openDm error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::subscribePresence(std::vector<UserId> userIds) {
    SocketModeRealtime *rt = _sharedRealtime;
    if (!rt)
        return;
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(userIds.size()));
    for (const auto &u : userIds)
        ids.append(u.value);
    rt->subscribePresence(&_events, std::move(ids));
}

rpl::producer<Event> PublicBackend::events() const {
    return _events.events();
}

// ── Phase 3 ───────────────────────────────────────────────────────

rpl::producer<std::vector<SearchResult>> PublicBackend::searchMessages(const QString &query) {
    return [this, query](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("query", query);
        params.addQueryItem("count", "20");

        _api->call(
            "search.messages",
            params,
            [consumer](QJsonObject resp) mutable {
                auto msgs = resp.value("messages").toObject().value("matches").toArray();
                consumer.put_next(JsonMappers::toSearchResults(msgs));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "searchMessages error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<QHash<QString, QString>> PublicBackend::loadEmojiList() {
    return [this](auto consumer) mutable {
        _api->call(
            "emoji.list",
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                QHash<QString, QString> map;
                const auto              emoji = resp.value("emoji").toObject();
                for (auto it = emoji.begin(); it != emoji.end(); ++it)
                    map.insert(it.key(), it.value().toString());
                consumer.put_next(std::move(map));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadEmojiList error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

void PublicBackend::uploadFiles(
    ConversationId                              conv,
    const QStringList                          &filePaths,
    const QString                              &initialComment,
    std::function<void(bool ok, QString error)> done
) {
    // Slack external upload flow: per file, files.getUploadURLExternal then a
    // raw POST of the bytes to the returned URL; once every file has settled,
    // ONE files.completeUploadExternal shares them all as a single message
    // with initial_comment as the message text.
    struct Batch {
        int        pending = 0;
        QJsonArray files; // {id, title} of successfully uploaded files
    };
    auto batch  = std::make_shared<Batch>();
    auto settle = std::make_shared<std::function<void(bool, QString)>>(std::move(done));

    auto finishOne = [this, conv, initialComment, batch, settle]() {
        if (--batch->pending > 0)
            return;
        if (batch->files.isEmpty()) {
            // Every upload failed; warnings already logged.
            if (*settle)
                (*settle)(false, QStringLiteral("file upload failed"));
            return;
        }
        QJsonObject body;
        body["channel_id"] = conv.value;
        body["files"]      = batch->files;
        if (!initialComment.isEmpty())
            body["initial_comment"] = initialComment;

        // Collect the uploaded file ids so the post-upload reconcile can match
        // the shared message even when there's no initial_comment to compare.
        QSet<QString> fileIds;
        for (const auto v : std::as_const(batch->files))
            fileIds.insert(v.toObject().value("id").toString());

        _api->postJson(
            "files.completeUploadExternal",
            body,
            [this, conv, fileIds, settle](QJsonObject) {
                // The upload landed but the response carries no message ts;
                // reconcile from history to emit the echo that de-ghosts the
                // optimistic copy without waiting on the realtime websocket.
                reconcileUpload(conv, fileIds);
                if (*settle)
                    (*settle)(true, {});
            },
            [settle](QString err) {
                qWarning() << "completeUploadExternal error:" << err;
                if (*settle)
                    (*settle)(false, err);
            }
        );
    };

    for (const QString &filePath : filePaths) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "uploadFiles: cannot open" << filePath;
            continue;
        }
        const QByteArray data     = f.readAll();
        const QString    filename = QFileInfo(filePath).fileName();
        ++batch->pending;

        // Step 1: get upload URL (filename + length are the only request args)
        QUrlQuery params;
        params.addQueryItem("filename", filename);
        params.addQueryItem("length", QString::number(data.size()));

        _api->call(
            "files.getUploadURLExternal",
            params,
            [this, filename, data, finishOne, batch](QJsonObject resp) mutable {
                const QString uploadUrl = resp.value("upload_url").toString();
                const QString fileId    = resp.value("file_id").toString();

                // Step 2: POST bytes to the upload URL (no auth header)
                _api->rawPost(
                    QUrl(uploadUrl),
                    data,
                    [filename, fileId, finishOne, batch]() mutable {
                        QJsonObject entry;
                        entry["id"]    = fileId;
                        entry["title"] = filename;
                        batch->files.append(entry);
                        finishOne();
                    },
                    [finishOne](QString err) mutable {
                        qWarning() << "upload POST error:" << err;
                        finishOne();
                    }
                );
            },
            [finishOne](QString err) mutable {
                qWarning() << "getUploadURLExternal error:" << err;
                finishOne();
            }
        );
    }

    // No file could even be opened — finishOne will never run.
    if (batch->pending == 0 && *settle)
        (*settle)(false, QStringLiteral("could not read the selected files"));
}

void PublicBackend::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    _api->downloadUrl(QUrl(url), std::move(onData), std::move(onError));
}

void PublicBackend::loadChannelCanvas(ConversationId id, std::function<void(QString, bool)> done) {
    QUrlQuery params;
    params.addQueryItem("channel", id.value);
    _api->call(
        "conversations.info",
        params,
        [done](QJsonObject resp) {
            const auto [fileId, isEmpty] =
                JsonMappers::channelCanvas(resp.value("channel").toObject());
            if (done)
                done(fileId, isEmpty);
        },
        [done, id](QString err) {
            qWarning() << "loadChannelCanvas error:" << id.value << err;
            if (done)
                done({}, true);
        }
    );
}

void PublicBackend::loadCanvasContent(
    const QString                    &fileId,
    std::function<void(QString html)> onHtml,
    std::function<void(QString)>      onError
) {
    // files.info → url_private → authed GET; canvases come back as HTML
    // (content-type text/html, <div class="quip-canvas-content">…).
    QUrlQuery params;
    params.addQueryItem("file", fileId);
    _api->call(
        "files.info",
        params,
        [this, onHtml, onError](QJsonObject resp) {
            const QString url = resp.value("file").toObject().value("url_private").toString();
            if (url.isEmpty()) {
                if (onError)
                    onError(QStringLiteral("canvas has no url_private"));
                return;
            }
            _api->downloadUrl(
                QUrl(url),
                [onHtml](QByteArray data) {
                    if (onHtml)
                        onHtml(QString::fromUtf8(data));
                },
                onError
            );
        },
        [onError](QString err) {
            // not_visible is the routine "canvas owned elsewhere" case the caller
            // already turns into the read-only no-access notice; don't warn on it.
            if (err == QLatin1String("not_visible"))
                qDebug() << "loadCanvasContent:" << err << "(handled)";
            else
                qWarning() << "loadCanvasContent error:" << err;
            if (onError)
                onError(err);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::loadCanvasImage(
    const QString                  &fileId,
    std::function<void(QByteArray)> onData,
    std::function<void(QString)>    onError
) {
    // files.info → a sized thumbnail (preferred — the canvas renders inline
    // images downscaled) → authed GET. The relative blob URL in the HTML has
    // no host or token, so it can't be fetched directly.
    QUrlQuery params;
    params.addQueryItem("file", fileId);
    _api->call(
        "files.info",
        params,
        [this, onData, onError](QJsonObject resp) {
            const QJsonObject f = resp.value("file").toObject();
            QString           url;
            // Prefer the original (url_private): at HiDPI the column needs more
            // pixels than the 1024px thumbnail provides, so a thumb would look
            // upscaled. Thumbnails are only a fallback when there's no original.
            for (const char *key : {"url_private", "thumb_1024", "thumb_960", "thumb_800"}) {
                url = f.value(QLatin1String(key)).toString();
                if (!url.isEmpty())
                    break;
            }
            if (url.isEmpty()) {
                if (onError)
                    onError(QStringLiteral("canvas image has no url"));
                return;
            }
            _api->downloadUrl(
                QUrl(url),
                [onData](QByteArray data) {
                    if (onData)
                        onData(data);
                },
                onError
            );
        },
        [onError](QString err) {
            if (onError)
                onError(err);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::createChannelCanvas(
    ConversationId                      id,
    const QString                      &markdown,
    std::function<void(QString fileId)> onSuccess,
    std::function<void(QString)>        onError
) {
    QJsonObject body;
    body["channel_id"] = id.value;
    if (!markdown.isEmpty())
        body["document_content"] = QJsonObject{{"type", "markdown"}, {"markdown", markdown}};
    _api->postJson(
        "conversations.canvases.create",
        body,
        [onSuccess](QJsonObject resp) {
            if (onSuccess)
                onSuccess(resp.value("canvas_id").toString());
        },
        [onError](QString e) {
            qWarning() << "createChannelCanvas error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::loadCanvasMeta(
    const QString                                                               &fileId,
    std::function<void(QString title, QString permalink, CanvasMetaState state)> done
) {
    QUrlQuery params;
    params.addQueryItem("file", fileId);
    _api->call(
        "files.info",
        params,
        [done](QJsonObject resp) {
            const auto file = resp.value("file").toObject();
            if (done)
                done(
                    file.value("title").toString(),
                    file.value("permalink").toString(),
                    CanvasMetaState::Ok
                );
        },
        [done](QString err) {
            auto state = CanvasMetaState::Ok; // unknown error — assume still there
            if (err == QLatin1String("file_deleted") || err == QLatin1String("file_not_found"))
                state = CanvasMetaState::Gone;
            else if (err == QLatin1String("not_visible"))
                state = CanvasMetaState::NoAccess;
            // Gone/NoAccess are expected and handled by the caller (deleted-elsewhere
            // canvas, or a canvas owned by someone else — e.g. an app/bot DM canvas
            // or a channel the user hasn't joined). conversations.info advertises the
            // canvas file_id regardless of whether files.info will let the user view
            // it, so this is routine; only log genuinely unexpected errors.
            if (state == CanvasMetaState::Ok)
                qWarning() << "loadCanvasMeta error:" << err;
            else
                qDebug() << "loadCanvasMeta:" << err << "(handled)";
            if (done)
                done({}, {}, state);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::deleteCanvas(
    const QString &canvasId, std::function<void(bool ok, QString err)> done
) {
    QJsonObject body;
    body["canvas_id"] = canvasId;
    _api->postJson(
        "canvases.delete",
        body,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "deleteCanvas error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::editCanvas(
    const QString                            &canvasId,
    const std::vector<CanvasChange>          &changes,
    std::function<void(bool ok, QString err)> done
) {
    if (changes.empty()) {
        if (done)
            done(true, {});
        return;
    }
    // canvases.edit rejects more than one item in "changes" (verified:
    // "[ERROR] no more than 1 items allowed") — send the ops one call each,
    // in order, aborting the sequence on the first failure.
    auto queue = std::make_shared<std::deque<CanvasChange>>(changes.begin(), changes.end());
    sendNextCanvasChange(canvasId, std::move(queue), std::move(done));
}

void PublicBackend::sendNextCanvasChange(
    const QString                            &canvasId,
    std::shared_ptr<std::deque<CanvasChange>> queue,
    std::function<void(bool ok, QString err)> done
) {
    const CanvasChange change = queue->front();
    queue->pop_front();

    QJsonObject body;
    body["canvas_id"] = canvasId;
    body["changes"]   = JsonMappers::toCanvasChanges({change});
    _api->postJson(
        "canvases.edit",
        body,
        [this, canvasId, queue = std::move(queue), done](QJsonObject) mutable {
            if (queue->empty()) {
                if (done)
                    done(true, {});
                return;
            }
            sendNextCanvasChange(canvasId, std::move(queue), std::move(done));
        },
        [done](QString e) {
            qWarning() << "editCanvas error:" << e;
            if (done)
                done(false, e);
        }
    );
}

} // namespace slack
