// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "public_backend.h"
#include "json_mappers.h"
#include "socket_mode_realtime.h"

#include <QUrlQuery>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

PublicBackend::PublicBackend(const QString &xoxpToken, const QString &xappToken)
    : _xappToken(xappToken)
    , _api(new WebApiClient(nullptr))
{
    _api->setToken(xoxpToken);
}

PublicBackend::~PublicBackend() {
    delete _realtime;
    delete _api;
}

rpl::producer<AuthState> PublicBackend::authState() const {
    return _authState.value();
}

Capabilities PublicBackend::capabilities() const {
    return Capabilities{}; // Phase 5 adds typing/presence via internal path
}

void PublicBackend::connectRealtime() {
    if (_xappToken.isEmpty() || _realtime) return;
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
        _api->call("auth.test", QUrlQuery{},
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
        auto accum = std::make_shared<std::vector<Conversation>>();
        QUrlQuery params;
        params.addQueryItem("types", "public_channel,private_channel,im,mpim");
        params.addQueryItem("exclude_archived", "true");

        _api->paginate("conversations.list", "channels", params,
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
        _api->paginate("users.list", "members", QUrlQuery{},
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
        _api->call("users.getPresence", params,
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

rpl::producer<MessagePage> PublicBackend::loadHistory(
    ConversationId conv, std::optional<QString> cursor)
{
    return [this, conv, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("channel", conv.value);
        params.addQueryItem("limit", "50");
        if (cursor) params.addQueryItem("cursor", *cursor);

        _api->call("conversations.history", params,
            [consumer](QJsonObject resp) mutable {
                MessagePage page;
                page.messages = JsonMappers::toMessages(resp.value("messages").toArray());
                auto meta = resp.value("response_metadata").toObject();
                auto next = meta.value("next_cursor").toString();
                if (!next.isEmpty()) page.olderCursor = next;
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

rpl::producer<MessagePage> PublicBackend::loadThread(
    ConversationId conv, Ts root, std::optional<QString> cursor)
{
    return [this, conv, root, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem("channel", conv.value);
        params.addQueryItem("ts",      root);
        params.addQueryItem("limit",   "50");
        if (cursor) params.addQueryItem("cursor", *cursor);

        _api->call("conversations.replies", params,
            [consumer](QJsonObject resp) mutable {
                MessagePage page;
                // conversations.replies returns oldest-first; no reversal needed.
                page.messages = JsonMappers::toMessages(resp.value("messages").toArray(), false);
                auto meta = resp.value("response_metadata").toObject();
                auto next = meta.value("next_cursor").toString();
                if (!next.isEmpty()) page.olderCursor = next;
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
    params.addQueryItem("text",    msg.text.text);
    if (msg.threadRoot)
        params.addQueryItem("thread_ts", *msg.threadRoot);
    _api->call("chat.postMessage", params, {}, [](QString e){
        qWarning() << "sendMessage error:" << e;
    });
}

void PublicBackend::editMessage(ConversationId conv, Ts ts, TextWithEntities text) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts",      ts);
    params.addQueryItem("text",    text.text);
    _api->call("chat.update", params, {}, [](QString e){
        qWarning() << "editMessage error:" << e;
    });
}

void PublicBackend::deleteMessage(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts",      ts);
    _api->call("chat.delete", params, {}, [](QString e){
        qWarning() << "deleteMessage error:" << e;
    });
}

void PublicBackend::addReaction(ConversationId conv, Ts ts, QString emoji) {
    QUrlQuery params;
    params.addQueryItem("channel",   conv.value);
    params.addQueryItem("timestamp", ts);
    params.addQueryItem("name",      emoji);
    _api->call("reactions.add", params, {}, [](QString e){
        qWarning() << "addReaction error:" << e;
    });
}

void PublicBackend::removeReaction(ConversationId conv, Ts ts, QString emoji) {
    QUrlQuery params;
    params.addQueryItem("channel",   conv.value);
    params.addQueryItem("timestamp", ts);
    params.addQueryItem("name",      emoji);
    _api->call("reactions.remove", params, {}, [](QString e){
        qWarning() << "removeReaction error:" << e;
    });
}

void PublicBackend::markRead(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem("channel", conv.value);
    params.addQueryItem("ts",      ts);
    _api->call("conversations.mark", params, {}, [](QString e){
        qWarning() << "markRead error:" << e;
    });
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

        _api->call("search.messages", params,
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

rpl::producer<QHash<QString,QString>> PublicBackend::loadEmojiList() {
    return [this](auto consumer) mutable {
        _api->call("emoji.list", QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                QHash<QString,QString> map;
                const auto emoji = resp.value("emoji").toObject();
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
    params.addQueryItem("length",   QString::number(length));
    params.addQueryItem("channel",  conv.value);

    _api->call("files.getUploadURLExternal", params,
        [this, conv, filename, data](QJsonObject resp) mutable {
            const QString uploadUrl = resp.value("upload_url").toString();
            const QString fileId    = resp.value("file_id").toString();

            // Step 2: PUT data to upload URL (S3 — no auth header)
            _api->rawPut(QUrl(uploadUrl), data,
                [this, conv, filename, fileId]() mutable {
                    // Step 3: complete the upload
                    QJsonObject body;
                    body["channel_id"] = conv.value;
                    QJsonArray filesArr;
                    QJsonObject fileEntry;
                    fileEntry["id"]    = fileId;
                    fileEntry["title"] = filename;
                    filesArr.append(fileEntry);
                    body["files"] = filesArr;

                    _api->postJson("files.completeUploadExternal", body,
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

void PublicBackend::downloadFile(const QString &url,
                                  std::function<void(QByteArray)> onData,
                                  std::function<void(QString)>    onError) {
    _api->downloadUrl(QUrl(url), std::move(onData), std::move(onError));
}
