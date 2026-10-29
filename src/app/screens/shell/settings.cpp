#include "screens/shell/settings.h"

#include "app/identity.h"
#include "app/screens/common/custom_theme.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/secret.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

// MSGA_GIPHY_KEY comes from credentials.cmake (screens/settings/CMakeLists.txt);
// empty when the build has none.
#ifndef MSGA_GIPHY_KEY
#define MSGA_GIPHY_KEY ""
#endif

namespace shell {

namespace {

// The plain preferences, by JSON key (top level of the file).
const struct {
    const char *key;
    bool Settings::*field;
} kBools[] = {
    {"useTray", &Settings::useTray},
    {"use24h", &Settings::use24h},
    {"threadsInline", &Settings::threadsInline},
    {"linkPreviews", &Settings::linkPreviews},
    {"ctrlEnterSends", &Settings::ctrlEnterSends},
    {"spellCheck", &Settings::spellCheck},
    {"markdownFormatting", &Settings::markdownFormatting},
    {"showAgentsApps", &Settings::showAgentsApps},
    {"unreadsOnly", &Settings::unreadsOnly},
    {"animateEmoji", &Settings::animateEmoji},
    {"animateMedia", &Settings::animateMedia},
    {"customTrayIcon", &Settings::customTrayIcon},
    {"notifications", &Settings::notifications},
    {"notifyHuddles", &Settings::notifyHuddles},
    {"boldMentionsOnly", &Settings::boldMentionsOnly},
    {"notifySound", &Settings::notifySound},
    {"voiceCleanup", &Settings::voiceCleanup},
    {"autoUpdates", &Settings::autoUpdates},
    {"minimizeToTray", &Settings::minimizeToTray},
    {"slackSession", &Settings::slackSession},
    {"trayMonochrome", &Settings::trayMonochrome},
};

// Integers, clamped: a corrupt or hand-edited file must not produce nonsense.
const struct {
    const char *key;
    int Settings::*field;
    int            min, max;
} kInts[] = {
    {"threadWidth", &Settings::threadWidth, 100, 4000},
    {"paletteLight", &Settings::paletteLight, 0, int(ui::Palette::Count) - 1},
    {"paletteDark", &Settings::paletteDark, 0, int(ui::Palette::Count) - 1},
    {"fontSize", &Settings::fontSize, 0, 2},
    {"relevantDays", &Settings::relevantDays, 1, 365},
    {"names", &Settings::names, 0, 2},
    {"notifyLevel", &Settings::notifyLevel, 0, 1},
    {"cacheLimitMb", &Settings::cacheLimitMb, 50, 10240},
    {"presence", &Settings::presence, 0, 2},
    {"emojiSkinTone", &Settings::emojiSkinTone, 0, 6},
};

// `secret`: its key in the secret store (base/secret.h),
// where the value lives instead of the file.
const struct {
    const char *key;
    std::string Settings::*field;
    const char            *secret = nullptr;
} kStrings[] = {
    {"language", &Settings::language},
    {"trayIconPath", &Settings::trayIconPath},
    {"soundId", &Settings::soundId},
    {"aiDefault", &Settings::aiDefault},
    {"aiLanguage", &Settings::aiLanguage},
    {"voiceGlossary", &Settings::voiceGlossary},
    {"slackClientId", &Settings::slackClientId},
    {"slackClientSecret", &Settings::slackClientSecret, "credentials/slackClientSecret"},
    {"slackAppToken", &Settings::slackAppToken, "credentials/slackXapp"},
    {"giphyKey", &Settings::giphyKey, "gif/giphy/apiKey"},
    {"claudeLastDir", &Settings::claudeLastDir},
    {"lastAttachDir", &Settings::lastAttachDir},
};

// Lists of strings (JSON arrays).
const struct {
    const char              *key;
    std::vector<std::string> Settings::*field;
} kStringLists[] = {
    {"spellLanguages", &Settings::spellLanguages},
    {"emojiRecent", &Settings::emojiRecent},
    {"zenWorkspaces", &Settings::zenWorkspaces},
};

const struct {
    const char *key;
    std::string AiProvider::*field;
} kAiFields[] = {
    {"id", &AiProvider::id},
    {"name", &AiProvider::name},
    {"url", &AiProvider::url},
    {"key", &AiProvider::key},
    {"model", &AiProvider::model},
    {"sttModel", &AiProvider::sttModel},
};

// An AI provider's key in the secret store.
std::string aiSecret(const AiProvider &p) {
    return "llm/" + p.id + "/apiKey";
}

// What the secret store holds, by its key, as far as this process knows:
// read at load, updated by successful writes. A save writes only what
// changed — each keychain write may raise an OS prompt.
std::unordered_map<std::string, std::string> &stored() {
    static std::unordered_map<std::string, std::string> m;
    return m;
}

std::string loadSecret(const std::string &key) {
    std::string v = secret::read(key);
    stored()[key] = v;
    return v;
}

// True when `value` is in the secret store; false = keep it in the file.
bool saveSecret(const std::string &key, const std::string &value) {
    auto &m = stored();
    if (auto it = m.find(key); it != m.end() ? it->second == value : value.empty())
        return true;
    if (!secret::write(key, value))
        return false;
    m[key] = value;
    return true;
}

const char *themeName(ui::ThemeMode m) {
    return m == ui::ThemeMode::Light ? "light" : m == ui::ThemeMode::Dark ? "dark" : "system";
}

} // namespace

Settings::Settings() {
    ai.push_back({"anthropic", "Anthropic", {}, {}, {}, {}});
    ai.push_back({"openai", "OpenAI", {}, {}, {}, {}});
}

void Settings::applyPalettes() const {
    ui::setCustomPalette(custom);
    ui::setPalette(false, ui::Palette(paletteLight));
    ui::setPalette(true, ui::Palette(paletteDark));
}

bool Settings::zenMode(std::string_view workspaceKey) const {
    return std::find(zenWorkspaces.begin(), zenWorkspaces.end(), workspaceKey) !=
           zenWorkspaces.end();
}

void Settings::setZenMode(const std::string &workspaceKey, bool on) {
    std::erase(zenWorkspaces, workspaceKey);
    if (on)
        zenWorkspaces.push_back(workspaceKey);
}

AiProvider *Settings::provider(std::string_view id) {
    for (AiProvider &p : ai)
        if (p.id == id)
            return &p;
    return nullptr;
}

const AiProvider *Settings::provider(std::string_view id) const {
    return const_cast<Settings *>(this)->provider(id);
}

std::string Settings::effectiveAiLanguage() const {
    if (!aiLanguage.empty())
        return aiLanguage;
    return language == "system" ? base::osLanguage() : language;
}

std::string Settings::buildGiphyKey() {
    return std::string(str::trim(MSGA_GIPHY_KEY));
}

std::string Settings::effectiveGiphyKey() const {
    return giphyKey.empty() ? buildGiphyKey() : giphyKey;
}

namespace {

// The file's values into `s` (a missing key keeps the default).
void fromJson(const json::Value r, Settings &s) {
    const json::Value w      = r["window"];
    s.width                  = int(std::clamp<int64_t>(w["width"].integer(s.width), 480, 16384));
    s.height                 = int(std::clamp<int64_t>(w["height"].integer(s.height), 360, 16384));
    s.hasPosition            = w.has("x") && w.has("y");
    s.x                      = int(w["x"].integer());
    s.y                      = int(w["y"].integer());
    s.maximized              = w["maximized"].boolean();
    const std::string_view t = r["theme"].str();
    s.theme                  = t == "light"  ? ui::ThemeMode::Light
                               : t == "dark" ? ui::ThemeMode::Dark
                                             : ui::ThemeMode::System;
    for (const auto &b : kBools)
        s.*b.field = r[b.key].boolean(s.*b.field);
    for (const auto &i : kInts)
        s.*i.field = int(std::clamp<int64_t>(r[i.key].integer(s.*i.field), i.min, i.max));
    for (const auto &k : kStrings)
        if (r.has(k.key))
            s.*k.field = std::string(r[k.key].str());
    for (const auto &k : kStringLists)
        for (const json::Value e : r[k.key])
            if (!e.str().empty())
                (s.*k.field).emplace_back(e.str());
    if (s.emojiSkinTone == 1)
        s.emojiSkinTone = 0; // tones are 2-6
    s.lastUpdateCheck = r["lastUpdateCheck"].integer();
    s.use24hSaved     = r.has("use24h");

    screens::readCustomTheme(r["customTheme"], &s.custom);

    for (const json::Value e : r["visitedAt"])
        if (!e.key().empty() && e.integer() > 0)
            s.visitedAt.emplace_back(std::string(e.key()), e.integer());
    for (const json::Value e : r["sidebarSorts"])
        if (!e["section"].str().empty() && !e["sort"].str().empty())
            s.sidebarSorts.push_back(
                {std::string(e["workspace"].str()),
                 std::string(e["section"].str()),
                 std::string(e["sort"].str())}
            );
    for (const json::Value e : r["claudeRecentDirs"])
        if (!e["path"].str().empty())
            s.claudeRecentDirs.push_back({std::string(e["path"].str()), e["usedAt"].integer()});
    // Earlier builds' one zen toggle: the Claude Code workspace's, the only
    // one with zen mode (claude::kService:kWorkspaceId).
    if (r["zenMode"].boolean(false) && s.zenWorkspaces.empty())
        s.zenWorkspaces.push_back("claude-code:local");
    for (const json::Value e : r["claudeTeammateDirs"])
        if (!e["role"].str().empty() && !e["dir"].str().empty())
            s.claudeTeammateDirs.emplace_back(
                std::string(e["role"].str()), std::string(e["dir"].str())
            );

    for (const json::Value p : r["ai"]) {
        AiProvider a;
        for (const auto &f : kAiFields)
            a.*f.field = std::string(p[f.key].str());
        if (a.id.empty())
            continue;
        if (AiProvider *have = s.provider(a.id))
            *have = std::move(a);
        else
            s.ai.push_back(std::move(a));
    }
}

} // namespace

Settings Settings::load(const std::string &path) {
    Settings       s;
    std::string    text;
    json::Document d;
    if (!path.empty() && file::readAll(path, &text) && d.parse(std::move(text), nullptr))
        fromJson(d.root(), s);
    // The secrets, also without a file. One in the file is one the store
    // refused (or a file from before the store).
    for (const auto &k : kStrings)
        if (k.secret && (s.*k.field).empty())
            s.*k.field = loadSecret(k.secret);
    for (AiProvider &p : s.ai)
        if (p.key.empty())
            p.key = loadSecret(aiSecret(p));
    return s;
}

std::string Settings::toFile() const {
    json::Writer w(true);
    w.beginObject().key("window").beginObject();
    w.key("width").value(width).key("height").value(height);
    if (hasPosition)
        w.key("x").value(x).key("y").value(y);
    w.key("maximized").value(maximized).endObject();
    w.key("theme").value(themeName(theme));
    for (const auto &b : kBools)
        w.key(b.key).value(this->*b.field);
    for (const auto &i : kInts)
        w.key(i.key).value(this->*i.field);
    for (const auto &k : kStrings)
        if (!(this->*k.field).empty() && !(k.secret && saveSecret(k.secret, this->*k.field)))
            w.key(k.key).value(this->*k.field);
        else if (k.secret && (this->*k.field).empty())
            saveSecret(k.secret, {});
    for (const auto &k : kStringLists) {
        if ((this->*k.field).empty())
            continue;
        w.key(k.key).beginArray();
        for (const std::string &v : this->*k.field)
            w.value(v);
        w.endArray();
    }
    if (lastUpdateCheck)
        w.key("lastUpdateCheck").value(lastUpdateCheck);
    w.key("customTheme");
    screens::writeCustomTheme(w, custom);
    if (!visitedAt.empty()) {
        w.key("visitedAt").beginObject();
        for (const auto &[id, at] : visitedAt)
            w.key(id).value(at);
        w.endObject();
    }
    if (!sidebarSorts.empty()) {
        w.key("sidebarSorts").beginArray();
        for (const SectionSortPref &p : sidebarSorts)
            w.beginObject()
                .key("workspace")
                .value(p.workspace)
                .key("section")
                .value(p.section)
                .key("sort")
                .value(p.sort)
                .endObject();
        w.endArray();
    }
    if (!claudeRecentDirs.empty()) {
        w.key("claudeRecentDirs").beginArray();
        for (const RecentDir &d : claudeRecentDirs)
            w.beginObject().key("path").value(d.path).key("usedAt").value(d.usedAt).endObject();
        w.endArray();
    }
    if (!claudeTeammateDirs.empty()) {
        w.key("claudeTeammateDirs").beginArray();
        for (const auto &[role, dir] : claudeTeammateDirs)
            w.beginObject().key("role").value(role).key("dir").value(dir).endObject();
        w.endArray();
    }
    // A removed provider's key goes from the store too (LlmTokenStore::clear).
    std::vector<std::string> gone;
    for (const auto &[key, value] : stored())
        if (str::startsWith(key, "llm/") && !value.empty()) {
            bool kept = false;
            for (const AiProvider &p : ai)
                kept = kept || aiSecret(p) == key;
            if (!kept)
                gone.push_back(key);
        }
    for (const std::string &key : gone)
        saveSecret(key, {});
    w.key("ai").beginArray();
    for (const AiProvider &p : ai) {
        const bool keyInFile = !saveSecret(aiSecret(p), p.key);
        if (p.preset() && (p.key.empty() || !keyInFile) && p.model.empty() && p.sttModel.empty())
            continue; // an untouched preset: nothing to keep
        w.beginObject();
        for (const auto &f : kAiFields)
            if (!(p.*f.field).empty() && (f.field != &AiProvider::key || keyInFile))
                w.key(f.key).value(p.*f.field);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.str() + "\n";
}

namespace {

// The settings writes, numbered as they are asked for (UI thread): per
// path the number of the newest one on disk, so a write overtaken by a
// newer one is skipped.
std::mutex                                g_writeMutex;
std::unordered_map<std::string, uint64_t> g_written;
uint64_t                                  g_asked = 0;

bool writeSettings(const std::string &path, const std::string &text, uint64_t n) {
    std::lock_guard<std::mutex> lock(g_writeMutex);
    uint64_t                   &done = g_written[path];
    if (n <= done)
        return true;
    done = n;
    return file::writeAtomic(path, text, 0600);
}

} // namespace

bool Settings::save(const std::string &path) const {
    return !path.empty() && writeSettings(path, toFile(), ++g_asked);
}

void Settings::saveInBackground(plat::App &app, const std::string &path) const {
    if (path.empty())
        return;
    model::runInBackground(
        app,
        [path, text = toFile(), n = ++g_asked] {
            if (!writeSettings(path, text, n))
                LOG_WARN("shell", "could not save %s", path.c_str());
        },
        [] {}
    );
}

std::string Settings::defaultPath(plat::App &app) {
    const std::string dir = identity::configDir(app);
    return dir.empty() ? std::string() : file::join(dir, "settings.json");
}

} // namespace shell
