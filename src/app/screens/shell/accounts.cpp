#include "screens/shell/accounts.h"

#include "app/cache/workspace_cache.h"
#include "app/identity.h"
#include "app/model/jobs.h"
#include "app/claude/backend.h"
#include "app/claude/cli.h"
#include "app/claude/common.h"
#include "app/claude/links.h"
#include "app/claude/roles.h"
#include "app/slack/oauth.h"
#include "app/slack/session.h"
#include "app/slack/slack_backend.h"
#include "app/slack/socket_mode.h"
#include "app/slack/web_api.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/log.h"
#include "base/i18n.h"
#include "base/str.h"
#include "net/net.h"
#include "screens/common/message_text.h"
#include "screens/common/remote_images.h"
#include "screens/shell/settings.h"
#include "screens/shell/shell.h"
#include "screens/shell/shell_dialogs.h"
#include "screens/shell/signin_dialogs.h"

#include <cstdlib>

namespace shell {

using i18n::tr;

namespace {

constexpr int kRetryMs   = 15000; // offline at start: try again
// The background workspaces start one at a time after the open one, spaced
// wide enough that each one's first sync (users, conversations, unreads) is
// not a burst on top of the others'.
constexpr int kStaggerMs = 1500;

enum : int { kAddSlack = 1, kAddClaudeCode };

// The server's workspace icon, downloaded once: named by its URL, so a new
// icon is a new file (and never a stale decoded copy).
std::string iconCachePath(plat::App &app, const std::string &url) {
    const std::string dir = identity::cacheDir(app);
    if (dir.empty() || url.empty())
        return {};
    const auto digest = crypto::sha256(url);
    return file::join(
        dir,
        str::concat({"workspace-icons/", crypto::hex(crypto::bytes(digest)).substr(0, 24), ".img"})
    );
}

// Destroyed on the next loop turn: these are often dropped from inside one
// of their own callbacks.
template <class T>
void releaseLater(plat::App &app, std::unique_ptr<T> &p) {
    if (!p)
        return;
    std::shared_ptr<T> dying(std::move(p));
    app.post([dying] {});
}

} // namespace

struct Accounts::Running {
    uint64_t                        serial = 0;
    std::string                     key;
    model::Store                    store; // before backend: it outlives it
    std::unique_ptr<model::Backend> backend;
    slack::SlackBackend            *slack      = nullptr; // backend, a Slack workspace
    claude::Backend                *claude     = nullptr; // backend, a Claude Code workspace
    plat::TimerId                   retryTimer = 0;
    net::RequestId                  iconReq    = 0;
    model::ConvRef                  lastOpen   = model::kNoConv; // shown when it was left
    bool                            live       = false;          // connected once
};

Accounts::Accounts(
    screens::Context     &ctx,
    Shell                &shell,
    ui::Window           &win,
    Settings             &settings,
    std::function<void()> saveSettings,
    model::BackendProxy  &proxy,
    model::Backend       &none,
    net::Client          &client,
    std::string           storePath
)
    : _ctx(ctx), _shell(shell), _win(win), _settings(settings),
      _saveSettings(std::move(saveSettings)), _proxy(proxy), _client(client),
      _store(std::move(storePath)), _none(none), _blank(ctx.store()) {
    imageAuth();
    // The links live in the Claude Code workspace's files (links.json) and
    // fetch the askers' files into its cache.
    plat::App        &pa    = ctx.app.platform();
    const std::string data  = identity::dataDir(pa);
    const std::string cache = identity::cacheDir(pa);
    _links                  = std::make_unique<claude::Links>(
        pa,
        data.empty() ? std::string() : file::join(data, "claude-code/links.json"),
        cache.empty() ? std::string() : file::join(cache, "claude-code")
    );
    _links->plainText = [](const model::Store &st, std::string_view text) {
        return screens::plainText(st, text, true);
    };
    _shell.setAgentLinks(_links.get());
}

Accounts::~Accounts() {
    plat::App &pa = _ctx.app.platform();
    // The screens let go first (their observers move to the blank Store):
    // as on every switch, nothing may point into the old Store when the slot
    // wakes them, or the open chat rebuilds from refs the blank one lacks.
    if (_ctx.remote)
        _ctx.remote->setAuth({}); // it captured a backend
    if (_active)
        _shell.leaveWorkspace(); // drafts stashed, the open chat and thread closed
    _proxy.setTarget(_none);
    _ctx.store.setTarget(_blank);
    _active = nullptr;
    _shell.setAgentLinks(nullptr);
    for (const auto &r : _running)
        shutdown(*r, true);  // quitting: what changed in the last second too
    model::stopBackground(); // the work still under way reports to nobody
    if (_staggerTimer)
        pa.cancelTimer(_staggerTimer);
    if (_migration && _migration->req)
        _client.cancel(_migration->req);
    _running.clear();
    _links.reset();
}

void Accounts::start() {
    if (_store.active().empty())
        showSignedOut();
    else
        activate(_store.active());
    // The rest still connect — badges and notifications must not
    // depend on clicking each one — one at a time after the open one.
    for (const auth::WorkspaceRecord &r : _store.all())
        if (!r.hasKey(_activeKey))
            _pending.push_back(r.key());
    startNext();
}

void Accounts::startNext() {
    if (_staggerTimer || _pending.empty())
        return;
    _staggerTimer = _ctx.app.platform().addTimer(kStaggerMs, false, [this] {
        _staggerTimer = 0;
        // Gone, or opened by hand meanwhile: ensure() is a no-op then.
        while (!_pending.empty()) {
            const std::string key = _pending.front();
            _pending.erase(_pending.begin());
            if (!find(key) && ensure(key))
                break;
        }
        startNext();
    });
}

void Accounts::promptAdd(ui::PointF anchor) {
    // Slack, and Claude Code (a single-workspace service, greyed out once
    // added). Teams and IMAP are not part of this app.
    std::vector<ui::MenuItem> items;
    items.push_back({kAddSlack, "Slack"});
    ui::MenuItem cc{kAddClaudeCode, "Claude Code"};
    for (const auto &r : _store.all())
        cc.enabled = cc.enabled && r.service != claude::kService; // one per machine
    items.push_back(cc);
    ui::Menu::popupAt(_win, anchor, std::move(items), [this](int id) {
        // After the menu has gone, before a browser opens.
        _ctx.app.platform().post([this, id] {
            if (id == kAddSlack)
                connectSlack();
            else if (id == kAddClaudeCode)
                connectClaudeCode();
        });
    });
}

void Accounts::connectClaudeCode() {
    // Accounts lives as long as the app: the answer always finds it.
    claude::checkSetup(_ctx.app.platform(), [this](claude::Credentials creds, std::string error) {
        if (!error.empty()) {
            showMessage(_win, tr("Login failed"), error);
            return;
        }
        adoptRecord(claude::toRecord(creds));
    });
}

void Accounts::connectSlack() {
    // A session is the default way in; the dialog's link falls back to OAuth.
    sessionDialog(true);
}

void Accounts::sessionDialog(bool appKeysLink) {
    SessionImportHooks h;
    h.imported = [this](std::vector<slack::Credentials> c) { addSessionWorkspaces(std::move(c)); };
    if (appKeysLink)
        h.useAppKeys = [this] { loginWithAppKeys(); };
    showSessionImportDialog(_ctx, _win, _client, std::move(h));
}

void Accounts::addSessionWorkspaces(std::vector<slack::Credentials> creds) {
    if (creds.empty())
        return;
    // Connecting with a session puts the app in session mode (the mode
    // follows how you connect).
    if (!_settings.slackSession) {
        _settings.slackSession = true;
        if (_saveSettings)
            _saveSettings();
    }
    std::vector<std::string> added;
    for (const auto &c : creds) {
        auth::WorkspaceRecord rec = slack::toRecord(c);
        added.push_back(rec.key());
        _store.save(std::move(rec));
    }
    const std::string first = added.front();
    _store.setActive(first);

    // The account's `d` cookie rotates on every browser sign-in, which stales
    // the other session workspaces' token + cookie. Re-mint theirs from the
    // stored URL with the new cookie (a workspace stored without a URL has to
    // be imported again once).
    const std::string               cookie = creds.front().cookie;
    std::vector<slack::TeamSession> stale;
    for (const auto &r : _store.all()) {
        if (r.service != slack::kService)
            continue;
        bool justAdded = false;
        for (const auto &k : added)
            justAdded = justAdded || r.hasKey(k);
        const slack::Credentials c = slack::fromRecord(r);
        if (justAdded || c.cookie.empty() || c.workspaceUrl.empty() || c.cookie == cookie)
            continue;
        slack::TeamSession t;
        t.workspaceUrl = c.workspaceUrl;
        t.teamId       = c.teamId;
        t.teamName     = c.teamName;
        t.iconUrl      = c.iconUrl;
        stale.push_back(std::move(t));
    }
    // Signed in again: a running one starts over on its new credentials.
    for (const auto &k : added)
        restart(k);
    if (stale.empty()) {
        activate(first);
        return;
    }
    _remint = std::make_unique<slack::TokenDeriver>(_client);
    _remint->run(
        cookie,
        std::move(stale),
        [this, first](std::vector<slack::Credentials> valid, std::string) {
            for (const auto &c : valid) {
                auth::WorkspaceRecord rec = slack::toRecord(c);
                const std::string     key = rec.key();
                _store.save(std::move(rec));
                restart(key);
            }
            releaseLater(_ctx.app.platform(), _remint);
            activate(first);
        }
    );
}

void Accounts::loginWithAppKeys() {
    const slack::AppConfig cfg = slack::appConfig(
        _settings.slackClientId, _settings.slackClientSecret, _settings.slackAppToken
    );
    releaseLater(_ctx.app.platform(), _oauth);
    _oauth = std::make_unique<slack::OAuthFlow>(_ctx.app.platform(), _client, cfg);
    _oauth->start([this](slack::Credentials c, std::string error) {
        releaseLater(_ctx.app.platform(), _oauth);
        if (!error.empty()) {
            // A user's cancel is not an error.
            if (error != "cancelled")
                showMessage(_win, tr("Login failed"), error);
            return;
        }
        // OAuth sign-in ⇒ app-keys mode (live push), the mode following how
        // you connect.
        if (_settings.slackSession) {
            _settings.slackSession = false;
            if (_saveSettings)
                _saveSettings();
        }
        adoptRecord(slack::toRecord(c));
    });
}

void Accounts::adoptRecord(auth::WorkspaceRecord rec) {
    const std::string key = rec.key();
    _store.save(std::move(rec));
    _store.setActive(key);
    restart(key);
    activate(key);
}

void Accounts::networkChanged(bool online) {
    if (auto sock = _socketMode.lock())
        sock->networkChanged(online);
}

bool Accounts::handleUrl(std::string_view url) {
    return _oauth && _oauth->handleCallback(url);
}

namespace {

// The Claude Code workspace's own files: <dataDir>/claude-code (the team,
// known sessions, your profile) and <cacheDir>/claude-code — the folders
// earlier versions used, whose files keep their format (app/identity.h).
void setClaudeDirs(plat::App &app) {
    const std::string data  = identity::dataDir(app);
    const std::string cache = identity::cacheDir(app);
    claude::Dirs      d;
    d.data  = data.empty() ? std::string() : file::join(data, "claude-code");
    d.cache = cache.empty() ? std::string() : file::join(cache, "claude-code");
    claude::setDirs(std::move(d));
}

} // namespace

Accounts::Running *Accounts::find(const std::string &key) const {
    for (const auto &r : _running)
        if (r->key == key)
            return r.get();
    return nullptr;
}

Accounts::Running *Accounts::bySerial(uint64_t serial) const {
    for (const auto &r : _running)
        if (r->serial == serial)
            return r.get();
    return nullptr;
}

// The workspace's backend over a Store of its own,
// watched by the shell from now on; what its cache kept shows before the
// network answers (badges before the first poll), then connect merges.
Accounts::Running *Accounts::ensure(const std::string &key) {
    if (Running *r = find(key))
        return r;
    const auth::WorkspaceRecord *rec = _store.find(key);
    if (!rec || (rec->service != slack::kService && rec->service != claude::kService))
        return nullptr;
    plat::App &pa          = _ctx.app.platform();
    auto       r           = std::make_unique<Running>();
    r->serial              = ++_serial;
    r->key                 = key;
    model::Store &st       = r->store;
    // What the record knows shows at once; connect() fills in the rest.
    st.workspaceId         = rec->id;
    st.workspaceName       = rec->displayName;
    st.workspaceMuted      = rec->muted;
    const uint64_t serial  = r->serial;
    // The error banner shows for the active workspace only.
    auto           onError = [this, serial](const std::string &message) {
        if (Running *x = bySerial(serial); x && x == _active)
            _shell.showError(message);
    };
    if (rec->service == claude::kService) {
        setClaudeDirs(pa);
        auto backend      = std::make_unique<claude::Backend>(st, pa, claude::fromRecord(*rec));
        backend->onError  = onError;
        // A message it never sent: back in its composer, whichever workspace is shown.
        backend->onUnsent = [this, serial](
                                model::ConvRef                  conv,
                                model::Ts                       thread,
                                const std::string              &text,
                                const std::vector<std::string> &files
                            ) {
            if (Running *x = bySerial(serial))
                _shell.giveBackUnsent(x->key, x->store, conv, thread, text, files);
        };
        backend->onCommandsChanged = [this, serial] {
            if (Running *x = bySerial(serial))
                _shell.commandsChanged(x->key);
        };
        backend->setZenMode(_settings.zenMode(key));
        backend->setWindowVisible(_shell.windowVisible());
        r->claude  = backend.get();
        r->backend = std::move(backend);
        _links->setAgents(&st, r->claude);
    } else {
        const std::string cached = iconCachePath(pa, rec->iconUrl);
        st.workspaceIcon         = !cached.empty() && file::exists(cached) ? cached : std::string();
        auto backend =
            std::make_unique<slack::SlackBackend>(st, pa, _client, slack::fromRecord(*rec));
        // Dead credentials found by any later call: the same as at connect().
        backend->onAuthLost = [this, serial](const std::string &error) { authLost(serial, error); };
        // A refresh rotated the token: this workspace's record keeps the new one.
        backend->onCredentialsChanged = [this, key](const slack::Credentials &c) {
            if (const auth::WorkspaceRecord *x = _store.find(key)) {
                auth::WorkspaceRecord rec = *x;
                rec.auth                  = slack::toRecord(c).auth;
                _store.save(std::move(rec));
            }
        };
        backend->setAppConfig(
            slack::appConfig(
                _settings.slackClientId, _settings.slackClientSecret, _settings.slackAppToken
            )
        );
        // Like the error banner, it shows for the active workspace only.
        backend->onParallelUsage = [this, serial] {
            if (Running *x = bySerial(serial); x && x == _active)
                _shell.showParallelUsage();
        };
        backend->onError       = onError;
        backend->onReminderDue = [this, serial](model::ConvRef conv, model::Ts ts) {
            if (Running *x = bySerial(serial))
                _shell.notifyReminderDue(x->key, x->store, conv, ts);
        };
        // Every OAuth workspace on these app keys shares the one socket.
        if (!backend->credentials().sessionAuth())
            backend->setRealtime(socketMode());
        backend->setPresenceMode(model::Backend::PresenceMode(_settings.presence));
        backend->setNamesMode(model::Backend::NamesMode(_settings.names));
        backend->setWindowVisible(_shell.windowVisible()); // started while hidden: polls slowly
        r->slack   = backend.get();
        r->backend = std::move(backend);
        _links->attach(st, *r->slack);
    }
    Running *raw = r.get();
    _running.push_back(std::move(r));
    imageAuth(); // a workspace signing in is the usual cure for a failed picture
    _shell.attachWorkspace(key, raw->store, *raw->backend);
    if (raw->slack)
        raw->slack->openCache(cache::WorkspaceCache::dirFor(pa, key));
    connect(serial);
    return raw;
}

// The app's one Socket Mode socket, shared by every
// workspace on these app keys (Slack spreads events over all of an app's
// sockets) and closed with the last of them. None in session mode — the
// hard off switch that keeps a build's app keys off the shared pool. The
// token: Settings, then SLACK_XAPP_TOKEN (dev), then the build's.
std::shared_ptr<slack::SocketMode> Accounts::socketMode() {
    if (_settings.slackSession)
        return nullptr;
    if (auto live = _socketMode.lock())
        return live;
    std::string xapp(str::trim(_settings.slackAppToken));
    if (xapp.empty())
        if (const char *env = std::getenv("SLACK_XAPP_TOKEN"))
            xapp = std::string(str::trim(env));
    if (xapp.empty())
        xapp = slack::appConfig({}, {}, {}).appToken;
    LOG_INFO(
        "accounts",
        "Socket Mode: xapp token prefix = %s",
        xapp.empty() ? "(empty)" : (xapp.substr(0, 12) + "\xE2\x80\xA6").c_str()
    );
    if (xapp.empty())
        return nullptr;
    auto sock   = std::make_shared<slack::SocketMode>(_ctx.app.platform(), _client, xapp);
    _socketMode = sock;
    return sock;
}

void Accounts::shutdown(Running &r, bool keepCache) {
    _shell.detachWorkspace(r.key);
    if (r.retryTimer)
        _ctx.app.platform().cancelTimer(r.retryTimer);
    if (r.iconReq)
        _client.cancel(r.iconReq);
    if (r.slack)
        r.slack->closeCache(keepCache);
    if (r.claude)
        r.claude->close(); // nothing more goes into the Store it is about to lose
    if (r.claude)
        _links->setAgents(nullptr, nullptr);
    else
        _links->detach(r.store);
}

void Accounts::drop(Running *r, bool keepCache) {
    if (!r)
        return;
    if (r == _active)
        showSignedOut(); // the screens let go of it first
    shutdown(*r, keepCache);
    plat::App &pa = _ctx.app.platform();
    for (auto it = _running.begin(); it != _running.end(); ++it)
        if (it->get() == r) {
            std::unique_ptr<Running> dying = std::move(*it);
            _running.erase(it);
            releaseLater(pa, dying); // often from inside one of its own callbacks
            return;
        }
}

void Accounts::restart(const std::string &key) {
    Running *r = find(key);
    if (!r)
        return;
    const bool open = r == _active;
    drop(r, true);
    if (open)
        activate(key);
    else
        ensure(key);
}

// The open workspace leaves the screens (it keeps
// running in the background), `key`'s takes its place — started first if
// it wasn't — and its last chat opens again.
void Accounts::activate(const std::string &key) {
    Running *r = ensure(key);
    if (!r) {
        // A workspace that can't be opened never leaves the window without
        // one: stay in the open one, else the signed-out page.
        if (!_active)
            showSignedOut();
        return;
    }
    _store.setActive(key);
    if (r == _active) {
        refreshRail();
        return;
    }
    if (_active) {
        _active->lastOpen = _shell.current();
        _active->backend->setActiveConversation(model::kNoConv, 0); // no foreground poll
        _shell.leaveWorkspace(); // drafts stashed, nothing points into its Store
    }
    // The proxy first: the views the slot wakes ask the backend what it can do.
    _proxy.setTarget(*r->backend);
    // Before the slot wakes the views: their downloads go out with r's
    // credentials, and failures from before it was on screen are retried.
    _active = r;
    imageAuth();
    _ctx.store.setTarget(r->store);
    _activeKey = key;
    refreshRail();
    _shell.setSignedIn(true);
    _shell.setLive(r->live);
    _shell.workspaceChanged();
    restoreLast(*r);
}

bool Accounts::restoreLast(Running &r) {
    model::ConvRef last = r.lastOpen;
    if (last == model::kNoConv)
        last = r.slack ? r.slack->lastConversation() : r.claude->lastConversation();
    if (last >= r.store.conversationCount() || !r.store.conversation(last).member)
        return false;
    _shell.open(last);
    return true;
}

void Accounts::connect(uint64_t serial) {
    Running *r = bySerial(serial);
    if (!r)
        return;
    r->backend->connect([this, serial](bool ok, const std::string &error) {
        Running *r = bySerial(serial);
        if (!r)
            return;
        if (ok) {
            // Keep the record's name and icon in step with the server's.
            if (const auth::WorkspaceRecord *rec = _store.find(r->key)) {
                const std::string &icon = r->store.workspaceIcon;
                if (rec->displayName != r->store.workspaceName ||
                    (screens::RemoteImages::isRemote(icon) && rec->iconUrl != icon)) {
                    auth::WorkspaceRecord x = *rec;
                    x.displayName           = r->store.workspaceName;
                    if (screens::RemoteImages::isRemote(icon))
                        x.iconUrl = icon;
                    _store.save(std::move(x));
                    refreshRail();
                }
            }
            fetchIcon(*r);
            // Its links watched (again), turns a restart cut short taken up.
            if (r->claude)
                _links->agentsReady();
            else
                _links->ready(r->store);
            const bool first = !r->live;
            r->live          = true;
            _shell.setWorkspaceLive(r->key, true);
            if (r == _active) {
                _shell.workspaceChanged();
                _shell.setLive(true);
                // A backend that lists its chats only once connected (Claude
                // Code's first scan runs on a worker) had nothing to restore
                // when the workspace opened; nothing open yet means do it now.
                if (first && _shell.current() == model::kNoConv)
                    restoreLast(*r);
            }
            return;
        }
        if (slack::SlackBackend::authError(error)) {
            authLost(serial, error);
            return;
        }
        // Offline or Slack unreachable: keep the workspace, try again.
        LOG_INFO("accounts", "%s: connect failed (%s), retrying", r->key.c_str(), error.c_str());
        r->retryTimer = _ctx.app.platform().addTimer(kRetryMs, false, [this, serial] {
            if (Running *x = bySerial(serial)) {
                x->retryTimer = 0;
                connect(serial);
            }
        });
    });
}

// The credentials are dead (signed out by the server): the record
// stays (signing in again replaces it), the workspace stops; the open one
// shows the signed-out page, the others keep running.
void Accounts::authLost(uint64_t serial, const std::string &error) {
    Running *r = bySerial(serial);
    if (!r)
        return;
    LOG_WARN("accounts", "%s: signed out (%s)", r->key.c_str(), error.c_str());
    _shell.notifySessionExpired(r->store.workspaceName);
    drop(r, true);
}

void Accounts::fetchIcon(Running &r) {
    plat::App        &pa  = _ctx.app.platform();
    const std::string url = r.store.workspaceIcon;
    if (!screens::RemoteImages::isRemote(url))
        return;
    const std::string path = iconCachePath(pa, url);
    if (path.empty())
        return;
    if (file::exists(path)) {
        r.store.workspaceIcon = path;
        if (&r == _active)
            _shell.workspaceChanged();
        return;
    }
    net::Request req;
    req.url = url;
    if (r.iconReq)
        _client.cancel(r.iconReq);
    const uint64_t serial = r.serial;
    r.iconReq = _client.send(std::move(req), [this, serial, url, path](net::Response resp) {
        Running *x = bySerial(serial);
        if (!x)
            return;
        x->iconReq = 0;
        if (!resp.ok() || resp.body.empty() || !file::writeAtomic(path, resp.body))
            return;
        if (x->store.workspaceIcon == url) {
            x->store.workspaceIcon = path;
            refreshRail(); // its tile, open or not
            if (x == _active)
                _shell.workspaceChanged();
        }
    });
}

// Downloads of Slack's files carry their workspace's token (and d cookie),
// per workspace: the team in a file URL's path
// picks the running workspace, the one on screen answers for the rest of
// Slack's hosts — background workspaces' notification pictures and files
// shown across workspaces included.
void Accounts::imageAuth() {
    if (!_ctx.remote)
        return;
    _ctx.remote->setAuth([this](const std::string &url, std::vector<net::Header> &h) {
        std::vector<slack::TeamAuth> signedIn;
        for (const auto &r : _running)
            if (r->slack)
                signedIn.push_back({r->slack->credentials().teamId, &r->slack->auth()});
        const slack::SlackBackend *open = _active ? _active->slack : nullptr;
        if (const slack::Auth *a =
                slack::downloadAuth(url, signedIn, open ? &open->auth() : nullptr))
            slack::addAuthHeaders(h, *a);
    });
}

// The screens show no workspace: the rail stays,
// nothing selected; the background workspaces keep running.
void Accounts::showSignedOut() {
    if (_active) {
        _active->lastOpen = _shell.current();
        _active->backend->setActiveConversation(model::kNoConv, 0);
    }
    _shell.leaveWorkspace();
    _proxy.setTarget(_none);
    _ctx.store.setTarget(_blank);
    _active = nullptr;
    _activeKey.clear();
    refreshRail();
    _shell.setSignedIn(false);
}

void Accounts::importSession() {
    sessionDialog(false);
}

int Accounts::oauthSlackWorkspaces() const {
    int n = 0;
    for (const auto &r : _store.all())
        n += r.service == slack::kService && slack::fromRecord(r).cookie.empty();
    return n;
}

void Accounts::convertToSession() {
    if (_migration)
        return;
    // The `d` cookie is per account: any session workspace's will do.
    auto m = std::make_unique<Migration>();
    for (const auto &r : _store.all()) {
        if (r.service != slack::kService)
            continue;
        slack::Credentials c = slack::fromRecord(r);
        if (!c.cookie.empty()) {
            if (m->cookie.empty())
                m->cookie = c.cookie;
        } else {
            m->items.push_back(std::move(c));
        }
    }
    if (m->cookie.empty()) {
        showMessage(
            _win,
            tr("Convert to session"),
            tr("Add one workspace with your Slack session first \xE2\x80\x94 its cookie is "
               "reused for the rest.")
        );
        return;
    }
    if (m->items.empty()) {
        showMessage(
            _win, tr("Convert to session"), tr("All Slack workspaces already use your session.")
        );
        return;
    }
    _migration = std::move(m);
    migrateNext();
}

void Accounts::migrateNext() {
    Migration &m = *_migration;
    if (m.index >= m.items.size()) {
        if (m.resolved.empty()) {
            migrateDone({}, m.lastError.empty() ? "no_workspaces" : m.lastError);
            return;
        }
        _remint = std::make_unique<slack::TokenDeriver>(_client);
        _remint->run(
            m.cookie,
            std::move(m.resolved),
            [this](std::vector<slack::Credentials> valid, std::string error) {
                releaseLater(_ctx.app.platform(), _remint);
                migrateDone(std::move(valid), error);
            }
        );
        return;
    }
    const slack::Credentials item = m.items[m.index];
    // team.info with the workspace's current token gives its domain.
    m.req                         = slack::apiCall(
        _client,
        {item.token, {}},
        "team.info",
        {},
        [this, item](const json::Document &doc, const std::string &err) {
            Migration &mm = *_migration;
            mm.req        = 0;
            if (err.empty()) {
                const std::string_view domain = doc.root()["team"]["domain"].str();
                if (!domain.empty()) {
                    slack::TeamSession t;
                    t.workspaceUrl = str::concat({"https://", domain, ".slack.com"});
                    t.teamId       = item.teamId;
                    t.teamName     = item.teamName;
                    t.iconUrl      = item.iconUrl;
                    mm.resolved.push_back(std::move(t));
                } else {
                    mm.lastError = "no_domain";
                }
            } else {
                mm.lastError = err;
            }
            ++mm.index;
            migrateNext();
        }
    );
}

void Accounts::migrateDone(std::vector<slack::Credentials> converted, const std::string &error) {
    _migration.reset();
    if (converted.empty()) {
        showMessage(
            _win,
            tr("Convert to session"),
            i18n::arg(tr("Couldn't convert your workspaces: %1"), error)
        );
        return;
    }
    for (const auto &c : converted)
        _store.save(slack::toRecord(c));
    if (!_settings.slackSession) {
        _settings.slackSession = true;
        if (_saveSettings)
            _saveSettings();
    }
    // No restart: each running Slack workspace starts over in session mode
    // on its new credentials.
    std::vector<std::string> keys;
    for (const auto &r : _running)
        if (r->slack)
            keys.push_back(r->key);
    for (const std::string &k : keys)
        restart(k);
}

void Accounts::refreshRail() {
    plat::App                    &pa = _ctx.app.platform();
    std::vector<Shell::Workspace> list;
    for (const auth::WorkspaceRecord &r : _store.all()) {
        Shell::Workspace w;
        w.key   = r.key();
        w.id    = r.id;
        w.name  = r.displayName;
        w.muted = r.muted;
        if (r.service == claude::kService) {
            setClaudeDirs(pa); // the picture is one of its files
            w.icon = claude::agentAvatarPath();
        } else if (
            const std::string cached = iconCachePath(pa, r.iconUrl);
            !cached.empty() && file::exists(cached)
        ) {
            w.icon = cached; // downloaded while it was open
        }
        list.push_back(std::move(w));
    }
    _shell.setWorkspaces(std::move(list), _activeKey);
}

void Accounts::switchTo(const std::string &key) {
    if (key != _activeKey && _store.find(key))
        activate(key);
}

void Accounts::reorder(const std::vector<std::string> &keys) {
    _store.setOrder(keys);
    refreshRail();
}

void Accounts::setMuted(const std::string &key, bool muted) {
    const std::string k = key.empty() ? _activeKey : key;
    if (!_store.find(k))
        return;
    _store.setMuted(k, muted);
    if (Running *r = find(k))
        r->store.workspaceMuted = muted;
    refreshRail();
}

// The workspace stops, its record, custom icon,
// cache and drafts go; the open one hands the screens to the next.
void Accounts::signOut(const std::string &key) {
    const std::string            k   = key.empty() ? _activeKey : key;
    const auth::WorkspaceRecord *rec = k.empty() ? nullptr : _store.find(k);
    if (!rec)
        return;
    const bool open = k == _activeKey;
    removeCustomWorkspaceIcon(_ctx.app.platform(), rec->id);
    drop(find(k), false);
    cache::WorkspaceCache::remove(_ctx.app.platform(), k); // its drafts go with it
    _shell.purgeHistory(k);
    _shell.forgetCollapsed(k);
    _store.remove(k);
    std::erase(_pending, k);
    if (open && !_store.active().empty())
        activate(_store.active());
    else
        refreshRail();
}

void Accounts::signOut() {
    signOut(std::string());
}

} // namespace shell
