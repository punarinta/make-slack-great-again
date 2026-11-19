// The Settings dialog: an in-window modal (ui::Dialog) with the section list
// on the left and the chosen page on the right:
//
//   Appearance     color mode, color theme (palettes + custom), font size,
//                  language, date/time, threads, link previews, composer,
//                  conversations, visual effects, tray icon — then Save
//   Notifications  on/off, level, huddles, highlight, sound, samples — Save
//   AI assistance  providers (inline editor), your language, voice input
//   Storage        cache (size, limit, clear), state
//   System         version/updates, window, presence, Slack connection,
//                  GIF picker key, memory
//   About          license, contact, bug reports
//
// The color mode, the theme cards and the tray icon apply
// and persist at once, as do the AI, Storage and System pages; the rest of
// Appearance and Notifications is kept in a draft until that page's Save
// (which also closes the dialog). Hooks::changed fires after every write to
// shell::Settings: the shell saves the file and applies what it owns.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/settings.h"
#include "ui/ui.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace update {
class Updater;
}

namespace settings {

class SettingsDialog final : public ui::Dialog {
public:
    enum class Page : uint8_t { Appearance, Notifications, Ai, Storage, System, About, Count };

    struct Hooks {
        std::function<void()> changed; // shell::Settings changed: persist and apply
        // System → Slack connection (unset: the buttons show disabled).
        std::function<void()> importSlackSession;       // "Import Slack session…"
        std::function<void()> convertToSession;         // "Convert them to session"
        int                   oauthSlackWorkspaces = 0; // Slack workspaces on app keys
#ifdef MSGA_SELF_UPDATE
        // System → Version: "Check for updates" (null: "Update checks not
        // available.").
        update::Updater *updater = nullptr;
#endif
        // System → "Save and restart" (the Slack app keys).
        std::function<void()>                            restart;
        // Appearance → Tray icon: the tray icon dialog; it updates the
        // settings itself, then calls `after` (also when cancelled).
        std::function<void(std::function<void()> after)> pickTrayIcon;
        // Storage → "Clear state": the sidebar's visit stamps are gone.
        std::function<void()>                            clearState;
        // Notifications → "Test": shows the sample notification; `result`
        // gets what the OS said about it,
        // possibly later. Unset: the dialog submits it itself.
        std::function<void(plat::Notification n, std::function<void(const std::string &)> result)>
            testNotification;
        // Appearance → Custom theme → "Use my Slack theme" (shown only when
        // set: a signed-in Slack workspace can read the account's theme
        // prefs): done(iaTheme, legacyValues, error).
        using SlackThemeDone =
            std::function<void(std::string iaTheme, std::string legacy, std::string error)>;
        std::function<void(SlackThemeDone)>                              fetchSlackTheme;
        // System → Claude Code → "Runs on" (shown only when both are set: a
        // Claude Code workspace exists). claudeHost: its SSH destination (""
        // = this computer) and msga's path there; setClaudeHost saves a new
        // one and starts the workspace over on it.
        std::function<void(std::string *destination, std::string *msga)> claudeHost;
        std::function<void(const std::string &destination, const std::string &msga)> setClaudeHost;
    };

    SettingsDialog(
        screens::Context &ctx, shell::Settings &settings, Hooks hooks, Page first = Page::Appearance
    );
    ~SettingsDialog() override;

    void                   showPage(Page p);
    Page                   page() const { return _page; }
    ui::SectionList       &sections() { return *_sections; }
    ui::ScrollView        &scroll() { return *_scroll; }
    // The first visible view on the current page whose accessible name is
    // `name` (a checkbox's label, a button's text, a heading) — for tests.
    ui::View              *find(std::string_view name) const;
    // The unsaved Appearance/Notifications choices (tests).
    const shell::Settings &draft() const { return _draft; }

    void styleChanged() override; // the OS theme flipped: "currently dark"

private:
    // Building blocks (settings_dialog.cpp).
    ui::Label    *heading(ui::View *parent, const char *text);
    ui::Label    *caption(ui::View *parent, std::string_view text, ui::C c = ui::C::FormTextMuted);
    ui::Label    *body(ui::View *parent, std::string_view text, ui::C c = ui::C::FormText);
    ui::Label    *link(ui::View *parent, std::string text, std::string url);
    ui::View     *group(ui::View *parent, float gap = 8);
    ui::View     *row(ui::View *parent, float gap = 8);
    ui::Button   *button(ui::View *parent, const char *label, ui::Button::Kind kind);
    ui::CheckBox *check(ui::View *parent, const char *label, bool *field, bool draft);
    ui::RadioGroup *
    radios(ui::View *parent, std::initializer_list<const char *> options, int *field);
    ui::RadioGroup *
    boolRadios(ui::View *parent, std::initializer_list<const char *> options, bool *field);
    ui::TextField *fieldRow(ui::View *parent, const char *label, std::string placeholder);
    void           saveButton(void (SettingsDialog::*save)());
    void           changed();

    // Pages.
    void buildAppearance();
    void buildNotifications();
    void buildAi();
    void buildStorage();
    void buildSystem();
    void buildClaudeHost(); // System → Claude Code: where its sessions run
    void buildAbout();

    // Appearance.
    void buildThemeRow(bool dark);
    void buildCustomEditor();
    void refreshCustomSection();
    void pickTrayIcon();
    void refreshSpellLanguages();
    void saveAppearance();
    // Notifications.
    void syncNotifyEnabled();
    void saveNotifications();
    // AI.
    void refreshAiList();
    void showAiEditor(const std::string &id); // "" = a new custom server
    void saveAiEditor();
    void probeAiEditor(bool fillModels); // "Test connection" / "Fetch models"
    void refreshVoiceHint();
    void saveGlossary();
    // Storage / System.
    void refreshCache();
    void refreshRam();

    struct Parts; // the current page's live widgets (settings_dialog.cpp)

    screens::Context      &_ctx;
    shell::Settings       &_s;
    shell::Settings        _draft; // Appearance + Notifications until Save
    Hooks                  _hooks;
    ui::SectionList       *_sections = nullptr;
    ui::ScrollView        *_scroll   = nullptr;
    ui::View              *_content  = nullptr;
    std::unique_ptr<Parts> _p;
    plat::TimerId          _ramTimer = 0, _glossaryTimer = 0;
#ifdef MSGA_SELF_UPDATE
    int _updListener = 0; // on _hooks.updater (the System page)
#endif
    Page _page = Page::Count;
};

// A Slack theme as text — the ia_theme JSON (with the app's own "gradient"
// and "pins") or the 8/10-colour legacy share string — into a whole custom
// palette; false, `out` untouched, on anything else. Settings → Custom
// theme's Import and "Use my Slack theme", and the import of settings from
// earlier versions.
bool parseSlackTheme(std::string_view text, ui::CustomPalette *out);

} // namespace settings
