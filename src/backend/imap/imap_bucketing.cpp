// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_bucketing.h"

#include "backend/imap/imap_providers.h"

#include <algorithm>

namespace imap {

namespace {

QString msgKey(const MsgRef &m) {
    return m.env.messageId.isEmpty() ? (QStringLiteral("uid:") + QString::number(m.uid))
                                     : m.env.messageId;
}

QString displayLabelFor(const MimeAddress &a) {
    if (!a.name.isEmpty())
        return a.name;
    const int at = a.email.indexOf('@');
    return at > 0 ? a.email : a.email; // keep full email; UI can shorten
}

// Union-find over message ids for the no-server-THREAD fallback.
class UnionFind {
public:
    QString find(const QString &x) {
        if (!_p.contains(x)) {
            _p[x] = x;
            return x;
        }
        QString r = x;
        while (_p[r] != r)
            r = _p[r];
        // path-compress
        QString c = x;
        while (_p[c] != r) {
            const QString n = _p[c];
            _p[c]           = r;
            c               = n;
        }
        return r;
    }
    void unite(const QString &a, const QString &b) {
        const QString ra = find(a), rb = find(b);
        if (ra != rb)
            _p[ra] = rb;
    }

private:
    QHash<QString, QString> _p;
};

} // namespace

BucketResult
Bucketer::run(const QList<MsgRef> &msgs, const QList<QList<quint32>> &serverThreads) const {
    BucketResult result;

    QHash<quint32, const MsgRef *> byUid;
    for (const MsgRef &m : msgs)
        byUid.insert(m.uid, &m);

    // ── 1. Thread groups (UID lists, root-first) ────────────────────────────
    QList<QList<quint32>> groups;
    if (!serverThreads.isEmpty()) {
        for (const QList<quint32> &g : serverThreads) {
            QList<quint32> known;
            for (quint32 u : g)
                if (byUid.contains(u))
                    known.append(u);
            if (!known.isEmpty())
                groups.append(known);
        }
    } else {
        groups = Bucketing::threadByReferences(msgs); // In-Reply-To / Message-ID union-find
    }

    // Collect users from every address seen.
    auto noteUser = [&](const MimeAddress &a) {
        if (a.email.isEmpty())
            return;
        User &u = result.users[a.email];
        u.id    = UserId{a.email};
        if (u.name.isEmpty())
            u.name = a.email;
        if (!a.name.isEmpty())
            u.displayName = a.name;
        if (u.avatarUrl.isEmpty())
            u.avatarUrl = gravatarUrl(a.email); // real photo if any, else initials fallback
    };

    // ── 2. Assign each thread to a conversation ─────────────────────────────
    for (const QList<quint32> &g : groups) {
        QList<MsgRef> gm;
        for (quint32 u : g)
            if (const MsgRef *m = byUid.value(u, nullptr))
                gm.append(*m);
        if (gm.isEmpty())
            continue;
        std::sort(gm.begin(), gm.end(), [](const MsgRef &a, const MsgRef &b) {
            return a.env.date < b.env.date;
        });

        // Participant union (minus me) + list detection + user harvest.
        QList<QString> participants; // ordered unique emails
        QSet<QString>  seenEmail;
        QString        listId;
        for (const MsgRef &m : gm) {
            for (const auto *lst : {&m.env.from, &m.env.to, &m.env.cc})
                for (const MimeAddress &a : *lst) {
                    noteUser(a);
                    if (a.email.isEmpty() || isMine(a.email) || seenEmail.contains(a.email))
                        continue;
                    seenEmail.insert(a.email);
                    participants.append(a.email);
                }
            if (listId.isEmpty() && !m.listId.isEmpty())
                listId = m.listId;
        }
        std::sort(participants.begin(), participants.end());

        // Classify → conversation id + kind (shared with the realtime path).
        const Bucketing::Classified cl     = Bucketing::classify(participants, listId, _cap);
        const QString               convId = cl.convId;
        const ConvKind              kind   = cl.kind;

        ConvData &cd     = result.byId[convId];
        cd.conv.id       = ConversationId{convId};
        cd.conv.kind     = kind;
        cd.conv.isMember = true;
        if (kind == ConvKind::Im && participants.size() == 1)
            cd.conv.dmUser = UserId{participants.first()};
        if (kind == ConvKind::Mpim) {
            cd.conv.members.clear();
            for (const QString &e : participants)
                cd.conv.members.push_back(UserId{e});
        }
        // Stash routing hints for naming/finalize.
        if (kind == ConvKind::PublicChannel && !listId.isEmpty() && cd.conv.name.isEmpty())
            cd.conv.name = listId;
        else if (convId == QStringLiteral("channel:broadcast") && cd.conv.name.isEmpty())
            cd.conv.name = QStringLiteral("Large threads");
        else if (convId == QStringLiteral("dm:self") && cd.conv.name.isEmpty())
            cd.conv.name = QStringLiteral("Me");

        // Thread linkage within this conversation.
        const QString rootId = msgKey(gm.first());
        for (const MsgRef &m : gm)
            cd.threadRootOf.insert(msgKey(m), rootId);
        cd.replyCountOf[rootId] += static_cast<int>(gm.size()) - 1;

        for (const MsgRef &m : gm)
            cd.messages.append(m);
    }

    // ── 3. Finalize each conversation (sort, names, counts) ─────────────────
    auto userLabel = [&](const QString &email) -> QString {
        const auto it = result.users.constFind(email);
        if (it != result.users.constEnd())
            return it->displayLabel();
        return email;
    };

    for (auto it = result.byId.begin(); it != result.byId.end(); ++it) {
        ConvData &cd = it.value();
        std::sort(cd.messages.begin(), cd.messages.end(), [](const MsgRef &a, const MsgRef &b) {
            return a.env.date < b.env.date;
        });
        int unread = 0;
        for (const MsgRef &m : cd.messages)
            if (!m.seen)
                ++unread;
        cd.conv.unread       = unread;
        const bool isDm      = cd.conv.kind == ConvKind::Im || cd.conv.kind == ConvKind::Mpim;
        cd.conv.mentionCount = isDm ? unread : 0;
        if (!cd.messages.isEmpty())
            cd.conv.latestTs = cd.messages.last().env.messageId;

        // Names.
        if (cd.conv.kind == ConvKind::Im && cd.conv.dmUser) {
            cd.conv.name = userLabel(cd.conv.dmUser->value);
        } else if (cd.conv.kind == ConvKind::Mpim) {
            QStringList names;
            for (const UserId &u : cd.conv.members)
                names << userLabel(u.value);
            cd.conv.name = names.join(QStringLiteral(", "));
        }
    }

    // Order conversations by most-recent activity (latest message date desc).
    QList<QString> ids = result.byId.keys();
    std::sort(ids.begin(), ids.end(), [&](const QString &a, const QString &b) {
        const auto     &ca = result.byId[a].messages;
        const auto     &cb = result.byId[b].messages;
        const QDateTime da = ca.isEmpty() ? QDateTime() : ca.last().env.date;
        const QDateTime db = cb.isEmpty() ? QDateTime() : cb.last().env.date;
        return da > db;
    });
    for (const QString &id : ids)
        result.conversations.push_back(result.byId[id].conv);

    return result;
}

namespace Bucketing {

QList<QString> participantsOf(const Envelope &env, const QSet<QString> &mine) {
    QList<QString> out;
    QSet<QString>  seen;
    for (const auto *lst : {&env.from, &env.to, &env.cc})
        for (const MimeAddress &a : *lst) {
            const QString e = a.email.toLower();
            if (e.isEmpty() || mine.contains(e) || seen.contains(e))
                continue;
            seen.insert(e);
            out.append(e);
        }
    std::sort(out.begin(), out.end());
    return out;
}

QList<QList<quint32>> threadByReferences(const QList<MsgRef> &msgs) {
    UnionFind uf;
    for (const MsgRef &m : msgs) {
        const QString k = msgKey(m);
        uf.find(k);
        if (!m.env.inReplyTo.isEmpty())
            uf.unite(k, m.env.inReplyTo);
    }
    // root key → the messages in that thread
    QHash<QString, QList<const MsgRef *>> grouped;
    for (const MsgRef &m : msgs)
        grouped[uf.find(msgKey(m))].append(&m);

    QList<QList<quint32>> out;
    for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) {
        QList<const MsgRef *> g = it.value();
        std::sort(g.begin(), g.end(), [](const MsgRef *a, const MsgRef *b) {
            return a->env.date < b->env.date; // oldest first → root first
        });
        QList<quint32> uids;
        for (const MsgRef *m : g)
            uids.append(m->uid);
        out.append(uids);
    }
    return out;
}

Classified classify(const QList<QString> &participants, const QString &listId, int cap) {
    Classified c;
    if (!listId.isEmpty()) {
        c.convId = QStringLiteral("list:") + listId;
        c.kind   = ConvKind::PublicChannel;
    } else if (participants.isEmpty()) {
        c.convId = QStringLiteral("dm:self");
        c.kind   = ConvKind::Im;
    } else if (participants.size() == 1) {
        c.convId = QStringLiteral("dm:") + participants.first();
        c.kind   = ConvKind::Im;
    } else if (participants.size() <= cap) {
        c.convId = QStringLiteral("mpim:") + participants.join(QLatin1Char(','));
        c.kind   = ConvKind::Mpim;
    } else {
        c.convId = QStringLiteral("channel:broadcast");
        c.kind   = ConvKind::PublicChannel;
    }
    return c;
}

} // namespace Bucketing
} // namespace imap
