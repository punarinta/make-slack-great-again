// Slack's per-workspace credentials and their place in the neutral
// WorkspaceStore record. The auth blob keeps the JSON shape stored
// workspaces already have: {xoxp, refreshToken, expiresAt (a string), cookie?,
// workspaceUrl?}.
#pragma once

#include "app/auth/workspaces.h"

#include <cstdint>
#include <string>

namespace slack {

inline constexpr const char *kService = "slack";

struct Credentials {
    std::string token;         // OAuth xoxp-, or a session xoxc- (stored as "xoxp")
    std::string teamId;        // the record id
    std::string teamName;      // the record's displayName
    std::string iconUrl;       // the record's iconUrl; may be empty
    std::string refreshToken;  // OAuth with token rotation only
    int64_t     expiresAt = 0; // epoch secs the token expires; 0 = unknown / never
    // Session auth: the `d` cookie (xoxd-…, verbatim as the browser keeps it).
    // Set ⇒ xoxc token + cookie, no Socket Mode, a token that never rotates.
    std::string cookie;
    // Session auth: "https://team.slack.com/", kept so the token can be
    // re-derived from a fresh cookie (the account cookie rotates on every
    // re-login and stales every session workspace at once).
    std::string workspaceUrl;
    // Session auth on Enterprise Grid: auth.test's enterprise_id, and the
    // org-level (E…) xoxc token with its URL ("https://org.enterprise.slack.com/")
    // for the methods only the org token is served. Empty off Grid, or when the session had no org
    // entry.
    std::string enterpriseId, orgToken, orgUrl;

    bool sessionAuth() const { return !cookie.empty(); }
};

auth::WorkspaceRecord toRecord(const Credentials &c);
Credentials           fromRecord(const auth::WorkspaceRecord &r);

// The app registration (credentials.cmake at build time), with the user's
// own keys from Settings → System taking precedence field by field.
struct AppConfig {
    std::string clientId, clientSecret, appToken; // appToken: xapp- (Socket Mode)
};
AppConfig appConfig(
    std::string_view personalId, std::string_view personalSecret, std::string_view personalXapp
);

} // namespace slack
