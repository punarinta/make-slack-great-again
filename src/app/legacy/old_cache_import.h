// Earlier versions' per-workspace cache (legacy.h), brought into the current
// one: what Slack can't give back first.
//
// Earlier versions kept one directory per workspace, <dataDir>/cache/<key with
// ':' as '_'> ("slack_T0123"; before multi-service, the bare team id), in this
// JSON:
//   conversations.json  [{id, ki kind, na name, mb member, lr lastRead,
//                       lt latest, un unread, mc mentions, dm peer, mu muted,
//                       st starred, lm "mute this person", ln local name,
//                       nl level 0 default 1 all 2 mentions 3 mute, ar role}]
//   users.json, bots.json  [{id, na, dn, av, bo, ex, ac, de, ad, ow, se, st,
//                       ti, em, tz}]
//   emoji.json          {name: url or "alias:name"}
//   usergroups.json     [{id, ha handle, na name, us [user ids]}]
//   meta.json           {conv, name (last open), meId, sweepAt, mutedThreads
//                       ["C\tts"], followedThreads ["C\tts"], aiTranscripts
//                       {file id: {text, by}}, reminders [{conv, ts, due,
//                       saved, root, snippet, author, botName, botAvatar,
//                       fired}], reminderPreviews [{key "C\tts", …}],
//                       deadConvIds [ids], userProbeTimes {user id: unix ms}}
//   messages/<conv>.json, images/   (not imported, see below)
//
// Slack: into <cacheDir>/workspaces/<key> (cache::WorkspaceCache). With no
// new cache yet (a first start) everything above but the messages becomes
// the new cache, so the start is a warm one; the Slack backend's own part of
// meta.json ("x": saved items with their previews and fired flags, followed
// threads, probe times, the sweep stamp, dead ids, user groups) is written in
// its format (SlackBackend::Read::saveExtras). Over an existing new cache
// only the app's own state goes in, where the new cache has nothing: mute,
// notification level, group-DM names, muted and followed threads, AI
// transcripts.
//
// Claude Code: the sessions' star, mute and level were kept in the same
// cache; they go into <dataDir>/claude-code/known-sessions.json, which
// the backend keeps them in (claude/backend.cpp loadKnown).
//
// Not imported: the unread counts (stale; the server's answer has them), the
// cached messages (the old ones were parsed text with entities, not Slack's
// mrkdwn — the network gives them back on open), the images (pictures are
// cached elsewhere now), the old notification level "Default" (= "All
// new posts" until there is a per-conversation default) and the agent role
// "ar" (Claude Code keeps it itself).
//
// The old files are only read.
#pragma once

#include <string>

namespace plat {
class App;
}

namespace legacy {

// The old cache directory of a workspace key ("slack:T0123"), "" when there
// is none.
std::string oldCacheDir(plat::App &app, const std::string &workspaceKey);

// One Slack workspace: `from` the old directory, `to` the new one
// (cache::WorkspaceCache::dirFor). False when `from` has no conversations.
bool importSlackCache(plat::App &app, const std::string &from, const std::string &to);

// One Claude Code workspace's sessions: star, mute and level, where
// known-sessions.json has none. False when nothing changed.
bool importClaudeCodeCache(const std::string &from, const std::string &knownSessionsPath);

// Every workspace of workspaces.json not done yet (`markerPath` lists the
// keys done, one per line): each is imported once, ever.
void importOldCaches(
    plat::App &app, const std::string &workspacesPath, const std::string &markerPath
);

} // namespace legacy
