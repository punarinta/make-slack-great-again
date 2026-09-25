// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "claude_code_backend.h"
#include "cc_catalog.h"
#include "cc_roles.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace claude_code {
namespace {

const QString kAssistantPrefix = QStringLiteral("claude:");
const QString kAvatarUrl       = QStringLiteral("qrc:/claude_code_avatar.png");
// Who Claude's messages (and "is typing…") come from: the session's teammate —
// one user per role (cc_roles), shared by all its sessions. Each session's own
// user only names the DM and carries its dot. The generalist keeps the id the
// one generic "Agent" had.
const UserId  kAgentUser{QStringLiteral("claude:agent")};
const QString kRoleUserPrefix = QStringLiteral("claude:role:");

const QString kGeneralist = QStringLiteral("generalist");

UserId roleUser(const QString &role) {
    if (role.isEmpty() || role == kGeneralist)
        return kAgentUser;
    return UserId{kRoleUserPrefix + role};
}

QString teamDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/claude-code/team");
}
const QString kNewPrefix = QStringLiteral("new-"); // conversation id of a "+" session

// A turn msga sent that shows no sign of life (no prompt in the transcript, no
// busy worker) is given up on after this long.
constexpr qint64 kLaunchTimeoutMs = 60'000;
// After our prompt landed: a session that went quiet without writing the turn's
// end (interrupted, crashed) stops counting as busy after this long.
constexpr qint64 kQuietTurnMs     = 15'000;

QString homeRelative(const QString &path) {
    const QString home = QDir::homePath();
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/'))))
        return QStringLiteral("~") + path.mid(home.size());
    return QDir::toNativeSeparators(path);
}

QString profilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/claude-code/profile.json");
}

QString loginName() {
    const QString n = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    return n.isEmpty() ? QCoreApplication::translate("claude_code", "You") : n;
}

QString knownSessionsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/claude-code/known-sessions.json");
}

// A background session stopped on a permission prompt: it reads "approve Bash: …"
// (verified 2026-09-25). Only `claude attach` can answer that one; a plain
// question ("blocked" with the question as needs) is answered by message.
bool awaitsApproval(const SessionInfo &s) {
    return s.kind == SessionInfo::Kind::Background && s.needs.startsWith(QLatin1String("approve "));
}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

bool isFolderTrusted(const QString &dir) {
    const QString env =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("CLAUDE_CONFIG_DIR"));
    QFile f(
        env.isEmpty() ? QDir::homePath() + QStringLiteral("/.claude.json")
                      : env + QStringLiteral("/.claude.json")
    );
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject projects =
        QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("projects")).toObject();
    // Trust is inherited: the folder itself or any parent counts.
    QString path = QDir(dir).absolutePath();
    for (;;) {
        if (projects.value(path).toObject().value(QLatin1String("hasTrustDialogAccepted")).toBool())
            return true;
        const QString parent = QFileInfo(path).path();
        if (parent == path)
            return false;
        path = parent;
    }
}

struct Backend::Tracked {
    QString          convId;
    SessionInfo      info; // latest roster data; sessionId empty until a "+" session started
    bool             listed = false; // in the latest roster scan
    QString          transcriptPath;
    TranscriptParser parser;
    qint64           offset = 0;
    Ts               lastRead;
    bool             skipPermissionChecks = false; // a "+" session not started yet
    QString          localName;                    // the user's own name for it ("Rename session…")
    // The teammate msga started it with (cc_roles); the transcript's own
    // record of it wins once there is one (roleOf).
    QString          role;
    UserId           renderedAuthor; // who `rendered` has Claude's messages from
    // Rendered messages, parallel to parser.items(): re-rendered only when the
    // item changed, so a growing transcript doesn't re-parse all its markdown.
    std::vector<std::pair<TranscriptItem, Message>> rendered;
    // What the Session was last told (visible messages by ts), for the diff.
    QMap<Ts, Message>                               announced;
    bool                                            announcedInit = false;
    Conversation                                    lastConv;
    User                                            lastUser;
    bool                                            lastBusy = false;
    bool wasLive = false; // listed or sending at the previous refresh
    // Sending: messages wait here while Claude is on a turn, and go out one per
    // turn. Each is shown as a message of msga's own (ts/date) from the moment
    // it's sent until its prompt is in the transcript — the Session's optimistic
    // copy wouldn't do: it's gone with the next reload of the chat, and a
    // message can wait for as long as Claude's turn takes.
    struct Outgoing {
        QString text;
        Ts      ts;
        qint64  date = 0;
    };
    QList<Outgoing>         outbox;
    std::optional<Outgoing> flying; // taken from the outbox, prompt not landed yet
    // The turn msga started: from launching it until its end is in the transcript.
    bool                    sending        = false;
    bool                    launching      = false; // the launcher hasn't reported back yet
    bool                    stopRequested  = false; // "Stop" while launching: once it has
    bool                    stopping       = false; // `claude stop` under way
    qint64                  sendStartedMs  = 0;
    bool                    promptLanded   = false;
    qint64                  promptLandedMs = 0;
    std::function<void(bool ok, QString err)> inFlight; // a /btw's done(), until its root lands
    // A session branched off another (a /btw, or `--fork-session` anywhere):
    // shown as a thread in its parent rather than in the list (detectForks).
    QString forkOf;   // the parent's conversation id; "" = a session of its own
    int  forkAt = -1; // parser.items() index of the thread's first prompt (its root); -1 = none yet
    Ts   forkRoot;    // that prompt's ts: the root message in the parent
    bool standalone        = false; // "Open as session": listed as a session of its own after all
    bool awaitingRoot      = false; // msga launched it: its first prompt settles the send
    bool announcedAsThread = false; // how `announced` was taken (thread replies vs a DM)
};

Backend::Backend(const Credentials &creds)
    : _creds(creds), _paths(Paths::detect()), _team(teamDir()) {
    _ctx      = new QObject();
    _launcher = new Launcher(_creds.claudePath, _paths, _ctx);
    loadKnown();
    loadProfile();
}

Backend::~Backend() {
    saveKnown();
    delete _ctx; // launcher, watcher and timers go with it
}

rpl::producer<AuthState> Backend::authState() const {
    return _authState.value();
}

Capabilities Backend::capabilities() const {
    Capabilities c;
    c.presence            = true;  // a session's dot: working vs not
    c.typing              = true;  // "… is typing" while Claude works on a turn (see pumpTyping)
    c.livePresence        = true;  // pushed from the roster watcher, no polling needed
    c.selfPresence        = false; // nothing to be "away" from
    c.zenMode             = true;  // hides the tool-call cards
    c.profileContact      = false; // your profile is a name and a picture, nothing more
    c.selfStatus          = false;
    c.threads             = true; // subagent runs
    c.agentSessions       = true;
    c.slashCommands       = true; // Claude Code's own, typed to it (conversationCommands)
    c.commandsAreMessages = true;
    // Prompts and answers alike, out of the transcript (deleteMessage) — when
    // nothing is writing to it (canDeleteMessage).
    c.deleteMessage       = true;
    c.deleteAnyMessage    = true;
    return c;
}

bool Backend::isBotId(UserId id) const {
    return id.value.startsWith(kAssistantPrefix);
}

bool Backend::isUserId(UserId id) const {
    return id == _me;
}

// ── Known sessions ──────────────────────────────────────────────────────────
// Ended interactive sessions are in no Claude Code registry — only their
// transcript remains — so msga remembers the sessions it has seen, and forgets
// each one once Claude Code has dropped it (plan §5.3).

void Backend::loadKnown() {
    QFile f(knownSessionsPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = f.readAll();
    const QJsonArray arr =
        QJsonDocument::fromJson(data).object().value(QLatin1String("sessions")).toArray();
    for (const auto &v : arr) {
        const QJsonObject o      = v.toObject();
        const QString     convId = o.value(QLatin1String("id")).toString();
        QString           sid    = o.value(QLatin1String("sessionId")).toString();
        if (sid.isEmpty() && !convId.startsWith(kNewPrefix))
            sid = convId; // written before conversation ids could differ
        if (convId.isEmpty() || sid.isEmpty())
            continue;
        Tracked &t       = ensureTracked(convId);
        t.info.sessionId = sid;
        _convOf.insert(sid, convId);
        t.info.name       = o.value(QLatin1String("name")).toString();
        t.info.cwd        = o.value(QLatin1String("cwd")).toString();
        t.info.kind       = o.value(QLatin1String("kind")).toString() == QLatin1String("background")
                                ? SessionInfo::Kind::Background
                                : SessionInfo::Kind::Interactive;
        t.info.entrypoint = o.value(QLatin1String("entrypoint")).toString();
        t.transcriptPath  = o.value(QLatin1String("transcript")).toString();
        t.lastRead        = o.value(QLatin1String("lastRead")).toString();
        t.localName       = o.value(QLatin1String("localName")).toString();
        t.standalone      = o.value(QLatin1String("standalone")).toBool();
        t.role            = o.value(QLatin1String("role")).toString();
    }
    const QJsonArray hidden =
        QJsonDocument::fromJson(data).object().value(QLatin1String("hidden")).toArray();
    for (const auto &v : hidden) {
        const QJsonObject o   = v.toObject();
        const QString     sid = o.value(QLatin1String("sessionId")).toString();
        if (sid.isEmpty() || _convOf.contains(sid))
            continue;
        _hidden.insert(
            sid,
            Hidden{
                qint64(o.value(QLatin1String("at")).toDouble()),
                o.value(QLatin1String("transcript")).toString()
            }
        );
    }
}

void Backend::saveKnown() {
    QJsonArray arr;
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it) {
        const Tracked &t = *it.value();
        if (t.info.sessionId.isEmpty())
            continue; // a "+" session nobody wrote to yet: nothing to come back to
        QJsonObject o;
        o[QStringLiteral("id")]         = it.key();
        o[QStringLiteral("sessionId")]  = t.info.sessionId;
        o[QStringLiteral("name")]       = titleOf(t);
        o[QStringLiteral("cwd")]        = t.info.cwd;
        o[QStringLiteral("kind")]       = t.info.kind == SessionInfo::Kind::Background
                                              ? QStringLiteral("background")
                                              : QStringLiteral("interactive");
        o[QStringLiteral("entrypoint")] = t.info.entrypoint;
        o[QStringLiteral("transcript")] = t.transcriptPath;
        o[QStringLiteral("lastRead")]   = t.lastRead;
        if (!t.localName.isEmpty())
            o[QStringLiteral("localName")] = t.localName;
        if (t.standalone)
            o[QStringLiteral("standalone")] = true; // a branch opened as a session
        if (const QString role = roleOf(t); role != kGeneralist)
            o[QStringLiteral("role")] = role;
        arr.append(o);
    }
    QJsonArray hidden;
    for (auto it = _hidden.cbegin(); it != _hidden.cend(); ++it) {
        // Forgotten once Claude Code has dropped the session: its transcript
        // and, for a background session, its job are both gone.
        if ((it->transcript.isEmpty() || !QFileInfo::exists(it->transcript)) &&
            !QFileInfo::exists(_paths.jobsDir() + QLatin1Char('/') + it.key().left(8)))
            continue;
        QJsonObject o;
        o[QStringLiteral("sessionId")]  = it.key();
        o[QStringLiteral("at")]         = double(it->atMs);
        o[QStringLiteral("transcript")] = it->transcript;
        hidden.append(o);
    }
    QDir().mkpath(QFileInfo(knownSessionsPath()).absolutePath());
    QSaveFile f(knownSessionsPath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    QJsonObject root;
    root[QStringLiteral("sessions")] = arr;
    root[QStringLiteral("hidden")]   = hidden;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.commit();
}

void Backend::scheduleSaveKnown() {
    if (_saveKnownTimer)
        _saveKnownTimer->start();
    else
        saveKnown();
}

Backend::Tracked &Backend::ensureTracked(const QString &convId) {
    auto it = _sessions.find(convId);
    if (it == _sessions.end()) {
        it                 = _sessions.insert(convId, std::make_shared<Tracked>());
        it.value()->convId = convId;
    }
    return *it.value();
}

Backend::Tracked *Backend::find(const QString &convId) {
    const auto it = _sessions.find(convId);
    return it == _sessions.end() ? nullptr : it.value().get();
}

QString Backend::convIdFor(const QString &sessionId) const {
    return _convOf.value(sessionId, sessionId);
}

// ── Session state ───────────────────────────────────────────────────────────

bool Backend::busy(const Tracked &t) const {
    if (t.stopping)
        return false; // the worker is on its way out
    if (t.sending)
        return true; // msga's turn: from launching it until its end is written
    return t.info.running && statusIsBusy(t.info.status);
}

bool Backend::needsUser(const Tracked &t) const {
    return !t.sending && t.info.running && statusNeedsUser(t.info.status);
}

QString Backend::roleOf(const Tracked &t) const {
    if (!t.parser.role().isEmpty())
        return t.parser.role();
    return t.role.isEmpty() ? kGeneralist : t.role;
}

Role Backend::roleFor(const Tracked &t) const {
    return _team.resolve(roleOf(t), t.parser.roleName());
}

QStringList Backend::roleIds() const {
    QStringList ids;
    for (const Role &r : _team.roles())
        ids << r.id;
    for (auto it = _team.formers().cbegin(); it != _team.formers().cend(); ++it)
        ids << it.key();
    return ids;
}

QString Backend::titleOf(const Tracked &t) const {
    if (!t.info.name.isEmpty())
        return t.info.name;
    if (!t.parser.aiTitle().isEmpty())
        return t.parser.aiTitle();
    if (!t.info.cwd.isEmpty())
        return QFileInfo(t.info.cwd).fileName();
    return t.info.sessionId.left(8);
}

QString Backend::readOnlyReason(const Tracked &t) const {
    if (t.info.running && t.info.kind == SessionInfo::Kind::Interactive) {
        if (t.info.entrypoint == QLatin1String("cli"))
            return QCoreApplication::translate(
                "claude_code",
                "This session is running in a terminal — reply there. Once it ends, "
                "you can continue it here."
            );
        return QCoreApplication::translate(
            "claude_code",
            "Another program is driving this session. Once it ends, you can continue "
            "it here."
        );
    }
    if (awaitsApproval(t.info))
        return QCoreApplication::translate(
                   "claude_code",
                   "Claude is waiting for your approval, which only the terminal can give: "
                   "run “claude attach %1”."
        )
            .arg(t.info.sessionId.left(8));
    if (_creds.claudePath.isEmpty())
        return QCoreApplication::translate(
            "claude_code",
            "Install the claude command-line tool to write to Claude Code sessions from here."
        );
    return {};
}

User Backend::assistantUser(const Tracked &t) const {
    User u;
    u.id          = UserId{kAssistantPrefix + t.convId};
    u.name        = shownTitle(t);
    u.displayName = u.name;
    u.avatarUrl   = roleFor(t).avatarUrl;
    u.title       = homeRelative(t.info.cwd); // shown on the profile card
    u.isActive    = busy(t);
    // Not working, yet not writable from here (open in a terminal, waiting on
    // an approval only the terminal can give): the yellow dot.
    u.unavailable = !u.isActive && !readOnlyReason(t).isEmpty();
    if (needsUser(t))
        u.statusText = QCoreApplication::translate("claude_code", "Waiting for you");
    else if (busy(t))
        u.statusText = QCoreApplication::translate("claude_code", "Working");
    else if (t.info.running && statusHasShell(t.info.status))
        u.statusText = QCoreApplication::translate("claude_code", "Running a background command");
    return u;
}

bool Backend::roleBusy(const QString &role) const {
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
        if (!asThread(*it.value()) && busy(*it.value()) && roleOf(*it.value()) == role)
            return true;
    return false;
}

// A teammate is yellow while none of its sessions works and one of them is
// yellow itself (see assistantUser) — green, as any working one makes it, wins.
bool Backend::roleUnavailable(const QString &role) const {
    if (roleBusy(role))
        return false;
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
        if (!asThread(*it.value()) && roleOf(*it.value()) == role &&
            !readOnlyReason(*it.value()).isEmpty())
            return true;
    return false;
}

User Backend::teammateUser(const Role &r) const {
    User u;
    u.id          = roleUser(r.id);
    u.name        = r.name;
    u.displayName = r.name;
    u.avatarUrl   = r.avatarUrl;
    u.title       = r.description; // shown on the profile card
    u.isActive    = roleBusy(r.id);
    u.unavailable = roleUnavailable(r.id);
    return u;
}

std::vector<AgentRole> Backend::agentRoles() {
    std::vector<AgentRole> out;
    for (const Role &r : _team.listed()) {
        AgentRole a;
        a.id          = r.id;
        a.name        = r.name;
        a.description = r.description;
        a.avatarUrl   = r.avatarUrl;
        a.user        = roleUser(r.id);
        a.prompt      = r.prompt;
        a.glyph       = r.glyph;
        a.color       = r.color.name();
        a.builtIn     = r.builtIn;
        a.edited      = r.edited;
        out.push_back(std::move(a));
    }
    return out;
}

QString Backend::saveAgentRole(const AgentRole &a, QString *error) {
    Role r;
    if (const Role *old = _team.find(a.id))
        r = *old;
    r.id             = a.id;
    r.name           = a.name;
    r.description    = a.description;
    r.glyph          = a.glyph;
    r.color          = QColor(a.color);
    r.prompt         = a.prompt; // new sessions only: a started one keeps the prompt it began with
    const QString id = _team.save(r, error);
    if (!id.isEmpty())
        teamChanged(id);
    return id;
}

void Backend::removeAgentRole(const QString &id) {
    if (_team.remove(id))
        teamChanged(id); // its sessions keep its name and picture
}

void Backend::restoreAgentRole(const QString &id) {
    if (_team.restore(id))
        teamChanged(id);
}

void Backend::teamChanged(const QString &id) {
    // The teammate's name and picture, on its messages and its sessions.
    _events.fire(EvUserChanged{teammateUser(_team.resolve(id))});
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it)
        if (!asThread(*it.value()) && roleOf(*it.value()) == id && _firstScanDone)
            announceChanged(*it.value());
}

Conversation Backend::conversationFor(const Tracked &t) const {
    Conversation c;
    c.id          = ConversationId{t.convId};
    c.kind        = ConvKind::Im;
    c.name        = titleOf(t);
    c.localName   = t.localName;
    c.description = homeRelative(t.info.cwd);
    if (t.skipPermissionChecks)
        c.description += QCoreApplication::translate("claude_code", " · no permission checks");
    c.isMember       = true;
    c.dmUser         = UserId{kAssistantPrefix + t.convId};
    c.lastRead       = t.lastRead;
    c.readOnlyReason = readOnlyReason(t);
    c.agentRole      = roleOf(t);
    // Unread = what Claude said since the last read; only its answers (and
    // "waiting for you") count toward the red counter, like the live path.
    for (auto it = t.announced.cbegin(); it != t.announced.cend(); ++it) {
        c.latestTs = it.key();
        if (it.value().author == _me || (!t.lastRead.isEmpty() && it.key() <= t.lastRead))
            continue;
        ++c.unread;
        if (!isProgressMessage(it.value()))
            ++c.mentionCount;
    }
    return c;
}

// ── Transcripts ─────────────────────────────────────────────────────────────

void Backend::tail(Tracked &t) {
    if (t.info.sessionId.isEmpty())
        return;
    if (t.transcriptPath.isEmpty() || !QFileInfo::exists(t.transcriptPath)) {
        const QString found = _paths.findTranscript(t.info.sessionId);
        if (found.isEmpty())
            return;
        t.transcriptPath = found;
    }
    QFile f(t.transcriptPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    if (f.size() < t.offset) {
        // Rewritten or truncated: start over.
        t.parser = {};
        t.offset = 0;
        t.rendered.clear();
    }
    if (f.size() == t.offset)
        return;
    f.seek(t.offset);
    const QByteArray bytes = f.readAll();
    t.offset += bytes.size();
    t.parser.feed(bytes);
    // A role this machine doesn't know (another machine's teammate, a deleted
    // file) keeps the name its sessions give it.
    if (_team.noteFormer(t.parser.role(), t.parser.roleName()))
        _events.fire(EvUserChanged{teammateUser(_team.resolve(t.parser.role()))});
}

int Backend::subagentReplyCount(const Tracked &t, const QString &agentId, Ts *latest) {
    const QString path = Paths::subagentTranscript(t.transcriptPath, agentId);
    const qint64  size = QFileInfo(path).size();
    auto         &c    = _subagentCounts[path];
    if (c.size != size) {
        c.size = size;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            TranscriptParser p;
            p.feed(f.readAll());
            c.count    = int(p.items().size());
            c.zenCount = int(std::count_if(p.items().begin(), p.items().end(), [](const auto &i) {
                return i.kind != TranscriptItem::Kind::ToolGroup;
            }));
            c.latest   = p.items().empty() ? Ts{} : p.items().back().ts;
        } else {
            c.count    = 0;
            c.zenCount = 0;
            c.latest.clear();
        }
    }
    *latest = c.latest;
    return _zen ? c.zenCount : c.count;
}

const Message &Backend::renderedAt(Tracked &t, size_t i) {
    const auto  &items  = t.parser.items();
    const UserId author = roleUser(roleOf(t));
    if (author != t.renderedAuthor) {
        t.rendered.clear(); // the role became known: Claude's messages change hands
        t.renderedAuthor = author;
    }
    if (t.rendered.size() > items.size())
        t.rendered.resize(items.size());
    while (t.rendered.size() <= i) {
        const auto &item = items[t.rendered.size()];
        t.rendered.emplace_back(item, toMessage(item, _me, author));
    }
    if (t.rendered[i].first != items[i])
        t.rendered[i] = {items[i], toMessage(items[i], _me, author)};
    return t.rendered[i].second;
}

std::vector<Message> Backend::visibleMessages(Tracked &t) {
    tail(t);
    const auto          &items     = t.parser.items();
    const UserId         assistant = roleUser(roleOf(t));
    const bool           isBusy    = busy(t);
    std::vector<Message> out;
    out.reserve(items.size() + 1);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isVisible(item, isBusy) || (_zen && item.kind == TranscriptItem::Kind::ToolGroup))
            continue;
        Message m = renderedAt(t, i);
        if (item.kind == TranscriptItem::Kind::Subagent && !item.agentId.isEmpty()) {
            Ts latest;
            m.replyCount = subagentReplyCount(t, item.agentId, &latest);
            if (!latest.isEmpty())
                m.latestReply = latest;
        }
        out.push_back(std::move(m));
    }

    // A session stopped for the user where the transcript doesn't show it — a
    // terminal session waiting at a prompt, a background one on a permission
    // prompt — gets a message of its own, so it notifies and badges like an
    // answer. (A background session that asked a question already shows the
    // question as its last answer.) Its ts is pinned to when the status changed,
    // so it stays the same message while the session waits.
    const bool terminalWaits =
        t.info.running && t.info.kind == SessionInfo::Kind::Interactive && needsUser(t);
    if (terminalWaits || awaitsApproval(t.info)) {
        const qint64 lastDate = out.empty() ? 0 : out.back().date;
        qint64       micros   = qint64(t.info.statusSinceMs) * 1000;
        if (micros <= lastDate)
            micros = lastDate + 1;
        Message m;
        m.ts     = microsToTs(micros);
        m.date   = micros;
        m.author = assistant;
        const QString text =
            terminalWaits
                ? QCoreApplication::translate("claude_code", "Waiting for you in the terminal.")
                : QCoreApplication::translate("claude_code", "Waiting for your approval: %1")
                      .arg(t.info.needs.mid(int(qstrlen("approve "))));
        m.rawText = text;
        m.text    = {text, {}};
        out.push_back(std::move(m));
    }

    appendOutgoing(t, out, std::nullopt);

    // Side conversations branched off this session (/btw): each one's first
    // prompt is a message here, rooting a thread with the rest of it.
    bool branched = false;
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &f = *it.value();
        if (f.forkOf != t.convId || !asThread(f))
            continue;
        const auto replies = threadMessages(f);
        if (f.forkAt < 0)
            continue;
        Message root    = renderedAt(f, size_t(f.forkAt));
        root.replyCount = int(replies.size());
        if (!replies.empty())
            root.latestReply = replies.back().ts;
        out.push_back(std::move(root));
        branched = true;
    }
    if (branched)
        std::stable_sort(out.begin(), out.end(), [](const Message &a, const Message &b) {
            return a.date < b.date;
        });
    return out;
}

// ── Branched sessions (/btw threads) ────────────────────────────────────────
// A fork starts with a copy of its parent's records — same timestamps, so the
// same leading items — and goes on with its own. Nothing in it names the parent,
// so the family is read off the transcripts: sessions whose first item is the
// same are one family, and each younger member is a fork of the older one it
// shares the longest start with. The thread's root is the fork's first prompt
// after that shared start. (docs/backend-modules-plan.md §10)

namespace {

qint64 bornMs(const QString &path) {
    const QFileInfo fi(path);
    const QDateTime born = fi.birthTime();
    return (born.isValid() ? born : fi.lastModified()).toMSecsSinceEpoch();
}

size_t sharedStart(const std::vector<TranscriptItem> &a, const std::vector<TranscriptItem> &b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n].ts == b[n].ts && a[n].kind == b[n].kind)
        ++n;
    return n;
}

} // namespace

bool Backend::asThread(const Tracked &t) const {
    return !t.forkOf.isEmpty() && !t.standalone && t.forkOf != t.convId &&
           _sessions.contains(t.forkOf);
}

Backend::Tracked *Backend::forkFor(const QString &parentConv, const Ts &root) {
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &f = *it.value();
        if (f.forkOf == parentConv && f.forkAt >= 0 && f.forkRoot == root && asThread(f))
            return &f;
    }
    return nullptr;
}

QSet<QString> Backend::detectForks() {
    QHash<QString, std::vector<Tracked *>> families; // by first item
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked    &t     = *it.value();
        const auto &items = t.parser.items();
        if (t.info.sessionId.isEmpty() || items.empty() || t.transcriptPath.isEmpty())
            continue;
        families[items.front().ts + QLatin1Char('\n') + items.front().text].push_back(&t);
    }
    QSet<QString> changed; // parents whose threads changed
    for (auto &family : families) {
        if (family.size() < 2)
            continue;
        std::vector<std::pair<qint64, Tracked *>> byAge;
        for (Tracked *t : family)
            byAge.emplace_back(bornMs(t->transcriptPath), t);
        std::stable_sort(byAge.begin(), byAge.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        for (size_t i = 1; i < byAge.size(); ++i) {
            Tracked &f      = *byAge[i].second;
            Tracked *parent = nullptr;
            size_t   best   = 0;
            for (size_t j = 0; j < i; ++j) {
                const size_t n = sharedStart(byAge[j].second->parser.items(), f.parser.items());
                if (!parent || n > best) {
                    parent = byAge[j].second;
                    best   = n;
                }
            }
            const auto &items = f.parser.items();
            int         at    = -1;
            for (size_t k = best; k < items.size(); ++k)
                if (items[k].kind == TranscriptItem::Kind::UserPrompt) {
                    at = int(k);
                    break;
                }
            const Ts root = at >= 0 ? items[size_t(at)].ts : Ts{};
            if (f.forkOf != parent->convId || f.forkAt != at || f.forkRoot != root) {
                if (!f.forkOf.isEmpty())
                    changed.insert(f.forkOf);
                changed.insert(parent->convId);
            }
            f.forkOf   = parent->convId;
            f.forkAt   = at;
            f.forkRoot = root;
        }
    }
    return changed;
}

std::vector<Message> Backend::threadMessages(Tracked &f) {
    tail(f);
    std::vector<Message> out;
    if (f.forkAt < 0)
        return out;
    const auto &items  = f.parser.items();
    const bool  isBusy = busy(f);
    for (size_t i = size_t(f.forkAt) + 1; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isVisible(item, isBusy) || (_zen && item.kind == TranscriptItem::Kind::ToolGroup))
            continue;
        Message m      = renderedAt(f, i);
        m.threadRoot   = f.forkRoot;
        m.parentUserId = _me; // you started it: its answers notify as replies to you
        out.push_back(std::move(m));
    }
    appendOutgoing(f, out, f.forkRoot);
    return out;
}

void Backend::appendOutgoing(
    const Tracked &t, std::vector<Message> &out, const std::optional<Ts> &threadRoot
) const {
    const auto add = [&](const Tracked::Outgoing &o) {
        TranscriptItem item;
        item.kind    = TranscriptItem::Kind::UserPrompt;
        item.ts      = o.ts;
        item.date    = o.date;
        item.text    = o.text;
        Message m    = toMessage(item, _me, _me);
        m.threadRoot = threadRoot;
        out.push_back(std::move(m));
    };
    if (t.flying)
        add(*t.flying);
    for (const auto &o : t.outbox)
        add(o);
}

bool Backend::isOutgoingCopy(const Tracked &t, const Ts &ts) const {
    return (t.flying && t.flying->ts == ts) ||
           std::any_of(t.outbox.begin(), t.outbox.end(), [&](const Tracked::Outgoing &o) {
               return o.ts == ts;
           });
}

std::vector<Message> Backend::shownMessages(Tracked &t) {
    return asThread(t) ? threadMessages(t) : visibleMessages(t);
}

void Backend::diffAndAnnounce(Tracked &t) {
    // A branched session's news is its thread's, in its parent's conversation.
    const bool thread = asThread(t);
    if (thread != t.announcedAsThread) {
        t.announcedAsThread = thread;
        t.announcedInit     = false; // what was announced belongs to the other place
    }
    auto msgs = shownMessages(t);
    // The message on its way is delivered once its prompt is in the transcript:
    // msga's copy of it goes in the same breath.
    if (t.flying && t.announcedInit && std::any_of(msgs.begin(), msgs.end(), [&](const Message &m) {
            return m.author == _me && !t.announced.contains(m.ts) && !isOutgoingCopy(t, m.ts);
        })) {
        t.flying.reset();
        msgs = shownMessages(t);
    }
    QMap<Ts, Message> now;
    for (const auto &m : msgs)
        now.insert(m.ts, m);
    const ConversationId conv{thread ? t.forkOf : t.convId};
    if (!t.announcedInit) {
        // First look at this session: its history is served by loadHistory,
        // nothing here is news.
        t.announced     = std::move(now);
        t.announcedInit = true;
        if (!thread && t.lastRead.isEmpty() && !t.announced.isEmpty())
            t.lastRead = t.announced.lastKey(); // don't greet a new session with old unreads
        return;
    }
    for (auto it = t.announced.cbegin(); it != t.announced.cend(); ++it)
        if (!now.contains(it.key()))
            _events.fire(EvMessageDeleted{conv, it.key(), it.value().threadRoot});
    bool sawOwnPrompt = false;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        const auto old = t.announced.constFind(it.key());
        if (old == t.announced.cend()) {
            _events.fire(EvMessageNew{conv, it.value()});
            sawOwnPrompt =
                sawOwnPrompt || (it.value().author == _me && !isOutgoingCopy(t, it.key()));
        } else if (!(old.value() == it.value())) {
            _events.fire(EvMessageChanged{conv, it.value(), false});
        }
    }
    t.announced = std::move(now);
    // A /btw is delivered once the branch's first prompt — the thread's root,
    // shown in the parent — is in its transcript.
    if (thread && t.awaitingRoot && t.forkAt >= 0) {
        t.awaitingRoot = false;
        sawOwnPrompt   = true;
    }
    // The turn msga started is under way once its prompt is in the transcript.
    if (sawOwnPrompt && t.sending && !t.promptLanded) {
        t.promptLanded   = true;
        t.promptLandedMs = nowMs();
        if (auto done = std::exchange(t.inFlight, {}))
            done(true, {});
    }
}

// ── Sending ─────────────────────────────────────────────────────────────────

void Backend::failSends(Tracked &t, const QString &reason) {
    if (!t.inFlight && !t.flying && t.outbox.isEmpty())
        return; // nothing on its way
    if (auto done = std::exchange(t.inFlight, {}))
        done(false, reason);
    const bool copies = t.flying || !t.outbox.isEmpty();
    t.flying.reset();
    t.outbox.clear();
    if (copies)
        diffAndAnnounce(t); // msga's copies of them go
    _events.fire(EvSendFailed{ConversationId{asThread(t) ? t.forkOf : t.convId}, reason});
}

void Backend::dispatch(Tracked &t) {
    // One turn at a time: the next message goes once Claude is done with the
    // last (and a session waiting on an approval takes nothing until it's given).
    // A background command still running waits too: sending stops the worker,
    // which would kill the command.
    if (t.outbox.isEmpty() || t.sending || t.stopping || busy(t) || awaitsApproval(t.info) ||
        (t.info.running && statusHasShell(t.info.status)))
        return;
    Tracked::Outgoing next = t.outbox.takeFirst();
    t.sending              = true;
    t.launching            = true;
    t.sendStartedMs        = nowMs();
    t.promptLanded         = false;
    t.flying               = std::move(next);
    const QString convId   = t.convId;
    auto          settled  = [this, convId](QString sessionId, QString error) {
        Tracked *t = find(convId);
        if (!t)
            return;
        t->launching = false;
        if (sessionId.isEmpty()) {
            t->sending       = false;
            t->stopRequested = false;
            failSends(*t, error);
            refresh();
            return;
        }
        if (t->info.sessionId.isEmpty()) {
            // A "+" session: Claude Code just picked its id.
            t->info.sessionId = sessionId;
            t->info.kind      = SessionInfo::Kind::Background;
            _convOf.insert(sessionId, convId);
            t->skipPermissionChecks = false; // saved with the session from here on
        }
        scheduleSaveKnown();
        if (std::exchange(t->stopRequested, false)) {
            stopWorker(*t); // "Stop" came while the turn was being launched
            return;
        }
        scheduleRefresh();
    };
    if (t.info.sessionId.isEmpty()) {
        _launcher->start(
            t.info.cwd,
            t.flying->text,
            t.skipPermissionChecks,
            appendedPrompt(_team.resolve(roleOf(t))),
            settled
        );
    } else {
        const bool background = t.info.kind == SessionInfo::Kind::Background;
        // A background session's worker idles on after a turn; it has to be
        // stopped before the session can be resumed (see Launcher).
        _launcher->resume(
            t.info.sessionId,
            t.info.cwd,
            t.flying->text,
            background,
            background && t.info.running,
            settled
        );
    }
    scheduleRefresh(); // the dot and "typing" follow at once
}

bool Backend::canStopAgentSession(ConversationId conv) {
    const Tracked *t = find(conv.value);
    if (!t || asThread(*t) || t->stopping || t->stopRequested)
        return false;
    if (t->sending || !t->outbox.isEmpty())
        return true; // msga's own turn, or messages waiting for one
    // An idle worker lingers after every turn: nothing to stop there.
    return t->info.kind == SessionInfo::Kind::Background && t->info.running &&
           (statusIsBusy(t->info.status) || statusNeedsUser(t->info.status) ||
            statusHasShell(t->info.status));
}

void Backend::stopAgentSession(ConversationId conv) {
    Tracked *t = find(conv.value);
    if (!t || t->stopping)
        return;
    // Messages still waiting are dropped: sending one would resume the session.
    const bool queued = !t->outbox.isEmpty();
    t->outbox.clear();
    if (t->launching) {
        // The CLI is starting the turn right now; stop it the moment it has.
        t->stopRequested = true;
        if (queued)
            diffAndAnnounce(*t);
        return;
    }
    if (t->info.sessionId.isEmpty()) { // a "+" session nothing was sent to yet
        if (queued)
            diffAndAnnounce(*t);
        return;
    }
    stopWorker(*t);
}

void Backend::stopWorker(Tracked &t) {
    t.stopping = true;
    t.sending  = false;
    // A prompt not in the transcript yet may never get there now; if it does,
    // it shows from there.
    t.flying.reset();
    const QString convId = t.convId;
    _launcher->stop(t.info.sessionId, t.info.cwd, [this, convId] {
        if (Tracked *t = find(convId))
            t->stopping = false;
        refresh();
    });
    refresh(); // no dot, no typing from here on
}

// ── Roster refresh ──────────────────────────────────────────────────────────

void Backend::scheduleRefresh() {
    if (_debounce)
        _debounce->start();
}

QString Backend::shownTitle(const Tracked &t) const {
    return t.localName.isEmpty() ? titleOf(t) : t.localName;
}

void Backend::refresh() {
    const auto    scanned = scanSessions(_paths);
    // A "+" session being started shows up in the roster a moment before the
    // launcher reports its id — don't list it twice meanwhile.
    QSet<QString> startingIn;
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
        if (it.value()->sending && it.value()->info.sessionId.isEmpty())
            startingIn.insert(QDir(it.value()->info.cwd).absolutePath());
    startingIn += _forkingIn; // likewise a /btw branch being launched
    QSet<QString> listedNow;  // conversation ids
    for (const auto &s : scanned) {
        const QString convId = convIdFor(s.sessionId);
        if (!_sessions.contains(convId) && startingIn.contains(QDir(s.cwd).absolutePath()))
            continue;
        if (const auto h = _hidden.constFind(s.sessionId); h != _hidden.cend()) {
            // Removed from msga: stays away until its transcript is written
            // again. No transcript (none yet, or Claude Code already deleted it
            // while the job lingers) means no new activity either.
            const QString   path = s.transcriptPath.isEmpty() ? h->transcript : s.transcriptPath;
            const QFileInfo fi(path);
            if (!fi.exists() || fi.lastModified().toMSecsSinceEpoch() <= h->atMs)
                continue;
            _hidden.erase(h);
        }
        listedNow.insert(convId);
        const bool    isNew   = !_sessions.contains(convId);
        Tracked      &t       = ensureTracked(convId);
        // Keep a known name when the fresh entry has none. A background
        // worker is named after the prompt it was resumed with, and a slash
        // command ("/compact") is no name for a session.
        const QString oldName = t.info.name;
        t.info                = s;
        if (t.info.name.startsWith(QLatin1Char('/')))
            t.info.name.clear();
        if (t.info.name.isEmpty())
            t.info.name = oldName;
        if (!s.transcriptPath.isEmpty())
            t.transcriptPath = s.transcriptPath;
        t.listed = true;
        if (isNew)
            t.announcedInit = false; // a session started elsewhere: its history isn't news
    }

    // Sessions that left the roster.
    QStringList dropped;
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &t = *it.value();
        if (listedNow.contains(it.key()))
            continue;
        t.listed       = false;
        t.info.running = false;
        t.info.status.clear();
        t.info.needs.clear();
        if (t.info.sessionId.isEmpty() || t.sending)
            continue; // a "+" session not started yet, or one being started right now
        // Kept exactly as long as Claude Code keeps the session: a background
        // job whose state dir is gone was removed (`claude rm`); any other
        // session lives on in its transcript until Claude Code cleans that up.
        const bool jobGone = t.info.kind == SessionInfo::Kind::Background;
        if (jobGone || (!t.transcriptPath.isEmpty() && !QFileInfo::exists(t.transcriptPath)) ||
            (t.transcriptPath.isEmpty() && _paths.findTranscript(t.info.sessionId).isEmpty()))
            dropped << it.key();
    }
    for (const auto &id : dropped) {
        _convOf.remove(_sessions.value(id)->info.sessionId);
        _sessions.remove(id);
        if (_firstScanDone)
            _events.fire(EvConversationRemoved{ConversationId{id}});
    }

    // Read what's new (only a live session can have changed, or one that just
    // ended: its last answer stops being "pending"; an ended one was read
    // already), then sort out which sessions are branches of which.
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &t = *it.value();
        if (!t.announcedInit || t.listed || t.sending || t.wasLive)
            tail(t);
    }
    QSet<QString> parentsToDiff = detectForks();

    // Announce what changed, per session — the /btw threads' replies first: a
    // reply arriving bumps its root's count on screen, so the root's own count
    // (announced with its parent) must already include it, never run ahead.
    std::vector<Tracked *> order;
    order.reserve(size_t(_sessions.size()));
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it)
        if (asThread(*it.value()))
            order.push_back(it.value().get());
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it)
        if (!asThread(*it.value()))
            order.push_back(it.value().get());
    for (Tracked *session : order) {
        Tracked   &t    = *session;
        const bool live = t.listed || t.sending;
        if (!t.announcedInit || live || t.wasLive) {
            diffAndAnnounce(t);
            if (asThread(t))
                parentsToDiff.insert(t.forkOf); // its root's reply count
        }
        t.wasLive = live;

        // msga's turn is over once its end is in the transcript — or, failing
        // that, once the session has sat idle for a while after our prompt.
        if (t.sending) {
            const bool ended =
                t.promptLanded &&
                (!t.parser.turnOpen() || (!(t.info.running && statusIsBusy(t.info.status)) &&
                                          nowMs() - t.promptLandedMs > kQuietTurnMs));
            if (ended) {
                t.sending = false;
                diffAndAnnounce(t); // a pending last answer becomes visible now
            } else if (!t.promptLanded && nowMs() - t.sendStartedMs > kLaunchTimeoutMs) {
                t.sending = false;
                failSends(
                    t,
                    QCoreApplication::translate(
                        "claude_code", "Claude Code didn't pick up the message."
                    )
                );
            }
        }
        dispatch(t); // the next queued message, if the turn is over

        if (asThread(t)) {
            // A thread, not a conversation — one listed before it was known
            // for a branch leaves the list.
            if (!t.lastConv.id.value.isEmpty() && _firstScanDone)
                _events.fire(EvConversationRemoved{ConversationId{t.convId}});
            t.lastConv = {};
            t.lastUser = {};
            continue;
        }
        if (!_firstScanDone) {
            t.lastConv = conversationFor(t);
            t.lastUser = assistantUser(t);
            t.lastBusy = busy(t);
            continue;
        }
        announceChanged(t);
        const User u = assistantUser(t);
        const bool b = busy(t);
        if (b != t.lastBusy)
            _events.fire(EvPresenceChanged{u.id, b});
        t.lastBusy = b;
    }
    for (const auto &id : parentsToDiff)
        if (Tracked *p = find(id))
            diffAndAnnounce(*p);
    // A teammate shows as working while any of its sessions does.
    for (const QString &role : roleIds()) {
        const bool b = roleBusy(role);
        if (_firstScanDone && b != _roleBusy.value(role))
            _events.fire(EvPresenceChanged{roleUser(role), b});
        _roleBusy.insert(role, b);
        const bool y = roleUnavailable(role);
        if (_firstScanDone && y != _roleUnavailable.value(role))
            _events.fire(EvUserChanged{teammateUser(_team.resolve(role))});
        _roleUnavailable.insert(role, y);
    }
    _firstScanDone = true;
    watchLive();
    scheduleSaveKnown();
    pumpTyping();
}

// Claude working on a turn shows as the session "typing" — the terminal's
// spinner ("Incubating… (17s · still thinking)"). The indicator forgets a typer
// after a few seconds without a refresh, so re-send every 3 s while any session
// is busy; the timer runs only then.
void Backend::pumpTyping() {
    if (!_typingTimer)
        return;
    bool any = false;
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it) {
        // A /btw thread working shows nowhere: "typing" is the session's own.
        if (!busy(*it.value()) || asThread(*it.value()))
            continue;
        any = true;
        _events.fire(EvTyping{ConversationId{it.key()}, roleUser(roleOf(*it.value()))});
    }
    if (any && !_typingTimer->isActive())
        _typingTimer->start();
    else if (!any)
        _typingTimer->stop();
}

void Backend::watchLive() {
    if (!_watcher)
        return;
    // Watch the two roster dirs (sessions come and go), every live session's
    // state file (status changes) and live transcripts (new messages). Paths
    // are re-added each time: atomic rewrites replace the inode.
    QStringList want = {_paths.sessionsDir(), _paths.jobsDir()};
    const QDir  sessions(_paths.sessionsDir());
    for (const auto &f : sessions.entryList({QStringLiteral("*.json")}, QDir::Files))
        want << sessions.filePath(f);
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it) {
        const Tracked &t = *it.value();
        if ((!t.listed && !t.sending) || t.info.sessionId.isEmpty())
            continue;
        if (t.info.kind == SessionInfo::Kind::Background)
            want << _paths.jobsDir() + QLatin1Char('/') + t.info.sessionId.left(8) +
                        QStringLiteral("/state.json");
        if (!t.transcriptPath.isEmpty())
            want << t.transcriptPath;
    }
    const QStringList current = _watcher->files() + _watcher->directories();
    QStringList       stale;
    for (const auto &p : current)
        if (!want.contains(p))
            stale << p;
    if (!stale.isEmpty())
        _watcher->removePaths(stale);
    QStringList add;
    for (const auto &p : want)
        if (!current.contains(p) && QFileInfo::exists(p))
            add << p;
    if (!add.isEmpty())
        _watcher->addPaths(add);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void Backend::connectRealtime() {
    if (_started)
        return;
    _started  = true;
    _debounce = new QTimer(_ctx);
    _debounce->setSingleShot(true);
    _debounce->setInterval(150); // a write burst (streamed reply) → one refresh
    QObject::connect(_debounce, &QTimer::timeout, _ctx, [this] { refresh(); });
    // File watching can coalesce or miss events (and re-created files drop off
    // the watch list); a slow sweep catches whatever it missed.
    _safetyPoll = new QTimer(_ctx);
    _safetyPoll->setInterval(10'000);
    QObject::connect(_safetyPoll, &QTimer::timeout, _ctx, [this] { refresh(); });
    _safetyPoll->start();
    _saveKnownTimer = new QTimer(_ctx);
    _saveKnownTimer->setSingleShot(true);
    _saveKnownTimer->setInterval(2000);
    QObject::connect(_saveKnownTimer, &QTimer::timeout, _ctx, [this] { saveKnown(); });
    _typingTimer = new QTimer(_ctx);
    _typingTimer->setInterval(3000);
    QObject::connect(_typingTimer, &QTimer::timeout, _ctx, [this] { pumpTyping(); });
    _watcher = new QFileSystemWatcher(_ctx);
    QObject::connect(_watcher, &QFileSystemWatcher::fileChanged, _ctx, [this] {
        scheduleRefresh();
    });
    QObject::connect(_watcher, &QFileSystemWatcher::directoryChanged, _ctx, [this] {
        scheduleRefresh();
    });
    if (!_firstScanDone)
        refresh();
    else
        watchLive();
}

void Backend::disconnectRealtime() {
    if (_safetyPoll)
        _safetyPoll->stop();
    if (_watcher)
        _watcher->removePaths(_watcher->files() + _watcher->directories());
}

// ── Snapshot loads ──────────────────────────────────────────────────────────

rpl::producer<UserId> Backend::loadMe() {
    return [this](auto consumer) {
        consumer.put_next(UserId{_me});
        consumer.put_done();
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<Conversation>> Backend::loadConversations() {
    return [this](auto consumer) {
        if (!_firstScanDone)
            refresh();
        std::vector<Conversation> out;
        out.reserve(size_t(_sessions.size()));
        for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
            if (!asThread(*it.value()))
                out.push_back(conversationFor(*it.value()));
        consumer.put_next(std::move(out));
        consumer.put_done();
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<User>> Backend::loadUsers() {
    return [this](auto consumer) {
        if (!_firstScanDone)
            refresh();
        std::vector<User> out;
        out.push_back(me());
        for (const QString &role : roleIds())
            out.push_back(teammateUser(_team.resolve(role)));
        for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
            if (!asThread(*it.value()))
                out.push_back(assistantUser(*it.value()));
        consumer.put_next(std::move(out));
        consumer.put_done();
        return rpl::lifetime();
    };
}

rpl::producer<bool> Backend::loadPresence(UserId id) {
    return [this, id](auto consumer) {
        bool active = id == _me;
        if (id == kAgentUser)
            active = roleBusy(kGeneralist);
        else if (id.value.startsWith(kRoleUserPrefix))
            active = roleBusy(id.value.mid(kRoleUserPrefix.size()));
        if (id.value.startsWith(kAssistantPrefix) && !id.value.startsWith(kRoleUserPrefix))
            if (const Tracked *t = find(id.value.mid(kAssistantPrefix.size())))
                active = busy(*t);
        consumer.put_next(bool(active));
        consumer.put_done();
        return rpl::lifetime();
    };
}

rpl::producer<Conversation> Backend::loadConversationInfo(ConversationId id, bool) {
    return [this, id](auto consumer) {
        if (const Tracked *t = find(id.value)) {
            consumer.put_next(conversationFor(*t));
        } else {
            Conversation gone;
            gone.id       = id;
            gone.notFound = true;
            consumer.put_next(std::move(gone));
        }
        consumer.put_done();
        return rpl::lifetime();
    };
}

rpl::producer<MessagePage> Backend::loadHistory(ConversationId id, std::optional<QString> cursor) {
    return [this, id, cursor](auto consumer) {
        MessagePage page;
        if (Tracked *t = find(id.value)) {
            auto msgs = visibleMessages(*t);
            if (!t->announcedInit) {
                for (const auto &m : msgs)
                    t->announced.insert(m.ts, m);
                t->announcedInit = true;
            }
            // Newest page first; the cursor is how many messages from the end
            // have been served already.
            constexpr int kPage = 200;
            const int     n     = int(msgs.size());
            const int     end   = std::max(0, n - (cursor ? cursor->toInt() : 0));
            const int     begin = std::max(0, end - kPage);
            page.messages.assign(msgs.begin() + begin, msgs.begin() + end);
            if (begin > 0)
                page.olderCursor = QString::number(n - begin);
        }
        consumer.put_next(std::move(page));
        consumer.put_done();
        return rpl::lifetime();
    };
}

rpl::producer<MessagePage> Backend::loadThread(ConversationId id, Ts root, std::optional<QString>) {
    return [this, id, root](auto consumer) {
        MessagePage page;
        Tracked    *t = find(id.value);
        if (Tracked *f = t ? forkFor(id.value, root) : nullptr) {
            // A /btw thread: the branch's first prompt, then the rest of it.
            auto replies = threadMessages(*f);
            if (f->forkAt >= 0) {
                Message r    = renderedAt(*f, size_t(f->forkAt));
                r.replyCount = int(replies.size());
                page.messages.push_back(std::move(r));
                for (auto &m : replies)
                    page.messages.push_back(std::move(m));
            }
            t = nullptr;
        }
        if (t) {
            // The root is the Subagent message; the replies are its own transcript,
            // everything in it said by the session's assistant (the prompt too —
            // the parent agent wrote it).
            const auto msgs = visibleMessages(*t);
            const auto it   = std::find_if(msgs.begin(), msgs.end(), [&](const Message &m) {
                return m.ts == root;
            });
            QString    agentId;
            for (const auto &item : t->parser.items())
                if (item.ts == root)
                    agentId = item.agentId;
            if (it != msgs.end()) {
                page.messages.push_back(*it);
                QFile f(Paths::subagentTranscript(t->transcriptPath, agentId));
                if (!agentId.isEmpty() && f.open(QIODevice::ReadOnly)) {
                    TranscriptParser p;
                    p.feed(f.readAll());
                    const UserId assistant = roleUser(roleOf(*t));
                    for (const auto &item : p.items()) {
                        if (_zen && item.kind == TranscriptItem::Kind::ToolGroup)
                            continue;
                        Message m      = toMessage(item, assistant, assistant);
                        m.threadRoot   = root;
                        m.parentUserId = assistant;
                        page.messages.push_back(std::move(m));
                    }
                }
            }
        }
        consumer.put_next(std::move(page));
        consumer.put_done();
        return rpl::lifetime();
    };
}

// ── Commands ────────────────────────────────────────────────────────────────

std::vector<SlashCommand> Backend::conversationCommands(ConversationId conv) {
    // Asked again after a while: skills and commands come and go.
    constexpr qint64 kFreshMs = 5 * 60'000;
    const Tracked   *t        = find(conv.value);
    if (!t || t->info.cwd.isEmpty())
        return {};
    const QString cwd  = t->info.cwd;
    CommandList  &list = _commands[cwd];
    if (!_creds.claudePath.isEmpty() && !list.loading &&
        (list.fetchedMs == 0 || nowMs() - list.fetchedMs > kFreshMs)) {
        list.loading = true;
        _launcher->listCommands(
            cwd, [this, cwd](std::vector<SlashCommand> commands, QJsonObject account) {
                CommandList &l = _commands[cwd];
                l.loading      = false;
                l.fetchedMs    = nowMs();
                if (!account.isEmpty())
                    _account = account;
                if (commands.empty())
                    return; // keep what we had; tried again after a while
                for (auto &c : commands)
                    c.iconUrl = kAvatarUrl;
                l.commands = std::move(commands);
            }
        );
    }
    // msga's own: Claude Code's /btw is a terminal panel, not something it can
    // run for msga — here a side question opens a thread (a branch of the session).
    std::vector<SlashCommand> out;
    SlashCommand              btw;
    btw.name = QStringLiteral("btw");
    btw.desc = QCoreApplication::translate(
        "claude_code", "Ask a side question in a thread; the session itself isn't touched"
    );
    btw.usage   = QCoreApplication::translate("claude_code", "<question>");
    btw.source  = QStringLiteral("msga");
    btw.iconUrl = kAvatarUrl;
    out.push_back(std::move(btw));
    // …and /status, a terminal panel too: msga shows what it knows in a dialog.
    SlashCommand status;
    status.name = QStringLiteral("status");
    status.desc = QCoreApplication::translate(
        "claude_code", "Show the session's Claude Code version, model, account and folder"
    );
    status.source  = QStringLiteral("msga");
    status.iconUrl = kAvatarUrl;
    status.local   = true;
    out.push_back(std::move(status));
    // /clear starts over: in the terminal the same window goes on with a new
    // session; here that is a new session chat in the same folder.
    SlashCommand clear;
    clear.name = QStringLiteral("clear");
    clear.desc = QCoreApplication::translate(
        "claude_code", "Start a new session in the same folder; this one stays as it is"
    );
    clear.source  = QStringLiteral("msga");
    clear.iconUrl = kAvatarUrl;
    clear.local   = true;
    out.push_back(std::move(clear));
    for (const auto &c : list.commands)
        if (c.name != QLatin1String("btw") && c.name != QLatin1String("status") &&
            c.name != QLatin1String("clear"))
            out.push_back(c);
    return out;
}

LocalCommandResult
Backend::runLocalCommand(ConversationId conv, const QString &name, const QString &) {
    LocalCommandResult r;
    Tracked           *t = find(conv.value);
    if (!t)
        return r;
    if (name == QLatin1String("status")) {
        r.status = conversationStatus(*t);
    } else if (name == QLatin1String("clear")) {
        r.error = cannotStartIn(t->info.cwd);
        if (r.error.isEmpty()) {
            // Same "agent type": a session started without permission checks
            // (bypassPermissions in its transcript) gets a successor like it.
            tail(*t);
            const bool skip = t->skipPermissionChecks ||
                              t->parser.permissionMode() == QLatin1String("bypassPermissions");
            r.open          = createSession(t->info.cwd, skip, roleOf(*t)).id;
        }
    }
    return r;
}

std::vector<std::pair<QString, QString>> Backend::conversationStatus(Tracked &tracked) {
    Tracked                                 *t = &tracked;
    std::vector<std::pair<QString, QString>> rows;
    tail(*t);
    const auto add = [&rows](const QString &label, const QString &value) {
        if (!value.isEmpty())
            rows.emplace_back(label, value);
    };
    add(QCoreApplication::translate("claude_code", "Version"), t->parser.version());
    add(QCoreApplication::translate("claude_code", "Session name"), shownTitle(*t));
    const Role mate = roleFor(*t);
    add(QCoreApplication::translate("claude_code", "Teammate"),
        mate.removed || mate.former
            ? QCoreApplication::translate("claude_code", "%1 (no longer on the team)")
                  .arg(mate.name)
            : mate.name);
    add(QCoreApplication::translate("claude_code", "Session ID"), t->info.sessionId);
    QString kind;
    if (t->info.kind == SessionInfo::Kind::Background)
        kind = QCoreApplication::translate("claude_code", "Background");
    else if (!t->info.running)
        kind = QCoreApplication::translate("claude_code", "Ended");
    else if (t->info.entrypoint == QLatin1String("cli"))
        kind = QCoreApplication::translate("claude_code", "Interactive, in a terminal");
    else
        kind = QCoreApplication::translate("claude_code", "Driven by another program");
    add(QCoreApplication::translate("claude_code", "Session kind"), kind);
    if (t->info.running && !t->info.peerSocket.isEmpty())
        add(QCoreApplication::translate("claude_code", "Peer address"),
            QStringLiteral("uds:") + t->info.peerSocket);
    add(QCoreApplication::translate("claude_code", "Folder"), homeRelative(t->info.cwd));
    const QString plan = _account.value(QLatin1String("subscriptionType")).toString();
    add(QCoreApplication::translate("claude_code", "Login method"),
        plan.isEmpty() ? _account.value(QLatin1String("apiProvider")).toString()
                       : QCoreApplication::translate("claude_code", "%1 account").arg(plan));
    add(QCoreApplication::translate("claude_code", "Organization"),
        _account.value(QLatin1String("organization")).toString());
    add(QCoreApplication::translate("claude_code", "Email"),
        _account.value(QLatin1String("email")).toString());
    add(QCoreApplication::translate("claude_code", "Model"), t->parser.model());
    add(QCoreApplication::translate("claude_code", "Permission mode"), t->parser.permissionMode());
    return rows;
}

void Backend::sendMessage(
    ConversationId conv, OutgoingMessage msg, std::function<void(bool ok, QString err)> done
) {
    Tracked                        *t    = find(conv.value);
    // The composer's text as typed: rawText is the composer's mrkdwn conversion,
    // but Claude reads markdown best as written.
    const QString                   text = msg.text.text.isEmpty() ? msg.rawText : msg.text.text;
    static const QRegularExpression kBtw(QStringLiteral("^/btw(?:\\s+([\\s\\S]*))?$"));
    const auto                      btw = kBtw.match(text.trimmed());
    QString                         reason;
    Tracked                        *target = t; // who gets the message
    if (!t) {
        reason = QCoreApplication::translate("claude_code", "This session no longer exists.");
    } else if (msg.threadRoot) {
        // A /btw thread continues its branch; a subagent's run is only there to read.
        target = forkFor(conv.value, *msg.threadRoot);
        reason = target ? readOnlyReason(*target)
                        : QCoreApplication::translate(
                              "claude_code", "Subagent threads can't be replied to."
                          );
    } else if (btw.hasMatch()) {
        // A side question: a branch of the session, which itself isn't touched —
        // so it can be asked while Claude is busy, or of a terminal's session.
        const QString question = btw.captured(1).trimmed();
        if (question.isEmpty())
            reason = QCoreApplication::translate("claude_code", "Type your question after /btw.");
        else if (t->info.sessionId.isEmpty())
            reason = QCoreApplication::translate(
                "claude_code", "Send the session its first message before asking on the side."
            );
        else if (_creds.claudePath.isEmpty())
            reason = readOnlyReason(*t);
        if (reason.isEmpty()) {
            startFork(*t, question, std::move(done));
            return;
        }
    } else {
        reason = readOnlyReason(*t);
    }
    if (!reason.isEmpty()) {
        _events.fire(EvSendFailed{conv, reason});
        if (done)
            done(false, reason);
        return;
    }
    if (!target->announcedInit)
        diffAndAnnounce(*target); // what's there already isn't news
    // msga's copy of it, from now on: after everything shown, uniquely timed.
    qint64 micros = nowMs() * 1000;
    for (const auto &m : shownMessages(*target))
        micros = std::max(micros, m.date + 1);
    target->outbox.append({text, microsToTs(micros), micros});
    // A record timed before that (clocks, the same millisecond) would otherwise
    // be tie-broken onto the copy's very ts, and its prompt never seen landing.
    target->parser.reserveTs(micros);
    dispatch(*target);        // at once when Claude is free; otherwise when its turn ends
    diffAndAnnounce(*target); // the copy replaces the Session's optimistic one
    if (done)
        done(true, {});
}

void Backend::startFork(
    Tracked &parent, const QString &question, std::function<void(bool, QString)> done
) {
    const QString parentConv = parent.convId;
    const QString cwd        = parent.info.cwd;
    const QString folder     = QDir(cwd).absolutePath();
    _forkingIn.insert(folder); // the branch mustn't flash up in the list meanwhile
    _launcher->fork(
        parent.info.sessionId,
        cwd,
        question,
        [this, parentConv, cwd, folder, done](QString sessionId, QString error) {
            _forkingIn.remove(folder);
            if (sessionId.isEmpty() || !find(parentConv)) {
                if (error.isEmpty())
                    error = QCoreApplication::translate(
                        "claude_code", "The session was removed from msga."
                    );
                _events.fire(EvSendFailed{ConversationId{parentConv}, error});
                if (done)
                    done(false, error);
                scheduleRefresh();
                return;
            }
            Tracked &f          = ensureTracked(convIdFor(sessionId));
            f.info.sessionId    = sessionId;
            f.info.cwd          = cwd;
            f.info.kind         = SessionInfo::Kind::Background;
            f.forkOf            = parentConv; // detectForks confirms it from the transcript
            f.awaitingRoot      = true;
            // Its turn is msga's, like any send: typing, then the answer.
            f.sending           = true;
            f.sendStartedMs     = nowMs();
            f.promptLanded      = false;
            f.inFlight          = done;
            // Everything in the thread is news.
            f.announcedAsThread = true;
            f.announcedInit     = true;
            f.announced.clear();
            scheduleSaveKnown();
            scheduleRefresh();
        }
    );
}

bool Backend::threadAcceptsReplies(ConversationId conv, Ts root) {
    return forkFor(conv.value, root) != nullptr;
}

ConversationId Backend::openThreadAsSession(ConversationId conv, Ts root) {
    Tracked *f = forkFor(conv.value, root);
    if (!f)
        return {};
    f->standalone = true;
    diffAndAnnounce(*f); // re-taken as a conversation of its own, silently
    if (Tracked *p = find(conv.value))
        diffAndAnnounce(*p); // the thread's root leaves the parent
    announceChanged(*f);     // and the session joins the list
    saveKnown();
    return ConversationId{f->convId};
}

// A prompt or an answer in a session of its own that nothing is writing to:
// Claude Code reads the transcript back when the session goes on, so what's
// taken out of it is gone for Claude too. Not while the session is working or
// driven from a terminal — its running process keeps the message — and not the
// history a branched session shares with this one (the thread is found by it).
const TranscriptItem *Backend::deletableItem(Tracked &t, const Ts &ts) {
    const bool drivenElsewhere = t.info.running && t.info.kind == SessionInfo::Kind::Interactive;
    if (asThread(t) || t.info.sessionId.isEmpty() || drivenElsewhere || busy(t) ||
        awaitsApproval(t.info) || !t.outbox.isEmpty() ||
        (t.info.running && statusHasShell(t.info.status)))
        return nullptr;
    tail(t);
    const auto &items = t.parser.items();
    const auto  it    = std::find_if(items.begin(), items.end(), [&](const TranscriptItem &i) {
        return i.ts == ts;
    });
    if (it == items.end() || it->uuid.isEmpty() || t.transcriptPath.isEmpty())
        return nullptr;
    const size_t index = size_t(it - items.begin());
    for (auto o = _sessions.begin(); o != _sessions.end(); ++o) {
        Tracked &other = *o.value();
        if (&other != &t && (other.forkOf == t.convId || t.forkOf == other.convId) &&
            index < sharedStart(items, other.parser.items()))
            return nullptr;
    }
    return &*it;
}

// A message still waiting for its turn can be taken back: it's only msga's.
Backend::Tracked *Backend::queuedHolder(const ConversationId &conv, const Ts &ts, int *index) {
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &t = *it.value();
        if ((asThread(t) ? t.forkOf : t.convId) != conv.value)
            continue;
        for (int i = 0; i < t.outbox.size(); ++i)
            if (t.outbox[i].ts == ts) {
                *index = i;
                return &t;
            }
    }
    return nullptr;
}

bool Backend::canDeleteMessage(ConversationId conv, Ts ts) {
    int index = 0;
    if (queuedHolder(conv, ts, &index))
        return true;
    Tracked *t = find(conv.value);
    return t && deletableItem(*t, ts);
}

void Backend::deleteMessage(ConversationId conv, Ts ts) {
    int index = 0;
    if (Tracked *q = queuedHolder(conv, ts, &index)) {
        q->outbox.removeAt(index);
        diffAndAnnounce(*q);
        return;
    }
    Tracked              *t    = find(conv.value);
    const TranscriptItem *item = t ? deletableItem(*t, ts) : nullptr;
    if (!item) {
        qWarning("claude code: %s can't be deleted now", qPrintable(ts));
        return;
    }
    QString error;
    if (!removeFromTranscript(t->transcriptPath, item->uuid, &error)) {
        qWarning("claude code: delete failed: %s", qPrintable(error));
        return;
    }
    // Read afresh: the file was rewritten, not appended to.
    t->parser = {};
    t->offset = 0;
    t->rendered.clear();
    tail(*t);
    diffAndAnnounce(*t);
}

void Backend::markRead(ConversationId conv, Ts ts) {
    Tracked *t = find(conv.value);
    if (!t || (!t->lastRead.isEmpty() && ts <= t->lastRead))
        return;
    t->lastRead = ts;
    scheduleSaveKnown();
}

// Pushes the session's conversation and user when what the list shows of them
// changed since the last push.
void Backend::announceChanged(Tracked &t) {
    const Conversation c = conversationFor(t);
    if (c.name != t.lastConv.name || c.localName != t.lastConv.localName ||
        c.readOnlyReason != t.lastConv.readOnlyReason || c.description != t.lastConv.description ||
        c.agentRole != t.lastConv.agentRole || t.lastConv.id.value.isEmpty())
        _events.fire(EvChannelCreated{c});
    t.lastConv   = c;
    const User u = assistantUser(t);
    if (u.name != t.lastUser.name || u.statusText != t.lastUser.statusText ||
        u.title != t.lastUser.title || u.unavailable != t.lastUser.unavailable ||
        u.avatarUrl != t.lastUser.avatarUrl)
        _events.fire(EvUserChanged{u});
    t.lastUser = u;
}

void Backend::setConversationLocalName(ConversationId conv, const QString &name) {
    Tracked *t = find(conv.value);
    if (!t || t->localName == name.trimmed())
        return;
    t->localName = name.trimmed();
    announceChanged(*t);
    saveKnown();
}

// ── Your profile ────────────────────────────────────────────────────────────
// Claude Code has no idea who you are beyond the login: the name and picture
// msga shows for you live in its own app data (profile.json + a copied image).

User Backend::me() const {
    User u;
    u.id          = _me;
    u.name        = _myName.isEmpty() ? loginName() : _myName;
    u.displayName = u.name;
    if (!_myAvatarPath.isEmpty() && QFileInfo::exists(_myAvatarPath))
        u.avatarUrl = QUrl::fromLocalFile(_myAvatarPath).toString();
    u.isActive = true;
    return u;
}

void Backend::loadProfile() {
    QFile f(profilePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    _myName             = o.value(QLatin1String("name")).toString();
    _myAvatarPath       = o.value(QLatin1String("avatar")).toString();
}

void Backend::saveProfile() const {
    QDir().mkpath(QFileInfo(profilePath()).absolutePath());
    QSaveFile f(profilePath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    QJsonObject o;
    o[QStringLiteral("name")]   = _myName;
    o[QStringLiteral("avatar")] = _myAvatarPath;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.commit();
}

void Backend::loadMyProfile(std::function<void(MyProfile)> done) {
    const User u = me();
    MyProfile  p;
    p.displayName = u.displayName;
    p.avatarUrl   = u.avatarUrl;
    if (done)
        done(p);
}

void Backend::updateProfile(
    const QHash<QString, QString> &fields, std::function<void(bool ok, QString err)> done
) {
    if (const auto it = fields.constFind(QStringLiteral("display_name")); it != fields.cend()) {
        // Cleared = back to the login name.
        _myName = it->trimmed() == loginName() ? QString() : it->trimmed();
        saveProfile();
        _events.fire(EvUserChanged{me()});
    }
    if (done)
        done(true, {});
}

void Backend::setPhoto(
    const QString &filePath, std::function<void(bool ok, QString err, QString url)> done
) {
    // A fresh file name per change: the image cache keys pictures by url.
    const QString dir = QFileInfo(profilePath()).absolutePath();
    QDir().mkpath(dir);
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    const QString copy   = dir + QStringLiteral("/avatar-%1.%2").arg(nowMs()).arg(suffix);
    if (!QFile::copy(filePath, copy)) {
        if (done)
            done(
                false, QCoreApplication::translate("claude_code", "Couldn't copy the picture."), {}
            );
        return;
    }
    if (!_myAvatarPath.isEmpty())
        QFile::remove(_myAvatarPath);
    _myAvatarPath = copy;
    saveProfile();
    const User u = me();
    _events.fire(EvUserChanged{u});
    if (done)
        done(true, {}, u.avatarUrl);
}

void Backend::setZenMode(bool on) {
    if (_zen == on)
        return;
    _zen = on;
    // Re-take what each session shows without announcing it: cards hidden or
    // revealed by the switch aren't new (or deleted) messages.
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &t = *it.value();
        if (!t.announcedInit)
            continue;
        QMap<Ts, Message> now;
        for (auto &m : shownMessages(t))
            now.insert(m.ts, std::move(m));
        t.announced = std::move(now);
    }
}

void Backend::leaveConversation(ConversationId conv) {
    if (!_sessions.contains(conv.value))
        return;
    // Its /btw threads go with it — they would otherwise turn up as sessions.
    QStringList forks;
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
        if (it.value()->forkOf == conv.value && asThread(*it.value()))
            forks << it.key();
    for (const auto &f : forks)
        hideSession(f);
    hideSession(conv.value);
    watchLive();
    pumpTyping();
    saveKnown();
}

void Backend::hideSession(const QString &convId) {
    const auto it = _sessions.find(convId);
    if (it == _sessions.end())
        return;
    Tracked &t = *it.value();
    // Messages still waiting here are dropped; a turn already handed to Claude
    // Code runs on there.
    failSends(t, QCoreApplication::translate("claude_code", "The session was removed from msga."));
    if (!t.info.sessionId.isEmpty()) {
        QString transcript = t.transcriptPath;
        if (transcript.isEmpty())
            transcript = _paths.findTranscript(t.info.sessionId);
        _hidden.insert(t.info.sessionId, Hidden{nowMs(), transcript});
        _convOf.remove(t.info.sessionId);
    }
    _sessions.erase(it);
}

void Backend::findAgentSessions(std::function<void(std::vector<FoundSession>)> done) {
    auto *watcher = new QFutureWatcher<std::vector<CatalogEntry>>(_ctx);
    QObject::connect(watcher, &QFutureWatcherBase::finished, _ctx, [this, watcher, done] {
        watcher->deleteLater();
        std::vector<FoundSession> out;
        for (const CatalogEntry &e : watcher->result()) {
            FoundSession f;
            f.id            = e.sessionId;
            f.title         = e.title;
            f.folder        = e.cwd;
            const Role mate = _team.resolve(e.role, e.roleName);
            f.agentRole     = mate.id;
            f.avatarUrl     = mate.avatarUrl;
            f.firstPrompt   = e.firstPrompt;
            f.lastPrompt    = e.lastPrompt;
            f.lastActiveMs  = e.modifiedMs;
            if (Tracked *t = find(convIdFor(e.sessionId))) {
                // A /btw branch is found in its parent.
                f.listed = ConversationId{asThread(*t) ? t->forkOf : t->convId};
                if (!asThread(*t))
                    f.title = shownTitle(*t);
            }
            out.push_back(std::move(f));
        }
        done(std::move(out));
    });
    watcher->setFuture(QtConcurrent::run(scanCatalog, _paths.projectsDir()));
}

ConversationId Backend::addFoundSession(const QString &sessionId) {
    if (Tracked *t = find(convIdFor(sessionId)))
        return ConversationId{asThread(*t) ? t->forkOf : t->convId};
    const QString transcript = _paths.findTranscript(sessionId);
    CatalogEntry  e;
    if (transcript.isEmpty() || !readCatalogEntry(transcript, e))
        return {};
    _hidden.remove(sessionId); // asked for by name: "Remove from msga" is undone
    QSet<QString> listedBefore;
    for (auto it = _sessions.cbegin(); it != _sessions.cend(); ++it)
        if (!asThread(*it.value()))
            listedBefore.insert(it.key());

    Tracked   &t      = ensureTracked(sessionId);
    const auto roster = scanSessions(_paths);
    const auto live   = std::find_if(roster.begin(), roster.end(), [&](const SessionInfo &s) {
        return s.sessionId == sessionId;
    });
    if (live != roster.end()) {
        t.info = *live; // running (it was only hidden)
    } else {
        // Ended, like the ones msga saw end. A background session's job would
        // have been in the roster, so this one resumes as a terminal one.
        t.info.sessionId  = sessionId;
        t.info.cwd        = e.cwd;
        t.info.kind       = SessionInfo::Kind::Interactive;
        t.info.entrypoint = QStringLiteral("cli");
    }
    t.transcriptPath = e.transcriptPath;
    tail(t);
    // Branch detection can't tell a /btw branch from a copy Claude Code made on
    // resume, and either way what's asked for here — and what was in the list
    // already — stays a session, never turns into a thread.
    detectForks();
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        Tracked &s = *it.value();
        if ((&s == &t || listedBefore.contains(it.key())) && asThread(s))
            s.standalone = true;
    }
    refresh(); // announces it (its history isn't news)
    saveKnown();
    return ConversationId{convIdFor(sessionId)};
}

QString Backend::cannotStartIn(const QString &directory) const {
    if (_creds.claudePath.isEmpty())
        return QCoreApplication::translate(
            "claude_code", "The claude command-line tool wasn't found on this computer."
        );
    if (!QFileInfo(directory).isDir())
        return QCoreApplication::translate("claude_code", "%1 isn't a folder.").arg(directory);
    // Background sessions refuse a folder Claude Code hasn't trusted; say so now
    // rather than on the first message.
    if (!isFolderTrusted(directory))
        return QCoreApplication::translate(
                   "claude_code",
                   "Claude Code doesn't trust %1 yet. Run `claude` in that folder once and "
                   "accept its trust prompt, then start the session again."
        )
            .arg(homeRelative(directory));
    return {};
}

Conversation
Backend::createSession(const QString &directory, bool skipPermissionChecks, const QString &role) {
    // The session itself starts with the first message (Claude Code picks its
    // id then); until then it is a conversation of its own.
    const QString convId   = kNewPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
    Tracked      &t        = ensureTracked(convId);
    t.info.cwd             = directory;
    t.info.kind            = SessionInfo::Kind::Background;
    t.skipPermissionChecks = skipPermissionChecks;
    t.role                 = _team.find(role) ? role : kGeneralist;
    t.announcedInit        = true; // everything in it is new, the first prompt included
    const Conversation c   = conversationFor(t);
    t.lastConv             = c;
    t.lastUser             = assistantUser(t);
    _events.fire(EvUserChanged{t.lastUser});
    _events.fire(EvChannelCreated{c});
    return c;
}

void Backend::startAgentSession(
    const QString                      &directory,
    bool                                skipPermissionChecks,
    const QString                      &role,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    if (const QString why = cannotStartIn(directory); !why.isEmpty()) {
        if (onError)
            onError(why);
        return;
    }
    const Conversation c = createSession(directory, skipPermissionChecks, role);
    if (onSuccess)
        onSuccess(c.id);
}

// ── Search / emoji / files ──────────────────────────────────────────────────

rpl::producer<std::vector<SearchResult>> Backend::searchMessages(const QString &query) {
    return [this, query](auto consumer) {
        std::vector<SearchResult> out;
        const QString             q = query.trimmed();
        if (!q.isEmpty()) {
            for (auto it = _sessions.begin(); it != _sessions.end() && out.size() < 200; ++it) {
                Tracked      &t      = *it.value();
                const bool    thread = asThread(t);
                const QString convId = thread ? t.forkOf : it.key();
                const QString title  = thread ? titleOf(*find(t.forkOf)) : titleOf(t);
                for (const auto &m : shownMessages(t))
                    if (m.text.text.contains(q, Qt::CaseInsensitive))
                        out.push_back({ConversationId{convId}, title, m});
            }
            std::sort(out.begin(), out.end(), [](const SearchResult &a, const SearchResult &b) {
                return a.msg.date > b.msg.date;
            });
        }
        consumer.put_next(std::move(out));
        consumer.put_done();
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
    ConversationId,
    const QStringList &,
    const QString &,
    std::optional<Ts>,
    std::function<void(bool, QString)> done
) {
    if (done)
        done(false, QStringLiteral("unsupported"));
}

// Files here are local: the pasted images saved in msga's cache.
void Backend::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    QFile f(QUrl(url).toLocalFile());
    if (!QUrl(url).isLocalFile() || !f.open(QIODevice::ReadOnly)) {
        if (onError)
            onError(QCoreApplication::translate("claude_code", "The file isn't there anymore."));
        return;
    }
    if (onData)
        onData(f.readAll());
}

rpl::producer<Event> Backend::events() const {
    return _events.events();
}

} // namespace claude_code
