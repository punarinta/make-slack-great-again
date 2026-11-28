#include "app/legacy/old_settings_import.h"

#include "app/identity.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "prim/bytes.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/settings.h"

#include <algorithm>

namespace legacy {

using shell::AiProvider;
using shell::Settings;

namespace {

using oldsettings::Map;
using oldsettings::Value;

const Value &at(const Map &m, std::string_view key) {
    static const Value none;
    const auto         it = m.find(key);
    return it == m.end() ? none : it->second;
}

bool has(const Map &m, std::string_view key) {
    return m.find(key) != m.end();
}

// Old bool keys → Settings members (the old defaults are the new ones).
const struct {
    const char *key;
    bool Settings::*field;
} kBools[] = {
    {"appearance/threadsInline", &Settings::threadsInline},
    {"appearance/showLinkPreviews", &Settings::linkPreviews},
    {"appearance/showAgentsApps", &Settings::showAgentsApps},
    {"appearance/unreadsOnly", &Settings::unreadsOnly},
    {"appearance/animateEmoji", &Settings::animateEmoji},
    {"appearance/animateMedia", &Settings::animateMedia},
    {"composer/ctrlEnterSends", &Settings::ctrlEnterSends},
    {"composer/spellCheck", &Settings::spellCheck},
    {"notifications/enabled", &Settings::notifications},
    {"notifications/huddles", &Settings::notifyHuddles},
    {"notifications/boldMentionsOnly", &Settings::boldMentionsOnly},
    {"notifications/sound", &Settings::notifySound},
    {"voice/cleanup", &Settings::voiceCleanup},
    {"updates/autoCheck", &Settings::autoUpdates},
    {"window/minimizeToTray", &Settings::minimizeToTray},
    {"tray/customIcon", &Settings::customTrayIcon},
    {"tray/customIconMonochrome", &Settings::trayMonochrome},
};

const struct {
    const char *key;
    int Settings::*field;
    int            min, max;
} kInts[] = {
    {"appearance/relevantDays", &Settings::relevantDays, 1, 365},
    {"notifications/level", &Settings::notifyLevel, 0, 1},
    {"storage/cacheCapMb", &Settings::cacheLimitMb, 50, 10240},
    {"window/threadWidth", &Settings::threadWidth, 100, 4000},
    {"emoji/skinTone", &Settings::emojiSkinTone, 0, 6},
};

const struct {
    const char *key;
    std::string Settings::*field;
} kStrings[] = {
    {"notifications/soundId", &Settings::soundId},
    {"llm/defaultProvider", &Settings::aiDefault},
    {"llm/nativeLanguage", &Settings::aiLanguage},
    {"credentials/slackClientId", &Settings::slackClientId},
    {"claudeCode/lastDir", &Settings::claudeLastDir},
};

// The stored theme presets' ids, in ui::Palette order.
int paletteOf(const std::string &id, int def) {
    static const char *const ids[] = {"purple", "charcoal", "blue", "green", "custom"};
    for (int i = 0; i < int(std::size(ids)); ++i)
        if (id == ids[i])
            return i;
    return def;
}

// appearance/customTheme: the custom theme JSON earlier versions wrote, or a
// Slack sidebar string of 8 or 10 hex colours — read by the same parser as
// Settings' Import, pins and all.
void customTheme(const std::string &text, ui::CustomPalette *c) {
    settings::parseSlackTheme(text, c);
}

int32_t be32(std::string_view b, size_t at) {
    return int32_t(prim::be32(b.data() + at));
}

// window/geometry: the saved window geometry's bytes (big-endian):
// magic 0x1D9D0CB, version (2 × u16), frameGeometry, normalGeometry, screen,
// maximized, fullScreen, [v2] screen width, [v3] geometry — each rectangle as
// left, top, right, bottom (inclusive). A maximized window restores to normalGeometry.
void windowGeometry(std::string_view b, Settings *s) {
    if (b.size() < 46 || be32(b, 0) != 0x1D9D0CB)
        return;
    const int  major     = int(uint8_t(b[4])) << 8 | uint8_t(b[5]);
    const bool maximized = b[44] != 0;
    size_t     rect      = 24; // normalGeometry
    if (!maximized && major >= 3 && b.size() >= 66)
        rect = 50; // geometry()
    const int left = be32(b, rect), top = be32(b, rect + 4);
    const int w = be32(b, rect + 8) - left + 1, h = be32(b, rect + 12) - top + 1;
    if (w < 100 || h < 100)
        return;
    s->width       = w;
    s->height      = h;
    s->x           = left;
    s->y           = top;
    s->hasPosition = true;
    s->maximized   = maximized;
}

bool isKnownService(std::string_view key) {
    return str::startsWith(key, "slack:") || str::startsWith(key, "claude-code:");
}

} // namespace

void importOldSettings(
    const Map                                &old,
    const Map                                &appStore,
    const std::string                        &dataDir,
    const std::vector<auth::WorkspaceRecord> &workspaces,
    Settings                                 *s
) {
    for (const auto &b : kBools)
        s->*b.field = at(old, b.key).toBool(s->*b.field);
    for (const auto &i : kInts)
        s->*i.field = int(std::clamp<int64_t>(at(old, i.key).toInt(s->*i.field), i.min, i.max));
    for (const auto &k : kStrings)
        if (has(old, k.key))
            s->*k.field = at(old, k.key).text();

    // A one-time migration earlier versions ran at start: the per-conversation
    // default became "All new posts".
    if (!at(old, "notifications/defaultMigrated").toBool(false) && s->notifyLevel == 1)
        s->notifyLevel = 0;

    // Appearance. Before appearance/mode existed, appearance/theme alone
    // chose the theme, "charcoal" meaning dark.
    const std::string light = at(old, "appearance/theme").text();
    if (has(old, "appearance/mode")) {
        const std::string mode = at(old, "appearance/mode").text();
        s->theme               = mode == "light"  ? ui::ThemeMode::Light
                                 : mode == "dark" ? ui::ThemeMode::Dark
                                                  : ui::ThemeMode::System;
        s->paletteLight        = paletteOf(light, s->paletteLight);
        s->paletteDark         = paletteOf(at(old, "appearance/themeDark").text(), s->paletteDark);
    } else if (light == "charcoal") {
        s->theme = ui::ThemeMode::Dark;
    } else if (!light.empty()) {
        s->paletteLight = paletteOf(light, s->paletteLight);
    }
    customTheme(at(old, "appearance/customTheme").text(), &s->custom);
    const std::string font = at(old, "appearance/fontSize").text();
    s->fontSize            = font == "small" ? 0 : font == "large" ? 2 : 1;
    if (has(old, "appearance/language"))
        s->language = at(old, "appearance/language").text();
    if (has(old, "appearance/timeFormat")) {
        s->use24h      = at(old, "appearance/timeFormat").text() == "24h";
        s->use24hSaved = true;
    }

    // System.
    const std::string presence = at(old, "presence/mode").text();
    s->presence                = presence == "native" ? 2 : presence == "using" ? 1 : 0;
    const std::string conn     = at(old, "slack/connectionMode").text();
    if (conn == "session" || conn == "appkeys") {
        s->slackSession = conn == "session";
    } else {
        // Never chosen: session, unless an OAuth workspace (no session cookie)
        // was signed in before the choice existed.
        s->slackSession = true;
        for (const auth::WorkspaceRecord &r : workspaces)
            if (r.service == "slack" && !r.auth.empty() &&
                r.auth.find("\"cookie\"") == std::string::npos)
                s->slackSession = false;
    }
    s->lastUpdateCheck = at(old, "updates/lastChecked").toInt(0);
    if (at(old, "tray/customIcon").toBool(false)) {
        const std::string tray = file::join(dataDir, "tray_icon.png");
        if (file::exists(tray))
            s->trayIconPath = tray;
    }
    windowGeometry(at(old, "window/geometry").s, s);

    // AI providers: the presets' models, the custom servers (their keys stay
    // in the secret store, read by Settings::load).
    for (const char *id : {"anthropic", "openai"})
        if (AiProvider *p = s->provider(id)) {
            const std::string base = str::concat({"llm/providers/", id, "/"});
            p->model               = at(old, base + "model").text();
            p->sttModel            = at(old, base + "sttModel").text();
        }
    for (const std::string &id : at(old, "llm/customProviders").toList()) {
        const std::string base = "llm/providers/" + id + "/";
        AiProvider        p;
        p.id       = id;
        p.name     = at(old, base + "name").text();
        p.url      = at(old, base + "baseUrl").text();
        p.model    = at(old, base + "model").text();
        p.sttModel = at(old, base + "sttModel").text();
        if (id.empty() || p.url.empty() || s->provider(id))
            continue; // a half-written entry: skipped
        s->ai.push_back(std::move(p));
    }
    std::string glossary;
    for (const std::string &term : at(old, "voice/glossary").toList())
        if (!str::trim(term).empty())
            glossary += glossary.empty() ? term : "\n" + term;
    s->voiceGlossary = glossary;

    // Claude Code folders: claudeCode/recentDirs is an INI array
    // (<key>/size, <key>/<1-based index>/path|usedAt); claudeCode/lastDir/<role>
    // each teammate's own pick.
    s->claudeRecentDirs.clear();
    const int64_t n = at(old, "claudeCode/recentDirs/size").toInt(0);
    for (int64_t i = 1; i <= n && i <= 100; ++i) {
        const std::string base = "claudeCode/recentDirs/" + str::number(i) + "/";
        const std::string path = at(old, base + "path").text();
        if (!path.empty())
            s->claudeRecentDirs.push_back({path, at(old, base + "usedAt").toInt(0)});
    }
    s->claudeTeammateDirs.clear();
    constexpr std::string_view kTeammate = "claudeCode/lastDir/";
    for (auto it = old.lower_bound(kTeammate);
         it != old.end() && str::startsWith(it->first, kTeammate);
         ++it)
        if (const std::string dir = it->second.text(); !dir.empty())
            s->claudeTeammateDirs.emplace_back(it->first.substr(kTeammate.size()), dir);

    if (s->emojiSkinTone == 1)
        s->emojiSkinTone = 0; // only 2-6 are tones
    s->spellLanguages = at(old, "composer/spellLanguages").toList();
    // conv/visitedAt: the sidebar's visit stamps, a JSON object of
    // conversation id → epoch seconds.
    s->visitedAt.clear();
    if (json::Document d; d.parse(at(old, "conv/visitedAt").text(), nullptr))
        for (const json::Value e : d.root())
            if (!e.key().empty() && e.integer() > 0)
                s->visitedAt.emplace_back(std::string(e.key()), e.integer());
    s->emojiRecent = at(old, "emoji/recent").toList();
    // zenMode/<percent-encoded workspace key>: the footer's zen toggle, per
    // workspace.
    s->zenWorkspaces.clear();
    constexpr std::string_view kZen = "zenMode/";
    for (auto it = old.lower_bound(kZen); it != old.end() && str::startsWith(it->first, kZen); ++it)
        if (it->second.toBool(false))
            s->zenWorkspaces.push_back(str::percentDecode(it->first.substr(kZen.size())));
    s->lastAttachDir = at(appStore, "composer/lastAttachDir").text();
}

std::vector<auth::WorkspaceRecord> oldWorkspaces(const Map &old, std::string *active) {
    std::vector<auth::WorkspaceRecord> out;
    *active        = at(old, "active").text();
    const auto add = [&](std::string service,
                         std::string id,
                         std::string name,
                         std::string icon,
                         std::string auth,
                         bool        muted) {
        auth::WorkspaceRecord r;
        r.service     = std::move(service);
        r.id          = std::move(id);
        r.displayName = std::move(name);
        r.iconUrl     = std::move(icon);
        r.auth        = std::move(auth);
        r.muted       = muted;
        out.push_back(std::move(r));
    };
    if (at(old, "storeVersion").toInt(0) < 2) {
        // The layouts before store version 2 (versions 0 and 1): one Slack workspace under auth/*,
        // then bare team ids with plain token fields. Their tokens were never in a keychain.
        const auto blob = [](const std::string &xoxp, const std::string &refresh, int64_t exp) {
            json::Writer w;
            w.beginObject().key("xoxp").value(xoxp).key("refreshToken").value(refresh);
            w.key("expiresAt").value(str::number(exp)).endObject();
            return w.take();
        };
        if (!has(old, "workspaces") && !at(old, "auth/xoxp").text().empty()) {
            std::string id = at(old, "auth/team_id").text();
            if (id.empty())
                id = "legacy";
            add("slack",
                id,
                at(old, "auth/team_name").text(),
                {},
                blob(at(old, "auth/xoxp").text(), {}, 0),
                false);
            *active = "slack:" + id;
            return out;
        }
        for (const std::string &id : at(old, "workspaces").toList()) {
            if (id.find(':') != std::string::npos)
                continue; // already a v2 handle: below
            const std::string base = "workspace/" + id + "/";
            add("slack",
                id,
                at(old, base + "name").text(),
                at(old, base + "iconUrl").text(),
                blob(
                    at(old, base + "xoxp").text(),
                    at(old, base + "refreshToken").text(),
                    at(old, base + "expiresAt").toInt(0)
                ),
                false);
            if (*active == id)
                *active = "slack:" + id;
        }
    }
    for (const std::string &handle : at(old, "workspaces").toList()) {
        const size_t colon = handle.find(':');
        if (colon == std::string::npos || !isKnownService(handle))
            continue; // Teams and IMAP are gone from this app (the old store keeps them)
        const std::string base = "workspace/" + handle + "/";
        if (!has(old, base + "displayName") && !has(old, base + "iconUrl") &&
            !has(old, base + "auth"))
            continue; // listed, but its record is gone
        add(handle.substr(0, colon),
            handle.substr(colon + 1),
            at(old, base + "displayName").text(),
            at(old, base + "iconUrl").text(),
            {},
            at(old, base + "muted").toBool(false));
    }
    return out;
}

void importOldWorkspaceIcons(const Map &old, const std::string &iconDir) {
    std::string active;
    for (const auth::WorkspaceRecord &r : oldWorkspaces(old, &active)) {
        const std::string from = at(old, "workspace/" + r.key() + "/customIcon").text();
        if (from.empty() || !file::exists(from))
            continue;
        // The new name: <lower(id)>-<ms>.img (shell_dialogs.cpp's iconFiles).
        const std::string to = file::join(
            iconDir,
            str::concat({str::asciiLower(r.id), "-", str::number(base::nowMicros() / 1000), ".img"})
        );
        if (!file::copy(from, to))
            LOG_WARN("legacy", "could not copy the workspace icon %s", from.c_str());
    }
}

void importOldSettingsAndWorkspaces(
    plat::App &app, const std::string &settingsPath, const std::string &workspacesPath
) {
    const bool wantSettings   = !settingsPath.empty() && !file::exists(settingsPath);
    const bool wantWorkspaces = !workspacesPath.empty() && !file::exists(workspacesPath);
    if (!wantSettings && !wantWorkspaces)
        return;
    const Map old = oldsettings::load();
    if (old.empty())
        return; // a fresh install: nothing to bring along
    // The imported records with their auth (read once: each keychain read of
    // an item an earlier version wrote may ask the user).
    std::vector<auth::WorkspaceRecord> workspaces;
    if (wantWorkspaces) {
        // Two statements: oldWorkspaces fills `active`, and the order a
        // call's arguments are evaluated in is unspecified.
        std::string          active;
        auto                 records = oldWorkspaces(old, &active);
        auth::WorkspaceStore store(workspacesPath);
        store.importRecords(std::move(records), active);
        workspaces = store.all();
        importOldWorkspaceIcons(old, file::join(identity::dataDir(app), "workspace_icons"));
    }
    if (wantSettings) {
        Settings s = Settings::load({});
        importOldSettings(old, oldsettings::load("MSGA"), identity::dataDir(app), workspaces, &s);
        s.save(settingsPath);
    }
    LOG_INFO("legacy", "imported the earlier version's settings and workspaces");
}

} // namespace legacy
