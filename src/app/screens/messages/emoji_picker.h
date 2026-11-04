// The emoji picker (reactions and the composer):
// a 354×460 card with the category bar (Frequently used, Smileys & people …
// Flags, Custom), "Search all emoji", a sectioned 9-column grid painted
// virtually (only the rows on screen), and the "Skin tone" selector.
// Typing filters into one "Search results" section with the top hit
// selected; the arrows move a selection (Up/Down by rows, Left/Right at the
// ends of the text) and Enter picks it. A pick is the shortcode, with
// "::skin-tone-N" when a tone is set and the emoji has variants.
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <functional>
#include <string>
#include <vector>

namespace screens {

class EmojiGrid;

class EmojiPicker : public ui::Popup {
public:
    using Pick = std::function<void(const std::string &shortcode)>;
    EmojiPicker(Context &ctx, Pick onPick);
    // Shows it above `anchor` (window coordinates), focused on the search.
    static EmojiPicker *show(ui::Window &w, ui::RectF anchor, Context &ctx, Pick onPick);

    void                     filter(std::string_view query);
    void                     pick(const std::string &name);
    int                      selected() const; // index into the shown cells, -1 = none
    size_t                   cellCount() const;
    const std::string       &cellName(size_t i) const;
    // Section headers top to bottom ("Frequently used", "Smileys & people" …).
    std::vector<std::string> sections() const;
    // The category tabs (one per section with an icon; hidden while searching).
    size_t                   tabCount() const { return _tabs.size(); }
    int                      activeTab() const;
    void                     setSkinTone(int tone); // 0 default, 2-6
    int                      skinTone() const;
    // The recent emoji and the skin tone: the shell hands back what it
    // saved, and is told whenever a pick or a tone changes either.
    using StateObserver = std::function<void(const std::vector<std::string> &recent, int tone)>;
    static void      restoreState(std::vector<std::string> recent, int tone);
    // An emoji by shortcode in `parent`, `size` px: a workspace emoji's
    // picture, else its character (":name:" when unknown).
    static ui::View *addGlyph(ui::View *parent, Context &ctx, const std::string &name, float size);
    static void      setStateObserver(StateObserver fn);
    bool             onEvent(ui::Event &e) override;

private:
    bool searchKey(const ui::Event &e);
    void syncTabs();

    Context                     &_ctx;
    Pick                         _onPick;
    ui::View                    *_catBar = nullptr, *_toneRow = nullptr, *_skinBtn = nullptr;
    ui::TextEdit                *_search = nullptr;
    ui::ScrollView              *_scroll = nullptr;
    EmojiGrid                   *_grid   = nullptr;
    std::vector<ui::Clickable *> _tabs;
    std::vector<int>             _tabSection;
    // The workspace emoji names folded, by Store::customEmojiImages index,
    // for the search (rebuilt only when the workspace's set changes).
    std::vector<std::string>     _customFolded;
    uint64_t                     _customRev = ~uint64_t(0);
    bool                         _searching = false;
};

} // namespace screens
