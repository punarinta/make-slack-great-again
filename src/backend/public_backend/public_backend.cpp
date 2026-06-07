// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "public_backend.h"
#include "json_mappers.h"
#include "socket_mode_realtime.h"
#include "auth/token_store.h"

#include <QUrlQuery>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDebug>

PublicBackend::PublicBackend(
    const TokenStore::Credentials &creds,
    const TokenStore::AppConfig   &appCfg,
    const QString                 &xappToken
)
    : _xappToken(xappToken), _teamId(creds.teamId), _refreshToken(creds.refreshToken),
      _api(new WebApiClient(nullptr)), _historyApi(new WebApiClient(nullptr)) {
    _api->setToken(creds.xoxp);
    _historyApi->setToken(creds.xoxp);
    // Pre-warm TLS so the first API calls skip the handshake latency.
    _api->preWarm("slack.com");
    _historyApi->preWarm("slack.com");

    // Always install the handler so token_expired triggers logout/refresh even
    // when the stored token has no companion refresh token yet.
    setupTokenRefresh(creds, appCfg);
}

void PublicBackend::setupTokenRefresh(
    const TokenStore::Credentials &creds, const TokenStore::AppConfig &appCfg
) {
    // Shared handler: both clients call this; deduplication ensures only one
    // in-flight refresh request even if both fire simultaneously.
    auto handler = [this, appCfg](std::function<void(bool)> done) {
        qDebug() << "[TokenRefresh] token_expired received; refreshInProgress="
                 << _refreshInProgress << "refreshToken present=" << !_refreshToken.isEmpty();
        _refreshWaiters.push_back(std::move(done));
        if (_refreshInProgress) {
            qDebug() << "[TokenRefresh] refresh already in flight, queuing";
            return;
        }
        _refreshInProgress = true;
        doRefresh(appCfg, [this](bool success) {
            qDebug() << "[TokenRefresh] doRefresh completed, success=" << success;
            _refreshInProgress = false;
            auto waiters       = std::move(_refreshWaiters);
            for (auto &w : waiters)
                w(success);
            if (!success)
                _authState.force_assign(AuthState::NotLoggedIn);
        });
    };
    _api->setOnTokenExpired(handler);
    _historyApi->setOnTokenExpired(handler);
    Q_UNUSED(creds)
}

void PublicBackend::doRefresh(const TokenStore::AppConfig &appCfg, std::function<void(bool)> done) {
    if (_refreshToken.isEmpty()) {
        qDebug() << "[TokenRefresh] no refresh token stored — forcing logout";
        done(false);
        return;
    }
    qDebug() << "[TokenRefresh] posting oauth.v2.exchange for team" << _teamId;

    auto *nam = new QNetworkAccessManager(_api); // _api owns it → cleaned up with backend
    QNetworkRequest req(QUrl(QStringLiteral("https://slack.com/api/oauth.v2.exchange")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QUrlQuery body;
    body.addQueryItem("grant_type", "refresh_token");
    body.addQueryItem("client_id", appCfg.clientId);
    body.addQueryItem("client_secret", appCfg.clientSecret);
    body.addQueryItem("refresh_token", _refreshToken);
    auto *reply = nam->post(req, body.toString(QUrl::FullyEncoded).toUtf8());

    QObject::connect(reply, &QNetworkReply::finished, _api, [this, reply, nam, done]() mutable {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[TokenRefresh] network error:" << reply->errorString();
            done(false);
            return;
        }
        const auto raw = reply->readAll();
        auto       obj = QJsonDocument::fromJson(raw).object();
        qDebug() << "[TokenRefresh] exchange response:" << raw;
        if (!obj.value("ok").toBool()) {
            qWarning() << "[TokenRefresh] Slack error:" << obj.value("error").toString();
            done(false);
            return;
        }
        const QString newToken   = obj.value("access_token").toString();
        const QString newRefresh = obj.value("refresh_token").toString();
        qDebug() << "[TokenRefresh] new access_token prefix:" << newToken.left(20)
                 << "... refresh_token present=" << !newRefresh.isEmpty();
        if (newToken.isEmpty()) {
            qWarning() << "[TokenRefresh] empty access_token in successful response";
            done(false);
            return;
        }

        // Update in-memory tokens on both clients
        _api->setToken(newToken);
        _historyApi->setToken(newToken);
        _refreshToken = newRefresh.isEmpty() ? _refreshToken : newRefresh;

        // Persist the new credentials
        auto saved         = TokenStore::loadWorkspace(_teamId);
        saved.xoxp         = newToken;
        saved.refreshToken = _refreshToken;
        TokenStore::saveWorkspace(saved);

        qDebug() << "Token refreshed successfully for team" << _teamId;
        done(true);
    });
}

PublicBackend::~PublicBackend() {
    delete _realtime;
    delete _historyApi;
    delete _api;
}

rpl::producer<AuthState> PublicBackend::authState() const {
    return _authState.value();
}

Capabilities PublicBackend::capabilities() const {
    return Capabilities{}; // Phase 5 adds typing/presence via internal path
}

void PublicBackend::connectRealtime() {
    if (_xappToken.isEmpty() || _realtime)
        return;
    _realtime = new SocketModeRealtime(_xappToken, &_events);
    _realtime->start();
}

void PublicBackend::disconnectRealtime() {
    if (_realtime) {
        _realtime->stop();
        delete _realtime;
        _realtime = nullptr;
    }
}

// ── Snapshot loads ────────────────────────────────────────────────

rpl::producer<UserId> PublicBackend::loadMe() {
    return [this](auto consumer) mutable {
        _api->call(
            "auth.test",
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
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
        params.addQueryItem("include_all_metadata", "true");

        _api->paginate(
            "users.conversations",
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

rpl::producer<std::vector<User>> PublicBackend::loadUsers() {
    return [this](auto consumer) mutable {
        auto accum = std::make_shared<std::vector<User>>();
        _api->paginate(
            "users.list",
            "members",
            QUrlQuery{},
            [accum](QJsonArray page) {
                auto batch = JsonMappers::toUsers(page);
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

rpl::producer<MessagePage>
PublicBackend::loadHistory(ConversationId conv, std::optional<QString> cursor) {
    return [this, conv, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("channel", conv.value);
        params.addQueryItem("limit", "50");
        if (cursor)
            params.addQueryItem("cursor", *cursor);

        _historyApi->call(
            "conversations.history",
            params,
            [consumer](QJsonObject resp) mutable {
                MessagePage page;
                page.messages = JsonMappers::toMessages(resp.value("messages").toArray());
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

// ── Commands ──────────────────────────────────────────────────────

void PublicBackend::sendMessage(ConversationId conv, OutgoingMessage msg) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("text", msg.rawText.isEmpty() ? msg.text.text : msg.rawText);
    if (msg.threadRoot)
        params.addQueryItem("thread_ts", *msg.threadRoot);
    _api->call("chat.postMessage", params, {}, [](QString e) {
        qWarning() << "sendMessage error:" << e;
    });
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
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts", ts);
    _api->call("chat.delete", params, {}, [](QString e) {
        qWarning() << "deleteMessage error:" << e;
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
    _api->call("chat.scheduleMessage", params, {}, [](QString e) {
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

void PublicBackend::subscribePresence(std::vector<UserId> userIds) {
    if (!_realtime)
        return;
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(userIds.size()));
    for (const auto &u : userIds)
        ids.append(u.value);
    _realtime->subscribePresence(std::move(ids));
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

void PublicBackend::uploadFile(ConversationId conv, const QString &filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "uploadFile: cannot open" << filePath;
        return;
    }
    const QByteArray data     = f.readAll();
    const QString    filename = QFileInfo(filePath).fileName();
    const qint64     length   = data.size();

    // Step 1: get upload URL
    QUrlQuery params;
    params.addQueryItem("filename", filename);
    params.addQueryItem("length", QString::number(length));
    params.addQueryItem("channel", conv.value);

    _api->call(
        "files.getUploadURLExternal",
        params,
        [this, conv, filename, data](QJsonObject resp) mutable {
            const QString uploadUrl = resp.value("upload_url").toString();
            const QString fileId    = resp.value("file_id").toString();

            // Step 2: PUT data to upload URL (S3 — no auth header)
            _api->rawPut(
                QUrl(uploadUrl),
                data,
                [this, conv, filename, fileId]() mutable {
                    // Step 3: complete the upload
                    QJsonObject body;
                    body["channel_id"] = conv.value;
                    QJsonArray  filesArr;
                    QJsonObject fileEntry;
                    fileEntry["id"]    = fileId;
                    fileEntry["title"] = filename;
                    filesArr.append(fileEntry);
                    body["files"] = filesArr;

                    _api->postJson(
                        "files.completeUploadExternal",
                        body,
                        [](QJsonObject) { /* success */ },
                        [](QString err) { qWarning() << "completeUploadExternal error:" << err; }
                    );
                },
                [](QString err) { qWarning() << "rawPut error:" << err; }
            );
        },
        [](QString err) { qWarning() << "getUploadURLExternal error:" << err; }
    );
}

void PublicBackend::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    _api->downloadUrl(QUrl(url), std::move(onData), std::move(onError));
}
