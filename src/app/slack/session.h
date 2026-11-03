// Slack session-token (xoxc + `d` cookie) sign-in, minus the UI: the shared
// types, the token deriver, and the pure parsers the browser sign-in uses.
//
// The `d` cookie is HttpOnly — no page script can read it — so it comes from
// a browser we drive (browser_login.h), the local Slack desktop app, or a
// hand paste. The xoxc token is then *derived*: GET the workspace's boot
// page with the cookie and scrape "api_token":"xoxc-…", then validate it
// with auth.test.
#pragma once

#include "app/slack/credentials.h"
#include "net/net.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace slack {

// A workspace found with a session. `token` may be empty (derive it from
// `workspaceUrl`); team fields are best effort until auth.test/team.info.
struct TeamSession {
    std::string token;        // xoxc-, or "" to derive
    std::string workspaceUrl; // "https://myteam.slack.com"
    std::string teamId, teamName, iconUrl;
};

// Validates each candidate (deriving its token first when needed) and
// collects the ones that work. done(valid, lastError): lastError explains
// an empty `valid` — "invalid_auth", "token_not_found", "no_workspace_url",
// "network", or another auth.test error.
class TokenDeriver {
public:
    using Done = std::function<void(std::vector<Credentials> valid, std::string lastError)>;
    explicit TokenDeriver(net::Client &client) : _client(client) {}
    ~TokenDeriver(); // cancels; done never runs afterwards
    TokenDeriver(const TokenDeriver &)            = delete;
    TokenDeriver &operator=(const TokenDeriver &) = delete;

    void run(std::string cookie, std::vector<TeamSession> candidates, Done done);

private:
    void next();
    void derive(const TeamSession &cand);
    void validate(const TeamSession &cand, std::string token);
    void fetchIcon(Credentials c);
    void fail(std::string why);

    net::Client             &_client;
    std::string              _cookie;
    std::vector<TeamSession> _queue;
    size_t                   _index = 0;
    std::vector<Credentials> _valid;
    std::string              _lastError;
    Done                     _done;
    net::RequestId           _pending = 0;
};

// ── Pure helpers (tests) ────────────────────────────────────────────────────


struct BootTokens {
    std::string token;
    std::string enterpriseToken;
};

// The xoxc- token in a boot page ("api_token":"xoxc-…", else any xoxc- run).
BootTokens               scrapeBootTokens(std::string_view html);
// "myteam", "myteam.slack.com" or a URL → "https://myteam.slack.com".
std::string              normalizeWorkspaceUrl(std::string_view v);
// A pasted cookie value: trimmed, a leading "d=" dropped.
std::string              normalizeCookie(std::string_view v);
// The web client's localStorage `localConfig_v2` → workspaces with tokens.
std::vector<TeamSession> parseLocalConfig(std::string_view json);
// Workspace hosts in arbitrary URLs / cookie domains, Slack's own
// infrastructure subdomains skipped. Tokens left empty (to derive).
std::vector<TeamSession> teamsFromHosts(const std::vector<std::string> &values);

} // namespace slack
