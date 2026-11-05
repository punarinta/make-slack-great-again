#include "app/slack/credentials.h"

#include "app/slack/slack_json.h"

#include "base/json.h"
#include "base/str.h"

#include <cstdlib>

// MSGA_CLIENT_ID / MSGA_CLIENT_SECRET / MSGA_XAPP come from credentials.cmake
// (see the root CMakeLists.txt); empty when the build has none.
#ifndef MSGA_CLIENT_ID
#define MSGA_CLIENT_ID ""
#endif
#ifndef MSGA_CLIENT_SECRET
#define MSGA_CLIENT_SECRET ""
#endif
#ifndef MSGA_XAPP
#define MSGA_XAPP ""
#endif

namespace slack {

auth::WorkspaceRecord toRecord(const Credentials &c) {
    json::Writer w;
    w.beginObject();
    w.key("xoxp").value(c.token);
    w.key("refreshToken").value(c.refreshToken);
    // A string, as stored blobs have it (doubles lose precision elsewhere).
    w.key("expiresAt").value(str::number(c.expiresAt));
    if (!c.cookie.empty())
        w.key("cookie").value(c.cookie);
    if (!c.workspaceUrl.empty())
        w.key("workspaceUrl").value(c.workspaceUrl);
    if (!c.enterpriseId.empty())
        w.key("enterpriseId").value(c.enterpriseId);
    if (!c.orgToken.empty())
        w.key("orgToken").value(c.orgToken);
    if (!c.orgUrl.empty())
        w.key("orgUrl").value(c.orgUrl);
    w.endObject();
    auth::WorkspaceRecord r;
    r.service     = kService;
    r.id          = c.teamId;
    r.displayName = c.teamName;
    r.iconUrl     = c.iconUrl;
    r.auth        = w.take();
    return r;
}

Credentials fromRecord(const auth::WorkspaceRecord &r) {
    Credentials    c;
    json::Document doc;
    doc.parse(r.auth, nullptr);
    const json::Value o = doc.root();
    c.token             = std::string(o["xoxp"].str());
    c.teamId            = r.id;
    c.teamName          = r.displayName;
    c.iconUrl           = r.iconUrl;
    c.refreshToken      = std::string(o["refreshToken"].str());
    const json::Value e = o["expiresAt"];
    c.expiresAt         = mapjson::epochSecs(e);
    c.cookie            = std::string(o["cookie"].str());
    c.workspaceUrl      = std::string(o["workspaceUrl"].str());
    c.enterpriseId      = std::string(o["enterpriseId"].str());
    c.orgToken          = std::string(o["orgToken"].str());
    c.orgUrl            = std::string(o["orgUrl"].str());
    return c;
}

AppConfig appConfig(
    std::string_view personalId, std::string_view personalSecret, std::string_view personalXapp
) {
    AppConfig cfg{MSGA_CLIENT_ID, MSGA_CLIENT_SECRET, MSGA_XAPP};
    if (!personalId.empty())
        cfg.clientId = std::string(personalId);
    if (!personalSecret.empty())
        cfg.clientSecret = std::string(personalSecret);
    if (!personalXapp.empty())
        cfg.appToken = std::string(personalXapp);
    return cfg;
}

} // namespace slack
