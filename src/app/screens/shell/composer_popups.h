// The composer's floating parts:
//  - PickList: the @-mention popup and the "#" / ":" / "/" completer —
//    non-modal lists above the trigger
//    character; the editor keeps the keyboard and forwards Up / Down / Tab /
//    Enter / Escape to handleKey();
//  - the link popup (URL, Display text, Cancel / Insert), the schedule-send
//    popup (Send at, Cancel / Schedule), the GIF picker (GIPHY key setup,
//    search), and the undo-send chip ("Message sent · Undo Ctrl+Z");
//  - HistorySearch: Ctrl+R's earlier-prompt search on the composer box, over a dimmed message area.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

class PickList : public ui::Popup {
public:
    // What a row shows. A mention row: avatar (or the megaphone for @channel
    // & co.), a bold name, APP badge, presence dot, dim subtitle, and a status
    // kept whole on the right ("Disabled in threads"). A channel row: hash or
    // lock and the bold name. A plain row: one line (emoji "👋  :wave:").
    struct Item {
        // Command: a two-line slash-command row (icon, "/name usage",
        // "source · description").
        enum class Kind : uint8_t { Mention, Alias, Channel, Plain, Command } kind = Kind::Plain;
        std::string display; // what goes into the editor (mentions: "@Name")
        std::string insert;  // the raw token ("<@U…>", "<#C…|name>", "👋")
        std::string title, subtitle, status;
        std::string usage, source; // commands: the argument hint, where it comes from
        std::string avatar;        // path
        bool        bot = false, privateChannel = false;
        int         presence                       = 0; // 0 none, 1 active, 2 away, 3 dnd
        bool        operator==(const Item &) const = default;
    };
    using Pick = std::function<void(const Item &)>;

    PickList(Avatars *avatars, std::vector<Item> items, bool wide, Pick onPick);
    // Shows it with its bottom edge 4 px above `anchor` (window coordinates).
    static PickList *show(
        ui::Window       &w,
        Avatars          *avatars,
        ui::PointF        anchor,
        std::vector<Item> items,
        bool              wide,
        Pick              onPick
    );

    // New rows in the open list (the next keystroke's), re-anchored; the
    // same rows again are kept as they are. Selects the first, as show() does.
    void        update(std::vector<Item> items, bool wide, ui::PointF anchor);
    // Tests: how many times the rows were made.
    int         builds() const { return _builds; }
    float       scrollOffset() const;          // tests: how far the rows are scrolled
    bool        handleKey(const ui::Event &e); // true when it took the key
    int         selected() const { return _sel; }
    size_t      count() const { return _items.size(); }
    const Item &item(size_t i) const { return _items[i]; }
    void        select(int i);
    void        confirm();

private:
    void                         build();
    void                         applyWidth(bool wide);
    Avatars                     *_avatars;
    std::vector<Item>            _items;
    std::vector<ui::Clickable *> _rows;
    std::vector<ui::View *>      _badges; // the selected row's "Enter" chip
    ui::ScrollView              *_scroll = nullptr;
    Pick                         _onPick;
    int                          _sel    = 0;
    int                          _builds = 0;
    bool                         _wide   = false;
};

// URL + Display text, Insert / Cancel; done(url, label) on Insert.
ui::Popup *showLinkPopup(
    ui::Window                                                           &w,
    ui::RectF                                                             anchor,
    std::string                                                           selected,
    std::function<void(const std::string &url, const std::string &label)> done
);

// "Send at" a local date and time (an hour from now), Cancel / Schedule.
ui::Popup *showSchedulePopup(ui::Window &w, ui::RectF anchor, std::function<void(int64_t)> done);

// The GIF picker: the backend's own GIF search (the demo's stand-in GIPHY)
// or, without one and without a GIPHY key, the setup form (text, "Get a free
// GIPHY key…", the key field, Save); then the search box, the two-column
// results and "Powered by GIPHY". Without a service of the backend's, the
// search is GIPHY's own (gif_search.h) with the key; a key GIPHY refuses
// brings the setup form back with the reason.
struct GifHooks {
    std::function<std::string()>                  key;
    std::function<void(const std::string &)>      setKey;
    std::function<void(const std::string &)>      openUrl;
    std::function<void(std::string, std::string)> picked; // url, title
};
ui::Popup *showGifPicker(ui::Window &w, ui::RectF anchor, screens::Context &ctx, GifHooks hooks);

// The entries holding every word
// of the query (case-insensitively; an empty query matches all), and where
// the words are in a text (byte start, length), sorted and merged.
std::vector<size_t> historyFilter(const std::vector<std::string> &entries, std::string_view query);
std::vector<std::pair<size_t, size_t>>
historyMatches(std::string_view text, std::string_view query);

// Ctrl+R in the composer: the matches on top,
// newest at the bottom next to the search field under them; ↑ / Ctrl+R go
// to older ones, ↓ to newer, PageUp / PageDown five at a time, Enter or a
// click takes one into the editor (not sent), Esc or a click on the dimmed
// area above closes it (the focus back to the composer), the focus going
// anywhere else just closes it.
class HistorySearch : public ui::Popup {
public:
    // `entries` newest first (repeats keep the newest); `box` is the
    // composer's box and `area` what is dimmed above it (window rects).
    static HistorySearch *open(
        ui::Window              &w,
        std::vector<std::string> entries,
        std::string_view         query,
        ui::RectF                box,
        ui::RectF                area
    );
    ~HistorySearch() override;

    std::function<void(const std::string &)> onPicked;
    std::function<void()>                    onCancelled; // Esc, a click on the shade

    const std::string       &query() const;
    std::vector<std::string> matches() const; // newest first
    std::string              selectedEntry() const;
    int                      selected() const { return _sel; }
    ui::TextEdit            &field() const { return *_field; }
    void                     select(int match);
    void                     pick();
    void                     dismiss();

    void layout() override;

private:
    HistorySearch(std::vector<std::string> entries, ui::RectF box);
    void refilter();
    bool key(const ui::Event &e);

    std::vector<std::string>     _entries;
    std::vector<std::string>     _folded;  // _entries through foldCase
    std::vector<size_t>          _matches; // into _entries, newest first
    std::vector<ui::Clickable *> _rows;    // by match
    ui::ScrollView              *_scroll = nullptr;
    ui::Label                   *_empty  = nullptr;
    ui::TextEdit                *_field  = nullptr;
    ui::Popup                   *_shade  = nullptr;
    ui::RectF                    _box;
    ui::SizeF                    _winSize;
    int                          _sel    = 0;
    bool                         _closed = false;
};

} // namespace shell
