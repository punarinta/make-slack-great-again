// The Settings dialog inside a whole shell on plat's headless backend, with
// input through plat's TestHooks (the path real input takes): it opens as an
// in-window overlay, Escape and the backdrop close it, sections switch pages,
// every page lists its items in order, controls write
// shell::Settings and the file (at once, or on the page's Save), and what
// can apply live does (theme and palettes, font size, time format, link
// previews, Ctrl+Enter, the notification level).
#include "app/fake/fake_backend.h"
#include "app/llm/service.h"
#include "support/fake_llm_server.h"
#include "app/media/sounds.h"
#include "app/spell/spell.h"
#include "base/json.h"
#include "base/file.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/testing.h"
#include "screens/settings/settings_dialog.h"
#include "app/mrkdwn/markdown.h"
#include "screens/shell/composer.h"
#include "screens/shell/shell.h"
#include "screens/shell/shortcuts.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <memory>

using namespace model;
using settings::SettingsDialog;

namespace {

ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p)
            std::fprintf(stderr, "ui::App::create: %s\n", err.c_str());
        return p;
    }();
    return *a;
}

plat::TestHooks &hooks() {
    return *app().platform().testHooks();
}

void pump(int n = 10) {
    for (int i = 0; i < n; ++i)
        app().pump(2);
}

std::string testPath() {
    const char *h = std::getenv("XDG_CONFIG_HOME");
    return file::join(h ? h : "/tmp", "msga-settings-test.json");
}

struct Harness {
    Store             store;
    fake::FakeBackend backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    shell::Settings               settings;
    std::string                   path;
    std::unique_ptr<ui::Window>   win;
    std::unique_ptr<shell::Shell> sh;

    explicit Harness(std::string settingsPath = {}) : path(std::move(settingsPath)) {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        for (int i = 0; i < 200 && !done; ++i)
            app().pump(5);
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, path);
        sh->open(store.findConversation("C0DESIGN"));
        pump();
    }
    ~Harness() {
        // The App outlives every case: leave its toolkit state as found.
        sh.reset();
        app().setThemeMode(ui::ThemeMode::System);
        app().setUserTextScale(1);
        shell::Settings().applyPalettes();
        base::setUse24h(false);
        base::setDateLanguage("en");
        shell::shortcuts::setCtrlEnterSends(false);
        shell::setMarkdownFormatting(false);
    }

    SettingsDialog &open() {
        sh->openSettings();
        pump();
        return *sh->settingsDialog();
    }
    void click(ui::View *v) {
        if (!CHECK(v != nullptr)) // a control this platform doesn't have: a failure, not a crash
            return;
        // Pages scroll: bring the target into view first, as a user would.
        if (SettingsDialog *d = sh->settingsDialog(); d && d->scroll().isAncestorOf(v)) {
            d->scroll().ensureVisible(v, 8);
            pump();
        }
        const ui::RectF r = v->windowRect();
        hooks().injectPointerMove(win->native(), {r.x + r.w / 2, r.y + r.h / 2});
        pump(2);
        hooks().injectButton(win->native(), plat::Button::Left, true);
        pump(2);
        hooks().injectButton(win->native(), plat::Button::Left, false);
        pump();
    }
    void clickAt(float x, float y) {
        hooks().injectPointerMove(win->native(), {x, y});
        pump(2);
        hooks().injectButton(win->native(), plat::Button::Left, true);
        pump(2);
        hooks().injectButton(win->native(), plat::Button::Left, false);
        pump();
    }
    void key(plat::Key k) {
        hooks().injectKey(win->native(), k, true);
        hooks().injectKey(win->native(), k, false);
        pump();
    }
};

ui::View *findIn(ui::View *v, std::string_view name) {
    if (!v->visible())
        return nullptr;
    if (v->accessibleName() == name)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findIn(v->child(i), name))
            return f;
    return nullptr;
}

int notificationCount() {
    return int(hooks().notificationCount());
}

} // namespace

TEST(
    "settings: a theme reads from the ia_theme JSON, palette names and pins, or the share string"
) {
    ui::CustomPalette t;
    REQUIRE(
        settings::parseSlackTheme(
            R"({"primary":{"hex":"#112233"},"highlight1":{"palette":"Jade"},"highlight2":"ocean",
            "brightness":14,"sidebarInverted":false,"pins":{"itemText":"#abc"}})",
            &t
        )
    );
    CHECK(t.primary == 0xff112233u);
    CHECK(t.highlight1 == 0xff2bac76u); // the swatch, any case
    CHECK(t.highlight2 == 0xff0e2a40u);
    CHECK(t.important == ui::CustomPalette().important); // not named: the default's
    CHECK(t.brightness == 10 && !t.sidebarInverted && t.gradient);
    CHECK(t.itemText == 0xffaabbccu && t.itemHover == 0);
    // Some other JSON is not a theme; hex keeps to opaque RGB.
    CHECK_FALSE(settings::parseSlackTheme(R"({"name":"x"})", &t));
    REQUIRE(settings::parseSlackTheme(R"({"primary":"#11223344"})", &t));
    CHECK(t.primary == ui::CustomPalette().primary);
    REQUIRE(
        settings::parseSlackTheme(
            "#3F0E40,#350d36,#1164A3,#FFFFFF,#350D36,#FFFFFF,#2BAC76,#CD2553", &t
        )
    );
    CHECK(t.primary == 0xff3f0e40u && t.itemSelText == 0xffffffffu);
}

TEST("settings: every value survives the file, which is owner-only") {
    const std::string path = testPath();
    file::remove(path);
    const shell::Settings def = shell::Settings::load(path);
    CHECK(def.useTray);
    CHECK(def.notifications);
    CHECK(def.fontSize == 1);
    CHECK(def.paletteLight == int(ui::Palette::Purple));
    CHECK(def.paletteDark == int(ui::Palette::Charcoal));
    CHECK(def.relevantDays == 14);
    CHECK(def.names == 0); // as set in Slack
    CHECK(def.cacheLimitMb == 250);
    CHECK_STR(def.language, "system");
    CHECK(def.ai.size() == 2);

    shell::Settings s;
    s.paletteLight               = int(ui::Palette::Green);
    s.paletteDark                = int(ui::Palette::Custom);
    s.custom.primary             = 0xff123456;
    s.custom.brightness          = 3;
    s.custom.sidebarInverted     = false;
    s.fontSize                   = 2;
    s.language                   = "ja";
    s.use24h                     = true;
    s.threadsInline              = true;
    s.linkPreviews               = false;
    s.ctrlEnterSends             = true;
    s.relevantDays               = 45;
    s.names                      = 2;
    s.unreadsOnly                = true;
    s.animateMedia               = false;
    s.notifyLevel                = 1;
    s.notifySound                = false;
    s.cacheLimitMb               = 600;
    s.minimizeToTray             = true;
    s.useTray                    = false;
    s.presence                   = 2;
    s.slackSession               = false;
    s.slackClientId              = "123.456";
    s.giphyKey                   = "gk";
    s.voiceGlossary              = "gRPC\nnginx";
    s.spellLanguages             = {"en_US", "de_DE_frami"};
    s.emojiRecent                = {"tada", "+1"};
    s.emojiSkinTone              = 3;
    s.lastAttachDir              = "/tmp/pics";
    s.provider("anthropic")->key = "sk-ant-abcd1234";
    s.ai.push_back({"custom-1", "vLLM", "http://localhost:8000/v1", "", "qwen", ""});
    s.aiDefault = "custom-1";
    REQUIRE(s.save(path));
#ifndef _WIN32
    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600); // API keys live in it
#endif
    const shell::Settings r = shell::Settings::load(path);
    CHECK(r.paletteLight == int(ui::Palette::Green));
    CHECK(r.paletteDark == int(ui::Palette::Custom));
    CHECK(r.custom.primary == 0xff123456u);
    CHECK(r.custom.brightness == 3);
    CHECK_FALSE(r.custom.sidebarInverted);
    CHECK(r.fontSize == 2);
    CHECK(r.fontScale() > 1.1f);
    CHECK_STR(r.language, "ja");
    CHECK(r.use24h && r.threadsInline && r.ctrlEnterSends && r.unreadsOnly);
    CHECK_FALSE(r.linkPreviews);
    CHECK_FALSE(r.animateMedia);
    CHECK(r.relevantDays == 45);
    CHECK(r.names == 2);
    CHECK(r.notifyLevel == 1);
    CHECK_FALSE(r.notifySound);
    CHECK(r.cacheLimitMb == 600);
    CHECK(r.minimizeToTray);
    CHECK_FALSE(r.useTray);
    CHECK(r.presence == 2);
    CHECK_FALSE(r.slackSession);
    CHECK_STR(r.slackClientId, "123.456");
    CHECK_STR(r.giphyKey, "gk");
    CHECK_STR(r.voiceGlossary, "gRPC\nnginx");
    REQUIRE(r.spellLanguages.size() == 2);
    CHECK_STR(r.spellLanguages[1], "de_DE_frami");
    REQUIRE(r.emojiRecent.size() == 2);
    CHECK_STR(r.emojiRecent[0], "tada");
    CHECK(r.emojiSkinTone == 3);
    CHECK_STR(r.lastAttachDir, "/tmp/pics");
    REQUIRE(r.ai.size() == 3);
    CHECK_STR(r.provider("anthropic")->key, "sk-ant-abcd1234");
    CHECK(r.provider("openai") != nullptr);
    CHECK_STR(r.provider("custom-1")->url, "http://localhost:8000/v1");
    CHECK_STR(r.aiDefault, "custom-1");
    file::remove(path);

    // A corrupt file clamps instead of breaking the dialog.
    REQUIRE(file::writeAtomic(path, R"({"relevantDays":0,"paletteDark":99,"notifyLevel":7})"));
    const shell::Settings c = shell::Settings::load(path);
    CHECK(c.relevantDays == 1);
    CHECK(c.paletteDark == int(ui::Palette::Count) - 1);
    CHECK(c.notifyLevel == 1);
    file::remove(path);
}

TEST("settings: the gear opens a window-filling overlay; Escape and the backdrop close it") {
    Harness   h;
    ui::View *gear = findIn(&h.win->root(), "Settings"); // the rail tooltip
    REQUIRE(gear != nullptr);
    h.click(gear);
    SettingsDialog *d = h.sh->settingsDialog();
    REQUIRE(d != nullptr);
    CHECK(h.win->topPopup() == d);
    CHECK(d->frame().w == 1200 && d->frame().h == 800);
    const ui::RectF p = d->panel()->frame();
    CHECK(p.w == 720 && p.h == 540);
    CHECK(p.x == 240 && p.y == 130);
    CHECK(d->page() == SettingsDialog::Page::Appearance);
    h.sh->openSettings();
    pump();
    CHECK(h.sh->settingsDialog() == d);

    h.key(plat::Key::Escape);
    CHECK(h.sh->settingsDialog() == nullptr);
    CHECK(h.win->topPopup() == nullptr);

    h.open();
    h.clickAt(600, 400);
    CHECK(h.sh->settingsDialog() != nullptr);
    h.clickAt(40, 40);
    CHECK(h.sh->settingsDialog() == nullptr);
}

TEST("settings: the pages and their sections, in order") {
    Harness         h;
    SettingsDialog &d = h.open();
    // Rows are 41 px apart under 8 px of padding: row 4 is System.
    const ui::RectF r = d.sections().windowRect();
    h.clickAt(r.x + 40, r.y + 8 + 41 * 4 + 20);
    CHECK(d.page() == SettingsDialog::Page::System);
    h.key(plat::Key::Down);
    CHECK(d.page() == SettingsDialog::Page::About);
    h.key(plat::Key::Home);
    CHECK(d.page() == SettingsDialog::Page::Appearance);

    static const struct {
        SettingsDialog::Page page;
        const char          *items[13];
    } kPages[] = {
        {SettingsDialog::Page::Appearance,
         {"Color mode",
          "Color theme",
          "Light theme",
          "Dark theme",
          "Font size",
          "Language",
          "Date/Time",
          "Threads",
          "Link previews",
          "Composer",
          "Formatting",
          "Conversations",
          "Visual effects"}},
        {SettingsDialog::Page::Notifications,
         {"Enable desktop notifications",
          "All new messages",
          "Direct messages and mentions only",
          "Notify me when a huddle starts",
          "Highlight mentions-only channels for any new message",
          "Play a sound for notifications",
          "Sound:",
          "msga chime",
          "Sample notifications",
          "New DM",
          "Test",
          "Save"}},
        {SettingsDialog::Page::Ai,
         {"AI provider",
          "Anthropic",
          "OpenAI",
          "Add OpenAI-compatible server\xE2\x80\xA6",
          "Your language",
          "Native language:",
          "English",
          "Voice input",
          "Glossary",
          "Clean up with AI"}},
        {SettingsDialog::Page::Storage,
         {"Cache",
          "Cache size:",
          "Limit cache to",
          "250 MB",
          "Clear cache",
          "State",
          "Clear state"}},
        {SettingsDialog::Page::System,
         {"Version",
#ifdef MSGA_SELF_UPDATE // built without it: no update checks
          "Check for updates automatically",
          "Check for updates",
#endif
          "Window",
          "Use tray",
#ifndef __APPLE__ // macOS minimizes to the Dock
          "Minimize to tray",
#endif
          "Presence",
          "Slack connection",
          "Import Slack session\xE2\x80\xA6",
          "GIF picker",
          "API key",
          "Memory"}},
        {SettingsDialog::Page::About,
         {"License", "View full license", "Contact", "Found a bug?", "Report a bug"}},
    };
    for (const auto &pg : kPages) {
        d.showPage(pg.page);
        pump();
        float lastY = -1;
        for (const char *item : pg.items) {
            if (!item)
                break;
            ui::View *v = d.find(item);
            if (!CHECK(v != nullptr))
                std::fprintf(stderr, "  missing: %s\n", item);
            else if (std::strcmp(item, "Test") != 0 && std::strcmp(item, "Save") != 0) {
                // Top to bottom as listed (rows side by side may share a y).
                const float y = v->mapToWindow({0, 0}).y + d.scroll().scrollOffset();
                if (!CHECK(y >= lastY - 8)) // a label and its control share a row
                    std::fprintf(stderr, "  out of order: %s\n", item);
                lastY = y;
            }
        }
    }
    // Nothing of Microsoft Teams (dropped from next).
    d.showPage(SettingsDialog::Page::System);
    pump();
    CHECK(d.find("Microsoft Teams") == nullptr);
    CHECK(d.find("Client ID") == nullptr); // the app-keys box is hidden in session mode
}

TEST("settings: color mode and theme cards apply and persist at once") {
    const std::string path = testPath();
    file::remove(path);
    Harness         h(path);
    SettingsDialog &d    = h.open();
    ui::View       *dark = d.find("Dark");
    REQUIRE(dark != nullptr);
    h.click(dark);
    CHECK(app().themeMode() == ui::ThemeMode::Dark);
    CHECK(h.settings.theme == ui::ThemeMode::Dark);
    CHECK(h.sh->settingsDialog() == &d); // survived the restyle
    h.click(d.find("Light"));
    CHECK(app().themeMode() == ui::ThemeMode::Light);

    // A light-row pick recolours the chrome now; a dark-row pick waits.
    const ui::Color purple = ui::color(ui::C::Rail);
    ui::View       *green  = d.find("Light Green");
    REQUIRE(green != nullptr);
    h.click(green);
    CHECK(h.settings.paletteLight == int(ui::Palette::Green));
    CHECK(ui::color(ui::C::Rail) != purple);
    CHECK(ui::color(ui::C::Rail) == ui::paletteColors(ui::Palette::Green, false).rail);
    CHECK(shell::Settings::load(path).paletteLight == int(ui::Palette::Green));
    const ui::Color rail = ui::color(ui::C::Rail);
    h.click(d.find("Dark Blue"));
    CHECK(h.settings.paletteDark == int(ui::Palette::Blue));
    CHECK(ui::color(ui::C::Rail) == rail);
    CHECK(ui::colorIn(ui::C::Rail, true) == ui::paletteColors(ui::Palette::Blue, true).rail);
    // Custom shows its editor; an imported Slack theme recolours it.
    CHECK(d.find("Custom theme") == nullptr);
    h.click(d.find("Light Custom"));
    CHECK(d.find("Custom theme") != nullptr);
    auto *imp = static_cast<ui::TextField *>(d.find("Paste a Slack theme (colour list or JSON)"));
    REQUIRE(imp != nullptr);
    imp->setText("#1A2B3C,#350D36,#1164A3,#FFFFFF,#350D36,#FFFFFF,#2BAC76,#CD2553");
    h.click(d.find("Import"));
    CHECK(d.find("Theme imported") != nullptr);
    CHECK(h.settings.custom.primary == 0xff1a2b3cu);
    CHECK(ui::color(ui::C::Rail) == 0xff1a2b3cu);
    CHECK(shell::Settings::load(path).custom.primary == 0xff1a2b3cu);
    // A 10-colour string pins what it names outright.
    imp->setText("#1A2B3C,#222222,#1164A3,#EEEEEE,#333333,#DDDDDD,#2BAC76,#CD2553,#101010,#C0C0C0");
    h.click(d.find("Import"));
    CHECK(ui::color(ui::C::SidebarHover) == 0xff333333u);
    CHECK(ui::color(ui::C::SidebarText) == 0xffddddddu);
    CHECK(ui::color(ui::C::SidebarSelectedText) == 0xffeeeeeeu);
    CHECK(ui::color(ui::C::TitleBar) == 0xff101010u);
    CHECK(ui::color(ui::C::TitleBarControl) == 0xffc0c0c0u);
    const shell::Settings back = shell::Settings::load(path);
    CHECK(back.custom.itemHover == 0xff333333u && back.custom.itemText == 0xffddddddu);
    CHECK(back.custom.itemSelText == 0xffeeeeeeu && back.custom.titleBarBg == 0xff101010u);
    CHECK(back.custom.titleBarText == 0xffc0c0c0u);
    // ia_theme JSON names no pins: the derived colours come back.
    imp->setText(R"({"primary":{"palette":"aubergine"},"highlight1":{"palette":"jade"}})");
    h.click(d.find("Import"));
    CHECK(h.settings.custom.itemHover == 0 && h.settings.custom.titleBarBg == 0);
    CHECK(ui::color(ui::C::TitleBar) == ui::color(ui::C::Rail));
    CHECK(ui::color(ui::C::SidebarText) == 0xffffffffu);
    imp->setText("not a theme");
    h.click(d.find("Import"));
    CHECK(
        d.find("Not a Slack theme. Paste 8 or 10 colours separated by commas, or theme JSON.") !=
        nullptr
    );
    file::remove(path);
}

TEST("settings: the Markdown formatting choice reaches the composer on Save") {
    const std::string path = testPath();
    file::remove(path);
    Harness         h(path);
    SettingsDialog &d = h.open();
    CHECK(d.find("Slack (*bold*, _italic_)") != nullptr);
    h.click(d.find("Markdown (**bold**, *italic*)"));
    CHECK(d.draft().markdownFormatting);
    CHECK_FALSE(shell::markdownFormatting()); // until Save
    ui::View *save = d.find("Save");
    REQUIRE(save != nullptr);
    h.click(save);
    CHECK(h.settings.markdownFormatting);
    CHECK(shell::markdownFormatting());
    auto &c = h.sh->composer();
    c.edit().insertText("*it* **bold**");
    CHECK_STR(mrkdwn::compose(c.mrkdwn()).mrkdwn, "_it_ *bold*");
    CHECK(shell::Settings::load(path).markdownFormatting); // saved
}

TEST("settings: Appearance changes wait for Save, which applies them and closes") {
    const std::string path = testPath();
    file::remove(path);
    Harness         h(path);
    SettingsDialog &d = h.open();
    h.click(d.find("Large"));
    h.click(d.find("24-hour clock (14:34)"));
    h.click(d.find("Show link previews"));
    h.click(d.find("Display names"));
    auto *days = static_cast<ui::SpinBox *>(d.find("14 days"));
    REQUIRE(days != nullptr);
    days->focus();
    h.key(plat::Key::Up);
    h.key(plat::Key::Up);
    CHECK(days->value() == 16);
    CHECK(d.draft().relevantDays == 16);
    CHECK(d.draft().fontSize == 2 && d.draft().use24h && !d.draft().linkPreviews);
    CHECK(d.draft().names == 2);
    CHECK(h.settings.names == 0);
    // A new language: the restart note says dates follow at once.
    auto *lang = static_cast<ui::Dropdown *>(d.find("System default"));
    REQUIRE(lang != nullptr);
    lang->onChange(2); // 日本語
    pump();
    CHECK_STR(d.draft().language, "ja");
    // Nothing applied or saved yet.
    CHECK_STR(base::dateLanguage(), "en");
    CHECK(h.settings.fontSize == 1);
    CHECK_FALSE(h.settings.use24h);
    CHECK(app().userTextScale() == 1.f);
    // The draft survives switching pages.
    d.showPage(SettingsDialog::Page::About);
    pump();
    d.showPage(SettingsDialog::Page::Appearance);
    pump();
    CHECK(static_cast<ui::SpinBox *>(d.find("16 days")) != nullptr);

    ui::View *save = d.find("Save");
    REQUIRE(save != nullptr);
    d.scroll().ensureVisible(save);
    pump();
    h.click(save);
    CHECK(h.sh->settingsDialog() == nullptr);
    CHECK(h.settings.fontSize == 2);
    CHECK(app().userTextScale() > 1.1f);
    CHECK(h.settings.use24h);
    CHECK(base::use24h());
    // …and dates are Japanese now, before any restart.
    CHECK_STR(base::dateLanguage(), "ja");
    CHECK_STR(
        base::formatDate(base::fromLocal(2026, 3, 15, 12, 0), base::fromLocal(2026, 9, 21, 12, 0)),
        "3\xE6\x9C\x88"
        "15\xE6\x97\xA5"
    );
    CHECK_FALSE(h.ctx.linkPreviews);
    CHECK(h.settings.relevantDays == 16);
    const shell::Settings r = shell::Settings::load(path);
    CHECK(r.fontSize == 2 && r.use24h && !r.linkPreviews && r.relevantDays == 16);
    CHECK(h.settings.names == 2 && r.names == 2);
    file::remove(path);
}

TEST("settings: unread-only greys out the activity window; typing digits sets it") {
    Harness         h;
    SettingsDialog &d    = h.open();
    auto           *days = static_cast<ui::SpinBox *>(d.find("14 days"));
    REQUIRE(days != nullptr);
    CHECK(days->enabled());
    h.click(d.find("Show only unread conversations"));
    CHECK_FALSE(days->enabled());
    h.click(d.find("Show only unread conversations"));
    CHECK(days->enabled());
    days->focus();
    pump();
    plat::Event t;
    t.type = plat::EventType::TextInput;
    t.text = "30";
    ui::Event e{ui::EventType::TextInput};
    e.raw = &t;
    days->onEvent(e);
    h.key(plat::Key::Enter);
    CHECK(days->value() == 30);
    CHECK(d.draft().relevantDays == 30);
    // Clamped to the range.
    t.text = "999";
    days->onEvent(e);
    h.key(plat::Key::Enter);
    CHECK(days->value() == 365);
}

TEST("settings: notification level, and the master switch greys out the rest") {
    Harness         h;
    SettingsDialog &d = h.open();
    d.showPage(SettingsDialog::Page::Notifications);
    pump();
    ui::View *master = d.find("Enable desktop notifications");
    ui::View *huddle = d.find("Notify me when a huddle starts");
    REQUIRE(master != nullptr);
    REQUIRE(huddle != nullptr);
    h.click(master);
    CHECK_FALSE(huddle->enabled());
    CHECK_FALSE(d.find("All new messages")->enabled());
    CHECK(d.find("Highlight mentions-only channels for any new message")->enabled());
    h.click(master);
    h.click(d.find("Direct messages and mentions only"));
    CHECK(h.settings.notifyLevel == 0); // until Save
    h.click(d.find("Save"));
    CHECK(h.settings.notifyLevel == 1);
    CHECK(h.sh->settingsDialog() == nullptr);

    // Level 1: a plain channel message stays quiet, a mention notifies.
    h.sh->setLive(true);
    const ConvRef eng = h.store.findConversation("C0ENG");
    REQUIRE(eng != kNoConv);
    UserRef other = kNoUser;
    for (UserRef u = 0; u < h.store.userCount() && other == kNoUser; ++u)
        if (u != h.store.me)
            other = u;
    Ts   ts   = 1900000000000000;
    auto post = [&](std::string text) {
        Message m;
        m.ts   = ++ts;
        m.user = other;
        m.text = std::move(text);
        h.store.addMessage(eng, std::move(m));
        pump();
    };
    const int before = notificationCount();
    post("a channel message");
    CHECK(notificationCount() == before);
    post("hey <@" + std::string(h.store.user(h.store.me).id) + ">");
    CHECK(notificationCount() == before + 1);
    h.settings.notifyLevel = 0;
    post("another channel message");
    CHECK(notificationCount() == before + 2);
    h.settings.notifications = false;
    post("with notifications off");
    CHECK(notificationCount() == before + 2);
}

#ifdef __linux__
TEST(
    "settings: the sound dropdown lists the chime and the OS's sounds; Test and notifications play "
    "it"
) {
    // Fake audio helpers (plat/audio.h finds them only in $PLAT_AUDIO_HELPERS)
    // and a sound theme of two sounds. pw-play logs what it was asked to play.
    const std::string dir    = base::test::makeTempDir("msga-settings-sound-"),
                      played = dir + "/played";
    REQUIRE(!dir.empty());
    file::writeAtomic(dir + "/pw-play", "#!/bin/sh\necho \"$@\" >> '" + played + "'\n", 0755);
    const std::string theme = dir + "/data/sounds/freedesktop/stereo";
    REQUIRE(file::makeDirs(theme));
    file::writeAtomic(theme + "/bell.oga", "x");
    file::writeAtomic(theme + "/message-new-instant.oga", "x");
    const std::string oldData = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME") : "";
    base::test::setEnv("PLAT_AUDIO_HELPERS", dir);
    base::test::setEnv("XDG_DATA_HOME", dir + "/data");
    auto log = [&] {
        std::string s;
        file::readAll(played, &s);
        return s;
    };
    {
        Harness         h;
        SettingsDialog &d = h.open();
        d.showPage(SettingsDialog::Page::Notifications);
        pump();
        auto *pick = static_cast<ui::Dropdown *>(d.find("msga chime"));
        REQUIRE(pick != nullptr);
        // The OS's sounds arrive from a worker thread.
        for (int i = 0; i < 500 && pick->options().size() < 3; ++i)
            app().pump(5);
        REQUIRE(pick->options().size() == 3);
        CHECK_STR(pick->options()[0], "msga chime");
        CHECK_STR(pick->options()[1], "Bell");
        CHECK_STR(pick->options()[2], "Message new instant");
        // Chosen from the menu (a divider follows the chime).
        h.click(pick);
        REQUIRE(pick->menu() != nullptr);
        h.key(plat::Key::Down);
        h.key(plat::Key::Enter);
        CHECK_STR(pick->accessibleName(), "Bell");
        ui::View *test = d.find("Test"); // the sound's, above the sample notification's
        REQUIRE(test != nullptr);
        CHECK(test->enabled());
        h.click(test);
        for (int i = 0; i < 500 && log().empty(); ++i)
            app().pump(5);
        CHECK_STR(log(), theme + "/bell.oga\n");
        CHECK_STR(h.settings.soundId, "bundled:notify"); // until Save
        h.click(d.find("Save"));
        CHECK_STR(h.settings.soundId, "system:bell");

        // A notification plays it; the OS's own sound is off on macOS
        // only.
        h.sh->setLive(true);
        const ConvRef eng = h.store.findConversation("C0ENG");
        REQUIRE(eng != kNoConv);
        UserRef other = kNoUser;
        for (UserRef u = 0; u < h.store.userCount() && other == kNoUser; ++u)
            if (u != h.store.me)
                other = u;
        const int before = notificationCount();
        Message   m;
        m.ts   = 1900000000000001;
        m.user = other;
        m.text = "ding";
        h.store.addMessage(eng, std::move(m));
        pump();
        CHECK(notificationCount() == before + 1);
        for (uint64_t id = 1; id < 4096; ++id) {
            plat::TestHooks::NotificationProbe p;
            if (hooks().notificationProbe(id, &p) && p.body.find("ding") != std::string::npos)
                CHECK(p.silent == sounds::kSilentNotifications);
        }
        for (int i = 0; i < 500 && log() == theme + "/bell.oga\n"; ++i)
            app().pump(5);
        CHECK_STR(log(), theme + "/bell.oga\n" + theme + "/bell.oga\n");
        // Sound off: the notification comes, silently.
        h.settings.notifySound = false;
        m.ts                   = 1900000000000002;
        m.user                 = other;
        m.text                 = "quiet";
        h.store.addMessage(eng, std::move(m));
        pump();
        CHECK(notificationCount() == before + 2);
        for (int i = 0; i < 40; ++i)
            app().pump(5);
        CHECK_STR(log(), theme + "/bell.oga\n" + theme + "/bell.oga\n");
    }
    base::test::setEnv("PLAT_AUDIO_HELPERS", "/nonexistent/msga-test-audio");
    if (oldData.empty())
        base::test::unsetEnv("XDG_DATA_HOME");
    else
        base::test::setEnv("XDG_DATA_HOME", oldData);
    (void)!std::system(("rm -rf '" + dir + "'").c_str());
}
#endif

TEST("settings: AI providers connect through the inline editor") {
    const std::string path = testPath();
    file::remove(path);
    Harness         h(path);
    SettingsDialog &d = h.open();
    d.showPage(SettingsDialog::Page::Ai);
    pump();
    CHECK(
        d.find("Voice input needs an OpenAI or OpenAI-compatible provider with speech-to-text.") !=
        nullptr
    );
    ui::View *connect = d.find("Connect");
    REQUIRE(connect != nullptr);
    h.click(connect); // Anthropic's
    CHECK(d.find("Connect Anthropic") != nullptr);
    ui::View *save = nullptr;
    for (const char *n : {"Save"})
        save = d.find(n);
    REQUIRE(save != nullptr);
    h.click(save);
    CHECK(d.find("Paste your API key") != nullptr); // the error (and the placeholder)
    auto *key = static_cast<ui::TextField *>(d.find("Paste your API key"));
    REQUIRE(key != nullptr);
    CHECK(key->masked());
    key->setText("sk-ant-test9876");
    h.click(save);
    CHECK_STR(h.settings.provider("anthropic")->key, "sk-ant-test9876");
    CHECK(
        d.find(
            "claude-opus-5 \xC2\xB7 Connected (\xE2\x80\xA6"
            "9876)"
        ) != nullptr
    );
    CHECK(d.find("Disconnect") != nullptr);
    CHECK_STR(shell::Settings::load(path).provider("anthropic")->key, "sk-ant-test9876");
    // Anthropic has no speech-to-text: the warning stays.
    CHECK(
        d.find("Voice input needs an OpenAI or OpenAI-compatible provider with speech-to-text.") !=
        nullptr
    );

    // A custom server needs a URL and a model.
    h.click(d.find("Add OpenAI-compatible server\xE2\x80\xA6"));
    CHECK(d.find("Add OpenAI-compatible server") != nullptr);
    auto *url = static_cast<ui::TextField *>(d.find("http://localhost:8000/v1"));
    REQUIRE(url != nullptr);
    // A LAN server is not warned about; a remote one typed without a scheme is.
    url->setText("192.168.1.20:8000");
    pump();
    CHECK(
        d.find("Unencrypted connection \xE2\x80\x94 the API key is sent in plain text.") == nullptr
    );
    url->setText("garbage:// x");
    h.click(d.find("Save"));
    CHECK(d.find("Enter the server URL (for example http://localhost:8000/v1)") != nullptr);
    url->setText("gpu.example.com:8000/chat/completions");
    pump();
    CHECK(
        d.find("Unencrypted connection \xE2\x80\x94 the API key is sent in plain text.") != nullptr
    );
    h.click(d.find("Save"));
    CHECK(d.find("Enter a model name, or fetch the list from the server") != nullptr);
    static_cast<ui::TextField *>(d.find("Model name"))->setText("qwen3");
    h.click(d.find("Save"));
    const shell::AiProvider *c = h.settings.provider("custom-1");
    REQUIRE(c != nullptr);
    CHECK_STR(c->name, "gpu.example.com");
    CHECK_STR(c->model, "qwen3");
    CHECK(d.find("qwen3 \xC2\xB7 http://gpu.example.com:8000/v1") != nullptr);
    CHECK(
        d.find("Voice input needs an OpenAI or OpenAI-compatible provider with speech-to-text.") ==
        nullptr
    );
    // Removing the default server clears the default: the next server added
    // (which reuses the id) doesn't inherit it.
    h.settings.aiDefault = "custom-1";
    h.click(d.find("Remove"));
    CHECK(h.settings.provider("custom-1") == nullptr);
    CHECK(h.settings.aiDefault.empty());
    file::remove(path);
}

TEST("settings: System and Storage controls persist at once") {
    const std::string path = testPath();
    file::remove(path);
    Harness         h(path);
    SettingsDialog &d = h.open();
    d.showPage(SettingsDialog::Page::System);
    pump();
#ifndef __APPLE__ // not offered on macOS
    h.click(d.find("Minimize to tray"));
    CHECK(h.settings.minimizeToTray);
    // Without the tray it is greyed out and shown off, the choice kept.
    auto *minimize = static_cast<ui::CheckBox *>(d.find("Minimize to tray"));
    h.click(d.find("Use tray"));
    CHECK_FALSE(h.settings.useTray);
    CHECK_FALSE(minimize->enabled());
    CHECK_FALSE(minimize->checked());
    CHECK(h.settings.minimizeToTray);
    h.click(d.find("Use tray"));
    CHECK(h.settings.useTray);
    CHECK(minimize->enabled());
    CHECK(minimize->checked());
#else
    h.click(d.find("Use tray"));
    CHECK_FALSE(h.settings.useTray);
    h.click(d.find("Use tray"));
#endif
    h.click(d.find("Leave my presence to the official Slack apps"));
    CHECK(h.settings.presence == 2);
    CHECK(d.find("Restart msga to apply this change.") == nullptr);
    h.click(d.find("Slack app keys \xE2\x80\x94 OAuth sign-in with live message push"));
    CHECK_FALSE(h.settings.slackSession);
    CHECK(d.find("Restart msga to apply this change.") != nullptr);
    CHECK(d.find("Client ID") != nullptr);
    CHECK(d.find("Import Slack session\xE2\x80\xA6") == nullptr);

    auto *gif = static_cast<ui::TextField *>(d.find("Paste your GIPHY API key"));
    REQUIRE(gif != nullptr);
    CHECK(gif->masked());
    h.click(gif->eye());
    CHECK_FALSE(gif->masked());
    gif->setText("  giphy-key ");
    ui::View                       *save = nullptr;
    // The GIF picker's Save is the last "Save" on the page.
    std::function<void(ui::View *)> walk = [&](ui::View *v) {
        if (v->visible() && v->accessibleName() == "Save")
            save = v;
        for (size_t i = 0; i < v->childCount(); ++i)
            walk(v->child(i));
    };
    walk(d.scroll().content());
    REQUIRE(save != nullptr);
    d.scroll().ensureVisible(save);
    pump();
    h.click(save);
    CHECK_STR(h.settings.giphyKey, "giphy-key");
    CHECK(d.find("Key saved.") != nullptr);
    const shell::Settings r = shell::Settings::load(path);
#ifndef __APPLE__
    CHECK(r.minimizeToTray);
#endif
    CHECK(r.presence == 2 && !r.slackSession);
    CHECK_STR(r.giphyKey, "giphy-key");
    CHECK(d.find("RAM used: \xE2\x80\x94") == nullptr); // a live number

    d.showPage(SettingsDialog::Page::Storage);
    pump();
    auto *clear = d.find("Clear cache");
    REQUIRE(clear != nullptr);
    h.click(clear);
    CHECK_FALSE(clear->enabled());
    // The wipe and the walk run on a worker; the size shows when done.
    for (int i = 0; i < 2000 && !d.find("0 B"); ++i)
        app().pump(2);
    CHECK(d.find("0 B") != nullptr);
    file::remove(path);
}

#ifdef MSGA_SELF_UPDATE
TEST("settings: the last update check reads as a relative time") {
    Harness h;
    h.settings.lastUpdateCheck = 0;
    SettingsDialog &d          = h.open();
    d.showPage(SettingsDialog::Page::System);
    pump();
    CHECK(d.find("Last checked: Never checked") != nullptr);
    h.key(plat::Key::Escape);
    h.settings.lastUpdateCheck = base::nowSecs() - 5 * 60 - 10;
    SettingsDialog &d2         = h.open();
    d2.showPage(SettingsDialog::Page::System);
    pump();
    CHECK(d2.find("Last checked: 5 minutes ago") != nullptr);
    h.key(plat::Key::Escape);
    h.settings.lastUpdateCheck = base::nowSecs() - 86400 - 10;
    SettingsDialog &d3         = h.open();
    d3.showPage(SettingsDialog::Page::System);
    pump();
    CHECK(d3.find("Last checked: 1 day ago") != nullptr);
}
#endif

TEST("settings: Send with Ctrl+Enter makes Enter a new line") {
    Harness h;
    h.settings.ctrlEnterSends = true;
    h.sh->applySettings();
    ui::TextEdit &edit = h.sh->composer().edit();
    edit.focus();
    edit.setText("hello");
    edit.setSelection(5, 5);
    const size_t before = h.store.conversation(h.sh->current()).messages.size();
    h.key(plat::Key::Enter);
    CHECK_STR(edit.text(), "hello\n");
    ui::Event e{ui::EventType::KeyDown};
    e.key = plat::Key::Enter;
#ifdef __APPLE__
    e.mods = plat::ModSuper;
#else
    e.mods = plat::ModCtrl;
#endif
    CHECK(edit.onKey && edit.onKey(e));
    pump();
    CHECK(edit.empty());
    CHECK(h.store.conversation(h.sh->current()).messages.size() == before + 1);
    h.settings.ctrlEnterSends = false;
    h.sh->applySettings();
}

namespace {
bool until(const std::function<bool()> &done, int ms = 10000) {
    const double end = app().nowMs() + ms;
    while (!done()) {
        if (app().nowMs() > end)
            return false;
        app().pump(2);
    }
    return true;
}
} // namespace

TEST("settings: Test connection and Fetch models ask the server for its models") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake AI provider\n");
        return;
    }
    fakellm::ctl(app().platform(), "POST", "/_ctl/reset");
    Harness      h;
    llm::Service ai(app().platform());
    h.ctx.ai          = &ai;
    SettingsDialog &d = h.open();
    d.showPage(SettingsDialog::Page::Ai);
    pump();
    h.click(d.find("Add OpenAI-compatible server\xE2\x80\xA6"));
    auto *url   = static_cast<ui::TextField *>(d.find("http://localhost:8000/v1"));
    auto *model = static_cast<ui::TextField *>(d.find("Model name"));
    auto *key   = static_cast<ui::TextField *>(d.find("Optional"));
    REQUIRE(url && model && key);
    ui::View *test  = d.find("Test connection");
    ui::View *fetch = d.find("Fetch models");
    REQUIRE(test && fetch);
    CHECK(test->enabled() && fetch->enabled());
    h.click(test);
    CHECK(d.find("Enter the server URL first") != nullptr);

    url->setText(base + "/v1/");
    key->setText("sk-local");
    static_cast<ui::Button *>(test)->onClick(); // no pumping: the answer is still out
    CHECK(d.find("Connecting\xE2\x80\xA6") != nullptr);
    CHECK_FALSE(test->enabled()); // until the answer
    const auto reached = [&] {
        return d.find("Reached the server \xE2\x80\x94 2 models available") != nullptr;
    };
    REQUIRE(until(reached));
    CHECK(test->enabled() && fetch->enabled());
    CHECK(model->text().empty()); // a test fills nothing in
    h.click(fetch);
    REQUIRE(until([&] { return !model->text().empty(); }));
    CHECK_STR(model->text(), "fake-small"); // an empty field takes the first
    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app().platform(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 2);
    CHECK_STR(log.root()[0]["path"].str(), "/v1/models");
    CHECK_STR(log.root()[0]["headers"]["authorization"].str(), "Bearer sk-local");

    // A refused key: the server's words in the error line.
    fakellm::script(
        app().platform(), "/v1/models", R"([{"__status":401,"error":{"message":"Bad key"}}])"
    );
    h.click(test);
    REQUIRE(until([&] { return d.find("Bad key") != nullptr; }));
    CHECK(test->enabled());
    h.ctx.ai = nullptr;
}

#ifdef __linux__
TEST("settings: Check spelling lists the dictionaries; the ticked ones are saved") {
    // Two made-up dictionaries on $DICPATH, beside whatever the system has.
    const std::string dir =
        app().platform().standardDir(plat::StandardDir::Temp) + "/msga-settings-dicts";
    file::makeDirs(dir);
    for (const char *code : {"xx_AA", "yy_BB"}) {
        REQUIRE(file::writeAtomic(dir + "/" + code + ".aff", "SET UTF-8\n"));
        REQUIRE(file::writeAtomic(dir + "/" + code + ".dic", "1\nzorp\n"));
    }
    base::test::setEnv("DICPATH", dir);
    {
        Harness         h;
        SettingsDialog &d = h.open();
        CHECK(
            d.find("A word is underlined when none of the checked languages knows it.") == nullptr
        ); // listed only while it is on
        ui::View *spellBox = d.find("Check spelling");
        REQUIRE(spellBox != nullptr);
        d.scroll().ensureVisible(spellBox);
        pump();
        h.click(spellBox);
        REQUIRE(until([&] { return d.find("xx_AA") != nullptr; }));
        CHECK(d.find("yy_BB") != nullptr);
        CHECK(
            d.find("A word is underlined when none of the checked languages knows it.") != nullptr
        );
        // yy_BB, not xx_AA: with no system dictionary for the locale (a bare
        // container) the default is the first one listed, xx_AA, already ticked.
        ui::View *yy = d.find("yy_BB");
        d.scroll().ensureVisible(yy);
        pump();
        h.click(yy);
        ui::View *save = d.find("Save");
        REQUIRE(save != nullptr);
        d.scroll().ensureVisible(save);
        pump();
        h.click(save);
        CHECK(h.settings.spellCheck);
        CHECK(
            std::find(
                h.settings.spellLanguages.begin(), h.settings.spellLanguages.end(), "yy_BB"
            ) != h.settings.spellLanguages.end()
        );
        // Applied: the checker loads them.
        REQUIRE(until([] { return spell::Checker::instance().active(); }));
    }
    spell::Checker::instance().configure(app().platform(), false, {});
    CHECK(!spell::Checker::instance().active());
    base::test::unsetEnv("DICPATH");
    for (const char *f : {"xx_AA.aff", "xx_AA.dic", "yy_BB.aff", "yy_BB.dic"})
        file::remove(dir + "/" + f);
    file::remove(dir);
}
#endif
