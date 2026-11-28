// "Sign in with <browser>" for Slack session auth:
// launches a Chromium-family browser on a throwaway profile at Slack's
// sign-in page, then reads the `d` cookie — and, when the web client boots,
// the signed-in workspaces with their xoxc- tokens — over the DevTools
// protocol (CDP). The temp profile is the sandbox: the user's real browser
// profile is never touched, and the profile is wiped when the flow ends.
//
// Non-obvious, all learned the hard way:
// - a fixed DevTools port (not --remote-debugging-port=0): snap/flatpak
//   browsers have a private /tmp, so the DevToolsActivePort file is
//   invisible, but they share the host network;
// - Browser.close over CDP is the only reliable way to close it: the
//   /usr/bin wrappers don't forward SIGTERM (signals go to the group as the
//   fallback);
// - Slack's post-login /ssb/redirect page launches the desktop app: the
//   profile refuses slack:// and the page is answered with our own HTML
//   (Fetch interception), which is also the completion signal;
// - Chromium rewrites profile files while shutting down: the wipe is retried.
// Firefox speaks WebDriver BiDi, not CDP — not supported.
#pragma once

#include "app/slack/session.h"
#include "base/json.h"
#include "base/process.h"
#include "net/net.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace plat {
class App;
}

namespace slack {

// The browser that would be driven ("Google Chrome", "Brave" …); "" when
// none is installed, or MSGA_BROWSER_LOGIN=0 turns the path off.
std::string browserLoginName();

class BrowserLogin {
public:
    // done(cookie, teams, error): `cookie` is the `d` value (xoxd-…) and
    // `teams` the discovered workspaces (maybe none: then ask for the
    // address). On failure cookie is "" and error one of no_browser,
    // launch_failed, no_devtools, cdp_failed, cancelled, timeout.
    using Done =
        std::function<void(std::string cookie, std::vector<TeamSession> teams, std::string error)>;

    BrowserLogin(plat::App &app, net::Client &client);
    ~BrowserLogin(); // aborts: closes the browser, wipes the profile; no callback

    // done runs exactly once (later, on the UI thread).
    void start(std::function<void(std::string)> progress, Done done);

    // ── Pure helpers (tests) ────────────────────────────────────────────────
    static std::string parseDebuggerUrl(std::string_view jsonVersion);

private:
    using Handler = std::function<void(const json::Value &result, bool ok)>;

    void probe();
    void openCdp(const std::string &wsUrl);
    void poll();
    void send(std::string_view method, std::string params, std::string_view session, Handler h);
    void onMessage(std::string text);
    void attachPage(const std::string &targetId);
    void interceptHandoff(const std::string &session);
    void
    handleHandoff(const std::string &url, const std::string &requestId, const std::string &session);
    void collectCookies(const json::Value &result);
    void discoverTeams();
    void readLocalConfig(const std::string &session);
    void finish(std::string cookie, std::vector<TeamSession> teams, std::string error);
    void shutdown();
    std::vector<TeamSession> hostTeams() const;
    void                     setProgress(std::string s);

    plat::App                      &_app;
    net::Client                    &_client;
    std::unique_ptr<net::WebSocket> _ws;
    std::shared_ptr<base::Process>  _proc; // shared with the post-close wipe timers
    std::string                     _profile;
    std::string                     _browserName;
    int                             _port       = 0;
    uint64_t                        _probeTimer = 0, _pollTimer = 0, _exitTimer = 0;
    net::RequestId                  _probeReq = 0;
    double                          _startMs = 0, _cookieAtMs = 0;
    bool                            _done = false, _evaluating = false, _wipeScheduled = false;
    std::string                     _cookie;
    std::vector<std::string>        _hosts;      // *.slack.com URLs/domains seen (fallback)
    std::string                     _webSession; // flat CDP session of the app.slack.com page
    std::unordered_map<std::string, std::string> _pageSessions; // target id → session
    std::unordered_map<int, Handler>             _pending;      // CDP id → continuation
    int                                          _nextId = 1;
    std::function<void(std::string)>             _progress;
    Done                                         _onDone;
};

} // namespace slack
