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

    // Credentials for org-scope methods
    std::string enterpriseId; // Enterprise id may differ from teamId if part of enterprise grid
    std::string orgToken; // Only populated if the workspace is an actual workspace in an enterprise grid
    std::string orgUrl; // Only populated if the workspace is an actual workspace in an enterprise grid

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
