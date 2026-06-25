// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Model-D bucketing engine (imap-backend-plan §3): turns a flat set of scanned
// messages into the derived conversation set — DMs (me + 1), MPDMs (me + several),
// list channels (List-Id) — by threading them and grouping each thread by its
// participant union (minus my own aliases). Pure (no socket/QObject), so the
// classification logic is unit-tested in isolation. The IMAP backend feeds it
// ENVELOPE-derived refs and (optionally) server THREAD groups, and consumes the
// per-conversation message index for loadHistory.
#pragma once

#include "backend/domain.h"
#include "backend/imap/imap_mappers.h" // Envelope

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <vector>

namespace imap {

// A scanned message reduced to what bucketing needs (no body).
struct MsgRef {
    quint32   uid = 0;
    QString   mailbox; // folder the uid is valid in (UIDs are per-mailbox)
    Envelope  env;
    QDateTime internalDate; // server arrival time — date fallback when the Date header is bad
    QString   listId;       // List-Id if known (empty in the ENVELOPE-only Phase 1 scan)
    bool      seen = false;
};

// Per-conversation data the backend keeps to serve loadHistory/loadThread.
struct ConvData {
    Conversation            conv;
    QList<MsgRef>           messages;     // oldest → newest
    QHash<QString, QString> threadRootOf; // messageId → its thread-root messageId
    QHash<QString, int>     replyCountOf; // root messageId → number of replies
};

struct BucketResult {
    std::vector<Conversation> conversations; // newest-active first
    QHash<QString, ConvData>  byId;          // ConversationId.value → data
    QHash<QString, User>      users;         // email → User (display names)
};

// Reusable classification (shared by the batch bucketer and the realtime path,
// so a newly-arrived message lands in the same conversation a rescan would give).
namespace Bucketing {

// from ∪ to ∪ cc minus `mine`, sorted unique (lowercased emails).
QList<QString> participantsOf(const Envelope &env, const QSet<QString> &mine);

// Reply-All addressing for `env`, like a normal mail client: To = the message's
// reply target (Reply-To if set, else From); Cc = its other recipients (To ∪ Cc).
// Both have `mine` removed and Cc is de-duplicated against To, preserving the
// senders' original casing/order. If that leaves To empty (the message was from
// me), To falls back to all participants so the thread stays reachable.
struct ReplyRecipients {
    QStringList to;
    QStringList cc;
};
ReplyRecipients replyRecipients(const Envelope &env, const QSet<QString> &mine);

struct Classified {
    QString  convId;
    ConvKind kind = ConvKind::Im;
};
// Map a participant set (+ optional List-Id) to a conversation id/kind (§3 rules).
Classified classify(const QList<QString> &participantsSorted, const QString &listId, int cap);

// Group messages into threads by Message-ID / In-Reply-To (JWZ-lite union-find).
// Returns groups of UIDs, each ordered oldest→newest (thread root first). Used for
// the channel thread-collapse + thread panel (§8) when no server THREAD is at hand.
QList<QList<quint32>> threadByReferences(const QList<MsgRef> &msgs);

} // namespace Bucketing

// Stable ConversationId.value forms (opaque to the UI):
//   DM      "dm:<email>"
//   self    "dm:self"               (a thread with only me)
//   MPDM    "mpim:<email,email,…>"  (participant emails, sorted)
//   list    "list:<list-id>"
//   oversized "channel:broadcast"   (no List-Id, > cap participants)
class Bucketer {
public:
    explicit Bucketer(QSet<QString> myAddresses, QString mePrimary, int mpdmCap = 8)
        : _me(std::move(myAddresses)), _mePrimary(std::move(mePrimary)), _cap(mpdmCap) {}

    // `serverThreads`: flattened UID groups from a server THREAD (preferred). When
    // empty, falls back to In-Reply-To/Message-ID union-find.
    BucketResult
    run(const QList<MsgRef> &msgs, const QList<QList<quint32>> &serverThreads = {}) const;

private:
    bool isMine(const QString &email) const { return _me.contains(email.toLower()); }

    QSet<QString> _me;
    QString       _mePrimary;
    int           _cap;
};

} // namespace imap
