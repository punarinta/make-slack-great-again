// SettingsDialog internals shared by its two .cpp files: the live widgets of
// the page on screen (reset whenever another page is built).
#pragma once

#include "screens/settings/settings_dialog.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace settings {

// Out of line: a braced list of literals costs a pointer array at the call.
std::vector<std::string> strs(std::initializer_list<const char *> l);
// A link in a page's text: the control font, underlined, link id 1.
text::Style              linkStyle();

struct SettingsDialog::Parts {
    // Appearance
    ui::Label                   *modeHint  = nullptr;
    ui::View                    *themeRows = nullptr, *customSection = nullptr;
    std::vector<ui::Clickable *> cards;
    std::vector<ui::Clickable *> quickSlots; // One-click reactions: Custom's three
    ui::TextField               *importField = nullptr;
    ui::Label                   *contrast = nullptr, *customStatus = nullptr;
    ui::CheckBox                *inverted = nullptr, *gradient = nullptr;
    ui::Label                   *daysLabel = nullptr, *daysDesc = nullptr;
    ui::SpinBox                 *days          = nullptr;
    ui::CheckBox                *trayCheck     = nullptr;
    ui::Button                  *trayChange    = nullptr;
    ui::View                    *spellGrid     = nullptr;
    ui::Label                   *spellHint     = nullptr;
    bool                         spellFilled   = false; // the grid was filled once
    // Notifications
    ui::RadioGroup              *notifyLevel   = nullptr;
    ui::CheckBox                *notifyHuddles = nullptr, *notifySound = nullptr;
    ui::View                    *soundRow = nullptr;
    ui::Dropdown                *sound    = nullptr;
    std::vector<std::string>     soundIds; // the dropdown's options' ids
    ui::Dropdown                *sample       = nullptr;
    ui::Button                  *sampleTest   = nullptr;
    ui::Label                   *sampleResult = nullptr;
    // AI
    ui::View                    *aiList = nullptr, *aiEditor = nullptr;
    ui::Label                   *aiTitle = nullptr, *aiError = nullptr, *aiProbe = nullptr;
    ui::View                    *aiNameRow = nullptr, *aiUrlRow = nullptr, *aiSttRow = nullptr;
    ui::TextField *aiName = nullptr, *aiUrl = nullptr, *aiKey = nullptr, *aiModel = nullptr;
    ui::TextField *aiStt       = nullptr;
    ui::Label     *aiCleartext = nullptr, *aiKeyLink = nullptr, *aiKeyHint = nullptr;
    ui::Dropdown  *aiModelPick = nullptr;
    std::string    aiEditing; // provider id; "" = adding a custom server
    ui::Label     *voiceWarn = nullptr;
    ui::TextField *glossary  = nullptr;
    // Storage / System
    ui::Label     *cacheSize = nullptr, *ramLabel = nullptr, *updStatus = nullptr;
    int            cacheSeq    = 0; // refreshCache's newest walk
    ui::Label     *modeRestart = nullptr, *credStatus = nullptr, *giphyStatus = nullptr;
    ui::View      *sessionBox = nullptr, *appKeysBox = nullptr;
    ui::TextField *credId = nullptr, *credSecret = nullptr, *credXapp = nullptr, *giphy = nullptr;

    // AI: the editor's "Test connection" / "Fetch models" (probeAiEditor). A
    // probe's answer counts only for the edit it was started for, and only
    // while these parts (the page) exist.
    ui::Button           *aiTest = nullptr, *aiFetch = nullptr;
    int                   aiProbeSeq = 0;
    std::shared_ptr<char> alive      = std::make_shared<char>(0);
};

} // namespace settings
