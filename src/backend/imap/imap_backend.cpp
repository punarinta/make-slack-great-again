// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_backend.h"

#include "backend/imap/imap_bimi.h"
#include "backend/imap/imap_client.h"
#include "backend/imap/imap_compose.h"
#include "backend/imap/imap_favicon.h"
#include "backend/imap/imap_html.h"
#include "backend/imap/imap_mappers.h"
#include "backend/imap/imap_providers.h"
#include "backend/imap/imap_smtp.h"
#include "backend/imap/mime_parser.h"
#include "llm/oauth_loopback.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>

#include <QPair>

#include <algorithm>
#include <limits>
#include <memory>

namespace imap {

namespace {

constexpr int kScanWindow   = 500; // newest N INBOX messages to index in Phase 1
constexpr int kFolderWindow = 80;  // newest N messages loaded when a folder channel is opened

// Domain-icon avatars (BIMI/favicon): batch window for the bulk EvUsersChanged
// emit + cache persist, and how long a probe result (hit or miss) stays valid
// before the domain is probed again.
constexpr int    kIconFlushMs       = 400;
constexpr qint64 kDomainIconTtlSecs = 7 * 24 * 3600;

QByteArray joinUids(const QList<quint32> &uids) {
    QByteArray s;
    for (quint32 u : uids) {
        if (!s.isEmpty())
            s += ',';
        s += QByteArray::number(u);
    }
    return s;
}

qint64 dateMicros(const Envelope &env, const QDateTime &internalDate) {
    const QDateTime dt = env.date.isValid() ? env.date : internalDate;
    return dt.isValid() ? dt.toMSecsSinceEpoch() * 1000 : 0;
}

// A user folder/label that should surface as a channel: selectable, not INBOX,
// and not a special-use system folder (Sent/Drafts/Trash/Junk/Archive/All) —
// those are the substrate, not topic channels (§3).
bool isUserFolder(const Mailbox &m) {
    if (!m.selectable || m.name.compare(QStringLiteral("INBOX"), Qt::CaseInsensitive) == 0)
        return false;
    for (const QString &su :
         {QStringLiteral("\\Sent"),
          QStringLiteral("\\Drafts"),
          QStringLiteral("\\Trash"),
          QStringLiteral("\\Junk"),
          QStringLiteral("\\Archive"),
          QStringLiteral("\\All")})
        if (m.hasFlag(su))
            return false;
    return true;
}

QString folderLabel(const Mailbox &m) {
    const QString seg = m.name.section(m.delimiter, -1);
    return seg.isEmpty() ? m.name : seg;
}

QString msgKeyOf(const MsgRef &m) {
    return m.env.messageId.isEmpty() ? (QStringLiteral("uid:") + QString::number(m.uid))
                                     : m.env.messageId;
}

// Render UIDs as an IMAP sequence set ("12,15,17") for a batched STORE.
QByteArray joinUidSet(const QList<quint32> &uids) {
    QByteArray s;
    for (quint32 u : uids) {
        if (!s.isEmpty())
            s += ',';
        s += QByteArray::number(u);
    }
    return s;
}

// Sequentially SELECT each mailbox and set +FLAGS (\Seen) on its UID batch. The
// command client is a strict FIFO, so SELECTs for different mailboxes can't be
// issued up front (a later STORE would run against the wrong selected mailbox);
// each STORE must complete before the next SELECT. Self-owning: kept alive by the
// in-flight callback and freed when the chain ends — no retained self-reference,
// so no cycle/leak.
struct SeenMarker : std::enable_shared_from_this<SeenMarker> {
    ImapClient                       *client = nullptr;
    QList<QPair<QString, QByteArray>> jobs; // (mailbox, uid-set)
    int                               i = 0;
    void                              run() {
        if (!client || i >= jobs.size())
            return;
        const auto job  = jobs[i++];
        auto       self = shared_from_this();
        client->select(job.first, [self, set = job.second](bool ok, SelectResult) {
            if (!ok) {
                self->run(); // skip this mailbox, continue with the rest
                return;
            }
            self->client->sendCommand(
                "UID STORE " + set + " +FLAGS (\\Seen)", [self](const Response &) { self->run(); }
            );
        });
    }
};

// Normalize a subject for change-detection: strip reply/forward prefixes
// (Re:/Fwd:/Sv:/Aw:…) and case, so "Re: Foo" reads as the same topic as "Foo".
QString subjectKey(const QString &subjectIn) {
    QString s = subjectIn.trimmed();
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const char *p : {"re:", "fwd:", "fw:", "sv:", "aw:", "antw:"}) {
            const QString pre = QString::fromLatin1(p);
            if (s.left(pre.size()).compare(pre, Qt::CaseInsensitive) == 0) {
                s        = s.mid(pre.size()).trimmed();
                stripped = true;
                break;
            }
        }
    }
    return s.toLower();
}

// Channel-kind conversations (label folders, mailing lists, the broadcast bucket)
// collapse to thread-roots + a thread panel (§8); DMs/MPDMs render inline.
bool isChannelConvId(const QString &id) {
    return id.startsWith(QLatin1String("list:")) || id.startsWith(QLatin1String("channel:")) ||
           id.startsWith(QLatin1String("folder:"));
}

// ── Pure worker functions (run on a thread pool; no `this`, no shared state) ──
// All of the CPU-heavy parsing/bucketing/MIME/HTML happens here so the UI thread
// never blocks on a scan or a large folder open (see scanInboxThen/loadHistory).

constexpr qint64 kMaxInlineImage = 4 * 1024 * 1024; // embed images up to 4 MB as data: URIs

Message buildMessage(const MsgRef &ref, const QByteArray &rawBody) {
    Message msg;
    msg.ts   = msgKeyOf(ref);
    msg.date = dateMicros(ref.env, ref.internalDate);
    if (!ref.env.from.isEmpty())
        msg.author = UserId{ref.env.from.first().email};
    if (rawBody.isEmpty())
        return msg; // envelope-only (no body fetched)

    const ParsedMessage pm = Mime::parse(rawBody);
    if (!pm.textHtml.trimmed().isEmpty())
        msg.text = htmlToEntities(pm.textHtml);
    else
        msg.text.text = normalizePlainText(pm.textPlain);

    for (const MimeAttachment &a : pm.attachments) {
        File f;
        f.name           = a.filename.isEmpty() ? QStringLiteral("attachment") : a.filename;
        f.mimeType       = a.mimeType;
        f.size           = a.content.size();
        f.id             = a.contentId; // cid (if any)
        const bool isImg = a.mimeType.startsWith(QLatin1String("image/"));
        if (isImg && a.content.size() <= kMaxInlineImage) {
            // Embedded image → inline preview via a data: URI (the bytes are
            // already in hand from the BODY[] fetch; downloadFile decodes it).
            const QImage img = QImage::fromData(a.content);
            f.imageWidth     = img.width();
            f.imageHeight    = img.height();
            f.urlPrivate     = QStringLiteral("data:") + a.mimeType + ";base64," +
                           QString::fromLatin1(a.content.toBase64());
        } else {
            // Other attachments: a lazy ref the user can download/open on click;
            // downloadFile re-fetches the part (avoids holding bytes in memory).
            f.urlPrivate = QStringLiteral("imapfile:") + ref.mailbox + "|" +
                           QString::number(ref.uid) + "|" + f.name;
        }
        msg.files.push_back(std::move(f));
    }
    return msg;
}

// Parse fetched BODY[] lines → a MessagePage. For a collapsed channel view each
// ref is a thread root: prepend the subject as a bold title and set reply counts.
MessagePage buildPage(
    const QList<QByteArray>       &lines,
    const QList<MsgRef>           &refs,
    bool                           collapse,
    const QHash<QString, int>     &replyCountOf,
    const QHash<QString, QString> &latestReplyOf
) {
    QHash<quint32, QByteArray> bodyByUid;
    for (const FetchItem &it : Mappers::parseFetch(lines))
        if (it.hasBody)
            bodyByUid.insert(it.uid, it.rawBody);
    // Prepend `subj` as a bold title line above the body (offsets shift to match).
    auto leadWithSubject = [](Message &msg, const QString &subj) {
        if (subj.isEmpty())
            return;
        const QString body   = msg.text.text;
        const QString prefix = body.isEmpty() ? subj : (subj + "\n\n");
        const int     shift  = body.isEmpty() ? 0 : int(prefix.size());
        msg.text.text        = body.isEmpty() ? subj : (prefix + body);
        for (auto &e : msg.text.entities)
            e.offset += shift;
        msg.text.entities.insert(
            msg.text.entities.begin(), TextEntity{EntityType::Bold, 0, int(subj.size()), {}}
        );
    };

    MessagePage   page;
    QString       prevSubjectKey;   // DM/MPDM: detect when the subject changes
    QSet<QString> seenInlineImages; // embedded-image content already shown earlier in this page
    for (const MsgRef &m : refs) {
        Message       msg  = buildMessage(m, bodyByUid.value(m.uid));
        const QString subj = m.env.subject.trimmed();
        if (collapse) {
            // Channel thread-root: always lead with the subject + reply counts.
            leadWithSubject(msg, subj);
            msg.replyCount = replyCountOf.value(msg.ts, 0);
            if (latestReplyOf.contains(msg.ts))
                msg.latestReply = latestReplyOf.value(msg.ts);
        } else {
            // DM/MPDM: email is subject-centric, so lead with the subject (bold)
            // whenever it changes — distinct mails (e.g. an alert and its later
            // all-clear) stay separable instead of blending into one bubble. A
            // back-and-forth thread keeps the same subject, so it's shown once.
            const QString key = subjectKey(subj);
            if (key != prevSubjectKey)
                leadWithSubject(msg, subj);
            prevSubjectKey = key;
        }
        // A reply re-embeds the quoted original's inline image (its collapsed body
        // still references the cid), so the same picture would appear in both
        // bubbles. An attachment belongs to the post where it first appeared —
        // drop inline images (keyed on the embedded bytes) already shown above.
        if (!msg.files.empty()) {
            std::vector<File> kept;
            kept.reserve(msg.files.size());
            for (auto &f : msg.files) {
                if (f.urlPrivate.startsWith(QLatin1String("data:"))) {
                    if (seenInlineImages.contains(f.urlPrivate))
                        continue; // same bytes as an earlier message → not shown again
                    seenInlineImages.insert(f.urlPrivate);
                }
                kept.push_back(std::move(f));
            }
            msg.files = std::move(kept);
        }
        page.messages.push_back(std::move(msg));
    }
    return page;
}

// Parse INBOX ENVELOPE lines (+ optional server THREAD) and run Model-D bucketing.
BucketResult scanBucket(
    const QList<QByteArray> &fetchLines,
    const QByteArray        &threadLine,
    bool                     useThread,
    const QSet<QString>     &me,
    const QString           &mePrimary
) {
    QList<MsgRef> refs;
    for (const FetchItem &it : Mappers::parseFetch(fetchLines)) {
        if (!it.hasEnvelope)
            continue;
        MsgRef r;
        r.uid          = it.uid;
        r.mailbox      = QStringLiteral("INBOX");
        r.env          = it.envelope;
        r.internalDate = it.internalDate;
        r.seen         = it.seen();
        refs.append(r);
    }
    const QList<QList<quint32>> threads =
        useThread ? Mappers::parseThread(threadLine) : QList<QList<quint32>>{};
    return Bucketer(me, mePrimary).run(refs, threads);
}

} // namespace

Backend::Backend(const Credentials &creds) : _creds(creds) {
    _myAddresses.insert(creds.user.toLower());
    for (const QString &a : creds.aliases)
        _myAddresses.insert(a.toLower());

    _authState = AuthState::LoggingIn;
    // Backend is not a QObject, so ImapClient is unparented and owned directly
    // (deleted in the destructor).
    _client    = new ImapClient();
    _client->setInsecure(creds.insecure);

    QObject::connect(_client, &ImapClient::loggedIn, _client, [this] {
        _ready     = true;
        _authState = AuthState::LoggedIn;
        flushReady();
    });
    QObject::connect(_client, &ImapClient::error, _client, [this](const QString &) {
        _failed    = true;
        _authState = AuthState::NotLoggedIn;
        flushReady(); // let queued loads complete (empty) instead of hanging
    });

    _client->connectToServer(creds.host, creds.port);
    loginClient(_client);
}

bool Backend::usingOAuth() const {
    return _creds.authMethod != AuthMethod::Password && !_creds.refreshToken.isEmpty();
}

void Backend::ensureFreshToken(std::function<void()> then) {
    if (!usingOAuth()) {
        then();
        return;
    }
    // 60s margin: treat a token expiring imminently as already stale.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!_creds.accessToken.isEmpty() && _creds.expiresAt > now + 60) {
        then();
        return;
    }
    const auto cfg = oauthConfigFor(_creds.authMethod, _creds.user);
    if (!cfg) { // misconfigured — try whatever token we have
        then();
        return;
    }
    auto *flow = new OAuthLoopbackFlow(*cfg, _client);
    QObject::connect(flow, &OAuthLoopbackFlow::done, _client, [this, flow, then](QJsonObject tok) {
        const QString access = tok.value(QStringLiteral("access_token")).toString();
        if (!access.isEmpty()) {
            _creds.accessToken = access;
            const qint64 ttl   = qint64(tok.value(QStringLiteral("expires_in")).toDouble(3600));
            _creds.expiresAt   = QDateTime::currentSecsSinceEpoch() + ttl;
            const QString rt   = tok.value(QStringLiteral("refresh_token")).toString();
            if (!rt.isEmpty())
                _creds.refreshToken = rt;                // Google rotates only sometimes
            TokenStore::saveWorkspace(toRecord(_creds)); // persist the refreshed token
        }
        flow->deleteLater();
        then();
    });
    QObject::connect(flow, &OAuthLoopbackFlow::failed, _client, [flow, then](const QString &e) {
        qWarning("imap oauth: token refresh failed: %s", qPrintable(e));
        flow->deleteLater();
        then(); // let the login attempt fail with a clear IMAP error
    });
    flow->refresh(_creds.refreshToken);
}

void Backend::loginClient(ImapClient *c) {
    if (usingOAuth())
        ensureFreshToken([this, c] { c->loginXOAuth2(_creds.user, _creds.accessToken); });
    else
        c->login(_creds.user, _creds.password);
}

Backend::~Backend() {
    if (_domainIconsDirty)
        saveDomainIconCache(); // don't lose probe results to an in-flight flush window
    delete _idleClient;        // deletes its child refresh timer too
    delete _bimi;
    delete _favicon;
    delete _client;
}

void Backend::resolveBimiForUsers(const QHash<QString, User> &users) {
    if (!_bimi) {
        _bimi = new BimiResolver();
        QObject::connect(
            _bimi,
            &BimiResolver::resolved,
            _client,
            [this](const QString &domain, const QString &logoUrl) {
                onBimiResolved(domain, logoUrl);
            }
        );
    }
    loadDomainIconCache();
    QSet<QString> domains;
    for (auto it = users.cbegin(); it != users.cend(); ++it) {
        const int at = it.key().indexOf('@');
        if (at > 0)
            domains.insert(it.key().mid(at + 1));
    }
    for (const QString &d : domains) {
        // Freemail domains never get a domain-level icon: the provider's brand
        // is not the person's avatar (Yahoo publishes BIMI — without this every
        // @yahoo.com peer would wear the Yahoo logo). Gravatar/initials remain.
        if (isFreemailDomain(d))
            continue;
        // Persisted probe result (survives restarts, expired entries dropped on
        // load): apply a hit right away and skip the DNS + up-to-~6 HTTP probes
        // this domain would otherwise cost on every app start.
        const auto cached = _domainIcons.constFind(d);
        if (cached != _domainIcons.constEnd()) {
            if (!cached.value().url.isEmpty())
                applyDomainIcon(d, cached.value().url);
            continue;
        }
        _bimi->resolve(d);
    }
}

void Backend::onBimiResolved(const QString &domain, const QString &logoUrl) {
    if (logoUrl.isEmpty()) {
        // No BIMI record → probe the domain's own web icons instead.
        if (!_favicon) {
            _favicon = new FaviconResolver();
            QObject::connect(
                _favicon,
                &FaviconResolver::resolved,
                _client,
                [this](const QString &d, const QString &iconUrl) {
                    // Record even a miss — negative caching is what stops an
                    // icon-less domain from being re-probed every start.
                    recordDomainIcon(d, iconUrl);
                    if (!iconUrl.isEmpty())
                        applyDomainIcon(d, iconUrl);
                }
            );
        }
        _favicon->resolve(domain);
        return;
    }
    recordDomainIcon(domain, logoUrl);
    applyDomainIcon(domain, logoUrl);
}

void Backend::applyDomainIcon(const QString &domain, const QString &iconUrl) {
    const QString suffix = QLatin1Char('@') + domain;
    for (auto it = _users.begin(); it != _users.end(); ++it)
        if (it.key().endsWith(suffix) && it.value().avatarUrl != iconUrl) {
            it.value().avatarUrl = iconUrl; // domain icon wins for the whole domain
            _pendingIconUsers.insert(it.key());
        }
    // Batched: results trickle in per-domain as probes finish, and a per-user
    // EvUserChanged costs Session a full roster copy + synchronous cache write
    // + conv-list rebuild EACH — the storm lagged the UI for seconds after the
    // scan. One EvUsersChanged per flush window collapses all of it.
    if (!_pendingIconUsers.isEmpty())
        scheduleIconFlush();
}

void Backend::applyCachedDomainIcons() {
    loadDomainIconCache();
    if (_domainIcons.isEmpty())
        return;
    for (auto it = _users.begin(); it != _users.end(); ++it) {
        const int at = it.key().indexOf('@');
        if (at <= 0)
            continue;
        const QString d      = it.key().mid(at + 1).toLower(); // cache keys are lowercased
        const auto    cached = _domainIcons.constFind(d);
        if (cached == _domainIcons.constEnd() || cached.value().url.isEmpty())
            continue;
        if (isFreemailDomain(d))
            continue; // a stale pre-guard cache entry must not slip through
        it.value().avatarUrl = cached.value().url;
    }
}

void Backend::recordDomainIcon(const QString &domain, const QString &iconUrl) {
    _domainIcons.insert(domain, {iconUrl, QDateTime::currentSecsSinceEpoch()});
    _domainIconsDirty = true;
    scheduleIconFlush();
}

void Backend::scheduleIconFlush() {
    if (!_iconFlush) {
        _iconFlush = new QTimer(_client); // dies with the command connection
        _iconFlush->setSingleShot(true);
        _iconFlush->setInterval(kIconFlushMs);
        QObject::connect(_iconFlush, &QTimer::timeout, _client, [this] { flushIconUpdates(); });
    }
    if (!_iconFlush->isActive())
        _iconFlush->start();
}

void Backend::flushIconUpdates() {
    if (!_pendingIconUsers.isEmpty()) {
        EvUsersChanged ev;
        ev.users.reserve(size_t(_pendingIconUsers.size()));
        for (const QString &email : std::as_const(_pendingIconUsers)) {
            const auto it = _users.constFind(email);
            if (it != _users.cend())
                ev.users.push_back(it.value());
        }
        _pendingIconUsers.clear();
        if (!ev.users.empty())
            _events.fire(std::move(ev));
    }
    if (_domainIconsDirty) {
        _domainIconsDirty = false;
        saveDomainIconCache();
    }
}

void Backend::loadDomainIconCache() {
    if (_domainIconsLoaded)
        return;
    _domainIconsLoaded = true;
    const QJsonObject root =
        QJsonDocument::fromJson(
            QSettings("msga", "msga").value(QStringLiteral("imap/domainIcons")).toByteArray()
        )
            .object();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject o  = it.value().toObject();
        const qint64      ts = qint64(o.value(QStringLiteral("ts")).toDouble());
        if (now - ts > kDomainIconTtlSecs)
            continue; // expired — re-probe this run, entry dropped on next save
        _domainIcons.insert(it.key(), {o.value(QStringLiteral("url")).toString(), ts});
    }
}

void Backend::saveDomainIconCache() const {
    QJsonObject root;
    for (auto it = _domainIcons.cbegin(); it != _domainIcons.cend(); ++it)
        root.insert(
            it.key(),
            QJsonObject{
                {QStringLiteral("url"), it.value().url},
                {QStringLiteral("ts"), double(it.value().ts)},
            }
        );
    QSettings("msga", "msga")
        .setValue(
            QStringLiteral("imap/domainIcons"), QJsonDocument(root).toJson(QJsonDocument::Compact)
        );
}

rpl::producer<AuthState> Backend::authState() const {
    return _authState.value();
}

Capabilities Backend::capabilities() const {
    Capabilities c;
    c.threads          = true;
    c.fileUpload       = true; // Phase 4 (attachments via MIME)
    c.deleteMessage    = true; // Phase 4 (move to Trash / \Deleted)
    c.deleteAnyMessage = true; // it's your own mailbox — any message is deletable, not just yours
    c.messageSubjects  = true; // composer subject field (§4)
    c.collapseQuotedReplies = true; // hide a reply's quoted history + signature behind a toggle
    // editMessage / reactions / canvases / huddles / slashCommands / typing /
    // presence / livePresence all stay false — email has no such notion. With
    // presence false the conv-footer drops its visible/hidden presence toggle.
    return c;
}

// ── Realtime via IMAP IDLE (RFC 2177) ───────────────────────────────────────
// A dedicated second connection SELECTs INBOX and IDLEs; on an EXISTS push it
// stops IDLE, fetches messages with UID >= the baseline, emits EvMessageNew
// (creating the conversation if new), advances the baseline, and re-IDLEs. A
// timer refreshes IDLE before the server's ~29-min cap. Phase-1-scope limits:
// only INBOX is watched, and live thread replies / flag changes in other folders
// refresh on reopen (mirrors the Teams realtime limitations).
void Backend::connectRealtime() {
    if (_idleClient)
        return;
    _idleClient = new ImapClient();
    _idleClient->setInsecure(_creds.insecure);
    QObject::connect(_idleClient, &ImapClient::loggedIn, _idleClient, [this] {
        _idleClient->select(QStringLiteral("INBOX"), [this](bool ok, SelectResult sel) {
            if (!ok)
                return;
            _idleUidNext = sel.uidNext; // anything from here on is new
            qInfo("imap idle: watching INBOX (uidnext=%u)", _idleUidNext);
            beginIdle();
        });
    });
    QObject::connect(_idleClient, &ImapClient::error, _idleClient, [](const QString &e) {
        qWarning("imap idle: %s", qPrintable(e));
    });
    _idleClient->connectToServer(_creds.host, _creds.port);
    loginClient(_idleClient);

    if (!_idleRefresh) {
        _idleRefresh = new QTimer(_idleClient);
        _idleRefresh->setInterval(29 * 60 * 1000); // beat the 30-min IDLE cap
        QObject::connect(_idleRefresh, &QTimer::timeout, _idleClient, [this] {
            if (_idleClient && _idleClient->isIdling())
                _idleClient->stopIdle([this] { beginIdle(); });
        });
    }
}

void Backend::disconnectRealtime() {
    if (_idleRefresh)
        _idleRefresh->stop();
    if (_idleClient) {
        _idleClient->stopIdle();
        delete _idleClient; // also deletes the child refresh timer
        _idleClient  = nullptr;
        _idleRefresh = nullptr;
    }
}

void Backend::beginIdle() {
    if (!_idleClient || !_idleClient->isLoggedIn())
        return;
    if (!_idleClient->hasCapability("IDLE"))
        return; // server without IDLE: Phase 1 has no polling fallback yet
    _idleClient->startIdle([this](const QByteArray &push) {
        if (push.contains("EXISTS") || push.contains("RECENT"))
            onIdleActivity();
    });
    if (_idleRefresh && !_idleRefresh->isActive())
        _idleRefresh->start();
}

void Backend::onIdleActivity() {
    if (!_idleClient || !_idleClient->isIdling())
        return;
    _idleClient->stopIdle([this] { fetchNewMail(); });
}

void Backend::fetchNewMail() {
    const QByteArray crit = "UID " + QByteArray::number(_idleUidNext) + ":*";
    _idleClient->uidSearch(crit, [this](bool ok, QList<quint32> uids) {
        QList<quint32> fresh;
        for (quint32 u : uids)
            if (u >= _idleUidNext)
                fresh.append(u);
        if (!ok || fresh.isEmpty()) {
            beginIdle();
            return;
        }
        std::sort(fresh.begin(), fresh.end());
        _idleUidNext         = fresh.last() + 1;
        const QByteArray set = joinUids(fresh);
        _idleClient->uidFetch(
            set,
            "UID FLAGS INTERNALDATE ENVELOPE BODY.PEEK[]",
            [this](bool, QList<QByteArray> lines) {
                for (const FetchItem &it : Mappers::parseFetch(lines)) {
                    if (!it.hasEnvelope)
                        continue;
                    MsgRef r;
                    r.uid          = it.uid;
                    r.mailbox      = QStringLiteral("INBOX");
                    r.env          = it.envelope;
                    r.internalDate = it.internalDate;
                    r.seen         = it.seen();

                    const QList<QString> parts =
                        Bucketing::participantsOf(it.envelope, _myAddresses);
                    const auto cl = Bucketing::classify(parts, r.listId, 8);

                    // New conversation? announce it first.
                    if (!_index.contains(cl.convId)) {
                        Conversation c = conversationFor(cl.convId, cl.kind, parts, r.listId);
                        _index[cl.convId].conv = c;
                        _events.fire(EvChannelCreated{c});
                    }
                    _index[cl.convId].messages.append(r);

                    const Message msg = buildMessage(r, it.hasBody ? it.rawBody : QByteArray{});
                    _events.fire(EvMessageNew{ConversationId{cl.convId}, msg});
                }
                beginIdle(); // resume watching
            }
        );
    });
}

Conversation Backend::conversationFor(
    const QString &convId, ConvKind kind, const QList<QString> &participants, const QString &listId
) const {
    auto label = [this](const QString &email) -> QString {
        const auto it = _users.constFind(email);
        return it != _users.constEnd() ? it->displayLabel() : email;
    };
    Conversation c;
    c.id       = ConversationId{convId};
    c.kind     = kind;
    c.isMember = true;
    if (kind == ConvKind::Im && participants.size() == 1) {
        c.dmUser = UserId{participants.first()};
        c.name   = label(participants.first());
    } else if (kind == ConvKind::Mpim) {
        QStringList names;
        for (const QString &e : participants) {
            c.members.push_back(UserId{e});
            names << label(e);
        }
        c.name = names.join(QStringLiteral(", "));
    } else if (!listId.isEmpty()) {
        c.name = listId;
    } else if (convId == QStringLiteral("channel:broadcast")) {
        c.name = QStringLiteral("Large threads");
    } else if (convId == QStringLiteral("dm:self")) {
        c.name = QStringLiteral("Me");
    }
    return c;
}

void Backend::whenReady(std::function<void()> fn) {
    if (_ready || _failed)
        fn();
    else
        _pendingReady.push_back(std::move(fn));
}

void Backend::flushReady() {
    auto pending = std::move(_pendingReady);
    _pendingReady.clear();
    for (auto &fn : pending)
        fn();
}

void Backend::whenScanned(std::function<void()> fn) {
    if (_scanned || _failed)
        fn();
    else
        _scanWaiters.push_back(std::move(fn));
}

void Backend::markScanned() {
    if (_scanned)
        return;
    _scanned     = true;
    auto waiters = std::move(_scanWaiters);
    _scanWaiters.clear();
    for (auto &fn : waiters)
        fn();
}

rpl::producer<UserId> Backend::loadMe() {
    return [this](auto consumer) mutable {
        const UserId me{_creds.user.toLower()};
        User         self;
        self.id          = me;
        self.name        = _creds.user;
        self.displayName = _creds.user;
        self.email       = _creds.user;
        self.avatarUrl   = gravatarUrl(_creds.user);
        _events.fire(EvUserChanged{self});
        consumer.put_next(UserId{me});
        consumer.put_done();
        return rpl::lifetime();
    };
}

void Backend::scan(std::function<void(std::vector<Conversation>)> done) {
    if (!_client->isLoggedIn()) {
        markScanned(); // no scan will run — release whenScanned() waiters
        done({});
        return;
    }
    // First LIST → folder/label channels, then scan INBOX for participant buckets.
    _client->list([this, done](bool, QList<Mailbox> mailboxes) {
        std::vector<Conversation> folderChannels;
        _folderMailbox.clear();
        for (const Mailbox &m : mailboxes) {
            if (m.hasFlag(QStringLiteral("\\Sent")))
                _sentMailbox = m.name; // for APPEND of sent messages
            if (!isUserFolder(m))
                continue;
            const QString id = QStringLiteral("folder:") + m.name;
            Conversation  c;
            c.id       = ConversationId{id};
            c.kind     = ConvKind::PublicChannel;
            c.name     = folderLabel(m);
            c.isMember = true;
            _folderMailbox.insert(id, m.name);
            folderChannels.push_back(c);
        }
        scanInboxThen(folderChannels, done);
    });
}

void Backend::scanInboxThen(
    std::vector<Conversation> folderChannels, std::function<void(std::vector<Conversation>)> done
) {
    _client->select(QStringLiteral("INBOX"), [this, folderChannels, done](bool ok, SelectResult) {
        if (!ok) {
            markScanned();        // release whenScanned() waiters
            done(folderChannels); // INBOX unreadable — still expose folder channels
            return;
        }
        _client->uidSearch("ALL", [this, folderChannels, done](bool ok2, QList<quint32> uids) {
            if (!ok2 || uids.isEmpty()) {
                markScanned();
                done(folderChannels);
                return;
            }
            std::sort(uids.begin(), uids.end());
            if (uids.size() > kScanWindow)
                uids = uids.mid(uids.size() - kScanWindow);
            const QByteArray set =
                QByteArray::number(uids.first()) + ":" + QByteArray::number(uids.last());

            _client->uidFetch(
                set,
                "UID FLAGS INTERNALDATE ENVELOPE",
                [this, set, folderChannels, done](bool ok3, QList<QByteArray> lines) {
                    if (!ok3) {
                        markScanned();
                        done(folderChannels);
                        return;
                    }
                    // Parse + bucket the (up to 500) ENVELOPEs ON A WORKER THREAD, then
                    // apply the result on the main thread — keeps the UI responsive.
                    auto offload = [this, lines, folderChannels, done](
                                       QByteArray threadLine, bool useThread
                                   ) {
                        auto *w = new QFutureWatcher<BucketResult>(_client);
                        QObject::connect(
                            w,
                            &QFutureWatcher<BucketResult>::finished,
                            _client,
                            [this, w, folderChannels, done] {
                                BucketResult br = w->result();
                                w->deleteLater();
                                _index = br.byId;
                                for (auto it = br.users.cbegin(); it != br.users.cend(); ++it)
                                    _users.insert(it.key(), it.value());
                                // Stamp persisted domain icons onto the fresh users BEFORE
                                // markScanned() delivers the loadUsers snapshot: bucketing
                                // reset every avatarUrl to Gravatar, and a snapshot carrying
                                // those would wipe the icons the UI already shows from cache
                                // until the batched re-apply lands — a visible flicker of
                                // every avatar on each start.
                                applyCachedDomainIcons();
                                // Users reach the UI in ONE bulk loadUsers() emission, not
                                // a per-user EvUserChanged storm (which froze the UI).
                                // markScanned() also releases any loadHistory calls that
                                // arrived while the scan was still running.
                                markScanned();
                                resolveBimiForUsers(br.users);
                                std::vector<Conversation> all = br.conversations;
                                all.insert(all.end(), folderChannels.begin(), folderChannels.end());
                                qInfo(
                                    "imap: %zu DM/MPDM convs + %zu folder channels, "
                                    "%d users",
                                    br.conversations.size(),
                                    folderChannels.size(),
                                    int(br.users.size())
                                );
                                done(all);
                            }
                        );
                        w->setFuture(
                            QtConcurrent::run(
                                scanBucket,
                                lines,
                                threadLine,
                                useThread,
                                _myAddresses,
                                _creds.user.toLower()
                            )
                        );
                    };

                    if (_client->hasCapability("THREAD=REFERENCES"))
                        _client->uidThread(
                            "REFERENCES",
                            "UTF-8",
                            "UID " + set,
                            [offload](bool, QByteArray threadLine) { offload(threadLine, true); }
                        );
                    else
                        offload({}, false); // bucketer falls back to In-Reply-To union-find
                }
            );
        });
    });
}

rpl::producer<std::vector<Conversation>> Backend::loadConversations() {
    return [this](auto consumer) mutable {
        whenReady([this, consumer]() mutable {
            scan([consumer](std::vector<Conversation> convs) mutable {
                consumer.put_next(std::move(convs));
                consumer.put_done();
            });
        });
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<User>> Backend::loadUsers() {
    return [this](auto consumer) mutable {
        // Deliver the full user map in one shot. If the INBOX scan (which
        // discovers the users) hasn't finished, wait for it rather than returning
        // an empty list — the scan flushes _usersWaiters on completion.
        auto deliver = [this, consumer]() mutable {
            std::vector<User> out;
            for (auto it = _users.cbegin(); it != _users.cend(); ++it)
                out.push_back(it.value());
            consumer.put_next(std::move(out));
            consumer.put_done();
        };
        whenReady([this, deliver]() mutable { whenScanned(std::move(deliver)); });
        return rpl::lifetime();
    };
}

rpl::producer<bool> Backend::loadPresence(UserId) {
    // Email has no presence; report "away" and complete.
    return [](auto consumer) {
        consumer.put_next(false);
        consumer.put_done();
        return rpl::lifetime();
    };
}

// Fetch BODY[] for `refs`, build the MessagePage ON A WORKER THREAD (MIME + HTML
// parsing is CPU-heavy), and emit it back on the main thread. `collapse` true →
// channel thread-roots with bold subject titles + reply counts.
template <typename Consumer>
void Backend::fetchBodiesAndEmit(
    const QList<MsgRef>           &refs,
    bool                           collapse,
    const QHash<QString, int>     &replyCountOf,
    const QHash<QString, QString> &latestReplyOf,
    const QString                 &olderCursor,
    Consumer                       consumer
) {
    QList<quint32> uids;
    for (const MsgRef &m : refs)
        uids.append(m.uid);
    if (uids.isEmpty()) {
        consumer.put_next(MessagePage{});
        consumer.put_done();
        return;
    }
    _client->uidFetch(
        joinUids(uids),
        "UID BODY.PEEK[]",
        [this, refs, collapse, replyCountOf, latestReplyOf, olderCursor, consumer](
            bool, QList<QByteArray> lines
        ) mutable {
            auto *w = new QFutureWatcher<MessagePage>(_client);
            QObject::connect(
                w,
                &QFutureWatcher<MessagePage>::finished,
                _client,
                [w, olderCursor, consumer]() mutable {
                    MessagePage page = w->result();
                    w->deleteLater();
                    if (!olderCursor.isEmpty()) // enables the UI's "load more"
                        page.olderCursor = olderCursor;
                    consumer.put_next(std::move(page)); // back on the main thread
                    consumer.put_done();
                }
            );
            w->setFuture(
                QtConcurrent::run(buildPage, lines, refs, collapse, replyCountOf, latestReplyOf)
            );
        }
    );
}

rpl::producer<MessagePage>
Backend::loadHistory(ConversationId conv, std::optional<QString> cursor) {
    return [this, conv, cursor](auto consumer) mutable {
        whenReady([this, conv, cursor, consumer]() mutable {
            if (!_client->isLoggedIn()) {
                consumer.put_next(MessagePage{});
                consumer.put_done();
                return;
            }
            const bool channel = isChannelConvId(conv.value);

            // ── Older-history paging: cursor = the oldest UID currently shown ──
            if (cursor && !cursor->isEmpty()) {
                const Pagination pg = paginationFor(conv.value);
                if (!pg.supported) {
                    consumer.put_next(MessagePage{}); // no "load more" for this conv kind
                    consumer.put_done();
                    return;
                }
                const quint32 cursorUid = cursor->toUInt();
                _client->select(
                    pg.mailbox, [this, pg, cursorUid, consumer](bool ok, SelectResult) mutable {
                        if (!ok) {
                            consumer.put_next(MessagePage{});
                            consumer.put_done();
                            return;
                        }
                        _client->uidSearch(
                            pg.criteria,
                            [this, pg, cursorUid, consumer](bool ok2, QList<quint32> uids) mutable {
                                QList<quint32> older;
                                for (quint32 u : uids)
                                    if (u < cursorUid)
                                        older.append(u);
                                std::sort(older.begin(), older.end());
                                if (!ok2 || older.isEmpty()) {
                                    consumer.put_next(MessagePage{}); // nothing older
                                    consumer.put_done();
                                    return;
                                }
                                const int      page = qMin(kFolderWindow, int(older.size()));
                                QList<quint32> slice =
                                    older.mid(older.size() - page); // newest of the older
                                const QString newCursor = older.size() > page
                                                              ? QString::number(slice.first())
                                                              : QString();
                                const QString mailbox   = pg.mailbox;
                                _client->uidFetch(
                                    joinUids(slice),
                                    "UID FLAGS INTERNALDATE ENVELOPE",
                                    [this, mailbox, newCursor, consumer](
                                        bool, QList<QByteArray> lines
                                    ) mutable {
                                        QList<MsgRef> refs;
                                        for (const FetchItem &it : Mappers::parseFetch(lines)) {
                                            if (!it.hasEnvelope)
                                                continue;
                                            MsgRef r;
                                            r.uid          = it.uid;
                                            r.mailbox      = mailbox;
                                            r.env          = it.envelope;
                                            r.internalDate = it.internalDate;
                                            r.seen         = it.seen();
                                            refs.append(r);
                                        }
                                        std::sort(
                                            refs.begin(),
                                            refs.end(),
                                            [](const MsgRef &a, const MsgRef &b) {
                                                return a.env.date < b.env.date;
                                            }
                                        );
                                        // Older pages render flat (threads may straddle pages).
                                        fetchBodiesAndEmit(
                                            refs, false, {}, {}, newCursor, consumer
                                        );
                                    }
                                );
                            }
                        );
                    }
                );
                return;
            }

            // ── Initial load ────────────────────────────────────────────────
            // Wait for the INBOX scan to populate _index / _folderMailbox before
            // deciding a conversation has no history — it runs on a worker thread,
            // so a conversation opened immediately after launch (sidebar served
            // from cache) would otherwise read a half-built index and wrongly show
            // "beginning of history".
            whenScanned([this, conv, channel, consumer]() mutable {
                // Emit a conversation we have a populated ConvData for, stamping an
                // olderCursor (the oldest shown UID) when the conv supports paging.
                auto emitConv = [this, conv, channel, consumer](
                                    const ConvData &cd, const QString &olderCursor
                                ) mutable {
                    if (cd.messages.isEmpty()) {
                        consumer.put_next(MessagePage{});
                        consumer.put_done();
                        return;
                    }
                    const QString mailbox = cd.messages.first().mailbox;

                    QList<MsgRef>           display;
                    QHash<QString, int>     replyCountOf;
                    QHash<QString, QString> latestReplyOf;
                    if (channel) {
                        QHash<QString, QList<MsgRef>> byRoot;
                        for (const MsgRef &m : cd.messages)
                            byRoot[cd.threadRootOf.value(msgKeyOf(m), msgKeyOf(m))].append(m);
                        for (auto it = byRoot.begin(); it != byRoot.end(); ++it) {
                            QList<MsgRef> g = it.value();
                            std::sort(g.begin(), g.end(), [](const MsgRef &a, const MsgRef &b) {
                                return a.env.date < b.env.date;
                            });
                            display.append(g.first());
                            replyCountOf[it.key()] = int(g.size()) - 1;
                            if (g.size() > 1)
                                latestReplyOf[it.key()] = msgKeyOf(g.last());
                        }
                    } else {
                        display = cd.messages; // DM/MPDM: inline, flat
                    }
                    std::sort(display.begin(), display.end(), [](const MsgRef &a, const MsgRef &b) {
                        return a.env.date < b.env.date;
                    });

                    _client->select(
                        mailbox,
                        [this,
                         channel,
                         display,
                         replyCountOf,
                         latestReplyOf,
                         olderCursor,
                         consumer](bool ok, SelectResult) mutable {
                            if (!ok) {
                                consumer.put_next(MessagePage{});
                                consumer.put_done();
                                return;
                            }
                            fetchBodiesAndEmit(
                                display, channel, replyCountOf, latestReplyOf, olderCursor, consumer
                            );
                        }
                    );
                };

                // Pre-indexed conversation (DM/MPDM/list/broadcast)?
                const auto cdIt = _index.constFind(conv.value);
                if (cdIt != _index.constEnd() && !cdIt->messages.isEmpty()) {
                    quint32 oldest = std::numeric_limits<quint32>::max();
                    for (const MsgRef &m : cdIt->messages)
                        oldest = std::min(oldest, m.uid);
                    const QString oc =
                        paginationFor(conv.value).supported ? QString::number(oldest) : QString();
                    emitConv(cdIt.value(), oc);
                    return;
                }
                // Folder channel: scan its recent messages on open, thread them, cache.
                const auto fIt = _folderMailbox.constFind(conv.value);
                if (fIt == _folderMailbox.constEnd()) {
                    consumer.put_next(MessagePage{});
                    consumer.put_done();
                    return;
                }
                const QString mailbox = fIt.value();
                _client->select(
                    mailbox,
                    [this, conv, mailbox, emitConv, consumer](bool ok, SelectResult) mutable {
                        if (!ok) {
                            consumer.put_next(MessagePage{});
                            consumer.put_done();
                            return;
                        }
                        _client->uidSearch(
                            "ALL",
                            [this, conv, mailbox, emitConv, consumer](
                                bool ok2, QList<quint32> uids
                            ) mutable {
                                if (!ok2 || uids.isEmpty()) {
                                    consumer.put_next(MessagePage{});
                                    consumer.put_done();
                                    return;
                                }
                                std::sort(uids.begin(), uids.end());
                                const bool more = uids.size() > kFolderWindow;
                                if (more)
                                    uids = uids.mid(uids.size() - kFolderWindow);
                                const QByteArray set = QByteArray::number(uids.first()) + ":" +
                                                       QByteArray::number(uids.last());
                                const QString olderCursor =
                                    more ? QString::number(uids.first()) : QString();
                                _client->uidFetch(
                                    set,
                                    "UID FLAGS INTERNALDATE ENVELOPE",
                                    [this, conv, mailbox, olderCursor, emitConv](
                                        bool, QList<QByteArray> lines
                                    ) mutable {
                                        QList<MsgRef> refs;
                                        for (const FetchItem &it : Mappers::parseFetch(lines)) {
                                            if (!it.hasEnvelope)
                                                continue;
                                            MsgRef r;
                                            r.uid          = it.uid;
                                            r.mailbox      = mailbox;
                                            r.env          = it.envelope;
                                            r.internalDate = it.internalDate;
                                            r.seen         = it.seen();
                                            refs.append(r);
                                        }
                                        ConvData cd;
                                        cd.conv.id   = conv;
                                        cd.conv.kind = ConvKind::PublicChannel;
                                        cd.messages  = refs;
                                        for (const QList<quint32> &g :
                                             Bucketing::threadByReferences(refs)) {
                                            if (g.isEmpty())
                                                continue;
                                            QString rootId;
                                            for (const MsgRef &m : refs)
                                                if (m.uid == g.first()) {
                                                    rootId = msgKeyOf(m);
                                                    break;
                                                }
                                            for (quint32 u : g)
                                                for (const MsgRef &m : refs)
                                                    if (m.uid == u)
                                                        cd.threadRootOf.insert(msgKeyOf(m), rootId);
                                            cd.replyCountOf[rootId] += int(g.size()) - 1;
                                        }
                                        _index.insert(conv.value, cd);
                                        emitConv(cd, olderCursor);
                                    }
                                );
                            }
                        );
                    }
                );
            }); // whenScanned
        });
        return rpl::lifetime();
    };
}

Backend::Pagination Backend::paginationFor(const QString &convId) const {
    Pagination p;
    if (convId.startsWith(QLatin1String("dm:"))) {
        const QString email = convId.mid(3);
        if (email == QLatin1String("self"))
            return p; // notes-to-self: not paged
        p.mailbox          = QStringLiteral("INBOX");
        const QByteArray e = Proto::quote(email.toUtf8());
        p.criteria         = "OR FROM " + e + " TO " + e; // mail to/from the peer, all history
        p.supported        = true;
    } else if (convId.startsWith(QLatin1String("folder:"))) {
        const auto it = _folderMailbox.constFind(convId);
        if (it != _folderMailbox.constEnd()) {
            p.mailbox   = it.value();
            p.criteria  = "ALL";
            p.supported = true;
        }
    }
    // MPDM / list / broadcast paging is a follow-up (no "load more" for now).
    return p;
}

rpl::producer<MessagePage>
Backend::loadThread(ConversationId conv, Ts root, std::optional<QString>) {
    return [this, conv, root](auto consumer) mutable {
        whenReady([this, conv, root, consumer]() mutable {
            const auto cdIt = _index.constFind(conv.value);
            if (!_client->isLoggedIn() || cdIt == _index.constEnd()) {
                consumer.put_next(MessagePage{});
                consumer.put_done();
                return;
            }
            const ConvData cd = cdIt.value();
            // All messages in the requested thread (root + replies), date order.
            QList<MsgRef>  thread;
            for (const MsgRef &m : cd.messages)
                if (cd.threadRootOf.value(msgKeyOf(m), msgKeyOf(m)) == root)
                    thread.append(m);
            std::sort(thread.begin(), thread.end(), [](const MsgRef &a, const MsgRef &b) {
                return a.env.date < b.env.date;
            });
            if (thread.isEmpty()) {
                consumer.put_next(MessagePage{});
                consumer.put_done();
                return;
            }
            const QString mailbox = thread.first().mailbox;
            _client->select(mailbox, [this, thread, consumer](bool ok, SelectResult) mutable {
                if (!ok) {
                    consumer.put_next(MessagePage{});
                    consumer.put_done();
                    return;
                }
                fetchBodiesAndEmit(
                    thread, /*collapse=*/false, {}, {}, /*olderCursor=*/{}, consumer
                );
            });
        });
        return rpl::lifetime();
    };
}

quint32 Backend::uidForTs(const QString &convId, const Ts &ts) const {
    const auto it = _index.constFind(convId);
    if (it == _index.constEnd())
        return 0;
    for (const MsgRef &m : it->messages)
        if (msgKeyOf(m) == ts)
            return m.uid;
    return 0;
}

QString Backend::mailboxForTs(const QString &convId, const Ts &ts) const {
    const auto it = _index.constFind(convId);
    if (it != _index.constEnd())
        for (const MsgRef &m : it->messages)
            if (msgKeyOf(m) == ts && !m.mailbox.isEmpty())
                return m.mailbox;
    return QStringLiteral("INBOX");
}

void Backend::markRead(ConversationId conv, Ts ts) {
    whenReady([this, conv, ts]() mutable {
        if (!_client->isLoggedIn())
            return;
        auto it = _index.find(conv.value);
        if (it == _index.end())
            return;
        // Mark the conversation read up to and including `ts` — opening it shows
        // every message, and Session passes the latest ts, so this is normally the
        // whole thread. Collect still-unseen UIDs grouped by their own mailbox
        // (UIDs are per-mailbox; a DM can span INBOX + Sent), and flip the local
        // \Seen flags now so the cleared state survives restart via the cache
        // instead of waiting for the next server rescan to re-derive it.
        QHash<QString, QList<quint32>> byMailbox;
        for (MsgRef &m : it->messages) {
            const bool isCutoff = (msgKeyOf(m) == ts);
            if (!m.seen && m.uid != 0) {
                byMailbox[m.mailbox.isEmpty() ? QStringLiteral("INBOX") : m.mailbox].append(m.uid);
                m.seen = true;
            }
            if (isCutoff)
                break; // don't mark anything newer than the read cursor
        }
        if (byMailbox.isEmpty())
            return; // already all seen
        auto marker    = std::make_shared<SeenMarker>();
        marker->client = _client;
        for (auto mb = byMailbox.constBegin(); mb != byMailbox.constEnd(); ++mb)
            marker->jobs.append({mb.key(), joinUidSet(mb.value())});
        marker->run();
    });
}

// Shared send path for plain messages and file uploads: resolve recipients/
// subject/threading from the conversation, build the MIME (+ attachments),
// SMTP-submit, APPEND to Sent, and echo EvMessageNew so Session reconciles the
// optimistic ghost. done(false, err) on any failure.
void Backend::submitMail(
    ConversationId                     conv,
    const QString                     &body,
    std::optional<Ts>                  threadRoot,
    const QString                     &subjectIn,
    const QList<OutgoingAttachment>   &attachments,
    std::function<void(bool, QString)> done
) {
    whenReady([this, conv, body, threadRoot, subjectIn, attachments, done]() mutable {
        const QString  me   = _creds.user.toLower();
        const auto     cdIt = _index.constFind(conv.value);
        const ConvData cd   = cdIt != _index.constEnd() ? cdIt.value() : ConvData{};

        QStringList toList, ccList;
        QString     subject = subjectIn.trimmed();
        QString     inReplyTo;
        QStringList references;

        // The reply target: an explicit thread root, else (for a DM/MPDM) the
        // latest message in the thread — sending into an existing email thread is
        // a reply, so it behaves like Reply-All in a normal mail client.
        const MsgRef *root = nullptr;
        if (threadRoot) {
            for (const MsgRef &m : cd.messages)
                if (msgKeyOf(m) == *threadRoot) {
                    root = &m;
                    break;
                }
        } else if (conv.value.startsWith(QLatin1String("dm:")) ||
                   conv.value.startsWith(QLatin1String("mpim:"))) {
            for (const MsgRef &m : cd.messages)
                if (!root || m.internalDate > root->internalDate)
                    root = &m;
        }

        if (root) {
            // Reply-All, like a normal mail client (To = sender, Cc = the rest).
            const auto rr = Bucketing::replyRecipients(root->env, _myAddresses);
            toList        = rr.to;
            ccList        = rr.cc;
            inReplyTo     = !root->env.messageId.isEmpty() ? root->env.messageId : msgKeyOf(*root);
            references << inReplyTo;
            if (subject.isEmpty()) {
                subject = root->env.subject.trimmed();
                if (!subject.startsWith(QLatin1String("Re:"), Qt::CaseInsensitive))
                    subject = QStringLiteral("Re: ") + subject;
            }
        } else if (conv.value.startsWith(QLatin1String("dm:"))) {
            const QString e = conv.value.mid(3).trimmed();
            if (!e.isEmpty() && e != QLatin1String("self") && !_myAddresses.contains(e.toLower()))
                toList << e;
        } else if (conv.value.startsWith(QLatin1String("mpim:"))) {
            for (const QString &e : conv.value.mid(5).split(QLatin1Char(','), Qt::SkipEmptyParts))
                if (!_myAddresses.contains(e.trimmed().toLower()))
                    toList << e.trimmed();
        }

        if (toList.isEmpty() && ccList.isEmpty()) {
            if (done)
                done(false, QStringLiteral("no_recipients"));
            return;
        }
        if (subject.isEmpty()) {
            if (done)
                done(false, QStringLiteral("subject_required"));
            return;
        }
        // SMTP envelope (RCPT TO) must list To *and* Cc so Cc'd people receive it.
        QStringList recipients = toList;
        recipients += ccList;

        ComposeParams cp;
        cp.fromEmail         = _creds.user;
        cp.to                = toList;
        cp.cc                = ccList;
        cp.subject           = subject;
        cp.bodyText          = body;
        cp.inReplyTo         = inReplyTo;
        cp.references        = references;
        cp.attachments       = attachments;
        const QString domain = me.section(QLatin1Char('@'), 1);
        cp.messageId         = QUuid::createUuid().toString(QUuid::WithoutBraces) + "@" + domain;
        cp.dateRfc2822       = QDateTime::currentDateTime().toString(Qt::RFC2822Date);
        if (!attachments.isEmpty())
            cp.boundary = "msga_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QByteArray raw = buildMimeMessage(cp);

        // Refresh the OAuth token (if any) so SMTP authenticates with a live
        // bearer; runs the body synchronously for password auth.
        ensureFreshToken([this, conv, cp, body, threadRoot, attachments, raw, recipients, done] {
            auto *smtp = new SmtpClient(_client);
            smtp->setInsecure(_creds.insecure);
            const QString smtpHost = !_creds.smtpHost.isEmpty() ? _creds.smtpHost : _creds.host;
            const bool    xoauth2  = usingOAuth();
            const QString secret   = xoauth2 ? _creds.accessToken : _creds.password;
            smtp->send(
                smtpHost,
                _creds.smtpPort,
                _creds.user,
                secret,
                _creds.user,
                recipients,
                raw,
                [this, conv, cp, body, threadRoot, attachments, raw, smtp, done](
                    bool ok, QString err
                ) {
                    smtp->deleteLater();
                    if (!ok) {
                        if (done)
                            done(false, err.isEmpty() ? QStringLiteral("smtp_failed") : err);
                        return;
                    }
                    // Save a copy to Sent (best-effort; needs LITERAL+).
                    if (!_sentMailbox.isEmpty() && _client->hasCapability("LITERAL+")) {
                        QByteArray cmd = "APPEND " + Proto::quote(_sentMailbox.toUtf8()) +
                                         " (\\Seen) {" + QByteArray::number(raw.size()) + "+}\r\n" +
                                         raw;
                        _client->sendCommand(cmd);
                    }
                    // Echo so Session replaces the optimistic ghost (matched by
                    // own-authored + withFiles kind, FIFO).
                    Message msg;
                    msg.ts         = cp.messageId;
                    msg.date       = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000;
                    msg.author     = UserId{_creds.user.toLower()};
                    msg.text.text  = body;
                    msg.threadRoot = threadRoot;
                    for (const OutgoingAttachment &a : attachments) {
                        File f;
                        f.name     = a.filename;
                        f.mimeType = a.mimeType;
                        f.size     = a.content.size();
                        msg.files.push_back(f);
                    }
                    _events.fire(EvMessageNew{conv, msg});
                    if (done)
                        done(true, {});
                },
                xoauth2
            );
        });
    });
}

void Backend::sendMessage(ConversationId conv, OutgoingMessage out) {
    submitMail(
        conv, out.text.text, out.threadRoot, out.subject, {}, [this, conv](bool ok, QString err) {
            if (!ok)
                _events.fire(EvSendFailed{conv, err});
        }
    );
}

void Backend::editMessage(ConversationId, Ts, TextWithEntities) {} // email: not supported

void Backend::deleteMessage(ConversationId conv, Ts ts) {
    whenReady([this, conv, ts]() mutable {
        if (!_client->isLoggedIn())
            return;
        const quint32 uid = uidForTs(conv.value, ts);
        if (uid == 0)
            return;
        const QString mailbox = mailboxForTs(conv.value, ts);
        const bool    uidPlus = _client->hasCapability("UIDPLUS");
        _client->select(mailbox, [this, conv, ts, uid, uidPlus](bool ok, SelectResult) {
            if (!ok)
                return;
            _client->sendCommand(
                "UID STORE " + QByteArray::number(uid) + " +FLAGS (\\Deleted)",
                [this, conv, ts, uid, uidPlus](const Response &r) {
                    if (!r.ok)
                        return;
                    // UID EXPUNGE removes just this message (UIDPLUS);
                    // plain EXPUNGE clears all \Deleted in the mailbox.
                    _client->sendCommand(
                        uidPlus ? ("UID EXPUNGE " + QByteArray::number(uid)) : QByteArray("EXPUNGE")
                    );
                    _events.fire(EvMessageDeleted{conv, ts, std::nullopt});
                }
            );
        });
    });
}

void Backend::labelMessage(
    ConversationId                     sourceConv,
    Ts                                 ts,
    ConversationId                     targetChannel,
    std::function<void(bool, QString)> done
) {
    whenReady([this, sourceConv, ts, targetChannel, done]() mutable {
        auto fail = [done](const QString &e) {
            if (done)
                done(false, e);
        };
        if (!_client->isLoggedIn())
            return fail(QStringLiteral("offline"));
        // Only folder/label channels are a valid label target.
        const auto fIt = _folderMailbox.constFind(targetChannel.value);
        if (fIt == _folderMailbox.constEnd())
            return fail(QStringLiteral("not_a_label"));
        const QString label = fIt.value();

        const quint32 uid = uidForTs(sourceConv.value, ts);
        if (uid == 0)
            return fail(QStringLiteral("message_not_found"));
        const QString mailbox = mailboxForTs(sourceConv.value, ts);
        const bool    gmail   = _client->hasCapability("X-GM-EXT-1");

        _client->select(mailbox, [this, uid, label, gmail, done](bool ok, SelectResult) {
            if (!ok) {
                if (done)
                    done(false, QStringLiteral("select_failed"));
                return;
            }
            // Gmail: add the label in place (message stays where it is). Generic
            // IMAP: COPY into the folder (the message then appears in both).
            const QByteArray cmd =
                gmail
                    ? ("UID STORE " + QByteArray::number(uid) + " +X-GM-LABELS (" +
                       Proto::quote(label.toUtf8()) + ")")
                    : ("UID COPY " + QByteArray::number(uid) + " " + Proto::quote(label.toUtf8()));
            _client->sendCommand(cmd, [done](const Response &r) {
                if (done)
                    done(r.ok, r.ok ? QString() : QString::fromUtf8(r.status));
            });
        });
    });
}

rpl::producer<std::vector<SearchResult>> Backend::searchMessages(const QString &query) {
    return [this, query](auto consumer) mutable {
        whenReady([this, query, consumer]() mutable {
            auto empty = [consumer]() mutable {
                consumer.put_next(std::vector<SearchResult>{});
                consumer.put_done();
            };
            const QByteArray q = query.trimmed().toUtf8();
            if (!_client->isLoggedIn() || q.isEmpty()) {
                empty();
                return;
            }
            _client->select(
                QStringLiteral("INBOX"), [this, q, consumer, empty](bool ok, SelectResult) mutable {
                    if (!ok) {
                        empty();
                        return;
                    }
                    // Gmail: X-GM-RAW (full Gmail query syntax). Generic: TEXT search
                    // (headers + body). Use a non-sync literal for UTF-8 when available.
                    const bool       gmail  = _client->hasCapability("X-GM-EXT-1");
                    const bool       lit    = _client->hasCapability("LITERAL+");
                    const QByteArray litArg = "{" + QByteArray::number(q.size()) + "+}\r\n" + q;
                    QByteArray       crit;
                    if (gmail)
                        crit = "X-GM-RAW " + (lit ? litArg : Proto::quote(q));
                    else if (lit)
                        crit = "CHARSET UTF-8 TEXT " + litArg;
                    else
                        crit = "TEXT " + Proto::quote(q);

                    _client->uidSearch(
                        crit, [this, consumer, empty](bool ok2, QList<quint32> uids) mutable {
                            if (!ok2 || uids.isEmpty()) {
                                empty();
                                return;
                            }
                            std::sort(uids.begin(), uids.end());
                            constexpr int kMax = 40; // most-recent matches
                            if (uids.size() > kMax)
                                uids = uids.mid(uids.size() - kMax);
                            _client->uidFetch(
                                joinUids(uids),
                                "UID FLAGS INTERNALDATE ENVELOPE BODY.PEEK[]",
                                [this, consumer](bool, QList<QByteArray> lines) mutable {
                                    std::vector<SearchResult> results;
                                    for (const FetchItem &it : Mappers::parseFetch(lines)) {
                                        if (!it.hasEnvelope)
                                            continue;
                                        MsgRef r;
                                        r.uid          = it.uid;
                                        r.mailbox      = QStringLiteral("INBOX");
                                        r.env          = it.envelope;
                                        r.internalDate = it.internalDate;
                                        r.seen         = it.seen();
                                        const auto parts =
                                            Bucketing::participantsOf(it.envelope, _myAddresses);
                                        const auto   cl = Bucketing::classify(parts, r.listId, 8);
                                        SearchResult sr;
                                        sr.conv = ConversationId{cl.convId};
                                        sr.convName =
                                            conversationFor(cl.convId, cl.kind, parts, r.listId)
                                                .name;
                                        sr.msg =
                                            buildMessage(r, it.hasBody ? it.rawBody : QByteArray{});
                                        results.push_back(std::move(sr));
                                    }
                                    std::reverse(results.begin(), results.end()); // newest first
                                    consumer.put_next(std::move(results));
                                    consumer.put_done();
                                }
                            );
                        }
                    );
                }
            );
        });
        return rpl::lifetime();
    };
}

rpl::producer<QHash<QString, QString>> Backend::loadEmojiList() {
    return [](auto consumer) {
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
    QList<OutgoingAttachment> atts;
    QMimeDatabase             db;
    for (const QString &path : filePaths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        OutgoingAttachment a;
        a.filename = QFileInfo(path).fileName();
        a.mimeType = db.mimeTypeForFile(path).name();
        a.content  = f.readAll();
        atts.append(a);
    }
    if (atts.isEmpty()) {
        if (done)
            done(false, QStringLiteral("no_files"));
        return;
    }
    submitMail(conv, initialComment, std::nullopt, {}, atts, done);
}

void Backend::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    // Inline images carry their bytes as a data: URI (decode in place).
    if (url.startsWith(QLatin1String("data:"))) {
        const int comma = url.indexOf(QLatin1Char(','));
        if (comma > 0) {
            const QByteArray payload = url.mid(comma + 1).toUtf8();
            const QByteArray bytes   = url.left(comma).contains(QLatin1String(";base64"))
                                           ? QByteArray::fromBase64(payload)
                                           : QByteArray::fromPercentEncoding(payload);
            if (onData)
                onData(bytes);
            return;
        }
        if (onError)
            onError(QStringLiteral("bad_data_uri"));
        return;
    }
    // Lazy attachment "imapfile:<mailbox>|<uid>|<filename>": re-fetch the message
    // and extract the named part on demand (downloads are infrequent).
    if (url.startsWith(QLatin1String("imapfile:"))) {
        const QStringList parts = url.mid(9).split(QLatin1Char('|'));
        if (parts.size() < 3) {
            if (onError)
                onError(QStringLiteral("bad_ref"));
            return;
        }
        const QString mailbox = parts[0];
        const quint32 uid     = parts[1].toUInt();
        const QString name    = parts.mid(2).join(QLatin1Char('|'));
        whenReady([this, mailbox, uid, name, onData, onError]() mutable {
            if (!_client->isLoggedIn()) {
                if (onError)
                    onError(QStringLiteral("offline"));
                return;
            }
            _client->select(mailbox, [this, uid, name, onData, onError](bool ok, SelectResult) {
                if (!ok) {
                    if (onError)
                        onError(QStringLiteral("select_failed"));
                    return;
                }
                _client->uidFetch(
                    QByteArray::number(uid),
                    "BODY.PEEK[]",
                    [name, onData, onError](bool, QList<QByteArray> lines) {
                        for (const FetchItem &it : Mappers::parseFetch(lines)) {
                            if (!it.hasBody)
                                continue;
                            const ParsedMessage pm = Mime::parse(it.rawBody);
                            for (const MimeAttachment &a : pm.attachments)
                                if (a.filename == name ||
                                    (a.filename.isEmpty() && name == "attachment")) {
                                    if (onData)
                                        onData(a.content);
                                    return;
                                }
                        }
                        if (onError)
                            onError(QStringLiteral("attachment_not_found"));
                    }
                );
            });
        });
        return;
    }
    if (onError)
        onError(QStringLiteral("not_supported"));
}

rpl::producer<Event> Backend::events() const {
    return _events.events();
}

} // namespace imap
