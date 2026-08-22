// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/session_import/browser_login.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace slack::session {
namespace {

constexpr int kProbeIntervalMs  = 250;
constexpr int kProbeTimeoutMs   = 20000; // browser start → DevTools listening
constexpr int kPollIntervalMs   = 1000;  // cookie/workspace polling cadence
constexpr int kTeamGraceMs      = 12000; // wait for the web client to boot after login
constexpr int kOverallTimeoutMs = 5 * 60 * 1000;
constexpr int kHttpTimeoutMs    = 3000;

// Slack's own sign-in entry point: workspace URL, email code, or SSO all start here.
const char *kSignInUrl = "https://slack.com/signin";

struct Browser {
    QString exe;
    QString name;
};

// Chromium-family browsers we can drive, most-preferred first. Firefox is absent
// on purpose: it speaks WebDriver BiDi, not CDP.
QList<Browser> browserCandidates() {
    QList<Browser> found;
    const auto     add = [&found](const QString &exe, const QString &name) {
        if (!exe.isEmpty() && QFileInfo::exists(exe))
            found.append({exe, name});
    };

#if defined(Q_OS_WIN)
    const QStringList roots{
        qEnvironmentVariable("ProgramFiles"),
        qEnvironmentVariable("ProgramFiles(x86)"),
        qEnvironmentVariable("LOCALAPPDATA"),
    };
    const QList<QPair<QString, QString>> rel{
        {QStringLiteral("Google/Chrome/Application/chrome.exe"), QStringLiteral("Google Chrome")},
        {QStringLiteral("Chromium/Application/chrome.exe"), QStringLiteral("Chromium")},
        {QStringLiteral("BraveSoftware/Brave-Browser/Application/brave.exe"),
         QStringLiteral("Brave")},
        {QStringLiteral("Microsoft/Edge/Application/msedge.exe"), QStringLiteral("Microsoft Edge")},
        {QStringLiteral("Vivaldi/Application/vivaldi.exe"), QStringLiteral("Vivaldi")},
    };
    for (const auto &[path, name] : rel)
        for (const QString &root : roots)
            if (!root.isEmpty())
                add(QDir(root).filePath(path), name);
#elif defined(Q_OS_MACOS)
    const QStringList roots{QStringLiteral("/Applications"), QDir::homePath() + "/Applications"};
    const QList<QPair<QString, QString>> rel{
        {QStringLiteral("Google Chrome.app/Contents/MacOS/Google Chrome"),
         QStringLiteral("Google Chrome")},
        {QStringLiteral("Chromium.app/Contents/MacOS/Chromium"), QStringLiteral("Chromium")},
        {QStringLiteral("Brave Browser.app/Contents/MacOS/Brave Browser"), QStringLiteral("Brave")},
        {QStringLiteral("Microsoft Edge.app/Contents/MacOS/Microsoft Edge"),
         QStringLiteral("Microsoft Edge")},
        {QStringLiteral("Vivaldi.app/Contents/MacOS/Vivaldi"), QStringLiteral("Vivaldi")},
    };
    for (const auto &[path, name] : rel)
        for (const QString &root : roots)
            add(QDir(root).filePath(path), name);
#else
    const QList<QPair<QString, QString>> bins{
        {QStringLiteral("google-chrome"), QStringLiteral("Google Chrome")},
        {QStringLiteral("google-chrome-stable"), QStringLiteral("Google Chrome")},
        {QStringLiteral("chromium"), QStringLiteral("Chromium")},
        {QStringLiteral("chromium-browser"), QStringLiteral("Chromium")},
        {QStringLiteral("brave-browser"), QStringLiteral("Brave")},
        {QStringLiteral("microsoft-edge"), QStringLiteral("Microsoft Edge")},
        {QStringLiteral("microsoft-edge-stable"), QStringLiteral("Microsoft Edge")},
        {QStringLiteral("vivaldi"), QStringLiteral("Vivaldi")},
        {QStringLiteral("vivaldi-stable"), QStringLiteral("Vivaldi")},
    };
    for (const auto &[bin, name] : bins)
        add(QStandardPaths::findExecutable(bin), name);
#endif
    return found;
}

// Snap/Flatpak browsers run with a private /tmp, so our profile directory lands
// somewhere we can't see. The DevTools port still works (they share the host network
// namespace), but a plain packaged browser is the safer bet — try those first.
bool isSandboxed(const QString &exe) {
    const QString real = QFileInfo(exe).canonicalFilePath();
    return exe.startsWith(QLatin1String("/snap/")) || real.startsWith(QLatin1String("/snap/")) ||
           real.contains(QLatin1String("/flatpak/"));
}

Browser pickBrowser() {
    if (qEnvironmentVariable("MSGA_BROWSER_LOGIN") == QLatin1String("0"))
        return {};
    const QList<Browser> found = browserCandidates();
    for (const Browser &b : found)
        if (!isSandboxed(b.exe))
            return b;
    return found.isEmpty() ? Browser{} : found.first();
}

// Bind-then-release a loopback port so we can pass it to the browser explicitly.
// (Preferred over --remote-debugging-port=0: the DevToolsActivePort file Chromium
// writes into the profile is unreadable for sandboxed browsers — see isSandboxed.)
int freeLoopbackPort() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
        return 0;
    const int port = server.serverPort();
    server.close();
    return port;
}

// Pre-seed the throwaway profile so the browser refuses `slack://` links.
// Slack's post-login page (…/ssb/redirect, "Launching <team> — click Open Slack")
// hands off to the installed desktop app; without this the real Slack pops up
// instead of our sign-in finishing. Chromium keeps the per-scheme "always deny"
// decision in the profile's Preferences file, and this profile is brand new, so we
// can simply write the decision before first run. Best-effort: if it doesn't take,
// the flow still completes (we close the window as soon as we see that page).
bool blockDeepLinks(const QString &profileDir) {
    QDir dir(profileDir);
    if (!dir.mkpath(QStringLiteral("Default")))
        return false;
    const QJsonObject prefs{
        {QStringLiteral("protocol_handler"),
         QJsonObject{
             {QStringLiteral("excluded_schemes"), QJsonObject{{QStringLiteral("slack"), true}}},
         }},
    };
    QFile f(dir.filePath(QStringLiteral("Default/Preferences")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(QJsonDocument(prefs).toJson(QJsonDocument::Compact)) > 0;
}

// Make every file and directory under `root` writable, so the sweep below can
// unlink them even if the browser left something read-only.
void makeTreeWritable(const QString &root) {
    QDirIterator it(
        root,
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories
    );
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        auto            perms = QFile::permissions(fi.absoluteFilePath()) | QFileDevice::WriteOwner;
        if (fi.isDir())
            perms |= QFileDevice::ExeOwner | QFileDevice::ReadOwner;
        QFile::setPermissions(fi.absoluteFilePath(), perms);
    }
}

// Wipe the throwaway profile `delayMs` from now, retrying once. Chromium recreates
// a handful of files (Local State, Variations, Preferences) while it shuts down, so
// a single immediate sweep can leave the directory behind. Path-based and bound to
// qApp, so it still runs when the BrowserLogin that made the profile is long gone.
void wipeProfileLater(const QString &path, int delayMs) {
    // Refuse to sweep anything that isn't one of our own temp profiles.
    if (path.isEmpty() || !path.startsWith(QDir::tempPath()))
        return;
    const auto sweep = [path] {
        makeTreeWritable(path);
        QDir(path).removeRecursively();
    };
    QTimer::singleShot(delayMs, qApp, [path, sweep] {
        sweep();
        if (QFileInfo::exists(path))
            QTimer::singleShot(3000, qApp, sweep);
    });
}

// Signal the browser's whole process group (see setChildProcessModifier above) —
// signalling the wrapper alone leaves the real browser running. Only ever used as
// the fallback for CDP's Browser.close; the group check keeps us from signalling
// anything but our own launch.
void signalBrowser(QProcess *proc, bool force) {
#if defined(Q_OS_UNIX)
    const auto pid = static_cast<pid_t>(proc->processId());
    if (pid > 0 && ::getpgid(pid) == pid) {
        ::kill(-pid, force ? SIGKILL : SIGTERM);
        return;
    }
#endif
    if (force)
        proc->kill();
    else
        proc->terminate();
}

// Slack's own infrastructure hosts, which are never a user's workspace.
bool isInfraSubdomain(const QString &sub) {
    static const QSet<QString> infra{
        QStringLiteral("app"),
        QStringLiteral("api"),
        QStringLiteral("a"),
        QStringLiteral("edgeapi"),
        QStringLiteral("files"),
        QStringLiteral("downloads"),
        QStringLiteral("www"),
        QStringLiteral("my"),
        QStringLiteral("status"),
        QStringLiteral("slack"),
        QStringLiteral("join"),
        QStringLiteral("signin"),
    };
    return infra.contains(sub);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Pure helpers
// ─────────────────────────────────────────────────────────────────────────────

QString parseDebuggerUrl(const QByteArray &jsonVersion) {
    const QJsonObject o = QJsonDocument::fromJson(jsonVersion).object();
    return o.value(QStringLiteral("webSocketDebuggerUrl")).toString();
}

QList<TeamSession> parseLocalConfig(const QString &json) {
    QList<TeamSession> out;
    const QJsonObject  teams =
        QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("teams")).toObject();
    for (auto it = teams.begin(); it != teams.end(); ++it) {
        const QJsonObject t = it.value().toObject();
        TeamSession       s;
        s.teamId   = t.value(QStringLiteral("id")).toString(it.key());
        s.teamName = t.value(QStringLiteral("name")).toString();
        s.token    = t.value(QStringLiteral("token")).toString();
        if (!s.token.startsWith(QLatin1String("xoxc-")))
            s.token.clear(); // let the deriver scrape a real one
        QString url = t.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            const QString domain = t.value(QStringLiteral("domain")).toString();
            if (!domain.isEmpty())
                url = QStringLiteral("https://") + domain + QStringLiteral(".slack.com");
        }
        while (url.endsWith(QLatin1Char('/')))
            url.chop(1);
        s.workspaceUrl        = url;
        const QJsonObject ico = t.value(QStringLiteral("icon")).toObject();
        s.iconUrl             = ico.value(QStringLiteral("image_88"))
                        .toString(ico.value(QStringLiteral("image_68")).toString());
        // Useless without either a token to validate or a host to derive one from.
        if (s.token.isEmpty() && s.workspaceUrl.isEmpty())
            continue;
        out.append(s);
    }
    return out;
}

QList<TeamSession> teamsFromHosts(const QStringList &values) {
    static const QRegularExpression host(QStringLiteral("([a-z0-9][a-z0-9-]*)\\.slack\\.com"));
    QList<TeamSession>              out;
    QSet<QString>                   seen;
    for (const QString &value : values) {
        auto it = host.globalMatch(value.toLower());
        while (it.hasNext()) {
            const QString sub = it.next().captured(1);
            if (isInfraSubdomain(sub) || seen.contains(sub))
                continue;
            seen.insert(sub);
            TeamSession t;
            t.workspaceUrl = QStringLiteral("https://") + sub + QStringLiteral(".slack.com");
            out.append(t);
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// BrowserLogin
// ─────────────────────────────────────────────────────────────────────────────

bool browserLoginSupported() {
    return !pickBrowser().exe.isEmpty();
}

QString browserLoginName() {
    return pickBrowser().name;
}

BrowserLogin::BrowserLogin(QObject *parent) : QObject(parent) {}

BrowserLogin::~BrowserLogin() {
    _done = true; // no signals out of a half-destroyed object
    shutdown();
    // Last resort. The scheduled sweeps are bound to qApp, so they never run if the
    // app is quitting right now — take the profile (and the session cookie in it) out
    // synchronously instead. Hard-kill first: a browser still shutting down would
    // recreate files underneath us, which is exactly how a profile gets left behind.
    if (_browserProc && _browserProc->state() != QProcess::NotRunning)
        signalBrowser(_browserProc, true);
    if (!_profilePath.isEmpty() && _profilePath.startsWith(QDir::tempPath())) {
        makeTreeWritable(_profilePath);
        QDir(_profilePath).removeRecursively();
    }
}

// Chromium's --window-size is a fixed guess like any other: 1024x768 does not fit
// a laptop whose work area is shorter than that (a 1920x1080 panel at 150% leaves
// 1280x720 of it). Shrink it to what the screen can hold, never grow it.
//
// Only the size, not the position: --window-position is in Chromium's own DIP
// space, which on Linux is usually raw pixels while Qt reports scaled logical
// coordinates. Placing by our numbers would put the window on the wrong monitor
// as often as on the right one, so leave placement to the browser.
static QSize loginWindowSize() {
    const QSize want(1024, 768);
    QScreen    *scr = QGuiApplication::screenAt(QCursor::pos());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (!scr)
        return want;
    const QRect avail = scr->availableGeometry();
    if (avail.isEmpty())
        return want;
    return QSize(std::min(want.width(), avail.width()), std::min(want.height(), avail.height()));
}

void BrowserLogin::start() {
    const Browser browser = pickBrowser();
    if (browser.exe.isEmpty()) {
        finish({}, {}, QStringLiteral("no_browser"));
        return;
    }
    _profile = std::make_shared<QTemporaryDir>();
    _port    = freeLoopbackPort();
    if (!_profile->isValid() || _port == 0) {
        finish({}, {}, QStringLiteral("launch_failed"));
        return;
    }
    // We sweep the profile ourselves (see wipeProfileLater) — QTemporaryDir's own
    // destructor fires too early to survive Chromium's shutdown writes.
    _profile->setAutoRemove(false);
    _profilePath = _profile->path();
    blockDeepLinks(_profilePath);

    // Throwaway profile + a known DevTools port. The flags suppress first-run UI and
    // anything that would pop an OS credential prompt; --remote-allow-origins keeps
    // Chromium ≥111 from rejecting our websocket handshake.
    const QSize       winSize = loginWindowSize();
    const QStringList args{
        QStringLiteral("--user-data-dir=") + _profile->path(),
        QStringLiteral("--remote-debugging-port=") + QString::number(_port),
        QStringLiteral("--remote-allow-origins=*"),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--no-service-autorun"),
        QStringLiteral("--disable-sync"),
        QStringLiteral("--disable-extensions"),
        QStringLiteral("--disable-component-update"),
        QStringLiteral("--hide-crash-restore-bubble"),
        QStringLiteral("--password-store=basic"), // Linux: no keyring prompt
        QStringLiteral("--use-mock-keychain"),    // macOS: ditto (ignored elsewhere)
        QStringLiteral("--window-size=%1,%2").arg(winSize.width()).arg(winSize.height()),
        QString::fromLatin1(kSignInUrl),
    };

    _proc        = new QProcess(this);
    _browserProc = _proc; // stays valid after shutdown() hands _proc off
    connect(_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish({}, {}, QStringLiteral("launch_failed"));
    });
    // Window closed before we got anywhere: hand back whatever we have, so a user
    // who signed in and quit early still lands in the guided flow with the cookie.
    connect(_proc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (_done)
            return;
        if (_cookie.isEmpty())
            finish({}, {}, QStringLiteral("cancelled"));
        else
            finish(_cookie, teamsFromHosts(QStringList(_hosts.begin(), _hosts.end())), {});
    });
    // Discard the browser's own output: forwarding it buries msga's log in Chromium
    // chatter, and leaving an unread pipe would stall the browser once it fills.
    _proc->setStandardOutputFile(QProcess::nullDevice());
    _proc->setStandardErrorFile(QProcess::nullDevice());
#if defined(Q_OS_UNIX)
    // /usr/bin/{google-chrome,brave-browser,…} are shell wrappers that launch the
    // real binary and wait — they do NOT forward SIGTERM, so signalling our direct
    // child leaves the browser window open (verified with brave-browser). Put the
    // whole launch in its own session/process group so shutdown() can signal the
    // group, not just the wrapper.
    _proc->setChildProcessModifier([] { ::setsid(); });
#endif
    _proc->start(browser.exe, args);

    _since.start();
    _nam = new QNetworkAccessManager(this);
    emit progress(tr("Opening %1…").arg(browser.name));

    _probe = new QTimer(this);
    _probe->setInterval(kProbeIntervalMs);
    connect(_probe, &QTimer::timeout, this, &BrowserLogin::probeDevTools);
    _probe->start();
}

void BrowserLogin::probeDevTools() {
    if (_done || _probing)
        return;
    if (_since.elapsed() > kProbeTimeoutMs) {
        finish({}, {}, QStringLiteral("no_devtools"));
        return;
    }
    _probing = true;
    QNetworkRequest req{QUrl(QStringLiteral("http://127.0.0.1:%1/json/version").arg(_port))};
    req.setTransferTimeout(kHttpTimeoutMs);
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        _probing = false;
        if (_done || reply->error() != QNetworkReply::NoError)
            return; // browser not listening yet — the timer retries
        const QString ws = parseDebuggerUrl(reply->readAll());
        if (ws.isEmpty())
            return;
        _probe->stop();
        openCdp(ws);
    });
}

void BrowserLogin::openCdp(const QString &wsUrl) {
    _ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(_ws, &QWebSocket::textMessageReceived, this, &BrowserLogin::onMessage);
    connect(_ws, &QWebSocket::connected, this, [this] {
        emit progress(
            tr("Sign in to Slack in the browser window — msga picks it up automatically.")
        );
        // Attach to the tab we opened (and to any the login flow spawns) so the
        // desktop-app handoff navigation can be intercepted before it ever loads.
        // Targets created later are held at start until interception is enabled.
        send(
            QStringLiteral("Target.setAutoAttach"),
            QJsonObject{
                {QStringLiteral("autoAttach"), true},
                {QStringLiteral("waitForDebuggerOnStart"), true},
                {QStringLiteral("flatten"), true},
            },
            {},
            {}
        );
        send(QStringLiteral("Target.getTargets"), {}, {}, [this](const QJsonObject &r, bool ok) {
            if (!ok || _done)
                return;
            for (const QJsonValue &v : r.value(QStringLiteral("targetInfos")).toArray()) {
                const QJsonObject t = v.toObject();
                if (t.value(QStringLiteral("type")).toString() == QLatin1String("page"))
                    attachPage(t.value(QStringLiteral("targetId")));
            }
        });
        _poll = new QTimer(this);
        _poll->setInterval(kPollIntervalMs);
        connect(_poll, &QTimer::timeout, this, &BrowserLogin::poll);
        _poll->start();
        poll();
    });
    connect(_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!_done && !_poll) // only fatal before the session is up
            finish({}, {}, QStringLiteral("cdp_failed"));
    });
    _ws->open(QUrl(wsUrl));
}

void BrowserLogin::poll() {
    if (_done)
        return;
    if (_since.elapsed() > kOverallTimeoutMs) {
        finish({}, {}, QStringLiteral("timeout"));
        return;
    }
    send(QStringLiteral("Storage.getCookies"), {}, {}, [this](const QJsonObject &r, bool ok) {
        if (ok)
            collectCookies(r);
    });
    if (_cookie.isEmpty())
        return;
    discoverTeams();
    // The web client never booted (e.g. the user stopped on Slack's "open the
    // desktop app" page): settle for the hosts we saw, or none at all.
    if (_cookieAt.elapsed() > kTeamGraceMs)
        finish(_cookie, teamsFromHosts(QStringList(_hosts.begin(), _hosts.end())), {});
}

void BrowserLogin::send(
    const QString &method, const QJsonObject &params, const QString &sessionId, Handler h
) {
    if (!_ws || _ws->state() != QAbstractSocket::ConnectedState)
        return;
    const int   id = _nextId++;
    QJsonObject msg{{QStringLiteral("id"), id}, {QStringLiteral("method"), method}};
    if (!params.isEmpty())
        msg[QStringLiteral("params")] = params;
    if (!sessionId.isEmpty())
        msg[QStringLiteral("sessionId")] = sessionId;
    _pending.insert(id, std::move(h));
    _ws->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void BrowserLogin::onMessage(const QString &text) {
    const QJsonObject o  = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto        it = o.constFind(QStringLiteral("id"));
    if (it == o.constEnd()) {
        onEvent(
            o.value(QStringLiteral("method")).toString(),
            o.value(QStringLiteral("params")).toObject(),
            o.value(QStringLiteral("sessionId")).toString()
        );
        return;
    }
    const Handler h = _pending.take(it->toInt());
    if (!h)
        return;
    h(o.value(QStringLiteral("result")).toObject(), !o.contains(QStringLiteral("error")));
}

void BrowserLogin::onEvent(
    const QString &method, const QJsonObject &params, const QString &sessionId
) {
    if (_done)
        return;
    if (method == QLatin1String("Target.attachedToTarget")) {
        const QString     child = params.value(QStringLiteral("sessionId")).toString();
        const QJsonObject info  = params.value(QStringLiteral("targetInfo")).toObject();
        if (info.value(QStringLiteral("type")).toString() == QLatin1String("page")) {
            _pageSessions.insert(info.value(QStringLiteral("targetId")).toString(), child);
            interceptHandoff(child);
        }
        // Auto-attached targets start paused (waitForDebuggerOnStart) — always let
        // them run, or the login page hangs forever on a blank screen.
        send(QStringLiteral("Runtime.runIfWaitingForDebugger"), {}, child, {});
        return;
    }
    if (method == QLatin1String("Fetch.requestPaused")) {
        handleHandoff(
            params.value(QStringLiteral("request"))
                .toObject()
                .value(QStringLiteral("url"))
                .toString(),
            params.value(QStringLiteral("requestId")).toString(),
            sessionId
        );
    }
}

void BrowserLogin::attachPage(const QJsonValue &targetId) {
    const QString id = targetId.toString();
    if (id.isEmpty() || _pageSessions.contains(id))
        return;
    _pageSessions.insert(id, QString()); // claim it; the reply fills the session in
    const QJsonObject p{
        {QStringLiteral("targetId"), targetId},
        {QStringLiteral("flatten"), true},
    };
    send(QStringLiteral("Target.attachToTarget"), p, {}, [this, id](const QJsonObject &r, bool ok) {
        if (_done)
            return;
        if (!ok) {
            _pageSessions.remove(id); // let a later tick retry
            return;
        }
        const QString sess = r.value(QStringLiteral("sessionId")).toString();
        _pageSessions.insert(id, sess);
        interceptHandoff(sess);
    });
}

void BrowserLogin::interceptHandoff(const QString &sessionId) {
    if (sessionId.isEmpty())
        return;
    // Catch the top-level navigation to Slack's desktop-app handoff page
    // (…/ssb/redirect) *before* it loads. That page immediately tries to open the
    // installed Slack app, which pops an "Open Slack?" prompt over our window and
    // can hand the user off to a different app mid-sign-in. We answer the request
    // ourselves instead — see handleHandoff.
    const QJsonObject params{
        {QStringLiteral("patterns"),
         QJsonArray{QJsonObject{
             {QStringLiteral("urlPattern"), QStringLiteral("*/ssb/*")},
             {QStringLiteral("resourceType"), QStringLiteral("Document")},
             {QStringLiteral("requestStage"), QStringLiteral("Request")},
         }}},
    };
    send(QStringLiteral("Fetch.enable"), params, sessionId, {});
}

void BrowserLogin::handleHandoff(
    const QString &url, const QString &requestId, const QString &sessionId
) {
    if (requestId.isEmpty())
        return;
    _hosts.insert(url);
    // Serve a placeholder in place of the handoff page: no slack:// link ever runs,
    // and the window closes on its own a moment later.
    const QString body = QStringLiteral(
                             "<!doctype html><meta charset=utf-8>"
                             "<body style=\"font:16px system-ui;text-align:center;"
                             "margin-top:20vh;color:#444\">%1</body>"
    )
                             .arg(tr("Signed in — finishing up in msga. You can close this."));
    const QJsonObject fulfill{
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("responseCode"), 200},
        {QStringLiteral("responseHeaders"),
         QJsonArray{QJsonObject{
             {QStringLiteral("name"), QStringLiteral("Content-Type")},
             {QStringLiteral("value"), QStringLiteral("text/html; charset=utf-8")},
         }}},
        {QStringLiteral("body"), QString::fromLatin1(body.toUtf8().toBase64())},
    };
    send(QStringLiteral("Fetch.fulfillRequest"), fulfill, sessionId, {});

    // Reaching this page means the sign-in completed, so the cookie is already set —
    // read it now rather than waiting for the next poll tick.
    send(QStringLiteral("Storage.getCookies"), {}, {}, [this](const QJsonObject &r, bool ok) {
        if (_done)
            return;
        if (ok)
            collectCookies(r);
        if (!_cookie.isEmpty())
            finish(_cookie, teamsFromHosts(QStringList(_hosts.begin(), _hosts.end())), {});
    });
}

void BrowserLogin::collectCookies(const QJsonObject &result) {
    for (const QJsonValue &v : result.value(QStringLiteral("cookies")).toArray()) {
        const QJsonObject c      = v.toObject();
        const QString     domain = c.value(QStringLiteral("domain")).toString();
        if (!domain.endsWith(QLatin1String("slack.com")))
            continue;
        _hosts.insert(domain);
        if (c.value(QStringLiteral("name")).toString() != QLatin1String("d"))
            continue;
        // Verbatim, still percent-encoded the way Slack stores it — the deriver
        // seeds it into its cookie jar unchanged.
        const QString value = c.value(QStringLiteral("value")).toString();
        if (!value.startsWith(QLatin1String("xoxd-")) || !_cookie.isEmpty())
            continue;
        _cookie = value;
        _cookieAt.start();
        emit progress(tr("Signed in — looking up your workspaces…"));
    }
}

void BrowserLogin::discoverTeams() {
    if (_evaluating)
        return;
    if (!_sessionId.isEmpty()) {
        readLocalConfig(_sessionId);
        return;
    }
    send(QStringLiteral("Target.getTargets"), {}, {}, [this](const QJsonObject &r, bool ok) {
        if (!ok || _done)
            return;
        QString webClient; // targetId of an app.slack.com page, if the client booted
        bool    handoff = false;
        for (const QJsonValue &v : r.value(QStringLiteral("targetInfos")).toArray()) {
            const QJsonObject t   = v.toObject();
            const QString     url = t.value(QStringLiteral("url")).toString();
            if (url.contains(QLatin1String("slack.com")))
                _hosts.insert(url);
            if (t.value(QStringLiteral("type")).toString() != QLatin1String("page"))
                continue;
            if (url.startsWith(QLatin1String("https://app.slack.com"))) {
                if (webClient.isEmpty())
                    webClient = t.value(QStringLiteral("targetId")).toString();
            } else if (url.contains(QLatin1String(".slack.com/ssb/"))) {
                handoff = true;
            }
        }
        if (!webClient.isEmpty()) {
            // The web client is up. We are already attached to every page (see
            // openCdp), so reuse that session and keep re-reading it until the SPA
            // has written localConfig_v2.
            const QString sess = _pageSessions.value(webClient);
            if (sess.isEmpty()) {
                attachPage(webClient); // not attached yet — read it on the next tick
                return;
            }
            _sessionId = sess;
            readLocalConfig(sess);
            return;
        }
        // Slack's desktop-app handoff page ("Launching <team>") — the sign-in is done
        // and this page never boots the web client, so stop here rather than sitting
        // on a page whose whole purpose is to open the real Slack app. The URL itself
        // carries the workspace host, which is all the deriver needs.
        if (handoff)
            finish(_cookie, teamsFromHosts(QStringList(_hosts.begin(), _hosts.end())), {});
    });
}

void BrowserLogin::readLocalConfig(const QString &sessionId) {
    if (sessionId.isEmpty() || _evaluating)
        return;
    _evaluating = true;
    const QJsonObject params{
        {QStringLiteral("expression"),
         QStringLiteral("window.localStorage.getItem('localConfig_v2')")},
        {QStringLiteral("returnByValue"), true},
    };
    send(
        QStringLiteral("Runtime.evaluate"),
        params,
        sessionId,
        [this](const QJsonObject &r, bool ok) {
            _evaluating = false;
            if (_done)
                return;
            if (!ok) {
                _sessionId.clear(); // page navigated away — re-attach next tick
                return;
            }
            const QList<TeamSession> teams = parseLocalConfig(r.value(QStringLiteral("result"))
                                                                  .toObject()
                                                                  .value(QStringLiteral("value"))
                                                                  .toString());
            if (!teams.isEmpty())
                finish(_cookie, teams, {});
        }
    );
}

void BrowserLogin::finish(
    const QString &cookie, const QList<TeamSession> &teams, const QString &error
) {
    if (_done)
        return;
    _done = true;
    shutdown();
    emit finished(cookie, teams, error);
}

void BrowserLogin::shutdown() {
    if (_probe)
        _probe->stop();
    if (_poll)
        _poll->stop();
    _pending.clear();
    if (_ws) {
        // Quit the browser over CDP — the reliable way, since signalling the wrapper
        // script we launched doesn't reach the real process. flush() before teardown
        // so the frame leaves even when we're being destroyed.
        if (_ws->state() == QAbstractSocket::ConnectedState) {
            const QJsonObject msg{
                {QStringLiteral("id"), _nextId++},
                {QStringLiteral("method"), QStringLiteral("Browser.close")},
            };
            _ws->sendTextMessage(
                QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact))
            );
            _ws->flush();
        }
        _ws->disconnect(this);
        _ws->close();
        _ws->deleteLater();
        _ws = nullptr;
    }
    if (!_proc)
        return;
    QProcess     *proc = _proc;
    const QString path = _profilePath;
    _proc              = nullptr;
    _profile.reset();
    proc->disconnect(this);
    if (proc->state() == QProcess::NotRunning) {
        proc->deleteLater();
        wipeProfileLater(path, 0);
        return;
    }
    // Wipe as soon as the browser is gone, and unconditionally a few seconds later
    // as well: if this object is destroyed (dialog closed) the connection below dies
    // with it, and a throwaway profile must never be left holding a session cookie.
    connect(proc, &QProcess::finished, proc, [proc, path](int, QProcess::ExitStatus) {
        proc->deleteLater();
        wipeProfileLater(path, 1500);
    });
    wipeProfileLater(path, 8000);
    // Browser.close (already sent above) is the graceful path; signals are only for
    // the case where CDP never came up. Give the browser room to shut down cleanly
    // first — killing it mid-flush is what leaves recreated files behind.
    QTimer::singleShot(2000, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            signalBrowser(proc, false);
    });
    QTimer::singleShot(6000, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            signalBrowser(proc, true);
    });
}

} // namespace slack::session
