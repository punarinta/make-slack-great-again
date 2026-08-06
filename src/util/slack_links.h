// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

// Single source of truth for Slack deep links. Keep every link Slack-shaped:
// these must match the URLs Slack's own clients (and API responses) use, so a
// link we hand to the OS / browser / a notification opens the native client.
namespace SlackLinks {

// Open (and join/start) a huddle in a conversation. This is the exact shape
// Slack returns in a room's `huddle_link` field, e.g.
// https://app.slack.com/huddle/TCF2J0TSP/CCEEHAA1E — NOT the
// /client/<team>/<channel>?open=start_huddle form, which does not start a
// huddle.
//
// CAVEAT — channels only for a *constructed* link: the /huddle/ landing page can
// START a huddle for a channel id (C…/G… channel), but for a DM (D…) or group DM
// it can only JOIN an already-live one. Handing it a DM id with no live huddle
// yields Slack's "Server Error" page. So construct this only for channels; for a
// DM, either use the room's authoritative huddle_link (a live huddle) or fall
// back to conversation() (open the DM so the user starts the huddle there).
inline QString huddle(const QString &teamId, const QString &channelId) {
    return QStringLiteral("https://app.slack.com/huddle/%1/%2").arg(teamId, channelId);
}

// Open a conversation in the Slack client, e.g.
// https://app.slack.com/client/TCF2J0TSP/DCF69AC02 — the plain "go to this
// channel/DM" URL. Used as the huddle fallback for DMs, where a constructed
// /huddle/ link can't start a huddle (see huddle() above).
inline QString conversation(const QString &teamId, const QString &channelId) {
    return QStringLiteral("https://app.slack.com/client/%1/%2").arg(teamId, channelId);
}

// A message permalink, decomposed. This is the URL `chat.getPermalink` returns
// and the one Slack's clients put on the clipboard for "Copy link to message":
//   https://<team>.slack.com/archives/<conv>/p<16 digits>
// A link to a thread reply carries the root in a `thread_ts` query parameter
// (Slack adds `cid` alongside it; it duplicates the path's conversation id).
struct MessageRef {
    QString host;     // "<team>.slack.com" — kept so the link can be rebuilt verbatim
    QString conv;     // "C…" / "D…" / "G…"
    QString ts;       // "1786008939.071009" (the p-digits with the dot restored)
    QString threadTs; // thread root; empty unless the link points at a reply
    QString author;   // "U…" of the linked message's author; only a rich_text
                      // `message_mention` element carries it — a URL does not

    bool isValid() const { return !conv.isEmpty() && !ts.isEmpty(); }
};

// A MessageRef flattened to one string, for a TextEntity's `data` and for the
// click anchor. '/'-joined with a fixed field count — none of the parts can
// contain a slash, so the split is unambiguous even with empty fields.
inline QString refToToken(const MessageRef &ref) {
    return ref.host + '/' + ref.conv + '/' + ref.ts + '/' + ref.threadTs + '/' + ref.author;
}

inline MessageRef refFromToken(const QString &token) {
    const QStringList p = token.split(QLatin1Char('/'));
    if (p.size() != 5)
        return {};
    return {.host = p[0], .conv = p[1], .ts = p[2], .threadTs = p[3], .author = p[4]};
}

// "p1786008939071009" → "1786008939.071009". Slack's permalink drops the dot of
// the message ts; the fraction is always the last 6 digits. Empty when `token`
// isn't a p-timestamp.
inline QString tsFromPermalinkToken(const QString &token) {
    if (token.size() < 8 || token[0] != QLatin1Char('p'))
        return {};
    const QString digits = token.mid(1);
    for (const QChar c : digits)
        if (!c.isDigit())
            return {};
    return digits.left(digits.size() - 6) + QLatin1Char('.') + digits.right(6);
}

// Parse a Slack message permalink. Returns an invalid ref for every other URL,
// including a bare channel link (…/archives/C123 with no message).
inline MessageRef parseMessageLink(const QString &url) {
    const QUrl u(url);
    if (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http"))
        return {};
    const QString host = u.host();
    if (host != QLatin1String("slack.com") && !host.endsWith(QLatin1String(".slack.com")))
        return {};
    const QStringList seg = u.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (seg.size() != 3 || seg[0] != QLatin1String("archives"))
        return {};
    // Conversation ids are uppercase alphanumeric ("C6HQE8G0Z"). Checked so a
    // lookalike path (…/archives/search/pdf) can't produce a dead chip.
    const QString conv = seg[1];
    if (conv.size() < 2 || !conv[0].isUpper())
        return {};
    for (const QChar c : conv)
        if (!(c.isUpper() || c.isDigit()))
            return {};
    MessageRef ref{
        .host = host, .conv = conv, .ts = tsFromPermalinkToken(seg[2]), .threadTs = {}, .author = {}
    };
    if (ref.ts.isEmpty())
        return {};
    const QString thread = QUrlQuery(u).queryItemValue(QStringLiteral("thread_ts"));
    // A reply's own permalink repeats its ts as the root when the message *is*
    // the root; only a genuine reply gets a thread target.
    if (!thread.isEmpty() && thread != ref.ts)
        ref.threadTs = thread;
    return ref;
}

// Rebuild the permalink a MessageRef came from — used to hand a link for a
// workspace we are not signed into back to the browser.
inline QString messagePermalink(const MessageRef &ref) {
    QString ts = ref.ts;
    ts.remove(QLatin1Char('.'));
    QString url = QStringLiteral("https://%1/archives/%2/p%3").arg(ref.host, ref.conv, ts);
    if (!ref.threadTs.isEmpty())
        url += QStringLiteral("?thread_ts=%1&cid=%2").arg(ref.threadTs, ref.conv);
    return url;
}

} // namespace SlackLinks
