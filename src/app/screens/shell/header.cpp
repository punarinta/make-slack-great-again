#include "screens/shell/header.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/common/user_search.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/session_dialogs.h"
#include "screens/shell/shell_text.h"

#include <algorithm>
#include <cmath>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvKind;
using model::ConvRef;
using model::kNoConv;

namespace shell {

// ── Header parts ────────────────────────────────────────────────────────────

// The star: outlined, or Slack's solid star in icon.starred. 28 px, 15 px icon.
class StarButton final : public GlyphButton {
public:
    StarButton() : GlyphButton(Icon::Star, 28, 15, C::FormIcon) {}
    void setStarred(bool on) {
        _on = on;
        setIcon(on ? Icon::StarSolid : Icon::Star);
        setTint(on ? C::IconStarred : C::FormIcon);
        setTooltip(on ? tr("Unstar conversation") : tr("Star conversation"));
    }
    bool starred() const { return _on; }

private:
    bool _on = false;
};

// A DM's peer: a 28 px avatar (radius 4) centred in 36, its dot on white; a
// group DM: up to three 24 px avatars overlapping 16 px apart, each on a 2 px
// ring of the surface, then the member count.
class HeaderAvatar final : public Clickable {
public:
    HeaderAvatar() {
        setLook({C::None, C::None, C::None, C::None, 0});
        style().size(36, 36).noShrink().dir = Dir::None;
    }
    void clear() {
        clearChildren();
        _single = nullptr;
        _count  = nullptr;
        _group  = 0;
        style().size(36, 36);
    }
    Avatar *peer() const { return _single; } // the DM's, else null
    // The DM's avatar: the one shown already, re-bound by the caller.
    Avatar *single() {
        if (_single)
            return _single;
        clear();
        _single = add<Avatar>();
        _single->setRadius(4);
        _single->setPlaceholder(C::PresenceAway);
        _single->setHitTransparent(true);
        return _single;
    }
    // `names`: the members' (their initials while the pictures load).
    void group(
        const std::vector<std::shared_ptr<const gfx::Bitmap>> &pics,
        const std::vector<std::string_view>                   &names,
        uint32_t                                               count
    ) {
        // The same stack again (a refresh): its pictures and count re-bound.
        const int         n    = int(std::min<size_t>(3, pics.size()));
        const std::string text = count > 0 ? str::number(int64_t(count)) : std::string();
        if (_single || n != _group || (_count != nullptr) != (count > 0) ||
            (_count && _count->text() != text)) {
            clear();
            _group = n;
            for (int i = 0; i < _group; ++i) {
                auto *a = add<Avatar>();
                a->setRadius(4);
                a->setPlaceholder(C::PresenceAway);
                a->setHitTransparent(true);
            }
            float w = kRing * 2 + kStack + float(std::max(0, _group - 1)) * kStep;
            if (count > 0) {
                _count = add<Label>(text, Font::BodyBold, C::TextMuted);
                w += 6 + std::ceil(_count->measure(kInf, kInf).w);
            }
            style().size(w, 36);
        }
        for (int i = 0; i < _group; ++i) {
            auto *a = static_cast<Avatar *>(child(size_t(i)));
            a->setBitmap(pics[size_t(i)]);
            a->setInitial(names[size_t(i)]);
        }
    }
    bool isGroup() const { return _group > 0; }
    void layout() override {
        if (_single) {
            _single->setFrame({4, 4, 28, 28});
            return;
        }
        const float y = (height() - kStack) / 2;
        for (int i = 0; i < _group; ++i)
            child(size_t(i))->setFrame({kRing + float(i) * kStep, y, kStack, kStack});
        if (_count) {
            const SizeF s = _count->measure(kInf, kInf);
            const float x = kRing * 2 + kStack + float(_group - 1) * kStep + 6;
            _count->setFrame({x, std::round((height() - s.h) / 2), s.w, s.h});
        }
    }
    void paint(gfx::Painter &p) override {
        const float y = (height() - kStack) / 2;
        for (int i = 0; i < _group; ++i) // each chip on a ring of the surface
            p.fillRoundRect(
                {float(i) * kStep, y - kRing, kStack + 2 * kRing, kStack + 2 * kRing},
                4 + kRing,
                color(C::Surface)
            );
    }

private:
    static constexpr float kStack = 24, kStep = 16, kRing = 2;
    Avatar                *_single = nullptr;
    Label                 *_count  = nullptr;
    int                    _group  = 0;
};

// The members button: people icon + count, secondary text, 28 px tall.
class MembersButton final : public Clickable {
public:
    MembersButton() {
        setLook({C::None, C::None, C::None, C::None, 0});
        setTooltip(tr("View members"));
        setRole(Role::Button);
        style().row().height(28).padding(4, 0).spacing(4).items(Align::Center).noShrink();
        add<IconView>(Icon::Users, 16, C::FormIcon);
        count = add<Label>("", Font::Caption, C::TextMuted);
    }
    Label *count;
};

// ── Members popup ───────────────────────────────────────────────────────────

namespace {

class MembersPopup final : public Popup {
public:
    MembersPopup(screens::Context &ctx, Avatars &avatars, uint32_t expected) : _ctx(ctx) {
        style().width(340).padding(8, 12, 8, 8).spacing(8);
        _title = add<Label>(tr("Members"), Font::SmallBold, C::TextMuted);
        _title->style().margins(4, 0, 4, 0);
        auto *box =
            add<TextField>(tr("Find members"), TextField::Size::Normal, uint16_t(Icon::Search));
        box->setQuiet();
        _search           = &box->edit();
        _search->onChange = [this] { filter(); };
        _search->onKey    = [this](const Event &e) { return key(e); };
        // Virtual: a large channel has thousands of members, and only the
        // rows in sight are built (and fetch their pictures). On the popup's
        // own background, 8 px in, stopping at the ends; a row opens on click.
        _list             = add<BrowseList>(avatars);
        _list->style().flex(0);
        _list->setOnContentSurface(true);
        _list->setOptions({8, false, false, false, false});
        _list->onActivated = [this](const std::string &id) { activate(_ctx.store().findUser(id)); };
        _message = add<Label>(tr("Loading members\xE2\x80\xA6"), Font::Control, C::TextMuted);
        _message->setAlign(text::LayoutOptions::Align::Center);
        fitRows(expected);
        _list->setVisible(false);
    }
    void setMembers(std::vector<model::UserRef> ids) {
        const auto &st = _ctx.store();
        std::erase_if(ids, [&](model::UserRef u) { return u >= st.userCount(); });
        // Sorted by the folded label.
        std::vector<std::pair<std::string, model::UserRef>> byName;
        byName.reserve(ids.size());
        for (model::UserRef u : ids)
            byName.emplace_back(utf8::foldCase(st.user(u).label()), u);
        std::sort(byName.begin(), byName.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        // A member: the name ("(you)" for me) over the handle and title.
        std::vector<BrowseList::Item> items;
        items.reserve(byName.size());
        for (const auto &[name, u] : byName) {
            const model::User &user = st.user(u);
            const std::string  label(user.label());
            BrowseList::Item   it;
            it.id     = user.id;
            it.avatar = user.avatar;
            it.title  = u == st.me ? i18n::arg(tr("%1 (you)"), label) : label;
            if (!user.name.empty() && user.name != label)
                it.subtitle = "@" + user.name;
            if (!user.title.empty())
                it.subtitle += (it.subtitle.empty() ? "" : " \xC2\xB7 ") + user.title;
            it.searchKey = screens::userSearchKey(user, true);
            items.push_back(std::move(it));
        }
        // The expected count may be missing or stale: the list itself sizes
        // the popup (the overlay re-places it under its anchor).
        fitRows(items.size());
        _title->setText(i18n::trn("%n member", "%n members", int64_t(items.size())));
        _list->setItems(std::move(items));
        _loaded = true;
        filter();
    }
    // The list failed to load.
    void showError(const std::string &err) {
        _list->setVisible(false);
        _message->setVisible(true);
        _message->setText(i18n::arg(tr("Couldn't load the members (%1)."), err));
    }
    ui::TextEdit &search() { return *_search; }
    size_t        shown() const { return _list->visibleCount(); }

private:
    // Whole rows, at most six; the message takes the list's place.
    void fitRows(size_t n) {
        const float h = float(std::clamp<size_t>(n, 1, 6)) * BrowseList::kRowH;
        if (_list->currentStyle().h == h)
            return;
        _list->style().height(h);
        _message->style().height(h);
    }
    void filter() {
        if (!_loaded)
            return;
        const std::string q(str::trim(_search->text()));
        _list->applyFilter(q);
        const bool none = _list->visibleCount() == 0;
        // Typing picks the first match for Enter.
        _list->setSelectedRow(q.empty() || none ? -1 : 0);
        _list->setVisible(!none);
        _message->setVisible(none);
        if (none)
            _message->setText(
                q.empty() ? std::string(tr("No members to show."))
                          : i18n::arg(tr("No one here matches \xE2\x80\x9C%1\xE2\x80\x9D."), q)
            );
    }
    bool key(const Event &e) {
        if (e.type != EventType::KeyDown)
            return false;
        if (e.key == plat::Key::Down || e.key == plat::Key::Up) {
            _list->moveSelection(e.key == plat::Key::Down ? 1 : -1);
            return true;
        }
        if ((e.key == plat::Key::Enter || e.key == plat::Key::KpEnter) &&
            _list->selectedRow() >= 0) {
            _list->activateSelected();
            return true;
        }
        return false;
    }
    void activate(model::UserRef u) {
        close();
        if (_ctx.messageUser && u != model::kNoUser) // the DM, or a teammate's page
            _ctx.messageUser(u);
    }
    screens::Context &_ctx;
    Label            *_title = nullptr, *_message = nullptr;
    TextEdit         *_search = nullptr;
    BrowseList       *_list   = nullptr;
    bool              _loaded = false;
};

} // namespace

// ── ConvHeader ──────────────────────────────────────────────────────────────

// A web hand-off, as huddles can't start over the API.
// A channel's /huddle/ link starts (or joins) its huddle; for a DM that shape
// server-errors, so a DM opens the conversation and the huddle starts there.
// A live room's own huddle_link comes first.
std::string huddleJoinUrl(const model::Store &st, ConvRef conv) {
    const auto &c = st.conversation(conv);
    if (!c.huddleLink.empty())
        return c.huddleLink;
    return str::concat(
        {"https://app.slack.com/", c.isDirect() ? "client/" : "huddle/", st.workspaceId, "/", c.id}
    );
}

ConvHeader::ConvHeader(screens::Context &ctx, Avatars &avatars) : _ctx(ctx), _avatars(avatars) {
#ifdef __APPLE__
    // The macOS unified header, which Shell puts in the title bar: the name centred in the window
    // between a blank column and the actions, both 200 px wide (room for the members button beside
    // the three icons), over the title bar's bottom rule.
    constexpr float kActionsW = 200;
    style().row().margins(0, 0, 0, 1);
    setBackground(C::Surface);
    add<View>()->style().width(kActionsW).noShrink();
    View *heading = add<View>();
    heading->style().row().flex(1).items(Align::Center);
    heading->add<View>()->style().flex(1);
    View *actions = add<View>();
    actions->style().row().width(kActionsW).padding(0, 0, 8, 0).spacing(2).noShrink();
    actions->style().items(Align::Center);
    actions->add<View>()->style().flex(1);
#else
    style().row().height(48).padding(16, 0, 8, 0).spacing(8).items(Align::Center).noShrink();
    View *heading = this, *actions = this;
#endif
    _avatar = heading->add<HeaderAvatar>();
    _avatar->setVisible(false);
    _avatar->onClick = [this] {
        if (_avatar->isGroup() && _ctx.backend.capabilities().memberList)
            openMembers(_avatar->windowRect());
    };
#ifdef __APPLE__
    _avatar->style().margins(0, 0, 8, 0);
    _title = heading->add<Label>("", Font::UnifiedTitle);
    _title->setMaxLines(1);
    _title->setAlign(text::LayoutOptions::Align::Center);
    // Not a control: a press on the name drags the window (Shell::hitTest).
    _title->setHitTransparent(true);
    heading->add<View>()->style().flex(1);
#else
    _title = heading->add<Label>("", Font::HeaderTitle);
    _title->setMaxLines(1);
    _title->style().flex(1).shrink = 1;
#endif
    auto *members = actions->add<MembersButton>();
    members->style().margins(0, 0, 2, 0);
    members->onClick = [this, members] { openMembers(members->windowRect()); };
    _members         = members;
    _count           = members->count;
    auto *huddle     = actions->add<GlyphButton>(
        Icon::Headphones, 28, 16, C::FormIcon, tr("Opens the huddle in Slack for web")
    );
    huddle->style().margins(0, 0, 2, 0);
    huddle->onClick = [this] {
        if (_conv != kNoConv && _ctx.openUrl)
            _ctx.openUrl(huddleJoinUrl(_ctx.store, _conv));
    };
    _huddle = huddle;
    _star   = actions->add<StarButton>();
    _star->style().margins(0, 0, 2, 0);
    _star->onClick = [this] {
        if (_conv != kNoConv)
            _ctx.backend.setStarred(_conv, !_ctx.store().conversation(_conv).starred);
    };
    auto *search =
        actions->add<GlyphButton>(Icon::Search, 32, 16, C::FormIcon, tr("Search messages"));
    search->onClick = [this] {
        if (onSearch)
            onSearch();
    };
    _search = search;
}

Clickable *ConvHeader::avatar() const {
    return _avatar;
}

Clickable *ConvHeader::star() const {
    return _star;
}

bool ConvHeader::starred() const {
    return _star->starred();
}

int ConvHeader::avatarPresence() const {
    const Avatar *a = _avatar->peer();
    return a ? int(a->presence()) : -1;
}

void ConvHeader::show(ConvRef conv) {
    _conv = conv;
    refresh();
}

void ConvHeader::refresh() {
    if (_conv == kNoConv || _conv >= _ctx.store().conversationCount())
        return;
    const auto &st   = _ctx.store();
    const auto &c    = st.conversation(_conv);
    const auto  caps = _ctx.backend.capabilities();
    _title->setText(convTitle(st, _conv));
    _star->setVisible(true);
    _star->setStarred(c.starred);
    _huddle->setVisible(caps.huddles);
    // Channels: the members button with conversations.list's count (a large
    // one shortened to "12k" so it fits).
    const bool channel = caps.memberList && !c.isDirect();
    _members->setVisible(channel);
    const uint32_t n = channel ? c.memberCount : 0;
    _count->setText(
        n == 0      ? std::string()
        : n < 10000 ? str::number(int64_t(n))
                    : str::concat({str::number(int64_t(n / 1000)), "k"})
    );
    _count->setVisible(n > 0);
    // A DM: the peer's avatar and presence; a group DM: its stacked members.
    _avatar->setVisible(c.isDirect());
    if (c.kind == ConvKind::Dm) {
        const auto &u = st.user(c.dmUser);
        Avatar     *a = _avatar->single();
        a->setBitmap(_avatars.get(u.avatar, 56));
        a->setInitial(u.label());
        // Phantom (yellow) for me while no official client is
        // connected, and for any peer that can't be reached (an agent
        // session that is gone: presenceOf).
        const bool self = c.dmUser == st.me;
        const auto sp   = _ctx.backend.selfPresence();
        a->setPresence(
            Avatar::presenceOf(&u, caps.presence, self && sp.phantomAway()),
            C::BadgeText // the header dot's ring is white
        );
        // The presence tooltip: nothing until my presence is known.
        _avatar->setTooltip(
            !self ? std::string()
            : sp.phantomAway()
                ? tr("You appear away to others \xE2\x80\x94 no official Slack client is connected")
            : sp.active ? tr("Active")
            : sp.loaded ? tr("Away")
                        : std::string()
        );
        _avatar->setCursor(plat::Cursor::Arrow);
    } else if (c.kind == ConvKind::Group) {
        // conversations.list leaves a renamed group DM's members out: ask once
        // (the backend files them into the conversation, which repaints this).
        if (caps.memberList && c.members.empty() && _membersAsked.insert(c.id).second)
            _ctx.backend.loadMembers(_conv, [](std::vector<model::UserRef>, std::string) {});
        std::vector<std::shared_ptr<const gfx::Bitmap>> pics;
        std::vector<std::string_view>                   names;
        for (model::UserRef u : c.members)
            if (u != st.me) {
                pics.push_back(_avatars.get(st.user(u).avatar, 48));
                names.push_back(st.user(u).label());
            }
        _avatar->group(pics, names, uint32_t(c.members.size()));
        _avatar->setTooltip({});
        _avatar->setCursor(caps.memberList ? plat::Cursor::Hand : plat::Cursor::Arrow);
    } else {
        _avatar->clear();
    }
    invalidateLayout();
}

void ConvHeader::openMembers(RectF anchor) {
    Window *w = window();
    if (!w || _conv == kNoConv)
        return;
    const auto &c = _ctx.store().conversation(_conv);
    auto        p = std::make_unique<MembersPopup>(
        _ctx, _avatars, c.kind == ConvKind::Group ? uint32_t(c.members.size()) : c.memberCount
    );
    auto *raw = p.get();
    p->setAnchor(anchor, Popup::Place::Below);
    w->showPopup(std::move(p));
    raw->search().focus();
    // The popup may be gone before the list comes: its onClosed holds the
    // token, so the token dies with it.
    auto token    = std::make_shared<int>(0);
    raw->onClosed = [token] {};
    _ctx.backend.loadMembers(
        _conv,
        [raw, weak = std::weak_ptr<int>(token)](std::vector<model::UserRef> ids, std::string err) {
            if (weak.expired())
                return;
            if (ids.empty() && !err.empty())
                raw->showError(err);
            else
                raw->setMembers(std::move(ids));
        }
    );
}

// ── Tabs ────────────────────────────────────────────────────────────────────

namespace {

class Tab final : public Clickable {
public:
    Tab(Icon icon, bool hasIcon, std::string text) : icon(icon), hasIcon(hasIcon) {
        setRole(Role::Tab);
        setLook({C::None, C::FormHighlight, C::FormHighlight, C::None, 6});
        setBand(4, 31);
        style().row().padding(lead(), 0, 10, 0);
        style().items(Align::Center).noShrink();
        label = add<Label>(std::move(text), Font::Control, C::TextMuted);
        label->setMaxLines(1);
        label->setHitTransparent(true);
    }
    // The hover box: its top inset in the strip and its height.
    void  setBand(float top, float h) { style().height(h).margins(0, top, 0, 0); }
    // Before the text: the icon and its gap, or the bare padding.
    float lead() const { return hasIcon ? 10 + 15 + 6 : 10; }
    void  setText(std::string t) {
        label->setText(std::move(t));
        fit();
    }
    // Sized with the bold face, so a tab never shifts when it activates; the
    // title elides past 240 px.
    void fit() {
        Label       probe(label->text(), Font::TabBold);
        const float w = std::min(240.f, std::ceil(probe.measure(kInf, kInf).w));
        label->style().width(w);
        style().padding(lead(), 0, 10, 0).width(lead() + w + 10);
    }
    void setActive(bool on) {
        active = on;
        label->setFont(on ? Font::TabBold : Font::Control);
        label->setColor(on ? C::Text : C::TextMuted);
        setLook(
            {C::None, on ? C::None : C::FormHighlight, on ? C::None : C::FormHighlight, C::None, 6}
        );
        update();
    }
    void paint(gfx::Painter &p) override {
        Clickable::paint(p);
        if (hasIcon)
            gfx::drawIcon(
                p,
                icon,
                {10, snapPx((height() - 15) / 2), 15, 15},
                color(active ? C::Text : C::TextMuted)
            );
    }
    Icon   icon;
    bool   hasIcon;
    Label *label;
    bool   active = false;
};

} // namespace

TabStrip::TabStrip() {
    setRole(Role::Group);
    style().row().height(38).padding(16, 0, 0, 0).spacing(4).noShrink();
    style().items(Align::Start);
}

int TabStrip::addTab(Icon icon, std::string text) {
    return addTab(icon, true, std::move(text));
}

int TabStrip::addTab(std::string text) {
    return addTab(Icon::AtSign, false, std::move(text));
}

int TabStrip::addTab(Icon icon, bool hasIcon, std::string text) {
    const int i = int(_tabs.size());
    auto     *t = add<Tab>(icon, hasIcon, std::move(text));
    if (_accent)
        t->setBand(3, 30);
    t->fit();
    t->setActive(i == _active);
    t->onClick = [this, i] {
        if (i == _active)
            return;
        setActive(i);
        if (onSelect)
            onSelect(i);
    };
    _tabs.push_back(t);
    return i;
}

void TabStrip::setTab(int i, Icon icon, std::string text) {
    auto *t    = static_cast<Tab *>(_tabs[size_t(i)]);
    t->icon    = icon;
    t->hasIcon = true;
    t->setText(std::move(text));
}

void TabStrip::setActive(int tab) {
    _active = tab;
    for (size_t i = 0; i < _tabs.size(); ++i)
        static_cast<Tab *>(_tabs[i])->setActive(int(i) == tab);
    update();
}

void TabStrip::setAccentLook() {
    // 3 px above the 30 px box, 3 px below it to the underline (36..38).
    _accent = true;
    for (auto *t : _tabs)
        static_cast<Tab *>(t)->setBand(3, 30);
    update();
}

std::string TabStrip::tabText(int i) const {
    return static_cast<Tab *>(_tabs[size_t(i)])->label->text();
}

void TabStrip::paint(gfx::Painter &p) {
    p.fillRect(bounds(), color(C::Surface));
    p.fillRect({0, height() - 1, width(), 1}, color(C::Border));
}

void TabStrip::paintOver(gfx::Painter &p) {
    // The active tab's underline sits on the divider.
    if (size_t(_active) >= _tabs.size())
        return;
    const View *t = _tabs[size_t(_active)];
    if (t->visible())
        p.fillRect(
            {t->frame().x, height() - 2, t->width(), 2}, color(_accent ? C::Accent : C::Text)
        );
}

ConvTabs::ConvTabs() {
    addTab(Icon::MessageCircle, tr("Messages"));
    addTab(Icon::StickyNotePlus, tr("Add canvas"));
}

void ConvTabs::setCanvas(bool visible, bool hasCanvas, std::string title) {
    _tabs[1]->setVisible(visible);
    setTab(
        1,
        hasCanvas ? Icon::Canvas : Icon::StickyNotePlus,
        hasCanvas ? (title.empty() ? std::string(tr("Untitled")) : std::move(title))
                  : std::string(tr("Add canvas"))
    );
    if (!visible && _active == 1)
        setActive(0); // can't stay on a hidden tab
}

} // namespace shell
