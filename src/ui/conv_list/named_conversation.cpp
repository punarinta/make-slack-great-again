// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "named_conversation.h"
#include "session/session.h"
#include "util/emoji.h"

#include <QSet>
#include <algorithm>

QStringList parseMpdmUsernames(const QString &mpdmName) {
    QString s = mpdmName;
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

namespace {

struct PeerInfo {
    QString displayName;
    QString avatarUrl;
    bool    isDeactivated = false;
};

qint64 secondsOf(const Ts &ts) {
    return ts.isEmpty() ? 0 : decimalTsToMicros(ts) / 1000000;
}

} // namespace

std::vector<NamedConversation>
namedConversationsFor(Session *session, const QHash<QString, qint64> &visitedAt) {
    std::vector<NamedConversation> out;
    if (!session)
        return out;

    const auto  &convs = session->currentConversations();
    const auto  &users = session->currentUsers();
    const UserId me    = session->meUserId();

    // Only the peers the DMs actually name get resolved — a large org's user
    // list is tens of thousands of entries, and Emoji::expandCodes per name is
    // what makes the sidebar's full rebuild noticeable.
    QSet<QString> wanted;
    QSet<QString> wantedNames; // members of group DMs the API named but didn't list
    for (const auto &c : convs) {
        if (!c.isMember)
            continue;
        if (c.dmUser)
            wanted.insert(c.dmUser->value);
        for (const auto &m : c.members)
            wanted.insert(m.value);
        if (c.kind == ConvKind::Mpim && c.members.empty() && groupDmCustomName(c).isEmpty()) {
            for (const QString &uname : parseMpdmUsernames(c.name))
                wantedNames.insert(uname);
        }
    }
    const bool               anyMpdmByName = !wantedNames.isEmpty();
    QHash<QString, PeerInfo> peers;
    QHash<QString, QString>  usernameToId; // only filled when some group DM needs it
    peers.reserve(wanted.size() + wantedNames.size());
    for (const auto &u : users) {
        if (anyMpdmByName && !u.name.isEmpty())
            usernameToId.insert(u.name, u.id.value);
        if (!wanted.contains(u.id.value) && !wantedNames.contains(u.name))
            continue;
        peers.insert(
            u.id.value,
            {.displayName   = Emoji::expandCodes(u.displayName.isEmpty() ? u.name : u.displayName),
             .avatarUrl     = u.avatarUrl,
             .isDeactivated = u.isDeactivated}
        );
    }
    const auto peerName = [&](const QString &uid) -> QString {
        const auto it = peers.constFind(uid);
        return it == peers.constEnd() ? QString() : it->displayName;
    };

    out.reserve(convs.size());
    for (const auto &c : convs) {
        if (!c.isMember)
            continue;
        NamedConversation nc;
        nc.id   = c.id;
        nc.kind = c.kind;
        if ((c.kind == ConvKind::Im || c.kind == ConvKind::Mpim) && c.dmUser) {
            // Same drops as ConvListWidget::rebuildFilteredConvs: a DM whose
            // peer is gone is not something to jump to.
            const auto it = peers.constFind(c.dmUser->value);
            if (it != peers.constEnd()) {
                if (it->isDeactivated || it->displayName == "deactivateduser" ||
                    session->isUnresolvedUserId(it->displayName))
                    continue;
            }
        }
        if (c.kind == ConvKind::Im && c.dmUser) {
            const auto it = peers.constFind(c.dmUser->value);
            if (it != peers.constEnd()) {
                nc.avatarUrl = it->avatarUrl;
                if (!it->displayName.isEmpty() && !session->isUnresolvedUserId(it->displayName))
                    nc.name = it->displayName;
            }
            if (nc.name.isEmpty()) // raw peer id — resolves (and fetches) like the sidebar
                nc.name = session->userDisplayName(*c.dmUser);
        } else if (c.kind == ConvKind::Mpim) {
            QStringList names;
            if (const QString custom = groupDmCustomName(c); !custom.isEmpty()) {
                names.append(custom);
            } else if (!c.members.empty()) {
                for (const auto &uid : c.members) {
                    if (!me.value.isEmpty() && uid == me)
                        continue;
                    const QString n = peerName(uid.value);
                    if (!n.isEmpty())
                        names.append(n);
                }
            } else {
                for (const QString &uname : parseMpdmUsernames(c.name)) {
                    const QString uid = usernameToId.value(uname);
                    if (!uid.isEmpty() && uid == me.value)
                        continue;
                    const QString n = uid.isEmpty() ? QString() : peerName(uid);
                    names.append(n.isEmpty() ? uname : n);
                }
            }
            nc.name = names.isEmpty() ? Emoji::expandCodes(c.name) : names.join(", ");
        } else {
            nc.name = Emoji::expandCodes(c.name);
        }
        nc.activitySeconds = std::max(
            {visitedAt.value(c.id.value, 0), secondsOf(c.latestTs), secondsOf(c.lastRead)}
        );
        out.push_back(std::move(nc));
    }
    std::sort(out.begin(), out.end(), NamedConversationOrder{});
    return out;
}
