#include "app/slack/browser_login.h"

#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdlib>

#ifndef _WIN32
#include <climits>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace slack {

namespace {

constexpr int kProbeIntervalMs  = 250;
constexpr int kProbeTimeoutMs   = 20000; // browser start → DevTools listening
constexpr int kPollIntervalMs   = 1000;  // cookie/workspace polling cadence
constexpr int kTeamGraceMs      = 12000; // the web client's boot after the login
constexpr int kOverallTimeoutMs = 5 * 60 * 1000;
constexpr int kHttpTimeoutMs    = 3000;

// Slack's own entry point: workspace URL, email code and SSO all start here.
constexpr const char *kSignInUrl     = "https://slack.com/signin";
// Every throwaway profile's name starts so; nothing else is ever wiped.
constexpr const char *kProfilePrefix = "msga-signin-";

struct Browser {
    std::string exe, name;
};

std::vector<Browser> candidates() {
    std::vector<Browser> found;
    // isExecutable: the browser is the host's in a Flatpak sandbox, which
    // doesn't see the host's /usr (base/host.h).
    auto                 add = [&](std::string exe, const char *name) {
        if (!exe.empty() && base::isExecutable(exe))
            found.push_back({std::move(exe), name});
    };
#if defined(_WIN32)
    static const char *const kRel[][2] = {
        {"Google/Chrome/Application/chrome.exe", "Google Chrome"},
        {"Chromium/Application/chrome.exe", "Chromium"},
        {"BraveSoftware/Brave-Browser/Application/brave.exe", "Brave"},
        {"Microsoft/Edge/Application/msedge.exe", "Microsoft Edge"},
        {"Vivaldi/Application/vivaldi.exe", "Vivaldi"},
    };
    const std::string roots[] = {
        base::env("ProgramFiles"), base::env("ProgramFiles(x86)"), base::env("LOCALAPPDATA")
    };
    for (const auto &r : kRel)
        for (const std::string &root : roots)
            if (!root.empty())
                add(file::join(root, r[0]), r[1]);
#elif defined(__APPLE__)
    static const char *const kRel[][2] = {
        {"Google Chrome.app/Contents/MacOS/Google Chrome", "Google Chrome"},
        {"Chromium.app/Contents/MacOS/Chromium", "Chromium"},
        {"Brave Browser.app/Contents/MacOS/Brave Browser", "Brave"},
        {"Microsoft Edge.app/Contents/MacOS/Microsoft Edge", "Microsoft Edge"},
        {"Vivaldi.app/Contents/MacOS/Vivaldi", "Vivaldi"},
    };
    const std::string roots[] = {"/Applications", file::join(base::env("HOME"), "Applications")};
    for (const auto &r : kRel)
        for (const std::string &root : roots)
            add(file::join(root, r[0]), r[1]);
#else
    static const char *const kBins[][2] = {
        {"google-chrome", "Google Chrome"},
        {"google-chrome-stable", "Google Chrome"},
        {"chromium", "Chromium"},
        {"chromium-browser", "Chromium"},
        {"brave-browser", "Brave"},
        {"microsoft-edge", "Microsoft Edge"},
        {"microsoft-edge-stable", "Microsoft Edge"},
        {"vivaldi", "Vivaldi"},
        {"vivaldi-stable", "Vivaldi"},
    };
    for (const auto &b : kBins)
        add(base::findExecutable(b[0]), b[1]);
#endif
    return found;
}

// Snap/Flatpak browsers run with a private /tmp: our profile would land
// somewhere we can't see. The DevTools port still works, but a plainly
// packaged browser is the safer bet — try those first.
bool sandboxed(const std::string &exe) {
#ifdef _WIN32
    (void)exe;
    return false;
#else
    char        buf[PATH_MAX];
    std::string real = ::realpath(exe.c_str(), buf) ? std::string(buf) : exe;
    return str::startsWith(exe, "/snap/") || str::startsWith(real, "/snap/") ||
           real.find("/flatpak/") != std::string::npos;
#endif
}

Browser pickBrowser() {
    if (base::env("MSGA_BROWSER_LOGIN") == "0")
        return {};
    const std::vector<Browser> found = candidates();
    for (const Browser &b : found)
        if (!sandboxed(b.exe))
            return b;
    return found.empty() ? Browser{} : found.front();
}

std::string tempRoot(plat::App &app) {
    std::string t = app.standardDir(plat::StandardDir::Temp);
#ifndef _WIN32
    if (t.empty())
        t = "/tmp";
#endif
    return t;
}

std::string makeProfileDir(plat::App &app) {
    const std::string root = tempRoot(app);
    if (root.empty())
        return {};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::string rnd = crypto::randomHex(6);
        if (rnd.empty())
            return {};
        const std::string dir = file::join(root, str::concat({kProfilePrefix, rnd}));
        if (file::exists(dir))
            continue;
#ifdef _WIN32
        if (file::makeDirs(dir))
            return dir;
#else
        if (::mkdir(dir.c_str(), 0700) == 0)
            return dir;
#endif
    }
    return {};
}

bool ownProfile(std::string_view path) {
    return path.find(kProfilePrefix) != std::string_view::npos &&
           file::baseName(path).substr(0, std::string_view(kProfilePrefix).size()) ==
               kProfilePrefix;
}

#ifndef _WIN32
// Chromium leaves read-only directories behind, inside which nothing can be
// unlinked: owner rwx on the profile's own directories first. lstat: a link
// is never followed out of the profile.
void makeWritable(const std::string &dir) {
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    ::chmod(dir.c_str(), 0700);
    std::vector<file::DirEntry> entries;
    if (file::listDir(dir, &entries))
        for (const auto &e : entries)
            if (e.isDir)
                makeWritable(file::join(dir, e.name));
}
#endif

// Removes a tree (file::removeTree clears Windows' read-only attribute
// itself).
void removeTree(const std::string &path) {
#ifndef _WIN32
    makeWritable(path);
#endif
    file::removeTree(path);
}

void wipeNow(const std::string &path) {
    if (!path.empty() && ownProfile(path))
        removeTree(path);
}

// Wipes `delayMs` from now and once more 3 s later: Chromium recreates
// Local State, Variations and Preferences while it shuts down. Bound to the
// app, not to the BrowserLogin, so it runs after the dialog is gone.
void wipeLater(plat::App &app, const std::string &path, int delayMs) {
    if (path.empty() || !ownProfile(path))
        return;
    plat::App *a = &app;
    app.addTimer(delayMs, false, [a, path] {
        wipeNow(path);
        if (file::exists(path))
            a->addTimer(3000, false, [path] { wipeNow(path); });
    });
}

// A new profile refuses slack:// links before its first run: the "open the
// desktop app" handoff after sign-in can't launch the real Slack. Chromium
// keeps that decision in Default/Preferences.
void blockDeepLinks(const std::string &profile) {
    file::writeAtomic(
        file::join(profile, "Default/Preferences"),
        R"({"protocol_handler":{"excluded_schemes":{"slack":true}}})",
        0600
    );
}

// The window must fit the work area (1024×768 does not fit a 1280×720
// laptop at 150 %). Size only: --window-position is in Chromium's own
// coordinate space, which differs from ours often enough to land on the
// wrong monitor.
void windowSize(plat::App &app, int *w, int *h) {
    *w = 1024, *h = 768;
    for (const plat::Monitor &m : app.monitors())
        if (m.primary && m.workArea.w > 0 && m.workArea.h > 0) {
            *w = std::min(*w, int(m.workArea.w));
            *h = std::min(*h, int(m.workArea.h));
        }
}

std::string quoted(std::string_view s) {
    std::string out;
    json::escapeString(out, s);
    return out;
}

} // namespace

std::string browserLoginName() {
    return pickBrowser().name;
}

std::string BrowserLogin::parseDebuggerUrl(std::string_view jsonVersion) {
    json::Document doc;
    if (!doc.parse(jsonVersion, nullptr))
        return {};
    return std::string(doc.root()["webSocketDebuggerUrl"].str());
}

BrowserLogin::BrowserLogin(plat::App &app, net::Client &client) : _app(app), _client(client) {}

BrowserLogin::~BrowserLogin() {
    _done = true; // no callbacks out of a half-destroyed object
    shutdown();
    // Last resort: the scheduled wipes never run when the app is quitting
    // right now, so take the profile (and the cookie in it) out at once.
    // Kill first — a browser still shutting down recreates files.
    if (_proc && _proc->running())
        _proc->kill(true);
    wipeNow(_profile);
}

void BrowserLogin::setProgress(std::string s) {
    if (_progress)
        _progress(std::move(s));
}

void BrowserLogin::start(std::function<void(std::string)> progress, Done done) {
    _progress       = std::move(progress);
    _onDone         = std::move(done);
    const Browser b = pickBrowser();
    _browserName    = b.name;
    if (b.exe.empty()) {
        // Never from inside start(): callers rely on done running later.
        _app.post([this] { finish({}, {}, "no_browser"); });
        return;
    }
    _profile = makeProfileDir(_app);
    _port    = net::freeLoopbackPort();
    if (_profile.empty() || _port == 0) {
        _app.post([this] { finish({}, {}, "launch_failed"); });
        return;
    }
    blockDeepLinks(_profile);
    int w, h;
    windowSize(_app, &w, &h);
    // A throwaway profile and a known DevTools port. The rest keeps first-run
    // UI and OS credential prompts away; --remote-allow-origins lets
    // Chromium ≥ 111 accept our WebSocket.
    const std::vector<std::string> args = {
        "--user-data-dir=" + _profile,
        "--remote-debugging-port=" + str::number(_port),
        "--remote-allow-origins=*",
        "--no-first-run",
        "--no-default-browser-check",
        "--no-service-autorun",
        "--disable-sync",
        "--disable-extensions",
        "--disable-component-update",
        "--hide-crash-restore-bubble",
        "--password-store=basic", // Linux: no keyring prompt
        "--use-mock-keychain",    // macOS: the same (ignored elsewhere)
        str::concat({"--window-size=", str::number(w), ",", str::number(h)}),
        kSignInUrl,
    };
    _proc = std::make_shared<base::Process>();
    if (!_proc->start(b.exe, args)) {
        _app.post([this] { finish({}, {}, "launch_failed"); });
        return;
    }
    _startMs = double(base::monotonicMs());
    setProgress(i18n::arg(i18n::tr("Opening %1\xE2\x80\xA6"), b.name));
    _probeTimer = _app.addTimer(kProbeIntervalMs, true, [this] { probe(); });
    // The window closed before we got anywhere: hand back what we have, so
    // someone who signed in and quit early still lands in the guided flow.
    _exitTimer  = _app.addTimer(500, true, [this] {
        if (_done || _proc->running())
            return;
        LOG_INFO("signin", "%s exited (%d)", _browserName.c_str(), _proc->exitStatus());
        if (_cookie.empty())
            finish({}, {}, "cancelled");
        else
            finish(_cookie, hostTeams(), {});
    });
}

std::vector<TeamSession> BrowserLogin::hostTeams() const {
    return teamsFromHosts(_hosts);
}

void BrowserLogin::probe() {
    if (_done || _probeReq)
        return;
    if (double(base::monotonicMs()) - _startMs > kProbeTimeoutMs) {
        finish({}, {}, "no_devtools");
        return;
    }
    net::Request req;
    req.url          = str::concat({"http://127.0.0.1:", str::number(_port), "/json/version"});
    req.timeoutMs    = kHttpTimeoutMs;
    req.maxRedirects = 0;
    _probeReq        = _client.send(std::move(req), [this](net::Response r) {
        _probeReq = 0;
        if (_done || !r.ok())
            return; // not listening yet: the timer retries
        const std::string ws = parseDebuggerUrl(r.body);
        if (ws.empty())
            return;
        _app.cancelTimer(_probeTimer);
        _probeTimer = 0;
        openCdp(ws);
    });
}

void BrowserLogin::openCdp(const std::string &wsUrl) {
    _ws         = std::make_unique<net::WebSocket>(_app);
    _ws->onText = [this](std::string text) { onMessage(std::move(text)); };
    _ws->onOpen = [this] {
        setProgress(
            i18n::tr(
                "Sign in to Slack in the browser window \xE2\x80\x94 msga picks "
                "it up automatically."
            )
        );
        // Attach to our tab (and any the login flow opens) so the desktop-app
        // handoff can be intercepted before it loads. Later targets start
        // paused until interception is on.
        send(
            "Target.setAutoAttach",
            R"({"autoAttach":true,"waitForDebuggerOnStart":true,"flatten":true})",
            {},
            {}
        );
        send("Target.getTargets", {}, {}, [this](const json::Value &r, bool ok) {
            if (!ok || _done)
                return;
            for (const json::Value t : r["targetInfos"])
                if (t["type"].str() == "page")
                    attachPage(std::string(t["targetId"].str()));
        });
        _pollTimer = _app.addTimer(kPollIntervalMs, true, [this] { poll(); });
        poll();
    };
    _ws->onClosed = [this](int code, std::string) {
        if (!_done && code == 0) // only fatal before the session is up
            finish({}, {}, "cdp_failed");
    };
    _ws->open(wsUrl);
}

void BrowserLogin::poll() {
    if (_done)
        return;
    const double now = double(base::monotonicMs());
    if (now - _startMs > kOverallTimeoutMs) {
        finish({}, {}, "timeout");
        return;
    }
    send("Storage.getCookies", {}, {}, [this](const json::Value &r, bool ok) {
        if (ok)
            collectCookies(r);
    });
    if (_cookie.empty())
        return;
    discoverTeams();
    // The web client never booted (stopped on "open the desktop app"):
    // settle for the hosts seen, or none.
    if (now - _cookieAtMs > kTeamGraceMs)
        finish(_cookie, hostTeams(), {});
}

void BrowserLogin::send(
    std::string_view method, std::string params, std::string_view session, Handler h
) {
    if (!_ws || !_ws->isOpen())
        return;
    const int   id  = _nextId++;
    std::string msg = str::concat({"{\"id\":", str::number(id), ",\"method\":", quoted(method)});
    if (!params.empty())
        msg += str::concat({",\"params\":", params});
    if (!session.empty())
        msg += str::concat({",\"sessionId\":", quoted(session)});
    msg += '}';
    if (h)
        _pending.emplace(id, std::move(h));
    _ws->sendText(msg);
}

void BrowserLogin::onMessage(std::string text) {
    json::Document doc;
    if (!doc.parse(std::move(text), nullptr))
        return;
    const json::Value o = doc.root();
    if (o.has("id")) {
        auto it = _pending.find(int(o["id"].integer()));
        if (it == _pending.end())
            return;
        Handler h = std::move(it->second);
        _pending.erase(it);
        h(o["result"], !o.has("error"));
        return;
    }
    if (_done)
        return;
    const std::string_view method = o["method"].str();
    const json::Value      params = o["params"];
    if (method == "Target.attachedToTarget") {
        const std::string child(params["sessionId"].str());
        const json::Value info = params["targetInfo"];
        if (info["type"].str() == "page") {
            _pageSessions[std::string(info["targetId"].str())] = child;
            interceptHandoff(child);
        }
        // Auto-attached targets start paused (waitForDebuggerOnStart): let
        // them run, or the sign-in page hangs on a blank screen.
        send("Runtime.runIfWaitingForDebugger", {}, child, {});
    } else if (method == "Fetch.requestPaused") {
        handleHandoff(
            std::string(params["request"]["url"].str()),
            std::string(params["requestId"].str()),
            std::string(o["sessionId"].str())
        );
    }
}

void BrowserLogin::attachPage(const std::string &targetId) {
    if (targetId.empty() || _pageSessions.count(targetId))
        return;
    _pageSessions[targetId] = {}; // claimed; the reply fills the session in
    send(
        "Target.attachToTarget",
        str::concat({"{\"targetId\":", quoted(targetId), ",\"flatten\":true}"}),
        {},
        [this, targetId](const json::Value &r, bool ok) {
            if (_done)
                return;
            if (!ok) {
                _pageSessions.erase(targetId); // a later tick retries
                return;
            }
            const std::string sess(r["sessionId"].str());
            _pageSessions[targetId] = sess;
            interceptHandoff(sess);
        }
    );
}

void BrowserLogin::interceptHandoff(const std::string &session) {
    if (session.empty())
        return;
    // Catch the top-level navigation to the desktop-app handoff
    // (…/ssb/redirect) before it loads: it would pop "Open Slack?" over our
    // window and can hand the user to another app mid sign-in.
    send(
        "Fetch.enable",
        R"({"patterns":[{"urlPattern":"*/ssb/*","resourceType":"Document","requestStage":"Request"}]})",
        session,
        {}
    );
}

void BrowserLogin::handleHandoff(
    const std::string &url, const std::string &requestId, const std::string &session
) {
    if (requestId.empty())
        return;
    _hosts.push_back(url);
    // A placeholder instead of the handoff page: no slack:// link ever runs.
    const std::string kPage = str::concat(
        {"<!doctype html><meta charset=utf-8><body style=\"font:16px system-ui;text-align:center;"
         "margin-top:20vh;color:#444\">",
         i18n::tr("Signed in \xE2\x80\x94 finishing up in msga. You can close this."),
         "</body>"}
    );
    send(
        "Fetch.fulfillRequest",
        str::concat(
            {"{\"requestId\":",
             quoted(requestId),
             ",\"responseCode\":200,\"responseHeaders\":[{\"name\":\"Content-Type\",\"value\":"
             "\"text/html; charset=utf-8\"}],\"body\":",
             quoted(crypto::base64(kPage)),
             "}"}
        ),
        session,
        {}
    );
    // Reaching this page means the sign-in completed: read the cookie now.
    send("Storage.getCookies", {}, {}, [this](const json::Value &r, bool ok) {
        if (_done)
            return;
        if (ok)
            collectCookies(r);
        if (!_cookie.empty())
            finish(_cookie, hostTeams(), {});
    });
}

void BrowserLogin::collectCookies(const json::Value &result) {
    for (const json::Value c : result["cookies"]) {
        const std::string_view domain = c["domain"].str();
        if (!str::endsWith(domain, "slack.com"))
            continue;
        _hosts.emplace_back(domain);
        if (c["name"].str() != "d")
            continue;
        // Verbatim, still percent-encoded as Slack stores it.
        const std::string_view value = c["value"].str();
        if (!str::startsWith(value, "xoxd-") || !_cookie.empty())
            continue;
        _cookie     = std::string(value);
        _cookieAtMs = double(base::monotonicMs());
        setProgress(i18n::tr("Signed in \xE2\x80\x94 looking up your workspaces\xE2\x80\xA6"));
    }
    // Bounded: a long sign-in polls the same domains every second.
    std::sort(_hosts.begin(), _hosts.end());
    _hosts.erase(std::unique(_hosts.begin(), _hosts.end()), _hosts.end());
}

void BrowserLogin::discoverTeams() {
    if (_evaluating)
        return;
    if (!_webSession.empty()) {
        readLocalConfig(_webSession);
        return;
    }
    send("Target.getTargets", {}, {}, [this](const json::Value &r, bool ok) {
        if (!ok || _done)
            return;
        std::string webClient; // an app.slack.com page, once the client booted
        bool        handoff = false;
        for (const json::Value t : r["targetInfos"]) {
            const std::string_view url = t["url"].str();
            if (url.find("slack.com") != std::string_view::npos)
                _hosts.emplace_back(url);
            if (t["type"].str() != "page")
                continue;
            if (str::startsWith(url, "https://app.slack.com")) {
                if (webClient.empty())
                    webClient = std::string(t["targetId"].str());
            } else if (url.find(".slack.com/ssb/") != std::string_view::npos) {
                handoff = true;
            }
        }
        if (!webClient.empty()) {
            // Attached to every page already (openCdp): reuse that session and
            // re-read it until the client has written localConfig_v2.
            auto it = _pageSessions.find(webClient);
            if (it == _pageSessions.end() || it->second.empty()) {
                attachPage(webClient); // read it next tick
                return;
            }
            _webSession = it->second;
            readLocalConfig(_webSession);
            return;
        }
        // The desktop-app handoff page never boots the web client; its URL
        // carries the workspace host, which is all the deriver needs.
        if (handoff)
            finish(_cookie, hostTeams(), {});
    });
}

void BrowserLogin::readLocalConfig(const std::string &session) {
    if (session.empty() || _evaluating)
        return;
    _evaluating = true;
    send(
        "Runtime.evaluate",
        R"js({"expression":"window.localStorage.getItem('localConfig_v2')","returnByValue":true})js",
        session,
        [this](const json::Value &r, bool ok) {
            _evaluating = false;
            if (_done)
                return;
            if (!ok) {
                _webSession.clear(); // the page navigated away: re-attach next tick
                return;
            }
            std::vector<TeamSession> teams = parseLocalConfig(r["result"]["value"].str());
            if (!teams.empty())
                finish(_cookie, std::move(teams), {});
        }
    );
}

void BrowserLogin::finish(std::string cookie, std::vector<TeamSession> teams, std::string error) {
    if (_done)
        return;
    _done = true;
    shutdown();
    Done d = std::move(_onDone);
    if (d)
        d(std::move(cookie), std::move(teams), std::move(error)); // may delete this
}

void BrowserLogin::shutdown() {
    for (uint64_t *t : {&_probeTimer, &_pollTimer, &_exitTimer})
        if (*t) {
            _app.cancelTimer(*t);
            *t = 0;
        }
    if (_probeReq) {
        _client.cancel(_probeReq);
        _probeReq = 0;
    }
    _pending.clear();
    if (_ws) {
        // Quit the browser over CDP (signalling the wrapper we launched
        // doesn't reach it). The socket lives on a moment so the frame
        // leaves; its callbacks no longer reach us.
        _ws->onText   = nullptr;
        _ws->onOpen   = nullptr;
        _ws->onClosed = nullptr;
        if (_ws->isOpen())
            _ws->sendText(
                str::concat({"{\"id\":", str::number(_nextId++), ",\"method\":\"Browser.close\"}"})
            );
        std::shared_ptr<net::WebSocket> ws(std::move(_ws));
        _app.addTimer(1000, false, [ws] {});
    }
    if (!_proc || _profile.empty() || _wipeScheduled)
        return;
    _wipeScheduled         = true;
    const std::string path = _profile;
    if (!_proc->running()) {
        wipeLater(_app, path, 0);
        return;
    }
    // Wipe once the browser is gone, and unconditionally a few seconds later:
    // a throwaway profile must never be left holding a session cookie.
    // Signals are only for a browser CDP never reached: give it room first,
    // killing it mid-flush is what leaves recreated files behind.
    auto       proc  = _proc;
    plat::App *app   = &_app;
    auto       watch = std::make_shared<uint64_t>(0);
    auto       ticks = std::make_shared<int>(0);
    *watch           = _app.addTimer(250, true, [app, proc, path, watch, ticks] {
        if (!proc->running()) {
            app->cancelTimer(*watch);
            wipeLater(*app, path, 1500);
            return;
        }
        ++*ticks;
        if (*ticks == 8) // 2 s
            proc->kill(false);
        else if (*ticks == 24) // 6 s
            proc->kill(true);
        else if (*ticks > 40)
            app->cancelTimer(*watch);
    });
    wipeLater(_app, path, 8000);
}

} // namespace slack
