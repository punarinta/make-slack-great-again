#include "app/slack/session.h"

#include "app/model/jobs.h"
#include "app/slack/local_import.h"

#include "app/slack/web_api.h"
#include "base/json.h"
#include "base/str.h"

#include <memory>
#include <ranges>
#include <algorithm>

namespace slack {

namespace {

constexpr int kTimeoutMs = 20000;

bool tokenChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// Slack's own hosts, never a user's workspace.
bool infraSubdomain(std::string_view sub) {
    static const char *const kInfra[] = {
        "app",
        "api",
        "a",
        "edgeapi",
        "files",
        "downloads",
        "www",
        "my",
        "status",
        "slack",
        "join",
        "signin",
    };
    for (const char *i : kInfra)
        if (sub == i)
            return true;
    return false;
}

std::string iconOf(const json::Value &icon) {
    std::string_view u = icon["image_88"].str();
    if (u.empty())
        u = icon["image_68"].str();
    return std::string(u);
}

// Links orgs to workspaces if they are in the same 'grid' and discard the org
void linkOrgTokens(std::vector<Credentials> &creds) {
    const auto isOrg = [](const Credentials &c) {
        return !c.enterpriseId.empty() && !c.token.empty() &&
               (c.teamId == c.enterpriseId || str::startsWith(c.teamId, "E"));
    };
    constexpr std::string_view kMark = "___";
    // Creds is assumed to be a small collection
    // so quadratic complexity is fine
    for (auto &org : creds) {
        if (!isOrg(org))
            continue;
        bool linked = false;
        for (auto &ws : creds) {
            if (&ws != &org && !isOrg(ws) && ws.enterpriseId == org.enterpriseId) {
                if (ws.orgToken.empty()) {
                    ws.orgToken = org.token;
                }

                if (ws.orgUrl.empty()) {
                    ws.orgUrl = org.workspaceUrl;
                }

                linked = true;
            }
        }
            
        if (linked) {
            org.enterpriseId = kMark; // used: drop below
        }
    }

    auto const marked = std::ranges::remove(creds, kMark, &Credentials::enterpriseId);
    creds.erase(marked.begin(), marked.end());
}

} // namespace

// ── Pure helpers ────────────────────────────────────────────────────────────

BootTokens scrapeBootTokens(std::string_view html) {
    static constexpr std::string_view kApi = "\"api_token\":\"";
    static constexpr std::string_view kEnt = "\"enterprise_api_token\":\"";
    constexpr auto tokenViewAt             = [](std::string_view s, size_t at) -> std::string_view {
        size_t end = at + 5; // past "xoxc-"
        while (end < s.size() && tokenChar(s[end]))
            ++end;
        return end > at + 5 ? s.substr(at, end - at) : std::string_view{};
    };

    BootTokens       out;
    std::string_view firstAny;
    for (size_t at = html.find("xoxc-"); at != std::string_view::npos;
         at        = html.find("xoxc-", at + 5)) {
        const std::string_view t = tokenViewAt(html, at);
        if (t.empty())
            continue;
        const std::string_view before = html.substr(0, at);
        if (str::endsWith(before, kEnt)) {
            if (out.enterpriseToken.empty())
                out.enterpriseToken = t;
        } else if (str::endsWith(before, kApi)) {
            if (out.token.empty())
                out.token = t;
        } else if (firstAny.empty()) {
            firstAny = t;
        }
        if (!out.token.empty() && !out.enterpriseToken.empty())
            break;
    }
    if (out.token.empty())
        out.token = firstAny;
    return out;
}

std::string normalizeWorkspaceUrl(std::string_view v) {
    v = str::trim(v);
    if (v.empty())
        return {};
    if (str::startsWith(v, "https://"))
        v.remove_prefix(8);
    else if (str::startsWith(v, "http://"))
        v.remove_prefix(7);
    std::string host(v.substr(0, v.find('/')));
    if (host.empty())
        return {};
    if (host.find('.') == std::string::npos)
        host += ".slack.com";
    return "https://" + host;
}

std::string normalizeCookie(std::string_view v) {
    v = str::trim(v);
    if (str::startsWith(v, "d="))
        v = str::trim(v.substr(2));
    return std::string(v);
}

std::vector<TeamSession> parseLocalConfig(std::string_view text) {
    std::vector<TeamSession> out;
    json::Document           doc;
    if (!doc.parse(text, nullptr))
        return out;
    for (const json::Value t : doc.root()["teams"]) {
        TeamSession s;
        s.teamId   = std::string(t["id"].str(t.key()));
        s.teamName = std::string(t["name"].str());
        s.token    = std::string(t["token"].str());
        if (!str::startsWith(s.token, "xoxc-"))
            s.token.clear(); // the deriver scrapes a real one
        std::string url(t["url"].str());
        if (url.empty() && !t["domain"].str().empty())
            url = str::concat({"https://", t["domain"].str(), ".slack.com"});
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        s.workspaceUrl = std::move(url);
        s.iconUrl      = iconOf(t["icon"]);
        if (s.token.empty() && s.workspaceUrl.empty())
            continue; // nothing to validate, nothing to derive from
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<TeamSession> teamsFromHosts(const std::vector<std::string> &values) {
    static constexpr std::string_view kSuffix = ".slack.com";
    std::vector<TeamSession>          out;
    std::vector<std::string>          seen;
    for (const std::string &raw : values) {
        const std::string v = str::asciiLower(raw);
        for (size_t at = v.find(kSuffix); at != std::string::npos; at = v.find(kSuffix, at + 1)) {
            // The label before ".slack.com": [a-z0-9][a-z0-9-]*
            size_t b = at;
            while (b > 0) {
                const char c = v[b - 1];
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
                    --b;
                else
                    break;
            }
            while (b < at && v[b] == '-')
                ++b;
            if (b == at)
                continue;
            const std::string sub = v.substr(b, at - b);
            if (infraSubdomain(sub))
                continue;
            bool dup = false;
            for (const auto &s : seen)
                dup = dup || s == sub;
            if (dup)
                continue;
            seen.push_back(sub);
            TeamSession t;
            t.workspaceUrl = str::concat({"https://", sub, kSuffix});
            out.push_back(std::move(t));
        }
    }
    return out;
}

// ── TokenDeriver ────────────────────────────────────────────────────────────

TokenDeriver::~TokenDeriver() {
    if (_pending)
        _client.cancel(_pending);
}

void TokenDeriver::run(std::string cookie, std::vector<TeamSession> candidates, Done done) {
    if (_pending)
        _client.cancel(_pending);
    _pending = 0;
    _cookie  = std::move(cookie);
    _queue   = std::move(candidates);
    _index   = 0;
    _valid.clear();
    _lastError.clear();
    _done = std::move(done);
    next();
}

void TokenDeriver::next() {
    if (_index >= _queue.size()) {
        _pending = 0;
        linkOrgTokens(_valid);
        Done d = std::move(_done);
        if (d)
            d(std::move(_valid), _valid.empty() ? _lastError : std::string());
        return;
    }
    const TeamSession &cand = _queue[_index];
    if (!cand.token.empty())
        validate(cand, cand.token);
    else
        derive(cand);
}

void TokenDeriver::fail(std::string why) {
    _lastError = std::move(why);
    ++_index;
    next();
}

void TokenDeriver::derive(const TeamSession &cand) {
    if (cand.workspaceUrl.empty()) {
        fail("no_workspace_url");
        return;
    }
    net::Request req;
    req.url       = cand.workspaceUrl;
    req.timeoutMs = kTimeoutMs;
    // The cookie rides every hop of / → /messages → /ssb/redirect (net
    // carries a caller's Cookie header along a redirect chain).
    req.headers.push_back({"Cookie", "d=" + _cookie});
    _pending = _client.send(std::move(req), [this, cand](net::Response r) {
        // Never gate on the status: Slack serves the logged-in boot page of a
        // workspace root with 403, token and all. A logged-out page has no
        // xoxc- run ("api_token":null), so a stale cookie still fails.
        if (r.body.empty()) {
            fail("network");
            return;
        }
        auto [token, enterpriseToken] = scrapeBootTokens(r.body);
        if (token.empty()) {
            fail("token_not_found");
            return;
        }
        validate(cand, std::move(token));
        if (!enterpriseToken.empty() &&
            std::ranges::find(_queue, enterpriseToken, &TeamSession::token) == _queue.end()) {
            _queue.emplace_back(
                TeamSession{
                    .token        = std::move(enterpriseToken),
                    .workspaceUrl = cand.workspaceUrl,
                }
            );
        }
    });
}

void TokenDeriver::validate(const TeamSession &cand, std::string token) {
    Auth a{token, _cookie};
    _pending = apiCall(
        _client,
        a,
        "auth.test",
        {},
        [this, cand, token](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                fail(err);
                return;
            }
            const json::Value o = doc.root();
            Credentials       c;
            c.token        = token;
            c.cookie       = _cookie;
            c.teamId       = std::string(o["team_id"].str(cand.teamId));
            c.teamName     = std::string(o["team"].str(cand.teamName));
            c.iconUrl      = cand.iconUrl;
            // auth.test's url is authoritative; the input is the fallback.
            c.workspaceUrl = std::string(o["url"].str(cand.workspaceUrl));
            c.enterpriseId = std::string(o["enterprise_id"].str());
            if (c.teamId.empty()) {
                fail("auth_failed");
                return;
            }
            fetchIcon(std::move(c));
        }
    );
}

void TokenDeriver::fetchIcon(Credentials c) {
    if (!c.iconUrl.empty()) {
        _valid.push_back(std::move(c));
        ++_index;
        next();
        return;
    }
    Auth a{c.token, c.cookie};
    _pending = apiCall(
        _client,
        a,
        "team.info",
        {},
        [this, c](const json::Document &doc, const std::string &err) mutable {
            // Best effort: the workspace counts either way.
            if (err.empty()) {
                const json::Value team = doc.root()["team"];
                c.iconUrl              = iconOf(team["icon"]);
                if (c.teamName.empty())
                    c.teamName = std::string(team["name"].str());
            }
            _valid.push_back(std::move(c));
            ++_index;
            next();
        }
    );
}

void importLocalSessionAsync(plat::App &app, std::function<void(LocalImport)> done) {
    auto r = std::make_shared<LocalImport>();
    model::runInBackground(
        app,
        [r] { *r = importLocalSession(); },
        [r, done = std::move(done)] {
            if (done)
                done(std::move(*r));
        }
    );
}

} // namespace slack
