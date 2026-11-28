// The upgrade from earlier versions' data (legacy.h), end to end on the Linux
// store: a settings file as earlier versions wrote it
// (support/old_settings_fixture.h) and cache files in the old cache format,
// in this test's own XDG_CONFIG_HOME / XDG_DATA_HOME, are imported into
// settings.json, workspaces.json and the new caches; the credentials are read
// where earlier versions keep them, and the old files are left byte for byte
// as they were.
#include "app/auth/workspaces.h"
#include "app/cache/workspace_cache.h"
#include "app/identity.h"
#include "app/legacy/legacy.h"
#include "app/legacy/old_cache_import.h"
#include "app/legacy/old_settings_import.h"
#include "app/model/store.h"
#include "app/slack/credentials.h"
#include "base/file.h"
#include "base/json.h"
#include "base/old_settings.h"
#include "base/str.h"
#include "support/test.h"
#include "support/old_settings_fixture.h"
#include "screens/shell/settings.h"
#include "ui/ui.h"

#include <memory>

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        return ui::App::create(&err);
    }();
    return *a;
}

std::string read(const std::string &path) {
    std::string s;
    file::readAll(path, &s);
    return s;
}

void removeTree(const std::string &dir) {
    std::vector<file::DirEntry> es;
    if (file::listDir(dir, &es))
        for (const auto &e : es) {
            const std::string p = file::join(dir, e.name);
            if (e.isDir)
                removeTree(p);
            else
                file::remove(p);
        }
    file::remove(dir);
}

// The old stores and the current files, gone.
void clean(plat::App &pa) {
    removeTree(file::join(identity::dataDir(pa), "cache"));
    removeTree(file::join(identity::dataDir(pa), "claude-code"));
    cache::WorkspaceCache::clearAll(pa);
    file::remove(file::join(identity::configDir(pa), "legacy-imported"));
    file::remove(oldsettings::iniPath());
    file::remove(oldsettings::iniPath("MSGA"));
    file::remove(file::join(identity::configDir(pa), "settings.json"));
    file::remove(file::join(identity::configDir(pa), "workspaces.json"));
    const std::string           icons = file::join(identity::dataDir(pa), "workspace_icons");
    std::vector<file::DirEntry> es;
    if (file::listDir(icons, &es))
        for (const auto &e : es)
            file::remove(file::join(icons, e.name));
}

} // namespace
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
TEST("old import: settings, workspaces and credentials from an earlier version") {
    plat::App &pa = app().platform();
    clean(pa);
    REQUIRE(file::writeAtomic(oldsettings::iniPath(), oldsettings_fixture::kIni));
    REQUIRE(
        file::writeAtomic(
            oldsettings::iniPath("MSGA"), "[composer]\nlastAttachDir=/home/u/Downloads\n"
        )
    );
    // The custom icons installed earlier in the data folder.
    const std::string data = identity::dataDir(pa);
    REQUIRE(file::writeAtomic(file::join(data, "tray_icon.png"), "PNG"));
    const std::string oldIcon = file::join(data, "workspace_icons/slack_T0123-1700000000000.png");
    REQUIRE(file::writeAtomic(oldIcon, "ICON"));
    REQUIRE(oldsettings::write("workspace/slack:T0123/customIcon", oldIcon));
    // The open one is not the first: the import must not fall back to it.
    REQUIRE(oldsettings::write("active", "slack:T0456"));
    const std::string before = read(oldsettings::iniPath());

    const std::string settingsPath = shell::Settings::defaultPath(pa);
    const std::string wsPath       = auth::WorkspaceStore::defaultPath(pa);
    legacy::importOldData(pa, settingsPath, wsPath);
    REQUIRE(file::exists(settingsPath));
    REQUIRE(file::exists(wsPath));

    // Workspaces: Slack + Claude Code in the old order, the open one, mute,
    // and the auth read from the old entries (never copied into the file).
    auth::WorkspaceStore ws(wsPath);
    REQUIRE(ws.all().size() == 3);
    CHECK_STR(ws.all()[0].key(), "slack:T0123");
    CHECK_STR(ws.all()[1].key(), "claude-code:local");
    CHECK_STR(ws.all()[2].key(), "slack:T0456");
    CHECK_STR(ws.active(), "slack:T0456");
    CHECK_STR(ws.all()[0].displayName, "Acme, Inc.");
    CHECK_STR(ws.all()[0].iconUrl, "https://a.slack-edge.com/x.png");
    CHECK(ws.all()[0].muted);
    CHECK_FALSE(ws.all()[2].muted);
    const slack::Credentials c = slack::fromRecord(ws.all()[0]);
    CHECK_STR(c.token, "xoxc-1-2");
    CHECK_STR(c.cookie, "xoxd-a%2Fb=c;d");
    CHECK(slack::fromRecord(ws.all()[2]).expiresAt == 1767225600);
    CHECK(read(wsPath).find("xoxc-1-2") == std::string::npos);
    // The custom icon under the new name in the old folder.
    std::vector<file::DirEntry> icons;
    REQUIRE(file::listDir(file::join(data, "workspace_icons"), &icons));
    int copied = 0;
    for (const auto &e : icons)
        copied += e.name.rfind("t0123-", 0) == 0 && str::endsWith(e.name, ".img");
    CHECK(copied == 1);

    // Settings.
    const shell::Settings s = shell::Settings::load(settingsPath);
    CHECK(s.theme == ui::ThemeMode::Dark);
    CHECK(s.paletteLight == int(ui::Palette::Blue));
    CHECK(s.paletteDark == int(ui::Palette::Custom));
    CHECK_STR(ui::hexColor(s.custom.primary), "#445566");
    CHECK_STR(ui::hexColor(s.custom.highlight1), "#112233");
    CHECK(s.custom.brightness == 4);
    CHECK_FALSE(s.custom.sidebarInverted);
    CHECK_FALSE(s.custom.gradient);
    CHECK(s.fontSize == 2);
    CHECK_STR(s.language, "ja");
    CHECK((s.use24hSaved && !s.use24h));
    CHECK(s.relevantDays == 30);
    CHECK(s.threadsInline);
    CHECK_FALSE(s.linkPreviews);
    CHECK(s.ctrlEnterSends);
    CHECK(s.spellCheck);
    REQUIRE(s.spellLanguages.size() == 2);
    CHECK_STR(s.spellLanguages[1], "de_DE");
    CHECK(s.notifications);
    CHECK(s.notifyLevel == 1); // defaultMigrated: the user's own choice
    CHECK_STR(s.soundId, "system:Glass");
    CHECK(s.presence == 1);
    CHECK_FALSE(s.slackSession);
    CHECK(s.cacheLimitMb == 500);
    CHECK_FALSE(s.autoUpdates);
    CHECK(s.lastUpdateCheck == 1767225600);
    CHECK(s.minimizeToTray);
    CHECK(s.threadWidth == 420);
    CHECK(s.customTrayIcon);
    CHECK_STR(s.trayIconPath, file::join(data, "tray_icon.png"));
    CHECK_FALSE(s.trayMonochrome);
    REQUIRE(s.visitedAt.size() == 1);
    CHECK_STR(s.visitedAt[0].first, "C1");
    CHECK(s.visitedAt[0].second == 1767225600);
    // The window geometry as it was saved.
    CHECK((s.width == 1300 && s.height == 820 && s.hasPosition && s.x == 100 && s.y == 100));
    CHECK_FALSE(s.maximized);
    // Composer.
    REQUIRE(s.emojiRecent.size() == 2);
    CHECK_STR(s.emojiRecent[1], "heart");
    CHECK(s.emojiSkinTone == 3);
    CHECK_STR(s.lastAttachDir, "/home/u/Downloads");
    // Claude Code folders.
    CHECK_STR(s.claudeLastDir, "/home/u/src");
    REQUIRE(s.claudeRecentDirs.size() == 2);
    CHECK_STR(s.claudeRecentDirs[1].path, "/home/u/x,y");
    CHECK(s.claudeRecentDirs[0].usedAt == 1767225000);
    REQUIRE(s.claudeTeammateDirs.size() == 1);
    CHECK_STR(s.claudeTeammateDirs[0].first, "architect");
    CHECK_STR(s.claudeTeammateDirs[0].second, "/home/u/src/a b");
    // AI: models, the custom server, the default, and the keys from the old
    // entries — not from settings.json.
    CHECK_STR(s.provider("anthropic")->model, "claude-x");
    CHECK_STR(s.provider("anthropic")->key, "sk-ant-1");
    const shell::AiProvider *custom = s.provider("custom-ab12cd34");
    REQUIRE(custom);
    CHECK_STR(custom->name, "Local");
    CHECK_STR(custom->url, "http://localhost:11434/v1");
    CHECK_STR(custom->sttModel, "whisper-1");
    CHECK_STR(custom->key, "k2");
    CHECK_STR(s.aiDefault, "custom-ab12cd34");
    CHECK_STR(s.aiLanguage, "sv");
    CHECK_STR(s.voiceGlossary, "msga\nKubernetes, k8s");
    CHECK_FALSE(s.voiceCleanup);
    // Slack app keys and GIPHY.
    CHECK_STR(s.slackClientId, "123.456");
    CHECK_STR(s.slackClientSecret, "sec\"ret\\x");
    CHECK_STR(s.slackAppToken, "xapp-1-A");
    CHECK_STR(s.giphyKey, "@giphy");
    const std::string json = read(settingsPath);
    for (const char *secret : {"sk-ant-1", "\"k2\"", "xapp-1-A", "@giphy", "sec\\\"ret"})
        CHECK(json.find(secret) == std::string::npos);

    // Saving again writes nothing into the old store: a rollback finds it
    // exactly as it was left.
    REQUIRE(s.save(settingsPath));
    CHECK(read(oldsettings::iniPath()) == before);

    // A second start imports nothing again.
    auth::WorkspaceStore(wsPath).setActive("slack:T0123");
    legacy::importOldData(pa, settingsPath, wsPath);
    CHECK_STR(auth::WorkspaceStore(wsPath).active(), "slack:T0123");
    clean(pa);
}

TEST("old import: changed credentials go to the old entries") {
    plat::App &pa = app().platform();
    clean(pa);
    REQUIRE(file::writeAtomic(oldsettings::iniPath(), oldsettings_fixture::kIni));
    const std::string settingsPath = shell::Settings::defaultPath(pa);
    const std::string wsPath       = auth::WorkspaceStore::defaultPath(pa);
    legacy::importOldData(pa, settingsPath, wsPath);

    // A token refresh: an earlier version (after a rollback) sees the new one.
    {
        auth::WorkspaceStore  ws(wsPath);
        auth::WorkspaceRecord r = *ws.find("slack:T0456");
        r.auth                  = R"({"xoxp":"xoxp-NEW","refreshToken":"xoxe-2","expiresAt":"1"})";
        ws.save(r);
    }
    CHECK_STR(
        oldsettings::get("workspace/slack:T0456/auth").text(),
        R"({"xoxp":"xoxp-NEW","refreshToken":"xoxe-2","expiresAt":"1"})"
    );
    // A new API key, a removed custom provider, a cleared GIPHY key.
    shell::Settings s         = shell::Settings::load(settingsPath);
    s.provider("openai")->key = "sk-oa";
    s.ai.erase(s.ai.begin() + 2); // custom-ab12cd34
    s.giphyKey.clear();
    REQUIRE(s.save(settingsPath));
    CHECK_STR(oldsettings::get("llm/openai/apiKey").text(), "sk-oa");
    CHECK(oldsettings::get("llm/custom-ab12cd34/apiKey").kind == oldsettings::Value::Kind::None);
    CHECK(oldsettings::get("gif/giphy/apiKey").kind == oldsettings::Value::Kind::None);
    CHECK_STR(oldsettings::get("llm/anthropic/apiKey").text(), "sk-ant-1");
    CHECK(read(settingsPath).find("sk-oa") == std::string::npos);
    const shell::Settings again = shell::Settings::load(settingsPath);
    CHECK_STR(again.provider("openai")->key, "sk-oa");
    // Signing out drops the credentials from the old entries too.
    auth::WorkspaceStore(wsPath).remove("slack:T0456");
    CHECK(oldsettings::get("workspace/slack:T0456/auth").kind == oldsettings::Value::Kind::None);
    clean(pa);
}

// Every kind of workspace record earlier versions stored, exactly as they
// wrote them (Slack and Claude Code records, the IMAP and Teams ones, the
// workspace order and mutes): two session workspaces (one an Enterprise Grid one
// with its own host), an OAuth one with a rotating token, Claude Code, and
// the services this app dropped in between.
constexpr char kEveryWorkspaceIni[] = R"INI([General]
active=slack:T0AAA
storeVersion=2
workspaces=slack:T0OAU, imap:me@example.com, slack:T0AAA, claude-code:local, slack:T0GRID, teams:tenant-1

[workspace]
claude-code%3Alocal\auth={\"claudePath\":\"/bin/false\"}
claude-code%3Alocal\displayName=Claude Code
claude-code%3Alocal\iconUrl=qrc:/claude_code_avatar.png
imap%3Ame%40example.com\auth="{\"host\":\"imap.example.com\",\"password\":\"imap-secret\"}"
imap%3Ame%40example.com\displayName=me@example.com
imap%3Ame%40example.com\iconUrl=
slack%3AT0AAA\auth="{\"cookie\":\"xoxd-COOKIE-acme\",\"expiresAt\":\"0\",\"refreshToken\":\"\",\"workspaceUrl\":\"https://acme.slack.com/\",\"xoxp\":\"xoxc-AAA-old\"}"
slack%3AT0AAA\displayName=Acme
slack%3AT0AAA\iconUrl=https://avatars.slack-edge.com/acme.png
slack%3AT0GRID\auth="{\"cookie\":\"xoxd-COOKIE-grid\",\"expiresAt\":\"0\",\"refreshToken\":\"\",\"workspaceUrl\":\"https://gridcorp-eng.enterprise.slack.com/\",\"xoxp\":\"xoxc-GRID-old\"}"
slack%3AT0GRID\displayName=Grid Corp
slack%3AT0GRID\iconUrl=https://avatars.slack-edge.com/grid.png
slack%3AT0GRID\muted=true
slack%3AT0OAU\auth="{\"expiresAt\":\"1790947553\",\"refreshToken\":\"xoxe-1-REFRESH-old\",\"xoxp\":\"xoxe.xoxp-OAUTH-old\"}"
slack%3AT0OAU\displayName=OAuth Team
slack%3AT0OAU\iconUrl=https://avatars.slack-edge.com/oauth.png
teams%3Atenant-1\auth={\"refreshToken\":\"teams-rt\"}
teams%3Atenant-1\displayName=Teams Org
teams%3Atenant-1\iconUrl=
)INI";

TEST("old import: every kind of old workspace comes back signed in") {
    plat::App &pa = app().platform();
    clean(pa);
    REQUIRE(file::writeAtomic(oldsettings::iniPath(), kEveryWorkspaceIni));
    const std::string settingsPath = shell::Settings::defaultPath(pa);
    const std::string wsPath       = auth::WorkspaceStore::defaultPath(pa);
    legacy::importOldData(pa, settingsPath, wsPath);

    // The old order without IMAP and Teams, the one that was open, the mute.
    auth::WorkspaceStore ws(wsPath);
    REQUIRE(ws.all().size() == 4);
    CHECK_STR(ws.all()[0].key(), "slack:T0OAU");
    CHECK_STR(ws.all()[1].key(), "slack:T0AAA");
    CHECK_STR(ws.all()[2].key(), "claude-code:local");
    CHECK_STR(ws.all()[3].key(), "slack:T0GRID");
    CHECK_STR(ws.active(), "slack:T0AAA");
    CHECK(ws.all()[3].muted);
    CHECK_STR(ws.all()[3].displayName, "Grid Corp");
    CHECK_STR(ws.all()[3].iconUrl, "https://avatars.slack-edge.com/grid.png");
    // Each with the credentials it had: the session's token, cookie and host
    // (the Grid workspace's own), the OAuth token with its refresh token.
    const slack::Credentials acme = slack::fromRecord(ws.all()[1]);
    CHECK_STR(acme.token, "xoxc-AAA-old");
    CHECK_STR(acme.cookie, "xoxd-COOKIE-acme");
    CHECK_STR(acme.workspaceUrl, "https://acme.slack.com/");
    const slack::Credentials grid = slack::fromRecord(ws.all()[3]);
    CHECK_STR(grid.token, "xoxc-GRID-old");
    CHECK_STR(grid.cookie, "xoxd-COOKIE-grid");
    CHECK_STR(grid.workspaceUrl, "https://gridcorp-eng.enterprise.slack.com/");
    const slack::Credentials oauth = slack::fromRecord(ws.all()[0]);
    CHECK_STR(oauth.token, "xoxe.xoxp-OAUTH-old");
    CHECK_STR(oauth.refreshToken, "xoxe-1-REFRESH-old");
    CHECK(oauth.expiresAt == 1790947553);
    CHECK(oauth.cookie.empty());
    CHECK_STR(ws.all()[2].auth, R"({"claudePath":"/bin/false"})");
    // Nothing secret in the new files; the old store as it was.
    CHECK(read(wsPath).find("xoxc-") == std::string::npos);
    CHECK(read(oldsettings::iniPath()) == kEveryWorkspaceIni);
    // Never chosen a connection mode, with an OAuth workspace: app keys, as
    // before.
    CHECK_FALSE(shell::Settings::load(settingsPath).slackSession);
    clean(pa);
}

TEST("old import: a fresh install has nothing to import") {
    plat::App &pa = app().platform();
    clean(pa);
    const std::string settingsPath = shell::Settings::defaultPath(pa);
    legacy::importOldData(pa, settingsPath, auth::WorkspaceStore::defaultPath(pa));
    CHECK_FALSE(file::exists(settingsPath));
    CHECK_FALSE(file::exists(oldsettings::iniPath()));
}
#endif

TEST("old import: the layouts before store version 2") {
    // v1: bare team ids, plain token fields.
    oldsettings::Map m = oldsettings::parseIni(
        "[General]\nworkspaces=T1, slack:T2\nactive=T1\nstoreVersion=1\n"
        "[workspace]\nT1\\xoxp=xoxp-1\nT1\\name=One\nT1\\refreshToken=r\nT1\\expiresAt=5\n"
        "slack%3AT2\\displayName=Two\n"
    );
    std::string active;
    auto        v1 = legacy::oldWorkspaces(m, &active);
    REQUIRE(v1.size() == 2);
    CHECK_STR(v1[0].key(), "slack:T1");
    CHECK_STR(v1[0].displayName, "One");
    const slack::Credentials c = slack::fromRecord(v1[0]);
    CHECK((c.token == "xoxp-1" && c.refreshToken == "r" && c.expiresAt == 5));
    CHECK_STR(v1[1].key(), "slack:T2");
    CHECK(v1[1].auth.empty()); // in the secret store
    CHECK_STR(active, "slack:T1");
    // v0: one workspace under auth/*.
    m       = oldsettings::parseIni("[auth]\nxoxp=xoxp-0\nteam_id=T9\nteam_name=Nine\n");
    auto v0 = legacy::oldWorkspaces(m, &active);
    REQUIRE(v0.size() == 1);
    CHECK_STR(v0[0].key(), "slack:T9");
    CHECK_STR(slack::fromRecord(v0[0]).token, "xoxp-0");
    CHECK_STR(active, "slack:T9");
    // Teams and IMAP workspaces stay behind.
    m = oldsettings::parseIni(
        "[General]\nworkspaces=teams:x, imap:y, slack:T1\nstoreVersion=2\n"
        "[workspace]\nteams%3Ax\\displayName=T\nimap%3Ay\\displayName=I\nslack%3AT1\\displayName="
        "S\n"
    );
    auto v2 = legacy::oldWorkspaces(m, &active);
    REQUIRE(v2.size() == 1);
    CHECK_STR(v2[0].key(), "slack:T1");
}

TEST("old import: theme and window edge cases") {
    // No appearance/mode: "charcoal" alone meant dark; a Slack sidebar string.
    shell::Settings s;
    legacy::importOldSettings(
        oldsettings::parseIni(
            "[appearance]\ntheme=charcoal\ncustomTheme=\"#111111,#222222,#333333,#444444,"
            "#555555,#666666,#777777,#888888\"\n[notifications]\nlevel=1\n"
        ),
        {},
        {},
        {},
        &s
    );
    CHECK(s.theme == ui::ThemeMode::Dark);
    CHECK_STR(ui::hexColor(s.custom.primary), "#111111");
    CHECK_STR(ui::hexColor(s.custom.highlight1), "#333333");
    CHECK_STR(ui::hexColor(s.custom.highlight2), "#777777");
    CHECK_STR(ui::hexColor(s.custom.important), "#888888");
    // The legacy string's pins: active_item_text, hover_item, text_color.
    CHECK_STR(ui::hexColor(s.custom.itemSelText), "#444444");
    CHECK_STR(ui::hexColor(s.custom.itemHover), "#555555");
    CHECK_STR(ui::hexColor(s.custom.itemText), "#666666");
    CHECK(s.custom.titleBarBg == 0); // 8 values: no top_nav_*
    CHECK(s.notifyLevel == 0);       // the old one-time migration to "All new posts"
    CHECK(s.slackSession);           // never chosen, no OAuth workspace
    CHECK(s.width == 1200);          // no geometry: the default
}

TEST("old import: the custom theme's JSON keeps its pins") {
    shell::Settings s;
    legacy::importOldSettings(
        oldsettings::parseIni(
            "[appearance]\ncustomTheme=\"{\\\"primary\\\":{\\\"hex\\\":\\\"#112233\\\"},"
            "\\\"brightness\\\":4,\\\"gradient\\\":false,\\\"pins\\\":{\\\"itemText\\\":"
            "\\\"#ABCDEF\\\",\\\"titleBarBg\\\":\\\"#010203\\\"}}\"\n"
        ),
        {},
        {},
        {},
        &s
    );
    CHECK_STR(ui::hexColor(s.custom.primary), "#112233");
    CHECK_STR(ui::hexColor(s.custom.highlight2), "#2bac76"); // not named: the default
    CHECK(s.custom.brightness == 4);
    CHECK(!s.custom.gradient);
    CHECK_STR(ui::hexColor(s.custom.itemText), "#abcdef");
    CHECK_STR(ui::hexColor(s.custom.titleBarBg), "#010203");
    CHECK(s.custom.itemHover == 0);
}

TEST("old import: zen mode per workspace") {
    // The INI file escapes the '%' of the old percent-encoded key once more.
    shell::Settings s;
    legacy::importOldSettings(
        oldsettings::parseIni(
            "[zenMode]\nslack%253AT0123=true\nclaude-code%253Alocal=true\nslack%253AT9=false\n"
        ),
        {},
        {},
        {},
        &s
    );
    REQUIRE(s.zenWorkspaces.size() == 2);
    CHECK_STR(s.zenWorkspaces[0], "claude-code:local");
    CHECK_STR(s.zenWorkspaces[1], "slack:T0123");
}

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

// The old cache files, as they were written (compact
// JSON; "\t" is a tab inside the strings).
constexpr char kOldConvs[] =
    R"([{"id":"D1","ki":2,"na":"","mb":true,"lr":"1767225000.000100","lt":"1767225600.000200","un":2,"dm":"U2","lm":true},)"
    R"({"id":"C1","ki":0,"na":"general","mb":true,"lr":"1767225000.000100","un":0,"st":true,"nl":2},)"
    R"({"id":"C2","ki":1,"na":"secret","mb":true,"lr":"","un":0,"nl":3},)"
    R"({"id":"G1","ki":3,"na":"mpdm-me--bob--carol-1","mb":true,"lr":"","un":0,"ln":"Team"},)"
    R"({"id":"C3","ki":0,"na":"random","mb":true,"lr":"","un":0,"mu":true}])";
constexpr char kOldUsers[] =
    R"([{"id":"U1","na":"me","dn":"Me Self","av":"https://a/1.png","bo":false,"ex":false,"ac":true,)"
    R"("de":false,"ad":true,"ow":false,"se":"","st":"","ti":"Dev","em":"me@x.se","tz":3600},)"
    R"({"id":"U2","na":"bob","dn":"Bob","av":"","bo":false,"ex":true,"ac":false,"de":false,)"
    R"("ad":false,"ow":false,"se":"palm_tree","st":"Away","ti":""}])";
constexpr char kOldBots[] =
    R"([{"id":"B1","na":"deploybot","dn":"Deploy","av":"https://a/b.png","bo":true,"ex":false,)"
    R"("ac":false,"de":false,"ad":false,"ow":false,"se":"","st":"","ti":""}])";
constexpr char kOldMeta[] =
    R"({"conv":"C1","name":"general","meId":"U1","sweepAt":1767220000,)"
    R"("mutedThreads":["C1\t1767225000.000100"],"followedThreads":["C1\t1767224000.000300"],)"
    R"("aiTranscripts":{"F1":{"text":"hello there","by":"OpenAI"}},)"
    R"("reminders":[{"conv":"C1","ts":"1767225000.000100","due":1767300000,"saved":1767225100,)"
    R"("snippet":"remember this","author":"U2","root":"1767224000.000300"},)"
    R"({"conv":"C3","ts":"1767225000.000500","due":0,"saved":1767225200,"fired":true}],)"
    R"("reminderPreviews":[{"key":"C3\t1767225000.000500","snippet":"bot said","botName":"Deploy",)"
    R"("botAvatar":"https://a/b.png"}],"deadConvIds":["C9"],"userProbeTimes":{"U2":1767225600000}})";

void put(const std::string &dir, const char *name, std::string_view text) {
    REQUIRE(file::writeAtomic(file::join(dir, name), text));
}

void oldSlackCache(const std::string &dir) {
    put(dir, "conversations.json", kOldConvs);
    put(dir, "users.json", kOldUsers);
    put(dir, "bots.json", kOldBots);
    put(dir, "emoji.json", R"({"party":"https://e/party.gif","yay":"alias:party"})");
    put(dir, "usergroups.json", R"([{"id":"S1","ha":"devs","na":"Developers","us":["U1","U2"]}])");
    put(dir, "meta.json", kOldMeta);
    put(dir, "messages/C1.json", R"([{"ts":"1767225000.000100","au":"U2","tx":{"x":"hi"}}])");
}

} // namespace

TEST("old cache: a first start brings the whole old cache along") {
    plat::App &pa = app().platform();
    clean(pa);
    REQUIRE(file::writeAtomic(oldsettings::iniPath(), oldsettings_fixture::kIni));
    const std::string data = identity::dataDir(pa);
    oldSlackCache(file::join(data, "cache/slack_T0123"));
    // Before multi-service: the bare team id.
    put(file::join(data, "cache/T0456"),
        "conversations.json",
        R"([{"id":"C7","ki":0,"na":"x","mb":true,"lr":"","un":0,"lm":true}])");
    // Claude Code: the sessions' star, mute and level.
    put(file::join(data, "cache/claude-code_local"),
        "conversations.json",
        R"([{"id":"s1","ki":2,"na":"A","mb":true,"lr":"","un":0,"st":true,"lm":true,"nl":2},)"
        R"({"id":"s2","ki":2,"na":"B","mb":true,"lr":"","un":0,"lm":true}])");
    const std::string known = file::join(data, "claude-code/known-sessions.json");
    put(file::join(data, "claude-code"),
        "known-sessions.json",
        R"({"sessions":[{"id":"s1","sessionId":"s1","name":"A"},)"
        R"({"id":"s2","sessionId":"s2","muted":true}],"started":["s1"],"open":"s1"})");
    const std::string oldMeta = read(file::join(data, "cache/slack_T0123/meta.json"));

    const std::string settingsPath = shell::Settings::defaultPath(pa);
    const std::string wsPath       = auth::WorkspaceStore::defaultPath(pa);
    legacy::importOldData(pa, settingsPath, wsPath);

    // Slack: the roster, users, emoji, groups and the app's own state.
    const std::string dir = cache::WorkspaceCache::dirFor(pa, "slack:T0123");
    model::Store      s;
    json::Document    meta;
    {
        cache::WorkspaceCache wc(pa, s, dir);
        REQUIRE(wc.load(&meta));
        CHECK_STR(wc.lastConversation(), "C1");
        wc.close(false);
    }
    const model::ConvRef d1 = s.findConversation("D1"), c1 = s.findConversation("C1");
    const model::ConvRef c2 = s.findConversation("C2"), g1 = s.findConversation("G1");
    const model::ConvRef c3 = s.findConversation("C3");
    REQUIRE((d1 != model::kNoConv && c1 != model::kNoConv && c2 != model::kNoConv));
    REQUIRE((g1 != model::kNoConv && c3 != model::kNoConv));
    CHECK(s.conversation(d1).muted); // "mute this person"
    CHECK(s.conversation(d1).kind == model::ConvKind::Dm);
    CHECK_STR(s.user(s.conversation(d1).dmUser).id, "U2");
    CHECK(s.conversation(d1).unread == 0); // the server's to say
    CHECK(s.conversation(c1).starred);
    CHECK(s.conversation(c1).notify == model::NotifyLevel::Mentions);
    CHECK_FALSE(s.conversation(c1).muted);
    CHECK(s.conversation(c1).lastRead == model::parseTs("1767225000.000100"));
    CHECK(s.conversation(c2).muted); // level "Mute"
    CHECK(s.conversation(c2).kind == model::ConvKind::Private);
    CHECK(s.conversation(g1).kind == model::ConvKind::Group);
    CHECK_STR(s.conversation(g1).localName, "Team");
    CHECK(s.conversation(c3).muted);
    CHECK_STR(s.user(s.me).id, "U1");
    const model::User &me = s.user(s.findUser("U1"));
    CHECK_STR(me.displayName, "Me Self");
    CHECK((me.hasTz && me.tzOffset == 3600 && me.admin));
    CHECK(s.user(s.findUser("U2")).stranger);
    CHECK(s.user(s.findUser("B1")).bot);
    CHECK(s.customEmoji().size() == 2);
    CHECK((s.myGroups.size() == 1 && s.myGroups[0] == "S1"));
    CHECK(s.threadMuted(c1, model::parseTs("1767225000.000100")));
    REQUIRE(s.aiTranscript("F1"));
    CHECK_STR(s.aiTranscript("F1")->text, "hello there");
    CHECK_STR(s.aiTranscript("F1")->by, "OpenAI");
    CHECK(s.reminderAt(c1, model::parseTs("1767225000.000100")) == 1767300000);
    // The Slack backend's own part, in its format (saveExtras).
    const json::Value x = meta.root()["x"];
    REQUIRE(x["saved"].size() == 2);
    CHECK_STR(x["saved"][0][0].str(), "C1");
    CHECK(x["saved"][0][2].integer() == 1767300000);
    CHECK(x["saved"][0][3].integer() == 1767225100);
    CHECK_STR(x["saved"][0][5].str(), "remember this");
    CHECK_STR(x["saved"][0][6].str(), "U2");
    CHECK(x["saved"][0][7].integer() == model::parseTs("1767224000.000300"));
    CHECK(x["saved"][1][4].boolean());             // fired
    CHECK_STR(x["saved"][1][5].str(), "bot said"); // from reminderPreviews
    CHECK_STR(x["saved"][1][8].str(), "Deploy");
    REQUIRE(x["followed"].size() == 1);
    CHECK(x["followed"][0][1].integer() == model::parseTs("1767224000.000300"));
    CHECK_STR(x["probed"][0][0].str(), "U2");
    CHECK(x["probed"][0][1].integer() == 1767225600); // ms → secs
    CHECK(x["sweep"].integer() == 1767220000);
    CHECK_STR(x["dead"][0].str(), "C9");
    CHECK_STR(x["ug"][0][1].str(), "devs");
    // Messages are not brought along (the network has them).
    CHECK_FALSE(file::exists(file::join(dir, "messages/C1.json")));

    // The pre-multi-service directory.
    {
        model::Store          s2;
        cache::WorkspaceCache wc(pa, s2, cache::WorkspaceCache::dirFor(pa, "slack:T0456"));
        REQUIRE(wc.load(nullptr));
        CHECK(s2.conversation(s2.findConversation("C7")).muted);
        wc.close(false);
    }

    // Claude Code: added where known-sessions.json had nothing.
    json::Document k;
    REQUIRE(k.parse(read(known)));
    const json::Value ss = k.root()["sessions"];
    CHECK(ss[0]["starred"].boolean());
    CHECK(ss[0]["muted"].boolean());
    CHECK(ss[0]["notify"].integer() == 1);
    CHECK_STR(ss[0]["name"].str(), "A");
    CHECK(ss[1]["muted"].boolean());
    CHECK_FALSE(ss[1].has("starred"));
    CHECK_STR(k.root()["open"].str(), "s1");
    CHECK_STR(k.root()["started"][0].str(), "s1");

    // The old files as they were; each workspace once, ever.
    CHECK(read(file::join(data, "cache/slack_T0123/meta.json")) == oldMeta);
    const std::string done = read(file::join(identity::configDir(pa), "legacy-imported"));
    CHECK_STR(done, "slack:T0123\nclaude-code:local\nslack:T0456\n");
    {
        model::Store          s3;
        cache::WorkspaceCache wc(pa, s3, dir);
        REQUIRE(wc.load(nullptr));
        s3.updateConversation(s3.findConversation("D1"), [](model::Conversation &c) {
            c.muted = false; // unmuted since the upgrade
        });
        wc.close(true);
    }
    legacy::importOldData(pa, settingsPath, wsPath);
    {
        model::Store          s4;
        cache::WorkspaceCache wc(pa, s4, dir);
        REQUIRE(wc.load(nullptr));
        CHECK_FALSE(s4.conversation(s4.findConversation("D1")).muted);
        wc.close(false);
    }
    clean(pa);
}

TEST("old cache: over an existing new cache, only the app's own state") {
    plat::App &pa = app().platform();
    clean(pa);
    const std::string from = file::join(identity::dataDir(pa), "cache/slack_T1");
    oldSlackCache(from);
    // The current version already ran: its roster knows C1 and D1, its backend state
    // has a sweep stamp and a followed thread of its own.
    const std::string dir = cache::WorkspaceCache::dirFor(pa, "slack:T1");
    {
        model::Store          s;
        cache::WorkspaceCache wc(pa, s, dir);
        CHECK_FALSE(wc.load(nullptr));
        const auto add = [&s](const char *id, model::ConvKind kind, const char *localName) {
            model::Conversation c;
            c.id        = id;
            c.kind      = kind;
            c.localName = localName;
            if (kind == model::ConvKind::Dm)
                c.dmUser = s.internUser("U2");
            s.addConversation(std::move(c));
        };
        add("C1", model::ConvKind::Channel, "");
        add("D1", model::ConvKind::Dm, "");
        add("G1", model::ConvKind::Group, "Mine");
        wc.saveExtras = [](json::Writer &w) {
            w.key("followed").beginArray().beginArray().value("C1").value(int64_t(7));
            w.endArray().endArray().key("sweep").value(int64_t(42));
        };
        wc.extrasChanged();
        wc.close(true);
    }
    REQUIRE(legacy::importSlackCache(pa, from, dir));
    model::Store          s;
    json::Document        meta;
    cache::WorkspaceCache wc(pa, s, dir);
    REQUIRE(wc.load(&meta));
    CHECK(s.conversationCount() == 3);         // no old conversation added
    CHECK(s.findUser("B1") == model::kNoUser); // nor users
    CHECK(s.conversation(s.findConversation("D1")).muted);
    CHECK(s.conversation(s.findConversation("C1")).notify == model::NotifyLevel::Mentions);
    CHECK_FALSE(s.conversation(s.findConversation("C1")).starred);         // Slack's to say
    CHECK_STR(s.conversation(s.findConversation("G1")).localName, "Mine"); // the newer name
    CHECK(s.threadMuted(s.findConversation("C1"), model::parseTs("1767225000.000100")));
    CHECK(s.aiTranscript("F1") != nullptr);
    const json::Value x = meta.root()["x"];
    CHECK(x["sweep"].integer() == 42);
    CHECK(x["followed"].size() == 2);
    CHECK_FALSE(x.has("saved"));
    wc.close(false);
    clean(pa);
}
#endif
