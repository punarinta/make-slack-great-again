#include "screens/shell/quick_switcher.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/common/avatar_initial.h"
#include "screens/shell/shortcuts.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/sidebar.h"

#include <algorithm>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvRef;

namespace shell {

std::optional<double> quickSwitchScore(const QuickSwitchName &n, const std::vector<uint32_t> &q) {
    if (n.text.folded.empty())
        return std::nullopt; // no name yet: not something to offer
    std::optional<double> s = fuzzyScore(q, n.text);
    if (!n.alt.folded.empty())
        if (const std::optional<double> a = fuzzyScore(q, n.alt); a && (!s || *a > *s))
            s = a;
    if (!s)
        return std::nullopt;
    return *s + (n.group ? -0.5 : 0.0);
}

namespace {

constexpr float kListH = 300; // the list's minimum height

// A workspace tab's bubble: the rail's 40 px bubble in a slot that
// keeps room for the selection ring (accent around the current one, a
// divider ring on hover), so picking one moves nothing; dimmed when its
// workspace has nothing for the query.
class TabBubble final : public Clickable {
public:
    static constexpr float kBubble = 40, kRadius = 10, kRing = 2, kRingGap = 2;
    static constexpr float kSlot = kBubble + 2 * (kRing + kRingGap);

    TabBubble(std::string name, std::shared_ptr<const gfx::Bitmap> icon)
        : _name(std::move(name)), _icon(std::move(icon)),
          _letter(screens::avatarInitial(_name.empty() ? std::string_view("?") : _name)) {
        style().size(kSlot, kSlot).noShrink();
        setLook({C::None, C::None, C::None, C::None, 0});
        setHoverRepaint(true);
        setCursor(plat::Cursor::Hand);
        setTooltip(_name);
    }
    void set(bool current, bool dimmed) {
        if (current != _current || dimmed != _dimmed) {
            _current = current;
            _dimmed  = dimmed;
            update();
        }
    }
    bool showsBitmap(const gfx::Bitmap *b) const override { return _icon.get() == b; }
    void paint(gfx::Painter &p) override {
        const float in = kRing + kRingGap;
        const RectF r{in, in, kBubble, kBubble};
        p.save();
        if (_dimmed && !_current)
            p.setOpacity(0.35f);
        if (_icon && !_icon->empty()) {
            p.save();
            p.clipRoundRect(r, kRadius);
            p.drawBitmap(_icon->view(), r, gfx::Sampling::Smooth);
            p.restore();
        } else {
            p.fillRoundRect(r, kRadius, color(C::Accent));
            screens::paintInitial(p, *this, r, kRadius, 0, _letter, _layout, 17);
        }
        p.restore();
        if (_current || hovered()) {
            const float out = kRingGap + kRing / 2;
            p.strokeRoundRect(
                {r.x - out, r.y - out, r.w + 2 * out, r.h + 2 * out},
                kRadius + out,
                kRing,
                color(_current ? C::Accent : C::FormDividerStrong)
            );
        }
    }

private:
    std::string                        _name;
    std::shared_ptr<const gfx::Bitmap> _icon;
    std::string                        _letter;
    std::unique_ptr<text::Layout>      _layout;
    bool                               _current = false, _dimmed = false;
};

} // namespace

namespace {

// Seconds of a message timestamp (epoch micros).
int64_t secs(model::Ts ts) {
    return ts / 1000000;
}

} // namespace

std::vector<ConvRef> quickSwitchOrder(
    const model::Store &store, const std::unordered_map<std::string, int64_t> &visited
) {
    struct Entry {
        ConvRef     c;
        int64_t     activity;
        std::string key; // the name, case-folded
    };
    std::vector<Entry> list;
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const model::Conversation &cv = store.conversation(c);
        if (!cv.member)
            continue;
        if (deadDm(store, cv)) // a DM whose peer is gone: nothing to jump to
            continue;
        const auto v = visited.find(cv.id);
        list.push_back(
            {c,
             std::max(
                 {v == visited.end() ? int64_t(0) : v->second, secs(cv.latest), secs(cv.lastRead)}
             ),
             utf8::foldCase(store.displayName(c))}
        );
    }
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) {
        if (a.activity != b.activity)
            return a.activity > b.activity; // most recent first
        return a.key < b.key;
    });
    std::vector<ConvRef> out;
    out.reserve(list.size());
    for (const Entry &e : list)
        out.push_back(e.c);
    return out;
}

std::vector<QuickSwitchName>
quickSwitchNames(const model::Store &store, const std::vector<ConvRef> &order) {
    std::vector<QuickSwitchName> out;
    out.reserve(order.size());
    for (ConvRef c : order) {
        const model::Conversation &cv = store.conversation(c);
        const std::string          nm = store.displayName(c);
        // A DM also by its peer's other names: the full or display name
        // that doesn't show (Settings → Names), and the handle.
        std::string                alt;
        if (cv.kind == model::ConvKind::Dm && cv.localName.empty() && cv.dmUser != model::kNoUser) {
            const model::User &u = store.user(cv.dmUser);
            for (const std::string *n : {&u.realName, &u.profileName, &u.name})
                if (!n->empty() && *n != nm)
                    alt = alt.empty() ? *n : str::concat({alt, " ", *n});
        }
        out.push_back(
            {FuzzyText(nm),
             cv.kind == model::ConvKind::Group,
             alt.empty() ? FuzzyText() : FuzzyText(alt)}
        );
    }
    return out;
}

namespace {

// What matches, with its score, in `order`.
QuickSwitcher::Scored scoreAll(
    const std::vector<uint32_t>        &q,
    const std::vector<ConvRef>         &order,
    const std::vector<QuickSwitchName> &names
) {
    QuickSwitcher::Scored scored;
    for (size_t i = 0; i < order.size() && i < names.size(); ++i)
        if (const std::optional<double> sc = quickSwitchScore(names[i], q))
            scored.emplace_back(*sc, order[i]);
    return scored;
}

// Best first; equal scores keep `order`.
std::vector<ConvRef> ranked(QuickSwitcher::Scored scored) {
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    std::vector<ConvRef> out;
    out.reserve(scored.size());
    for (const auto &s : scored)
        out.push_back(s.second);
    return out;
}

std::optional<double> bestOf(const QuickSwitcher::Scored &scored) {
    std::optional<double> best;
    for (const auto &s : scored)
        if (!best || s.first > *best)
            best = s.first;
    return best;
}

} // namespace

std::vector<ConvRef> quickSwitchFilter(
    std::string_view                    query,
    const std::vector<ConvRef>         &order,
    const std::vector<QuickSwitchName> &names
) {
    return ranked(scoreAll(fuzzyQuery(str::trim(query)), order, names));
}

std::vector<ConvRef> quickSwitchFilter(
    const model::Store &store, std::string_view query, const std::vector<ConvRef> &order
) {
    return quickSwitchFilter(query, order, quickSwitchNames(store, order));
}

// The list rows, virtual (a large workspace has thousands of channels):
// a DM's photo, a group DM's initial disc, a
// channel's # or lock, then the name.
class QuickSwitcher::Rows final : public VirtualList::Adapter {
public:
    explicit Rows(QuickSwitcher &q) : _q(q) {}
    class Row final : public Clickable {
    public:
        Row() {
            setLook({C::None, C::Hover, C::Pressed, C::Selection, 6});
            style().row().height(32).padding(10, 0).spacing(10).items(Align::Center);
            avatar = add<Avatar>();
            avatar->style().size(20, 20);
            avatar->setRadius(5);
            icon = add<IconView>(Icon::Hash, 16, C::TextMuted);
            icon->style().size(20, 20);
            name = add<Label>("", Font::Body);
            name->setMaxLines(1);
            name->style().flex(1);
        }
        Avatar   *avatar = nullptr;
        IconView *icon   = nullptr;
        Label    *name   = nullptr;
        int       index  = -1;
    };
    int                   count() const override { return int(_q._results.size()); }
    std::unique_ptr<View> create(int) override {
        auto r     = std::make_unique<Row>();
        Row *raw   = r.get();
        r->onClick = [this, raw] { _q.choose(raw->index); };
        return r;
    }
    void bind(View &v, int i) override {
        Row                &r      = static_cast<Row &>(v);
        const model::Store &st     = *_q._tabs[size_t(_q._tab)].store;
        const ConvRef       c      = _q._results[size_t(i)];
        const auto         &cv     = st.conversation(c);
        const std::string   nm     = st.displayName(c);
        const bool          person = cv.isDirect();
        r.index                    = i;
        r.avatar->setVisible(person);
        r.icon->setVisible(!person);
        if (person) {
            r.avatar->setInitial(nm);
            r.avatar->setBitmap(
                cv.kind == model::ConvKind::Dm ? _q._avatars.get(st.user(cv.dmUser).avatar, 40)
                                               : nullptr
            );
        } else {
            r.icon->setIcon(convIcon(cv));
        }
        r.name->setText(nm);
        r.setChecked(i == _q._current);
    }
    float estimateHeight(int) const override { return 32; }

private:
    QuickSwitcher &_q;
};

QuickSwitcher::QuickSwitcher(
    screens::Context           &ctx,
    Avatars                    &avatars,
    std::vector<ConvRef>        order,
    std::vector<QuickSwitchTab> tabs
)
    : _ctx(ctx), _avatars(avatars), _tabs(std::move(tabs)) {
    // Open on the workspace on screen: with no query the list is its
    // conversations, most recent first, exactly what one workspace shows.
    _tab = -1;
    for (size_t i = 0; i < _tabs.size(); ++i)
        if (_tabs[i].store == &ctx.store()) {
            _tabs[i].order = order;
            _tab           = int(i);
        }
    if (_tab < 0) { // the demo, tests: the open workspace alone
        _tabs = {QuickSwitchTab{{}, {}, {}, &ctx.store(), std::move(order)}};
        _tab  = 0;
    }
    const bool multi = _tabs.size() > 1;
    style().width(520).padding(10).spacing(8);
    // The tabs first — the card's header when there is a choice to make.
    if (multi) {
        _strip = add<View>();
        _strip->style().row().spacing(6).items(Align::Center);
        for (size_t i = 0; i < _tabs.size(); ++i) {
            const QuickSwitchTab &t = _tabs[i];
            auto                 *b =
                _strip->add<TabBubble>(t.name, t.icon.empty() ? nullptr : avatars.get(t.icon, 80));
            b->onClick = [this, i] { setTab(int(i)); };
        }
        // The current workspace's name, so the letter bubbles don't have to
        // be decoded.
        _tabName = _strip->add<Label>("", Font::Title);
        _tabName->setMaxLines(1);
        _tabName->style().flex(1).margins(4, 0, 0, 0);
    }
    _field = add<TextEdit>();
    _field->setPlaceholder(tr("Jump to a conversation\xE2\x80\xA6"));
    _field->setMaxLines(1);
    _field->setBackground(C::InputBg, 6);
    _field->setBorder(C::InputBorderFocus);
    _field->style().padding(10, 8);
    _field->onChange = [this] { applyFilter(); };
    // The switcher's keys, whatever the modifiers: Up/Down move (wrapping),
    // Enter opens; with several workspaces ←/→ and Tab/Shift+Tab switch
    // between them instead of moving the caret.
    _field->onKey    = [this, multi](const Event &e) {
        if (e.type != EventType::KeyDown)
            return false;
        if (e.key == plat::Key::Down || e.key == plat::Key::Up) {
            const int n = int(_results.size());
            if (n)
                highlight((_current + (e.key == plat::Key::Down ? 1 : n - 1)) % n);
            return true;
        }
        if (e.key == plat::Key::Enter || e.key == plat::Key::KpEnter) {
            choose(_current);
            return true;
        }
        if (multi && (e.key == plat::Key::Left || e.key == plat::Key::Right)) {
            stepTab(e.key == plat::Key::Left ? -1 : 1);
            return true;
        }
        if (multi && e.key == plat::Key::Tab) {
            stepTab(e.mods & plat::ModShift ? -1 : 1);
            return true;
        }
        return false;
    };
    // The list: every match, scrolling, at least kListH (300) tall.
    _rows = std::make_unique<Rows>(*this);
    _list = add<VirtualList>(_rows.get());
    _list->setGap(1);
    _list->style().height(kListH);
    // In the list's place when nothing matches.
    _empty = add<Label>(tr("No conversations match."), Font::Body, C::TextMuted);
    _empty->setAlign(text::LayoutOptions::Align::Center);
    _empty->style().height(kListH);
    _empty->setVisible(false);
    // Arrow keys aren't discoverable on a field that looks like plain search.
    const char       *upDown = "\xE2\x86\x91\xE2\x86\x93", *leftRight = "\xE2\x86\x90\xE2\x86\x92";
    const std::string enter = shortcuts::nativeKeys(shortcuts::Id::SendMessage);
    auto             *hint  = add<Label>(
        multi ? i18n::arg(
                    tr("%1 to move \xC2\xB7 %2 to switch workspace \xC2\xB7 %3 to open"),
                    upDown,
                    leftRight,
                    enter
                )
              : i18n::arg(tr("%1 to move \xC2\xB7 %2 to open"), upDown, enter),
        Font::Small,
        C::TextMuted
    );
    hint->setAlign(text::LayoutOptions::Align::Center);
    refreshStrip();
    refilter();
}

// The best score: the same key and bias as the list rows, so "the
// workspace with the best match" is the one whose top row would rank highest.
std::optional<double> QuickSwitcher::bestScore(QuickSwitchTab &t, std::string_view query) {
    const std::vector<uint32_t> q = fuzzyQuery(str::trim(query));
    std::optional<double>       best;
    for (const QuickSwitchName &n : names(t))
        if (const std::optional<double> s = quickSwitchScore(n, q); s && (!best || *s > *best))
            best = s;
    return best;
}

// Names change with Meta and Users changes only (renames, a member's new
// label): rebuilt then, not per keystroke.
const std::vector<QuickSwitchName> &QuickSwitcher::names(QuickSwitchTab &t) {
    if (t.namesRev != t.store->metaRevision()) {
        t.names    = quickSwitchNames(*t.store, t.order);
        t.namesRev = t.store->metaRevision();
    }
    return t.names;
}

void QuickSwitcher::applyFilter() {
    const std::string query(str::trim(_field->text()));
    const int         n = int(_tabs.size());
    _dimmed.clear();
    if (query.empty()) {
        _manualTab = false; // a fresh query starts with a fresh mind
    } else if (n > 1) {
        // Each tab scored once: its best picks the tab, and the one picked
        // is the list (refilter doesn't score it again).
        const std::vector<uint32_t>        q = fuzzyQuery(query);
        std::vector<Scored>                all(static_cast<size_t>(n));
        std::vector<std::optional<double>> scores;
        int                                best = -1;
        for (int i = 0; i < n; ++i) {
            QuickSwitchTab &t = _tabs[size_t(i)];
            all[size_t(i)]    = scoreAll(q, t.order, names(t));
            scores.push_back(bestOf(all[size_t(i)]));
            _dimmed.push_back(!scores.back());
            if (scores.back() && (best < 0 || *scores.back() > *scores[size_t(best)]))
                best = i;
        }
        // Ties keep the tab that is showing; a tab picked by hand holds while
        // it still has something.
        const std::optional<double> &cur = scores[size_t(_tab)];
        if (best >= 0 && cur && *cur == *scores[size_t(best)])
            best = _tab;
        if (best >= 0 && best != _tab && (!_manualTab || !cur))
            _tab = best;
        refreshStrip();
        refilter(&all[size_t(_tab)]);
        return;
    }
    refreshStrip();
    refilter();
}

void QuickSwitcher::setTab(int index) {
    if (index < 0 || index >= int(_tabs.size()) || index == _tab)
        return;
    _tab       = index;
    // Picked while a query is up: the user overruling the re-aim. Over an
    // empty field it is just browsing.
    _manualTab = !str::trim(_field->text()).empty();
    refreshStrip();
    refilter();
}

void QuickSwitcher::stepTab(int delta) {
    const int n = int(_tabs.size());
    if (n > 1)
        setTab(((_tab + delta) % n + n) % n);
}

void QuickSwitcher::refreshStrip() {
    if (!_strip)
        return;
    for (size_t i = 0; i < _tabs.size(); ++i)
        static_cast<TabBubble *>(_strip->child(i))
            ->set(int(i) == _tab, i < _dimmed.size() && _dimmed[i]);
    _tabName->setText(_tabs[size_t(_tab)].name);
}

std::string QuickSwitcher::emptyText() const {
    return _empty->visible() ? std::string(_empty->text()) : std::string();
}

QuickSwitcher::~QuickSwitcher() = default;

void QuickSwitcher::refilter(Scored *scored) {
    QuickSwitchTab &tab = _tabs[size_t(_tab)];
    _results            = scored ? ranked(std::move(*scored))
                                 : quickSwitchFilter(_field->text(), tab.order, names(tab));
    // Preselect the top match so Enter always opens something.
    _current            = 0;
    _list->reset();
    _list->scrollTo(0);
    // Point at the other tabs when they hold what this
    // one doesn't.
    const bool any = !_results.empty();
    _list->setVisible(any);
    _empty->setVisible(!any);
    if (any)
        return;
    bool              elsewhere = false;
    const std::string query(str::trim(_field->text()));
    // The dimming says it for this query already (applyFilter scored them).
    const bool        known = _dimmed.size() == _tabs.size();
    if (_tabs.size() > 1 && !query.empty())
        for (size_t i = 0; i < _tabs.size() && !elsewhere; ++i)
            elsewhere =
                int(i) != _tab && (known ? !_dimmed[i] : bestScore(_tabs[i], query).has_value());
    _empty->setText(
        elsewhere ? i18n::arg(tr("No matches in %1. Other workspaces have some."), tab.name)
                  : std::string(tr("No conversations match."))
    );
}

void QuickSwitcher::highlight(int index) {
    const int prev = _current;
    _current       = index;
    if (prev >= 0 && prev < int(_results.size()))
        _list->itemsChanged(prev, 1); // rebinds its checked state
    _list->itemsChanged(index, 1);
    _list->scrollToItem(index, VirtualList::ItemAlign::Nearest, false);
}

void QuickSwitcher::choose(int index) {
    if (index < 0 || size_t(index) >= _results.size())
        return;
    const ConvRef         c    = _results[size_t(index)];
    const QuickSwitchTab &tab  = _tabs[size_t(_tab)];
    const std::string     key  = tab.key;
    const bool            here = tab.store == &_ctx.store();
    const auto            open = here ? _ctx.openConversation : std::function<void(ConvRef)>();
    const auto            in   = onChooseIn;
    close();
    if (open)
        open(c);
    else if (!here && in)
        in(key, c); // opened in its own workspace
}

} // namespace shell
