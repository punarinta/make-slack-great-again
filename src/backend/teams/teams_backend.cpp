// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "teams_backend.h"

#include "auth/token_store.h"
#include "backend/teams/json_mappers.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <memory>
#include <tuple>
#include <QDebug>

namespace teams {

namespace {
// Split a Graph @odata.nextLink (an absolute URL) into a path + query relative to
// the client base so GraphClient::get (which prepends the base) can follow it.
std::pair<QString, QUrlQuery> relFromNextLink(const QString &link) {
    QString rel =
        link.startsWith(GraphClient::kBaseUrl) ? link.mid(GraphClient::kBaseUrl.size()) : link;
    const int q = rel.indexOf(QLatin1Char('?'));
    if (q < 0)
        return {rel, QUrlQuery{}};
    return {rel.left(q), QUrlQuery(rel.mid(q + 1))};
}

// Map one page of Graph chatMessages into a MessagePage (skipping system events),
// carrying the @odata.nextLink as the older-history cursor.
MessagePage pageFromMessages(const QJsonObject &resp) {
    MessagePage page;
    for (const auto v : resp.value(QStringLiteral("value")).toArray()) {
        const auto mo = v.toObject();
        if (mo.value(QStringLiteral("messageType")).toString() != QLatin1String("message"))
            continue; // skip systemEventMessage etc.
        page.messages.push_back(teams::JsonMappers::toMessage(mo));
    }
    const auto next = resp.value(QStringLiteral("@odata.nextLink")).toString();
    if (!next.isEmpty())
        page.olderCursor = next;
    // Graph returns messages newest-first; the UI expects a page oldest-first
    // (it appends in array order), so reverse — same as slack::toMessages does.
    std::reverse(page.messages.begin(), page.messages.end());
    return page;
}

// Graph path of a conversation's message collection. For a channel reply, appends
// the parent's "/{root}/replies"; chats are flat (threadRoot ignored).
QString messagesPath(const ConversationId &conv, const std::optional<Ts> &threadRoot) {
    if (teams::isChannelConvId(conv.value)) {
        const auto [teamId, chanId] = teams::splitChannelConvId(conv.value);
        QString base                = "teams/" + teamId + "/channels/" + chanId + "/messages";
        if (threadRoot && !threadRoot->isEmpty())
            base += "/" + *threadRoot + "/replies";
        return base;
    }
    return "chats/" + conv.value + "/messages";
}

// Graph path of a single (top-level) message — for edit/delete/react. NOTE:
// channel *replies* live under "/{root}/replies/{id}" — Backend::messageItemPath
// (a member, so it can consult _replyParent) handles that case.
} // namespace

Backend::Backend(const Credentials &creds, const AppConfig &appCfg)
    : _creds(creds), _app(appCfg), _client(new GraphClient()), _authState(AuthState::LoggedIn) {
    _client->setToken(_creds.accessToken);
    setupTokenRefresh();
}

Backend::~Backend() {
    delete _client;
}

void Backend::fetchPhoto(const QString &userId, std::function<void(QString)> cb) {
    // Authenticated GET of the user's photo; downloadUrl adds the Bearer token and
    // bypasses the queue. 404 (no photo) / error → empty string.
    _client->downloadUrl(
        QUrl(GraphClient::kBaseUrl + "users/" + userId + "/photo/$value"),
        [cb](QByteArray bytes) {
            if (bytes.isEmpty())
                cb(QString());
            else
                cb(QStringLiteral("data:image/jpeg;base64,") +
                   QString::fromLatin1(bytes.toBase64()));
        },
        [cb](QString) { cb(QString()); }
    );
}

void Backend::resolveMessageMedia(const ConversationId &conv, const Message &msg) {
    const auto       inlineImgs = teams::extractInlineImages(msg.rawText);
    // Image file attachments still lacking a preview (toMessage leaves them as
    // chips — their SharePoint contentUrl isn't Graph-fetchable).
    std::vector<int> attachIdx;
    for (int i = 0; i < static_cast<int>(msg.files.size()); ++i)
        if (msg.files[i].mimeType.startsWith(QLatin1String("image/")) &&
            msg.files[i].thumbUrl.isEmpty() && !msg.files[i].permalink.isEmpty())
            attachIdx.push_back(i);

    const int total = static_cast<int>(inlineImgs.size()) + static_cast<int>(attachIdx.size());
    if (total == 0)
        return;

    auto base           = std::make_shared<Message>(msg);
    auto remaining      = std::make_shared<int>(total);
    auto inlineResolved = std::make_shared<std::vector<File>>(inlineImgs.size());
    auto finalize       = [this, conv, base, inlineResolved]() {
        for (auto &f : *inlineResolved)
            if (!f.thumbUrl.isEmpty())
                base->files.push_back(std::move(f));
        _events.fire(EvMessageChanged{conv, *base});
    };
    auto done = [remaining, finalize]() mutable {
        if (--*remaining == 0)
            finalize();
    };

    // (a) inline <img> hostedContents → data URI
    for (int i = 0; i < static_cast<int>(inlineImgs.size()); ++i) {
        const auto im = inlineImgs[i];
        File       f;
        f.name        = QStringLiteral("image");
        f.mimeType    = QStringLiteral("image/png");
        f.imageWidth  = im.width > 0 ? im.width : 1; // >0 so isImage()/hasPreview() hold
        f.imageHeight = im.height;
        auto store    = [inlineResolved, i, f, done](const QString &dataUri) mutable {
            if (!dataUri.isEmpty()) {
                f.thumbUrl           = dataUri;
                (*inlineResolved)[i] = std::move(f);
            }
            done();
        };
        if (im.url.startsWith(QLatin1String("http")))
            _client->downloadUrl(
                QUrl(im.url),
                [store](QByteArray b) mutable {
                    store(
                        b.isEmpty() ? QString()
                                    : QStringLiteral("data:image/png;base64,") +
                                          QString::fromLatin1(b.toBase64())
                    );
                },
                [store](QString) mutable { store(QString()); }
            );
        else
            store(QString());
    }

    // (b) image attachments → shares API → public thumbnail + downloadUrl
    for (const int idx : attachIdx) {
        const QString contentUrl = base->files[idx].permalink;
        // Graph "shares" share-id: "u!" + base64url(url) without padding.
        const QString shareId    = QStringLiteral("u!") +
                                QString::fromLatin1(contentUrl.toUtf8().toBase64(
                                    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals
                                ));
        QUrlQuery q;
        q.addQueryItem("$expand", "thumbnails");
        _client->get(
            "shares/" + shareId + "/driveItem",
            q,
            [base, idx, done](QJsonObject item) mutable {
                File &f = base->files[idx];
                if (const auto th = item.value("thumbnails").toArray(); !th.isEmpty()) {
                    const auto large = th.first().toObject().value("large").toObject();
                    f.thumbUrl       = large.value("url").toString(); // public svc.ms URL
                    f.imageWidth     = large.value("width").toInt();
                    f.imageHeight    = large.value("height").toInt();
                }
                // Full-size for the image viewer (a public pre-authenticated URL).
                if (const auto dl = item.value("@microsoft.graph.downloadUrl").toString();
                    !dl.isEmpty())
                    f.urlPrivate = dl;
                if (f.imageWidth == 0)
                    f.imageWidth = 1; // ensure isImage()/hasPreview() hold
                done();
            },
            [done](QString) mutable { done(); },
            /*quietErrors=*/true
        );
    }
}

rpl::producer<AuthState> Backend::authState() const {
    return _authState.value();
}

Capabilities Backend::capabilities() const {
    Capabilities c;
    c.presence      = true; // Graph presence (polled) → online/away dots
    c.reactions     = true;
    c.editMessage   = true;
    c.deleteMessage = true;
    c.threads       = true;
    c.fileUpload    = true;
    // typing / livePresence / huddles / canvases / slashCommands stay false:
    // Graph offers no live typing, canvas, huddle, or slash-command analog for a
    // delegated client. livePresence may flip true once the websocket presence
    // subscription lands.
    return c;
}

// --- Realtime via delta-style polling (see the header note on why no websocket
// push path exists for Teams messages). Polls every opened conversation's newest
// messages every few seconds and emits the arrivals. ---
void Backend::connectRealtime() {
    if (_pollTimer)
        return;
    _pollTimer = new QTimer(_client);
    _pollTimer->setInterval(5000); // ~5s; Graph chatMessage push latency is <10s anyway
    QObject::connect(_pollTimer, &QTimer::timeout, _client, [this]() { pollTracked(); });
    _pollTimer->start();
}

void Backend::disconnectRealtime() {
    if (_pollTimer) {
        _pollTimer->stop();
        _pollTimer->deleteLater();
        _pollTimer = nullptr;
    }
}

void Backend::trackConversation(const ConversationId &conv, const MessagePage &page) {
    _tracked.insert(conv.value);
    qint64 mx = _lastSeen.value(conv.value, 0);
    for (const auto &m : page.messages)
        mx = std::max(mx, m.date);
    _lastSeen[conv.value] = mx; // baseline so polling only emits genuinely newer messages
}

void Backend::pollTracked() {
    // Every ~60 s, refresh the whole conversation list so channels/teams/chats
    // that were created, renamed, or deleted elsewhere show up. EvRealtimeReconnected
    // is the existing "re-sync from the backend" signal: Session reloads the
    // conversation list (handles add/rename/delete uniformly) and the open
    // MessageList re-fetches + merges. No add/rename/delete-specific event exists,
    // so a full reload is the clean way to reflect structural changes.
    if (++_pollTicks % 12 == 0)
        _events.fire(EvRealtimeReconnected{});

    for (const QString &cid : _tracked) {
        const ConversationId conv{cid};
        QUrlQuery            q;
        q.addQueryItem("$top", "20");
        _client->get(
            messagesPath(conv, std::nullopt),
            q,
            [this, conv](QJsonObject resp) {
                const qint64         baseline = _lastSeen.value(conv.value, 0);
                qint64               mx       = baseline;
                std::vector<Message> fresh;
                for (const auto v : resp.value(QStringLiteral("value")).toArray()) {
                    const auto mo = v.toObject();
                    if (mo.value(QStringLiteral("messageType")).toString() !=
                        QLatin1String("message"))
                        continue;
                    Message m = teams::JsonMappers::toMessage(mo);
                    if (m.date > baseline) {
                        mx = std::max(mx, m.date);
                        fresh.push_back(std::move(m));
                    }
                }
                _lastSeen[conv.value] = mx;
                // Graph returns newest-first; emit oldest-first so they append in order.
                std::sort(fresh.begin(), fresh.end(), [](const Message &a, const Message &b) {
                    return a.date < b.date;
                });
                for (auto &m : fresh) {
                    resolveMessageMedia(
                        conv, m
                    ); // EvMessageChanged after EvMessageNew fills images
                    _events.fire(EvMessageNew{conv, std::move(m)});
                }
            },
            [](QString) {}, // poll failures are silent; next tick retries
            /*quietErrors=*/true
        );
    }
}

rpl::producer<UserId> Backend::loadMe() {
    return [this](auto consumer) mutable {
        _client->get(
            "me",
            QUrlQuery{},
            [this, consumer](QJsonObject resp) mutable {
                const User self = teams::JsonMappers::toUser(resp);
                consumer.put_next(UserId{self.id.value});
                consumer.put_done();
                // Teams has no flat roster and an empty tenant may have no chats,
                // so seed self into the user map directly (name now; avatar when
                // the photo arrives) — otherwise "me" has no User and shows no
                // name/avatar anywhere.
                _events.fire(EvUserChanged{self});
                fetchPhoto(self.id.value, [this, self](QString uri) {
                    if (uri.isEmpty())
                        return;
                    User u      = self;
                    u.avatarUrl = uri;
                    _events.fire(EvUserChanged{u});
                });
            },
            [consumer](QString err) mutable {
                qWarning() << "teams loadMe error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

bool Backend::isUserId(UserId id) const {
    return !id.value.isEmpty();
}

rpl::producer<User> Backend::loadUser(UserId id) {
    return [this, id](auto consumer) mutable {
        _client->get(
            "users/" + id.value,
            QUrlQuery{},
            [this, consumer](QJsonObject resp) mutable {
                const User u = teams::JsonMappers::toUser(resp);
                // Resolve the photo before emitting so the avatar arrives with the
                // name (one extra authenticated GET; empty on no-photo).
                fetchPhoto(u.id.value, [consumer, u](QString uri) mutable {
                    User withPhoto = u;
                    if (!uri.isEmpty())
                        withPhoto.avatarUrl = uri;
                    consumer.put_next(std::move(withPhoto));
                    consumer.put_done();
                });
            },
            [consumer](QString err) mutable {
                qWarning() << "teams loadUser error:" << err;
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

// Conversations = the user's chats (DMs/group chats) + every channel of every
// joined team. Three stages chained over the serialized queue; results accumulate
// in a shared vector emitted once everything completes.
rpl::producer<std::vector<Conversation>> Backend::loadConversations() {
    const QString me = _creds.userId;
    return [this, me](auto consumer) mutable {
        auto out = std::make_shared<std::vector<Conversation>>();

        QUrlQuery chatsQ;
        chatsQ.addQueryItem("$expand", "members");
        chatsQ.addQueryItem("$top", "50");

        _client->paginate(
            "me/chats",
            chatsQ,
            [out, me](QJsonArray page) {
                for (const auto v : page)
                    out->push_back(teams::JsonMappers::toChatConversation(v.toObject(), me));
            },
            [this, out, consumer]() mutable {
                auto teams = std::make_shared<std::vector<QPair<QString, QString>>>();
                _client->paginate(
                    "me/joinedTeams",
                    QUrlQuery{},
                    [teams](QJsonArray page) {
                        for (const auto v : page) {
                            const auto t = v.toObject();
                            teams->push_back(
                                {t.value("id").toString(), t.value("displayName").toString()}
                            );
                        }
                    },
                    [this, out, teams, consumer]() mutable {
                        if (teams->empty()) {
                            consumer.put_next(std::move(*out));
                            consumer.put_done();
                            return;
                        }
                        auto remaining = std::make_shared<int>(static_cast<int>(teams->size()));
                        auto finishOne = [out, remaining, consumer]() mutable {
                            if (--(*remaining) == 0) {
                                consumer.put_next(std::move(*out));
                                consumer.put_done();
                            }
                        };
                        for (const auto &tp : *teams) {
                            const QString teamId = tp.first, teamName = tp.second;
                            _client->paginate(
                                "teams/" + teamId + "/channels",
                                QUrlQuery{},
                                [out, teamId, teamName](QJsonArray page) {
                                    for (const auto v : page)
                                        out->push_back(
                                            teams::JsonMappers::toChannelConversation(
                                                v.toObject(), teamId, teamName
                                            )
                                        );
                                },
                                finishOne,
                                [finishOne](QString) mutable { finishOne(); }
                            );
                        }
                    },
                    [out, consumer](QString err) mutable {
                        qWarning() << "teams joinedTeams error:" << err;
                        consumer.put_next(std::move(*out));
                        consumer.put_done();
                    }
                );
            },
            [out, consumer](QString err) mutable {
                qWarning() << "teams chats error:" << err;
                consumer.put_next(std::move(*out));
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

// Teams has no flat workspace roster; seed the user map from the participants of
// the user's chats (DM/group peers). Channel-only senders resolve lazily via
// loadUser (gated by isUserId).
rpl::producer<std::vector<User>> Backend::loadUsers() {
    return [this](auto consumer) mutable {
        auto      users = std::make_shared<std::vector<User>>();
        auto      seen  = std::make_shared<QSet<QString>>();
        QUrlQuery q;
        q.addQueryItem("$expand", "members");
        q.addQueryItem("$top", "50");
        _client->paginate(
            "me/chats",
            q,
            [this, users, seen](QJsonArray page) {
                for (const auto cv : page)
                    for (const auto mv : cv.toObject().value("members").toArray()) {
                        const auto m   = mv.toObject();
                        const auto uid = m.value("userId").toString();
                        if (uid.isEmpty() || seen->contains(uid))
                            continue;
                        seen->insert(uid);
                        const User member = teams::JsonMappers::toMember(m);
                        users->push_back(member);
                        // The base list (names) is emitted at onDone; avatars fill
                        // in as each member's photo arrives. These members are
                        // already cached, so loadUser won't re-fetch them — hence
                        // we fetch their photos here.
                        fetchPhoto(member.id.value, [this, member](QString uri) {
                            if (uri.isEmpty())
                                return;
                            User u      = member;
                            u.avatarUrl = uri;
                            _events.fire(EvUserChanged{u});
                        });
                    }
            },
            [users, consumer]() mutable {
                consumer.put_next(std::move(*users));
                consumer.put_done();
            },
            [users, consumer](QString err) mutable {
                qWarning() << "teams loadUsers error:" << err;
                consumer.put_next(std::move(*users));
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<bool> Backend::loadPresence(UserId id) {
    return [this, id](auto consumer) mutable {
        _client->get(
            "users/" + id.value + "/presence",
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(
                    teams::JsonMappers::presenceActive(resp.value("availability").toString())
                );
                consumer.put_done();
            },
            [consumer](QString) mutable {
                consumer.put_next(false);
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

rpl::producer<MessagePage>
Backend::loadHistory(ConversationId conv, std::optional<QString> cursor) {
    return [this, conv, cursor](auto consumer) mutable {
        QString   path;
        QUrlQuery q;
        if (cursor && !cursor->isEmpty()) {
            std::tie(path, q) = relFromNextLink(*cursor); // follow @odata.nextLink
        } else if (teams::isChannelConvId(conv.value)) {
            const auto [teamId, chanId] = teams::splitChannelConvId(conv.value);
            path                        = "teams/" + teamId + "/channels/" + chanId + "/messages";
            q.addQueryItem("$top", "30");
            // Embed each message's replies (+ a replies@odata.count sibling) so the
            // reply bar can show the thread; the full chain loads via loadThread.
            q.addQueryItem("$expand", "replies");
        } else {
            path = "chats/" + conv.value + "/messages";
            q.addQueryItem("$top", "30");
        }
        _client->get(
            path,
            q,
            [this, conv, consumer](QJsonObject resp) mutable {
                MessagePage page = pageFromMessages(resp);
                // Opening a conversation marks it for realtime polling and seeds
                // the baseline so we don't re-emit history as "new".
                trackConversation(conv, page);
                for (const auto &m : page.messages)
                    resolveMessageMedia(
                        conv, m
                    ); // fills inline images in-place via EvMessageChanged
                consumer.put_next(std::move(page));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "teams loadHistory error:" << err;
                consumer.put_next(MessagePage{});
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<MessagePage>
Backend::loadThread(ConversationId conv, Ts root, std::optional<QString> cursor) {
    return [this, conv, root, cursor](auto consumer) mutable {
        // Only channel messages have threads; chats are flat.
        if (!teams::isChannelConvId(conv.value)) {
            consumer.put_next(MessagePage{});
            consumer.put_done();
            return rpl::lifetime();
        }
        QString   path;
        QUrlQuery q;
        if (cursor && !cursor->isEmpty()) {
            std::tie(path, q) = relFromNextLink(*cursor);
        } else {
            const auto [teamId, chanId] = teams::splitChannelConvId(conv.value);
            path = "teams/" + teamId + "/channels/" + chanId + "/messages/" + root + "/replies";
            q.addQueryItem("$top", "50");
        }
        _client->get(
            path,
            q,
            [this, conv, root, consumer](QJsonObject resp) mutable {
                MessagePage page = pageFromMessages(resp);
                // Remember each reply's parent so edit/delete/react can address it
                // under ".../messages/{root}/replies/{id}".
                for (const auto &m : page.messages) {
                    _replyParent.insert(m.ts, root);
                    resolveMessageMedia(conv, m);
                }
                consumer.put_next(std::move(page));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "teams loadThread error:" << err;
                consumer.put_next(MessagePage{});
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

void Backend::sendMessage(ConversationId conv, OutgoingMessage msg) {
    // Send as plain text (contentType "text"); Graph escapes it. Rich
    // mrkdwn→HTML formatting is a later refinement.
    const QString content = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
    QJsonObject   body{{"body", QJsonObject{{"contentType", "text"}, {"content", content}}}};
    _client->postJson(
        messagesPath(conv, msg.threadRoot),
        body,
        [this, conv](QJsonObject resp) {
            // The 201 response is the created message; emit it as the echo so
            // Session reconciles the optimistic ghost (matched by own-author + FIFO).
            _events.fire(EvMessageNew{conv, teams::JsonMappers::toMessage(resp)});
        },
        [this, conv](QString err) {
            // TODO(teams): on kConnectionLost the send may have landed — a
            // reconcile-scan (like slack) would avoid a false failure. For now the
            // ghost is dropped and the user can retry.
            qWarning() << "teams sendMessage error:" << err;
            _events.fire(EvSendFailed{conv, err});
        }
    );
}

QString Backend::messageItemPath(const ConversationId &conv, const Ts &ts) const {
    if (teams::isChannelConvId(conv.value)) {
        const auto [teamId, chanId] = teams::splitChannelConvId(conv.value);
        const QString base          = "teams/" + teamId + "/channels/" + chanId + "/messages/";
        const auto    it            = _replyParent.find(ts);
        if (it != _replyParent.end())
            return base + it.value() + "/replies/" + ts; // a known channel reply
        return base + ts;
    }
    return "chats/" + conv.value + "/messages/" + ts;
}

void Backend::editMessage(ConversationId conv, Ts ts, TextWithEntities text) {
    QJsonObject body{{"body", QJsonObject{{"contentType", "text"}, {"content", text.text}}}};
    // PATCH returns 204; the edit reflects via the realtime echo (increment 4) or
    // a refetch — mirrors slack::editMessage, which relies on the realtime echo.
    _client->patchJson(messageItemPath(conv, ts), body, {}, [](QString e) {
        qWarning() << "teams editMessage error:" << e;
    });
}

void Backend::deleteMessage(ConversationId conv, Ts ts) {
    _client->postJson(
        messageItemPath(conv, ts) + "/softDelete",
        QJsonObject{}, // no body → POSTed as an empty action
        [this, conv, ts](QJsonObject) { _events.fire(EvMessageDeleted{conv, ts}); },
        [](QString e) { qWarning() << "teams deleteMessage error:" << e; }
    );
}

void Backend::addReaction(ConversationId conv, Ts ts, QString emoji) {
    const QString type = teams::graphReactionType(emoji);
    _client->postJson(
        messageItemPath(conv, ts) + "/setReaction",
        QJsonObject{{"reactionType", type}},
        [this, conv, ts, emoji](QJsonObject) {
            // setReaction returns 204; fire the echo ourselves (the reaction is
            // ours) so the UI updates without waiting for the next poll.
            _events.fire(EvReactionAdded{conv, ts, emoji, UserId{_creds.userId}});
        },
        [](QString e) { qWarning() << "teams addReaction error:" << e; }
    );
}

void Backend::removeReaction(ConversationId conv, Ts ts, QString emoji) {
    const QString type = teams::graphReactionType(emoji);
    _client->postJson(
        messageItemPath(conv, ts) + "/unsetReaction",
        QJsonObject{{"reactionType", type}},
        [this, conv, ts, emoji](QJsonObject) {
            _events.fire(EvReactionRemoved{conv, ts, emoji, UserId{_creds.userId}});
        },
        [](QString e) { qWarning() << "teams removeReaction error:" << e; }
    );
}

// TODO(teams): Graph's delegated per-message read state is limited (chats have
// markChatReadForUser; channels have no per-message read API). Left a no-op until
// the read-state-parity spike (§5.8 #5) settles how unread degrades for channels.
void Backend::markRead(ConversationId, Ts) {}

// Teams exposes NO slash commands of its own — they're a Slack convention, and a
// Teams composer shouldn't surface "/away", "/shrug", … Presence/status/DM are
// reached through the UI instead. (Native command support can be added here if
// Teams ever gains a slash-command UX.) The default Backend::nativeCommands() →
// {} applies, so no override.

rpl::producer<std::vector<SearchResult>> Backend::searchMessages(const QString &query) {
    return [this, query](auto consumer) mutable {
        QJsonObject req{
            {"requests",
             QJsonArray{QJsonObject{
                 {"entityTypes", QJsonArray{"chatMessage"}},
                 {"query", QJsonObject{{"queryString", query}}},
                 {"from", 0},
                 {"size", 25},
             }}}
        };
        _client->postJson(
            "search/query",
            req,
            [consumer](QJsonObject resp) mutable {
                std::vector<SearchResult> out;
                for (const auto rv : resp.value("value").toArray())
                    for (const auto hcv : rv.toObject().value("hitsContainers").toArray())
                        for (const auto hv : hcv.toObject().value("hits").toArray()) {
                            const auto res = hv.toObject().value("resource").toObject();
                            if (!res.isEmpty())
                                out.push_back(teams::JsonMappers::toSearchResult(res));
                        }
                consumer.put_next(std::move(out));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "teams searchMessages error:" << err;
                consumer.put_next(std::vector<SearchResult>{});
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

void Backend::setPresence(bool away, std::function<void(bool, QString)> done) {
    const auto ok = [done](QJsonObject) {
        if (done)
            done(true, {});
    };
    const auto err = [done](QString e) {
        if (done)
            done(false, e);
    };
    if (away)
        // A preferred "Away"; clears (auto) when the user goes active again.
        _client->postJson(
            "me/presence/setUserPreferredPresence",
            QJsonObject{{"availability", "Away"}, {"activity", "Away"}},
            ok,
            err
        );
    else
        _client->postJson("me/presence/clearUserPreferredPresence", QJsonObject{}, ok, err);
}

void Backend::setStatus(
    const QString & /*emoji*/,
    const QString                     &text,
    qint64                             expirationTs,
    std::function<void(bool, QString)> done
) {
    // Teams status messages carry text only — no emoji field — so the emoji is dropped.
    QJsonObject sm{{"message", QJsonObject{{"content", text}, {"contentType", "text"}}}};
    if (expirationTs > 0)
        sm["expiryDateTime"] = QJsonObject{
            {"dateTime", QDateTime::fromSecsSinceEpoch(expirationTs).toUTC().toString(Qt::ISODate)},
            {"timeZone", "UTC"}
        };
    _client->postJson(
        "me/presence/setStatusMessage",
        QJsonObject{{"statusMessage", sm}},
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            if (done)
                done(false, e);
        }
    );
}

void Backend::setDndSnooze(int minutes, std::function<void(bool, QString)> done) {
    const auto ok = [done](QJsonObject) {
        if (done)
            done(true, {});
    };
    const auto err = [done](QString e) {
        if (done)
            done(false, e);
    };
    if (minutes > 0)
        _client->postJson(
            "me/presence/setUserPreferredPresence",
            QJsonObject{
                {"availability", "DoNotDisturb"},
                {"activity", "DoNotDisturb"},
                {"expirationDuration", QStringLiteral("PT%1M").arg(minutes)},
            },
            ok,
            err
        );
    else
        _client->postJson("me/presence/clearUserPreferredPresence", QJsonObject{}, ok, err);
}

void Backend::loadMyProfile(std::function<void(MyProfile)> done) {
    if (!done)
        return;
    _client->get(
        "me",
        QUrlQuery{},
        [this, done](QJsonObject resp) {
            MyProfile p = teams::JsonMappers::toMyProfile(resp);
            // Include the avatar as a data URI — the profile dialog falls back to
            // the initial placeholder when MyProfile.avatarUrl is empty, which would
            // wipe the photo it pre-set from the user entry.
            fetchPhoto(_creds.userId, [done, p](QString uri) mutable {
                if (!uri.isEmpty())
                    p.avatarUrl = uri;
                done(p);
            });
        },
        [done](QString) { done(MyProfile{}); },
        /*quietErrors=*/true
    );
}

void Backend::updateProfile(
    const QHash<QString, QString> &fields, std::function<void(bool, QString)> done
) {
    // Map Slack profile keys → the writable Graph /me properties. Email is omitted
    // (a user can't change their own primary mail via Graph).
    QJsonObject body;
    if (fields.contains("display_name"))
        body["displayName"] = fields.value("display_name");
    else if (fields.contains("real_name"))
        body["displayName"] = fields.value("real_name");
    if (fields.contains("title"))
        body["jobTitle"] = fields.value("title");
    if (fields.contains("phone"))
        body["mobilePhone"] = fields.value("phone");
    if (body.isEmpty()) {
        if (done)
            done(true, {});
        return;
    }
    _client->patchJson(
        "me",
        body,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            if (done)
                done(false, e);
        }
    );
}

void Backend::setPhoto(const QString &filePath, std::function<void(bool, QString, QString)> done) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (done)
            done(false, QStringLiteral("cannot_open_file"), {});
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    _client->putBinary(
        "me/photo/$value",
        bytes,
        "image/jpeg",
        [this, done](QJsonObject) {
            // Re-fetch as a data URI and refresh self in the user map app-wide.
            fetchPhoto(_creds.userId, [this, done](QString uri) {
                _client->get(
                    "me",
                    QUrlQuery{},
                    [this, done, uri](QJsonObject me) {
                        User self = teams::JsonMappers::toUser(me);
                        if (!uri.isEmpty())
                            self.avatarUrl = uri;
                        _events.fire(EvUserChanged{self});
                        if (done)
                            done(true, {}, uri);
                    },
                    [done, uri](QString) {
                        if (done)
                            done(true, {}, uri);
                    },
                    /*quietErrors=*/true
                );
            });
        },
        [done](QString e) {
            if (done)
                done(false, e, {});
        }
    );
}

void Backend::openDm(
    UserId user, std::function<void(ConversationId)> onSuccess, std::function<void(QString)> onError
) {
    // Create (or resume) a 1:1 chat. Graph returns the existing oneOnOne if present.
    const auto bind = [](const QString &id) {
        return QStringLiteral("https://graph.microsoft.com/v1.0/users('%1')").arg(id);
    };
    const auto member = [&](const QString &id) {
        return QJsonObject{
            {"@odata.type", "#microsoft.graph.aadUserConversationMember"},
            {"roles", QJsonArray{"owner"}},
            {"user@odata.bind", bind(id)},
        };
    };
    QJsonObject body{
        {"chatType", "oneOnOne"},
        {"members", QJsonArray{member(_creds.userId), member(user.value)}},
    };
    _client->postJson(
        "chats",
        body,
        [onSuccess](QJsonObject resp) {
            if (onSuccess)
                onSuccess(ConversationId{resp.value("id").toString()});
        },
        [onError](QString e) {
            qWarning() << "teams openDm error:" << e;
            if (onError)
                onError(e);
        }
    );
}

// NOT implemented — these Slack affordances have no clean delegated-Graph path
// that fits the current (Slack-shaped) UI, so they keep the Backend's no-op
// defaults rather than ship something misleading:
//   • createChannel(name, isPrivate) — a Teams channel must live in a *team*; the
//     workspace-level (name, isPrivate) dialog carries no team, so there's nothing
//     to create it in. A team-aware create flow would be a different UX.
//   • joinChannel — standard channels are joined by joining the *team* (not a
//     per-channel action); private/shared channels are invite-only. No Slack-style
//     "join this public channel".
//   • scheduleMessage — Graph has no delegated scheduled-send for chat/channel.
//   • pinMessage/unpinMessage — only chats expose pinnedMessages (channels don't),
//     and unpin needs a separate pinned-id lookup + DELETE; low value, deferred.
//   • starConversation — Graph exposes no favorite/star for chats or channels.

rpl::producer<QHash<QString, QString>> Backend::loadEmojiList() {
    return [](auto consumer) mutable {
        consumer.put_next(QHash<QString, QString>{});
        consumer.put_done();
        return rpl::lifetime();
    };
}

void Backend::uploadFiles(
    ConversationId                     conv,
    const QStringList                 &filePaths,
    const QString                     &initialComment,
    std::function<void(bool, QString)> done
) {
    if (filePaths.isEmpty()) {
        if (done)
            done(false, QStringLiteral("no_files"));
        return;
    }

    struct Pending {
        QString    name;
        QByteArray bytes;
        QByteArray mime;
    };
    auto files = std::make_shared<std::vector<Pending>>();
    for (const auto &p : filePaths) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) {
            if (done)
                done(false, QStringLiteral("cannot_open_file"));
            return;
        }
        files->push_back(
            {QFileInfo(p).fileName(),
             f.readAll(),
             QMimeDatabase().mimeTypeForFile(p).name().toUtf8()}
        );
    }
    const bool isChat = !teams::isChannelConvId(conv.value);

    // Shared state across the per-file uploads. Attachments are indexed by file so
    // order is preserved; empty slots (failed uploads) are skipped.
    auto atts      = std::make_shared<std::vector<QJsonObject>>(files->size());
    auto remaining = std::make_shared<int>(static_cast<int>(files->size()));
    auto firstErr  = std::make_shared<QString>();

    // Post the final HTML message once every file is uploaded (+ shared for chats).
    auto postOnce = [this, conv, initialComment, atts, firstErr, done]() {
        QJsonArray attArr;
        for (auto &a : *atts)
            if (!a.isEmpty())
                attArr.append(a);
        if (attArr.isEmpty()) {
            if (done)
                done(false, firstErr->isEmpty() ? QStringLiteral("upload_failed") : *firstErr);
            return;
        }
        QString body = initialComment.toHtmlEscaped();
        for (const auto av : attArr)
            body += QStringLiteral("<attachment id=\"%1\"></attachment>")
                        .arg(av.toObject().value("id").toString());
        QJsonObject msg{
            {"body", QJsonObject{{"contentType", "html"}, {"content", body}}},
            {"attachments", attArr},
        };
        _client->postJson(
            messagesPath(conv, std::nullopt),
            msg,
            [this, conv, done](QJsonObject resp) {
                const Message m = teams::JsonMappers::toMessage(resp);
                _events.fire(EvMessageNew{conv, m}); // reconciles the optimistic ghost
                resolveMessageMedia(conv, m);        // resolve the just-uploaded image preview
                if (done)
                    done(true, {});
            },
            [this, conv, done](QString e) {
                qWarning() << "teams uploadFiles post error:" << e;
                _events.fire(EvSendFailed{conv, e});
                if (done)
                    done(false, e);
            }
        );
    };
    auto finishOne = [remaining, postOnce]() mutable {
        if (--*remaining == 0)
            postOnce();
    };

    // Build a "reference" attachment from an uploaded driveItem. Teams keys the
    // attachment by the file's eTag GUID (must match the body's <attachment id>).
    auto attachmentFor = [](const QJsonObject &item, const QString &name, const QString &url) {
        const auto m = QRegularExpression(QStringLiteral("[0-9a-fA-F-]{36}"))
                           .match(item.value("eTag").toString());
        const QString id =
            m.hasMatch() ? m.captured(0) : QUuid::createUuid().toString(QUuid::WithoutBraces);
        return QJsonObject{
            {"id", id}, {"contentType", "reference"}, {"contentUrl", url}, {"name", name}
        };
    };

    // PUT one file's bytes to a drive path → driveItem → attachment.
    auto uploadAt = [this, files, atts, finishOne, firstErr, isChat, attachmentFor](
                        int i, const QString &putPath
                    ) mutable {
        _client->putBinary(
            putPath,
            (*files)[i].bytes,
            (*files)[i].mime,
            [this, i, files, atts, finishOne, isChat, attachmentFor](QJsonObject item) mutable {
                const QString name   = (*files)[i].name;
                const QString webUrl = item.value("webUrl").toString();
                if (!isChat) {
                    (*atts)[i] = attachmentFor(item, name, webUrl);
                    finishOne();
                    return;
                }
                // Chat files live in the sender's OneDrive — grant chat members
                // access via an org-scoped sharing link (channel files are already
                // shared with the team and skip this).
                const QString driveId =
                    item.value("parentReference").toObject().value("driveId").toString();
                const QString itemId = item.value("id").toString();
                _client->postJson(
                    "drives/" + driveId + "/items/" + itemId + "/createLink",
                    QJsonObject{{"type", "view"}, {"scope", "organization"}},
                    [i, item, name, webUrl, atts, finishOne, attachmentFor](
                        QJsonObject link
                    ) mutable {
                        const QString lu = link.value("link").toObject().value("webUrl").toString();
                        (*atts)[i]       = attachmentFor(item, name, lu.isEmpty() ? webUrl : lu);
                        finishOne();
                    },
                    [i, item, name, webUrl, atts, finishOne, attachmentFor](QString) mutable {
                        (*atts)[i] = attachmentFor(item, name, webUrl);
                        finishOne();
                    }
                );
            },
            [firstErr, finishOne](QString e) mutable {
                if (firstErr->isEmpty())
                    *firstErr = e;
                finishOne();
            }
        );
    };

    const auto enc = [](const QString &s) {
        return QString::fromUtf8(QUrl::toPercentEncoding(s, "/")); // keep path separators
    };

    if (isChat) {
        for (int i = 0; i < static_cast<int>(files->size()); ++i)
            uploadAt(
                i,
                "me/drive/root:/" + enc("Microsoft Teams Chat Files/" + (*files)[i].name) +
                    ":/content"
            );
    } else {
        const auto [teamId, chanId] = teams::splitChannelConvId(conv.value);
        _client->get(
            "teams/" + teamId + "/channels/" + chanId + "/filesFolder",
            QUrlQuery{},
            [files, uploadAt, finishOne, enc](QJsonObject folder) mutable {
                const QString driveId =
                    folder.value("parentReference").toObject().value("driveId").toString();
                const QString folderId = folder.value("id").toString();
                if (driveId.isEmpty() || folderId.isEmpty()) {
                    for (int i = 0; i < static_cast<int>(files->size()); ++i)
                        finishOne();
                    return;
                }
                for (int i = 0; i < static_cast<int>(files->size()); ++i)
                    uploadAt(
                        i,
                        "drives/" + driveId + "/items/" + folderId + ":/" + enc((*files)[i].name) +
                            ":/content"
                    );
            },
            [files, finishOne](QString) mutable {
                for (int i = 0; i < static_cast<int>(files->size()); ++i)
                    finishOne();
            },
            /*quietErrors=*/true
        );
    }
}

void Backend::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    _client->downloadUrl(QUrl(url), std::move(onData), std::move(onError));
}

rpl::producer<Event> Backend::events() const {
    return _events.events();
}

// --- Token refresh: reactive (GraphClient calls the hook on
// InvalidAuthenticationToken) + proactive (the periodic check below). ---
void Backend::setupTokenRefresh() {
    _client->setOnTokenExpired([this](std::function<void(bool)> done) {
        doRefresh(std::move(done));
    });
    if (_creds.refreshToken.isEmpty())
        return; // nothing to refresh proactively
    _proactiveRefreshTimer = new QTimer(_client);
    _proactiveRefreshTimer->setInterval(60 * 1000); // wall-clock check every minute
    QObject::connect(_proactiveRefreshTimer, &QTimer::timeout, _client, [this]() {
        maybeProactiveRefresh();
    });
    _proactiveRefreshTimer->start();
    // Deferred first check (don't dispatch a refresh from within the constructor).
    QTimer::singleShot(0, _client, [this]() { maybeProactiveRefresh(); });
}

void Backend::maybeProactiveRefresh() {
    if (_creds.refreshToken.isEmpty() || _creds.expiresAt == 0 || _refreshInProgress)
        return;
    // Teams access tokens live ~1h, so refresh only in the last few minutes
    // (a 1h margin like Slack's 12h tokens would refresh on every check). After
    // resume the token has often already expired → secsLeft < 0 → refresh now,
    // before any user-facing call can 401.
    constexpr qint64 kRefreshMarginSecs = 300;
    const qint64     secsLeft           = _creds.expiresAt - QDateTime::currentSecsSinceEpoch();
    if (secsLeft > kRefreshMarginSecs)
        return;
    qDebug() << "[teams TokenRefresh] proactive: token expires in" << secsLeft << "s, refreshing";
    doRefresh([](bool) {});
}

void Backend::doRefresh(std::function<void(bool)> done) {
    _refreshWaiters.push_back(std::move(done));
    if (_refreshInProgress)
        return;
    if (_creds.refreshToken.isEmpty()) {
        _refreshInProgress = false;
        auto waiters       = std::move(_refreshWaiters);
        _authState.force_assign(AuthState::NotLoggedIn);
        for (auto &w : waiters)
            w(false);
        return;
    }
    _refreshInProgress = true;

    QUrlQuery params;
    params.addQueryItem("client_id", _app.clientId);
    params.addQueryItem("grant_type", "refresh_token");
    params.addQueryItem("refresh_token", _creds.refreshToken);
    // Deliberately NO `scope`: a refresh_token grant must request a SUBSET of the
    // scopes the token was granted, so sending the full app scope list fails with
    // invalid_grant whenever the stored token predates a scope addition (or an
    // admin-consent-only scope like User.ReadWrite wasn't granted). Omitting it
    // returns a token with the originally-granted scopes — what MSAL does. This is
    // the bug behind "teams token refresh failed: invalid_grant" after resume.

    // Per-workspace tenant authority keeps the refresh scoped to the right org.
    const QString tenant =
        _creds.tenantId.isEmpty() ? QStringLiteral("organizations") : _creds.tenantId;
    auto           *nam = new QNetworkAccessManager(_client);
    QNetworkRequest req(
        QUrl(QStringLiteral("https://login.microsoftonline.com/%1/oauth2/v2.0/token").arg(tenant))
    );
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    auto *reply = nam->post(req, params.toString(QUrl::FullyEncoded).toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, _client, [this, reply, nam] {
        reply->deleteLater();
        nam->deleteLater();
        const auto obj     = QJsonDocument::fromJson(reply->readAll()).object();
        const bool success = !obj.contains("error") && obj.contains("access_token");
        if (success) {
            _creds.accessToken = obj.value("access_token").toString();
            const auto rt      = obj.value("refresh_token").toString();
            if (!rt.isEmpty())
                _creds.refreshToken = rt;
            const qint64 expiresIn = obj.value("expires_in").toInteger(0);
            _creds.expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
            _client->setToken(_creds.accessToken);
            TokenStore::saveWorkspace(toRecord(_creds)); // persist the rotated token
        }
        // A definitive auth failure (the refresh token is dead — expired/revoked,
        // or consent withdrawn) means the user must sign in again. A transient
        // failure (network blip on resume, 5xx, empty body) must NOT log them out —
        // leave the session intact so the next call retries the refresh.
        const QString err                   = obj.value("error").toString();
        const bool    definitiveAuthFailure = err == QLatin1String("invalid_grant") ||
                                           err == QLatin1String("invalid_client") ||
                                           err == QLatin1String("unauthorized_client") ||
                                           err == QLatin1String("interaction_required");
        if (!success)
            qWarning() << "teams token refresh failed:" << (err.isEmpty() ? "(transport)" : err)
                       << (definitiveAuthFailure ? "— signing out" : "— will retry");

        _refreshInProgress = false;
        auto waiters       = std::move(_refreshWaiters);
        if (!success && definitiveAuthFailure) {
            _authState.force_assign(AuthState::NotLoggedIn);
            disconnectRealtime(); // stop the poll loop from hammering with a dead token
            if (_proactiveRefreshTimer)
                _proactiveRefreshTimer->stop(); // and stop re-trying a dead refresh token
        }
        for (auto &w : waiters)
            w(success);
    });
}

} // namespace teams
