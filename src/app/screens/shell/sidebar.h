// The conversation sidebar:
// uniform 30 px rows under a 6 px top inset — the "Activity", "Threads",
// "Saved messages" and "Scheduled messages" entries, then the Starred / Channels /
// Direct messages / Agents & apps sections (a click on a header collapses
// it, hiding every row; hovering shows the chevron that says what a click
// does, and on the right the header's "+" — Channels: "Add channels",
// Direct messages: open one — and "⋮" More actions), "N more channels"
// for the ones outside the relevant-days window, "Add channels", and the
// footer (avatar + menu, presence toggle). Over the nav gradient.
//
// More actions (a right click on the header too) is the section menu
// (Menus::showSection): how the section sorts its rows (SectionSort, kept
// per workspace by the shell) and, with Capabilities::customSections, the
// user's own sections (Store::sections). Those come between Starred and
// Channels, each under its emoji (the list icon without one) and holding
// its conversations in its own order, whatever the filters; a
// conversation's menu moves it between them.
//
// An agent workspace (Capabilities::agentSessions, Claude Code) calls the
// direct messages "Sessions" and ends them with "Add sessions" (its "+" and
// that row open the find-or-create menu), and lists the team
// (Backend::agentRoles) in a "Team" section under them: a teammate's row
// opens its page, its dot is its user's presence, its "+" adds a teammate.
//
// Rows are live: a Store observer restyles a row when its counts change —
// semibold for unread, a red count for DM unreads and channel mentions, a
// blue dot for other unread in an "All new posts" channel; muted
// conversations and ones quiet for over 30 days show no badge.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shell {

// The notification window: a message older than this never notifies, and a
// conversation idle for longer neither badges nor counts.
inline constexpr int64_t kMaxNotifyAgeSecs = 30 * 86400; // 30 days

// A workspace's share of the tray dot, the launcher badge and its rail
// tile's dot: `important` = unread direct
// messages + mentions (an agent workspace: only what needs you, its
// answers), `unread` = other unread activity in channels on "All new posts"
// (`fallback`: the global level). Muted, "Nothing", left and closed
// conversations, and ones idle past the notification window, count for
// neither.
struct Attention {
    int  important = 0;
    bool unread    = false;
    int  dot() const { return important > 0 ? 2 : unread ? 1 : 0; } // 0, 1 blue, 2 red
    bool operator==(const Attention &) const = default;
};
Attention
workspaceAttention(const model::Store &store, model::NotifyLevel fallback, int64_t nowSecs);

// The liveness filter, shared by the sidebar and the
// quick switcher: a DM outlives its peer (the service never prunes it), so
// one whose peer was deactivated or never resolves to a name is not listed.
// A peer not loaded yet (a placeholder) is let through.
bool deadDm(const model::Store &store, const model::Conversation &c);

// How a section orders its rows: as listed (the service's order, a custom
// section's own), by name, or most recent activity first; unreads on top
// lifts the bold rows over the rest, mentions on top the rows with a red
// count (a mention, an unread direct message) over those, each part in that
// order.
struct SectionSort {
    enum class By : uint8_t { Default, Alpha, Recent };
    By          by            = By::Default;
    bool        unreadsFirst  = false;
    bool        mentionsFirst = false;
    bool        plain() const { return by == By::Default && !unreadsFirst && !mentionsFirst; }
    bool        operator==(const SectionSort &) const = default;
    // "alpha", "recent,unreads,mentions"… ("" = plain); unknown parts are
    // ignored.
    std::string encode() const;
    static SectionSort decode(std::string_view s);
};

class ConvRow;
class Menus;
class SectionHeader;
class SidebarFooter;
class TeammateRow;

class Sidebar : public ui::View {
public:
    Sidebar(screens::Context &ctx, Avatars &avatars);
    ~Sidebar() override;

    // A section header's kind. Every Custom section (Store::sections) shares
    // this one value, told apart by sectionId. Team lists teammates, not
    // conversations.
    enum Kind : uint8_t {
        Starred,
        Channels,
        Direct,
        Apps,
        Team,
        Custom,
        External,
        Vip,
        Drafts,
        KindCount
    };

    // The Settings → Appearance / Notifications choices the list follows.
    struct Filters {
        int                relevantDays   = 14;
        bool               showAgentsApps = true, unreadsOnly = false;
        bool               externalSection = true; // off: Connect chats stay in Channels / DMs
        bool               unreadsOnlyAppliesToCustomSections = false,
                           unreadsOnlyAppliesToStarred = false;
        bool               directAndExternalFirst = false;
        bool               highlightMentionsOnly  = true; // "Highlight mentions-only channels…"
        model::NotifyLevel defaultLevel           = model::NotifyLevel::All;
        bool               operator==(const Filters &) const = default;
    };
    void setFilters(const Filters &f);
    // Visit stamps: when each conversation was last opened here (or
    // seen unread), which keeps it listed for the relevant days. The shell
    // persists it (Settings::visitedAt) whenever onVisitedChanged fires.
    using VisitStamps = std::unordered_map<std::string, int64_t>; // conv id → epoch secs

    void                  setVisited(VisitStamps stamps);
    const VisitStamps    &visited() const { return _visited; }
    void                  clearVisited(); // Settings → "Clear state"
    std::function<void()> onVisitedChanged;
    // Each section's sort by its key (sectionKey; plain ones left out). The
    // shell keeps them per workspace and sets them on a switch;
    // onSortsChanged fires when the section menu changed one.
    using Sorts = std::unordered_map<std::string, SectionSort>;
    void                  setSorts(Sorts sorts);
    const Sorts          &sorts() const { return _sorts; }
    std::function<void()> onSortsChanged;
    void                  setSort(const std::string &key, SectionSort sort);
    // The sections the user collapsed, by key (sectionKey; only collapsed
    // ones listed). The shell keeps them per workspace and sets them when one
    // opens; onCollapsedChanged fires when a header click changed them, or a
    // custom section in them was deleted (onSortsChanged likewise for the
    // sorts). A section opened up to show the chat being opened stays
    // collapsed here: that is no choice of the user's, and it lasts until the
    // next set.
    using Collapsed = std::unordered_map<std::string, bool>;
    void                  setCollapsed(Collapsed collapsed);
    // The Store open at the last setCollapsed goes (detached): nothing is
    // pruned against the open one until the next set.
    void                  forgetStore();
    const Collapsed      &collapsed() const { return _collapsed; }
    std::function<void()> onCollapsedChanged;
    // A section's key: a custom one's id, a "#"-prefixed name for every
    // other built-in.
    static std::string    sectionKey(Kind kind, const std::string &id);

    void                        rebuild();                   // from the Store (roster changed)
    void                        select(model::ConvRef conv); // highlight only (kNoConv clears)
    model::ConvRef              selected() const { return _selected; }
    // Conversations in on-screen order (the quick switcher's empty query).
    std::vector<model::ConvRef> order() const;

    // What a row shows (tests, and the tray/badge code).
    struct RowState {
        bool        exists = false, visible = false, bold = false, dot = false, selected = false;
        int         badge  = 0;
        int         huddle = 0; // a live huddle's pill: its participant count (1 when none listed)
        std::string section;    // its section header's title
        bool        draft = false; // the "Draft" label shows
    };
    RowState                 rowState(model::ConvRef conv) const;
    // Tests: how many times the rows were rebuilt, and a row's identity (the
    // view object; null: no row), to prove a change restyled in place.
    int                      rebuildCount() const { return _rebuilds; }
    const ui::View          *rowView(model::ConvRef conv) const;
    // Sum the app badge shows: mentions + unread direct messages.
    int                      attentionCount() const;
    // The "N more channels" row's N (0: no such row).
    int                      hiddenChannels() const { return _hiddenChannels; }
    // Titles top to bottom: the visible nav entries, then the sections.
    std::vector<std::string> sectionTitles() const;
    // Collapses or expands a section by title (tests; a click does the same).
    bool                     toggleSection(std::string_view title);
    // A row dropped on the section titled so (tests; a drag does the same):
    // false where it can't go (its own section, the Team…).
    bool                     dropOnSection(model::ConvRef conv, std::string_view title);
    // The custom section a conversation's row is under ("" = a built-in one).
    std::string              rowSection(model::ConvRef conv) const;
    void                     showAllChannels(); // the "N more channels" row

    std::function<void()>                    onActivity;      // the Activity entry
    std::function<void()>                    onThreads;       // the Threads entry
    std::function<void()>                    onSavedMessages; // the Saved messages entry
    std::function<void()>                    onScheduled;     // the Scheduled messages entry
    std::function<void()>                    onFindChannel;   // "Add channels" → Find a channel
    std::function<void()>                    onCreateChannel; // "Add channels" → Create a channel
    std::function<void()>                    onBrowsePeople;  // the Direct messages header's "+"
    // Agent workspace: the Sessions "+" and "Add sessions", a
    // teammate's row, the Team header's "+".
    std::function<void(ui::PointF at)>       onSessionMenu;
    std::function<void(const std::string &)> onTeammate;
    std::function<void()>                    onAddTeammate;
    // Highlights a teammate's row as the open page ("" clears it); no
    // conversation is highlighted meanwhile.
    void                                     selectTeammate(const std::string &role);
    const std::string                       &selectedTeammate() const { return _selectedTeammate; }
    // The entries over the sections, each an overview page: Activity,
    // Threads, Saved messages (shown while something is saved,
    // Store::hasSaved), Scheduled messages (while something is scheduled,
    // Store::hasScheduled).
    enum class Nav : uint8_t { None, Activity, Threads, Saved, Scheduled };
    // Highlights an entry as the open page (None: none); opening a
    // conversation or a teammate clears it.
    void selectNav(Nav n);
    Nav  selectedNav() const { return _navSelected; }
    bool threadsUnread() const;  // the Threads entry is bright (Store::unreadThreads)
    bool activityUnread() const; // the Activity entry is bright (Store::unreadActivity)
    // A click on a row's huddle pill (tests): false when it shows none.
    bool joinHuddle(model::ConvRef conv);
    bool scheduledShown() const; // tests
    // The Team section's rows, top to bottom (tests): role ids.
    std::vector<std::string> teammates() const;
    // A teammate row's look (tests): bold, selected, its avatar's presence.
    struct TeammateState {
        bool exists = false, bold = false, selected = false;
        int  presence = 0; // Avatar::Presence
    };
    TeammateState  teammateState(const std::string &role) const;
    int            conversationPresence(model::ConvRef conv) const; // Avatar::Presence of a DM row
    // Context menus (right click, long press, Menu key): a conversation's on
    // its row.
    void           setMenus(Menus *m) { _menus = m; }
    SidebarFooter &footer() { return *_footer; }

    void paint(gfx::Painter &p) override; // the nav gradient

private:
    friend class ConvRow;
    friend class Menus; // the notify section ticks level()
    friend class SectionHeader;
    friend class TeammateRow;
    void               refresh(model::ConvRef conv); // one row from the Store
    void               refreshAll();
    void               refreshSections();
    void               refreshNav(); // which of the entries show
    ConvRow           *rowFor(model::ConvRef conv) const;
    ui::View          *navRow(Nav n) const;
    void               showNavSelection(); // the entries' checked look
    // The unread / effective level (Settings' default) / badge rules.
    bool               paintsUnread(const model::Conversation &c) const;
    // The red count a row shows (0: none): every unread message of a direct
    // conversation, a channel's mentions; nothing muted or long gone quiet.
    int                redCount(const model::Conversation &c) const;
    model::NotifyLevel level(const model::Conversation &c) const;
    bool               muted(const model::Conversation &c) const;
    bool               isApp(const model::Conversation &c) const;
    bool               isExternal(const model::Conversation &c) const;
    bool               isVip(const model::Conversation &c) const;
    // The section kind this conversation belongs in. Shared by rebuild() and
    // the Meta-event flip check so they can't disagree.
    Kind               classify(model::ConvRef ref) const;
    bool               misplaced(model::ConvRef ref) const; // its row would move
    bool               relevant(model::ConvRef c) const;
    void               applyCollapse(SectionHeader *h);
    // A header's state from _collapsed and _openedUp, its rows shown or hidden.
    void               restoreCollapse(SectionHeader *h);
    void               toggleCollapse(SectionHeader *h); // a click on the header
    void               openUp(SectionHeader *h);         // shows a row in it
    void               forgetDeletedSections();          // see its definition
    // A conversation row dragged onto a section (ConvRow's press handling).
    SectionHeader     *sectionAt(ui::PointF windowPos) const;
    void               dragRow(ConvRow *row, ui::PointF windowPos);
    void               dropRow(bool drop); // false: cancelled
    void               moveInto(model::ConvRef conv, SectionHeader *to);
    // reveal: scroll the selected row into view afterwards.
    void               rebuildSoon(bool reveal = true);
    void               revealSelectedAfterLayout(); // once its row has a frame
    void               usersSoon();
    void               sectionsSoon(); // refreshSections + refreshTeammates, coalesced
    uint64_t           userShape() const;
    void               noteShape(uint64_t shape); // a userShape() and what it was taken at
    void               addChannelsMenu(ui::View *row);
    void               addChannelsMenu(ui::PointF at);
    void               sectionMenu(SectionHeader *h, ui::PointF at); // More actions
    SectionSort        sortOf(const SectionHeader *h) const;
    // The order a section's conversations show in under `sort`: indexes
    // into `rows` (as listed; unchanged when plain).
    std::vector<size_t>
         sortedOrder(SectionSort sort, const std::vector<model::ConvRef> &rows) const;
    void resortSoon();      // a row may have left its sort's place: check (sectionsSoon)
    bool sortsHold() const; // every sorted section still in its order
    void refreshTeammates();

    screens::Context                      &_ctx;
    Avatars                               &_avatars;
    ui::ScrollView                        *_scroll = nullptr;
    ui::View                              *_items  = nullptr;
    SidebarFooter                         *_footer = nullptr;
    Menus                                 *_menus  = nullptr;
    std::vector<ConvRow *>                 _rows;
    std::vector<ConvRow *>                 _rowOf; // by ConvRef: the row, else null
    std::vector<TeammateRow *>             _teamRows;
    std::vector<model::Backend::AgentRole> _team; // as last listed (rebuild)
    std::string                            _selectedTeammate;
    std::vector<SectionHeader *>           _sections;
    std::vector<ui::View *>                _nav; // Activity, Threads, Saved, Scheduled messages
    std::vector<const char *>              _navTitles;
    ui::View                *_savedRow = nullptr, *_threadsRow = nullptr, *_scheduledRow = nullptr;
    ui::View                *_activityRow = nullptr;
    Nav                      _navSelected = Nav::None;
    VisitStamps              _visited; // opened here (visit stamps)
    Sorts                    _sorts;
    Filters                  _filters;
    model::ConvRef           _selected        = model::kNoConv;
    model::Store::ObserverId _observer        = 0;
    std::shared_ptr<int>     _alive           = std::make_shared<int>(0);
    plat::TimerId            _rebuildTimer    = 0;
    plat::TimerId            _usersTimer      = 0;
    plat::TimerId            _sectionsTimer   = 0;
    uint64_t                 _userShape       = 0; // userShape() at the last rebuild
    // The Store and its profile / text revisions _userShape was taken at.
    const model::Store      *_shapeStore      = nullptr;
    uint64_t                 _shapeProfileRev = 0, _shapeTextRev = 0;
    model::UserRef           _shapeMe                = model::kNoUser;
    int                      _rebuilds               = 0;
    bool                     _revealOnRebuild        = false;
    bool                     _revealSelectedOnLayout = false; // see revealSelectedAfterLayout()
    int                      _hiddenChannels         = 0;

    // The user's collapsed sections, and those of them opened up to show a
    // chat (see setCollapsed).
    Collapsed                       _collapsed;
    std::unordered_set<std::string> _openedUp;
    // The Store open at setCollapsed, and the one (and its sections
    // revision) deleted sections were last looked for in.
    const model::Store             *_collapsedOf = nullptr, *_prunedOf = nullptr;
    uint64_t                        _prunedRev = 0;

    uint64_t _sectionsRev     = 0;       // Store::sectionsRevision at the last rebuild
    ConvRow *_dragRow         = nullptr; // a row being dragged
    bool     _showAllChannels = false;
    bool     _resort          = false; // sectionsSoon checks the sorted sections' order
};

} // namespace shell
