#include "screens/shell/channel_dialogs.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/common/user_search.h"
#include "screens/shell/shell_text.h"
#include "ui/controls.h"

#include <algorithm>
#include <memory>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvKind;
using model::ConvRef;
using model::UserRef;
using V = Button::Kind;

namespace shell {

namespace {

// One of the dialog's tabs (a checkable button): 8/16 padding,
// the active one bold in the primary colour over a 2-px accent underline.
class FinderTab final : public Clickable {
public:
    explicit FinderTab(std::string text) {
        setRole(Role::Tab);
        setFocusable(false);
        setLook({C::None, C::None, C::None, C::None, 0});
        style().padding(16, 8).noShrink();
        _label = add<Label>(std::move(text), Font::Body, C::FormTextMuted);
    }
    void setActive(bool on) {
        setChecked(on);
        _label->setFont(on ? Font::BodyBold : Font::Body);
        tint();
    }
    bool onEvent(Event &e) override {
        const bool r = Clickable::onEvent(e);
        if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave)
            tint(); // the primary colour on hover
        return r;
    }
    void paintOver(gfx::Painter &p) override {
        if (checked())
            p.fillRect({0, height() - 2, width(), 2}, color(C::Accent));
    }

private:
    void   tint() { _label->setColor(checked() || hovered() ? C::FormText : C::FormTextMuted); }
    Label *_label;
};

// ── New group DM ───────────────────────────────────────────────────────────

constexpr int kVisibleRows = 6; // the lists' height, in rows

// A group DM needs two people besides me, so the rows are picks rather than
// opens: a click toggles a row's check box and "Message" takes the set. One
// person is a DM, which is what the People tab already does with a click.
class GroupFinder final : public Dialog {
public:
    GroupFinder(
        screens::Context                                 &ctx,
        Avatars                                          &avatars,
        std::function<void(const std::vector<UserRef> &)> done
    )
        : Dialog(tr("New group DM"), 560), _ctx(ctx), _done(std::move(done)) {
        content()->style().spacing(12);
        _search = content()->add<TextField>(
            tr("Search for people"), TextField::Size::Normal, uint16_t(Icon::Search)
        );
        _list = content()->add<BrowseList>(avatars);
        _list->style().height(kVisibleRows * BrowseList::kRowH).noShrink();
        BrowseList::Options opt;
        opt.multi = true;
        _list->setOptions(opt);
        _list->setItems(groupItems(ctx.store));
        _list->onPickedChanged = [this] { refreshMessageButton(); };
        // The people a roster load brings in land in the open list.
        _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
            if (ch.kind != model::ChangeKind::Roster)
                return;
            _list->refreshItems(groupItems(_ctx.store));
            _list->applyFilter(_search->text());
        });
        _search->edit().onChange = [this] { _list->applyFilter(_search->text()); };
        _search->edit().onKey    = [this](const Event &e) { return _list->navKey(e); };

        Button *cancel = makeButton(tr("Cancel"), V::Secondary);
        _message       = makeButton(tr("Message"), V::Primary);
        _count         = new Label(std::string(), Font::Small, C::FormTextMuted);
        addButtonRow(_message, cancel, _count);
        _message->onClick = [this] { startConversation(); };
        refreshMessageButton();
    }
    ~GroupFinder() override { _ctx.store.unobserve(_observer); }

    BrowseList &list() { return *_list; }
    TextField  &search() { return *_search; }
    Button     &messageButton() { return *_message; }

private:
    // The People list without me: I am in every group DM I start.
    static std::vector<BrowseList::Item> groupItems(const model::Store &store) {
        std::vector<BrowseList::Item> items = peopleItems(store);
        if (store.me != model::kNoUser) {
            const std::string &me = store.user(store.me).id;
            std::erase_if(items, [&me](const BrowseList::Item &it) { return it.id == me; });
        }
        return items;
    }
    void refreshMessageButton() {
        const size_t n = _list->picked().size();
        _message->setEnabled(n > 0);
        _count->setText(n ? i18n::trn("%n person", "%n people", int64_t(n)) : std::string());
        _count->setVisible(n > 0);
    }
    void startConversation() {
        std::vector<UserRef> people;
        for (const std::string &id : _list->picked())
            if (const UserRef u = _ctx.store().findUser(id); u != model::kNoUser)
                people.push_back(u);
        if (people.empty())
            return;
        auto cb = std::move(_done);
        accept();
        if (cb)
            cb(people);
    }

    screens::Context                                 &_ctx;
    std::function<void(const std::vector<UserRef> &)> _done;
    TextField                                        *_search   = nullptr;
    BrowseList                                       *_list     = nullptr;
    Button                                           *_message  = nullptr;
    Label                                            *_count    = nullptr;
    model::Store::ObserverId                          _observer = 0;
};

// ── Find a channel ──────────────────────────────────────────────────────────

class ChannelFinder final : public Dialog {
public:
    ChannelFinder(
        screens::Context                                 &ctx,
        Avatars                                          &avatars,
        std::function<void(ConvRef)>                      channel,
        std::function<void(UserRef)>                      person,
        std::function<void()>                             create,
        std::function<void(const std::vector<UserRef> &)> people
    )
        : Dialog(std::string(), 720), _ctx(ctx), _avatars(avatars), _channel(std::move(channel)),
          _person(std::move(person)), _create(std::move(create)), _people(std::move(people)) {
        // A bare card: its own chrome, the lists edge to edge.
        content()->style().spacing(0);

        // ── Top bar: search + the dialog's buttons + close ──
        auto *top = content()->add<View>();
        top->style()
            .row()
            .items(Align::Center)
            .spacing(8)
            .padding(24, 20, 24 - Dialog::kCloseInset, 16);
        _search = top->add<TextField>(
            tr("Search for channels"), TextField::Size::Normal, uint16_t(Icon::Search)
        );
        _search->style().flex(1).minW = 200;
        // The People tab's second button: a DM between several people.
        _groupBtn =
            top->add<Button>(tr("New group DM"), Button::Kind::Secondary, Button::Form::Normal);
        _groupBtn->setFocusable(false);
        _groupBtn->onClick = [this] {
            Window *w = window();
            if (!w)
                return;
            // The finder may be gone by the time Message is pressed: the
            // group starts all the same, and only a live finder closes.
            std::weak_ptr<int> alive = _alive;
            auto              *d     = new GroupFinder(
                _ctx, _avatars, [this, alive, cb = _people](const std::vector<UserRef> &users) {
                    if (!alive.expired())
                        reject();
                    if (cb)
                        cb(users);
                }
            );
            w->showPopup(std::unique_ptr<Popup>(d));
            d->search().edit().focus(); // the popup takes the focus when it shows
        };
        auto *createBtn = top->add<Button>(
            tr("Create Channel"), V::Primary, Button::Form::Normal
        ); // this casing on purpose
        createBtn->setFocusable(false);
        createBtn->onClick = [this] {
            auto cb = _create;
            reject();
            if (cb)
                cb();
        };
        addDialogCloseButton(top)->onClick = [this] { reject(); };

        // ── Tab bar ──
        auto *tabs = content()->add<View>();
        tabs->style().row().padding(20, 0, 24, 0);
        _tabs[0] = tabs->add<FinderTab>(tr("Channels"));
        _tabs[1] = tabs->add<FinderTab>(tr("People"));
        for (int i = 0; i < 2; ++i)
            _tabs[i]->onClick = [this, i] {
                selectTab(i);
                _search->edit().focus(); // the tabs take no focus from the field
            };
        content()->add<Separator>(false, C::FormDivider);

        // ── The two lists: whole rows, none cut at the bottom ──
        auto *stack = content()->add<View>();
        stack->style().stack().height(kVisibleRows * BrowseList::kRowH).noShrink();
        for (int i = 0; i < 2; ++i) {
            _lists[i]              = stack->add<BrowseList>(avatars);
            _lists[i]->onActivated = [this, i](const std::string &id) { activated(i, id); };
        }
        _lists[0]->setItems(channelItems(ctx.store));
        _lists[1]->setItems(peopleItems(ctx.store));
        // The channels I'm not in come on demand: the list follows them in.
        _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
            if (ch.kind != model::ChangeKind::Roster)
                return;
            _lists[0]->setItems(channelItems(_ctx.store));
            if (_tab == 0)
                applyFilter();
        });
        ctx.backend.loadChannelDirectory();

        _search->edit().onChange = [this] { applyFilter(); };
        // The search field drives the open list: arrows move, Enter opens.
        _search->edit().onKey    = [this](const Event &e) { return _lists[_tab]->navKey(e); };
        selectTab(0);
    }
    ~ChannelFinder() override { _ctx.store.unobserve(_observer); }

    void selectTab(int tab) {
        _tab = tab;
        for (int i = 0; i < 2; ++i) {
            _tabs[i]->setActive(i == tab);
            _lists[i]->setVisible(i == tab);
        }
        _groupBtn->setVisible(tab == 1 && bool(_people));
        _search->edit().setPlaceholder(
            tab == 0 ? tr("Search for channels") : tr("Search for people")
        );
        applyFilter();
    }
    TextField &search() { return *_search; }
    Button    &groupButton() { return *_groupBtn; }

private:
    // The open list only; the other one is filtered when its tab opens.
    void applyFilter() { _lists[_tab]->applyFilter(_search->text()); }
    void activated(int list, const std::string &id) {
        // The ids are the Store's: channels by conversation id, people by user id.
        const ConvRef c  = list == 0 ? _ctx.store().findConversation(id) : model::kNoConv;
        const UserRef u  = list == 1 ? _ctx.store().findUser(id) : model::kNoUser;
        auto          ch = _channel;
        auto          pe = _person;
        accept();
        if (c != model::kNoConv && ch)
            ch(c);
        if (u != model::kNoUser && pe)
            pe(u);
    }

    screens::Context                                 &_ctx;
    Avatars                                          &_avatars;
    std::function<void(ConvRef)>                      _channel;
    std::function<void(UserRef)>                      _person;
    std::function<void()>                             _create;
    std::function<void(const std::vector<UserRef> &)> _people;
    TextField                                        *_search   = nullptr;
    Button                                           *_groupBtn = nullptr;
    FinderTab                                        *_tabs[2]  = {};
    BrowseList                                       *_lists[2] = {};
    int                                               _tab      = 0;
    model::Store::ObserverId                          _observer = 0;
    std::shared_ptr<int>                              _alive    = std::make_shared<int>(0);
};

// ── Create a channel ────────────────────────────────────────────────────────

constexpr int kMaxNameLen = 80;

class CreateChannel final : public Dialog {
public:
    CreateChannel(std::string workspaceName, std::function<void(const std::string &, bool)> done)
        : Dialog(tr("Create a channel")), _workspace(std::move(workspaceName)),
          _done(std::move(done)) {
        const text::Style bold  = pxFont(15, text::Weight::Bold, themed(C::FormText));
        const text::Style muted = pxFont(13, text::Weight::Regular, themed(C::FormTextMuted));

        // ── Step 1: Name ──
        _page1 = content()->add<View>();
        _page1->style().spacing(12);
        styledLabel(_page1, tr("Name"), bold);
        _name = _page1->add<TextField>(tr("e.g. plan-budget"), TextField::Size::Normal);
        _name->setPrefix("#");
        _name->setMaxLength(kMaxNameLen);
        styledLabel(
            _page1,
            tr("Channels are where conversations happen around a topic. Use a name that is easy "
               "to find and understand."),
            muted
        );
        auto *row1 = _page1->add<View>();
        row1->style().row().items(Align::Center).margins(0, 12, 0, 0);
        row1->add<View>()->style().flex(1);
        _next = makeButton(tr("Next"), V::Primary);
        row1->adopt(std::unique_ptr<View>(_next));
        _next->setEnabled(false);
        _next->onClick         = [this] { goNext(); };
        auto prevChange        = _name->edit().onChange; // the length counter
        _name->edit().onChange = [this, prevChange] {
            if (prevChange)
                prevChange();
            _next->setEnabled(!str::trim(_name->text()).empty());
        };
        _name->onReturn = [this] {
            if (_next->enabled())
                goNext();
        };

        // ── Step 2: Visibility ──
        _page2 = content()->add<View>();
        _page2->style().spacing(12);
        _subtitle = styledLabel(_page2, std::string(), muted);
        styledLabel(_page2, tr("Visibility"), bold)->style().margins(0, 4, 0, 0);
        _visibility = _page2->add<RadioGroup>(
            std::vector<std::string>{std::string(), tr("Private — only specific people")}
        );
        styledLabel(_page2, tr("Can only be viewed or joined by invitation"), muted)
            ->style()
            .margins(32, 0, 0, 0);
        auto *row2 = _page2->add<View>();
        row2->style().row().items(Align::Center).spacing(12).margins(0, 12, 0, 0);
        styledLabel(row2, tr("Step 2 of 2"), muted);
        row2->add<View>()->style().flex(1);
        Button *back = makeButton(tr("Back"), V::Secondary);
        Button *make = makeButton(tr("Create"), V::Primary);
        row2->adopt(std::unique_ptr<View>(back));
        row2->adopt(std::unique_ptr<View>(make));
        back->onClick = [this] { showStep(1); };
        make->onClick = [this] {
            const std::string name = channelName(_name->text());
            const bool        priv = _visibility->selected() == 1;
            auto              cb   = std::move(_done);
            accept();
            if (cb)
                cb(name, priv);
        };
        showStep(1);
    }

    TextField &name() { return *_name; }

private:
    void goNext() {
        _subtitle->setText("# " + channelName(_name->text()));
        _visibility->radio(0)->setLabel(
            i18n::arg(
                tr("Public — anyone in %1"), _workspace.empty() ? tr("this workspace") : _workspace
            )
        );
        showStep(2);
    }
    void showStep(int step) {
        _page1->setVisible(step == 1);
        _page2->setVisible(step == 2);
        if (step == 1)
            _name->edit().focus();
    }

    std::string                                    _workspace;
    std::function<void(const std::string &, bool)> _done;
    View                                          *_page1 = nullptr, *_page2 = nullptr;
    TextField                                     *_name       = nullptr;
    Button                                        *_next       = nullptr;
    Label                                         *_subtitle   = nullptr;
    RadioGroup                                    *_visibility = nullptr;
};

} // namespace

// ── Rows ────────────────────────────────────────────────────────────────────

std::vector<BrowseList::Item> channelItems(const model::Store &store) {
    std::vector<ConvRef> channels;
    for (ConvRef c = 0; c < store.conversationCount(); ++c)
        if (!store.conversation(c).isDirect() && !store.conversation(c).id.empty())
            channels.push_back(c);
    std::vector<std::string> keys(store.conversationCount());
    for (ConvRef c : channels)
        keys[c] = utf8::foldCase(store.conversation(c).name);
    std::stable_sort(channels.begin(), channels.end(), [&](ConvRef a, ConvRef b) {
        return keys[a] < keys[b];
    });
    std::vector<BrowseList::Item> items;
    items.reserve(channels.size());
    for (ConvRef r : channels) {
        const model::Conversation &c = store.conversation(r);
        BrowseList::Item           it;
        it.id    = c.id;
        it.title = c.name;
        if (c.memberCount > 0) {
            it.subtitle = i18n::arg(
                tr("%1 %2"),
                str::number(c.memberCount),
                c.memberCount == 1 ? tr("member") : tr("members")
            );
            if (!c.topic.empty())
                it.subtitle += " \xC2\xB7 " + c.topic;
        } else {
            it.subtitle = c.topic;
        }
        it.titleIcon = uint16_t(convIcon(c));
        if (c.member)
            it.badge = tr("Joined");
        it.searchKey = utf8::foldCase(str::concat({c.name, " ", c.topic}));
        items.push_back(std::move(it));
    }
    return items;
}

std::vector<BrowseList::Item> peopleItems(const model::Store &store) {
    // Strangers surface only because they share a channel with us, but
    // conversations.open rejects them: a dead end as a DM target.
    std::vector<UserRef> people;
    for (UserRef u = 0; u < store.userCount(); ++u) {
        const model::User &x = store.user(u);
        if (!x.deleted && !x.stranger && !x.placeholder && !x.id.empty())
            people.push_back(u);
    }
    std::vector<std::string> keys(store.userCount());
    for (UserRef u : people)
        keys[u] = utf8::foldCase(
            store.user(u).displayName.empty() ? store.user(u).name : store.user(u).displayName
        );
    std::stable_sort(people.begin(), people.end(), [&](UserRef a, UserRef b) {
        return keys[a] < keys[b];
    });
    std::vector<BrowseList::Item> items;
    items.reserve(people.size());
    for (UserRef r : people) {
        const model::User &u    = store.user(r);
        const std::string &name = u.displayName.empty() ? u.name : u.displayName;
        BrowseList::Item   it;
        it.id     = u.id;
        it.title  = name;
        it.avatar = u.avatar;
        if (!u.name.empty() && u.name != name)
            it.subtitle = "@" + u.name;
        it.searchKey = screens::userSearchKey(u);
        items.push_back(std::move(it));
    }
    return items;
}

std::string channelName(std::string_view typed) {
    std::string s = utf8::foldCase(str::trim(typed));
    std::replace(s.begin(), s.end(), ' ', '-');
    return s;
}

// ── Entry points ────────────────────────────────────────────────────────────

Popup *showFindChannel(
    screens::Context                                 &ctx,
    Window                                           &w,
    Avatars                                          &avatars,
    int                                               tab,
    std::function<void(ConvRef)>                      channel,
    std::function<void(UserRef)>                      person,
    std::function<void()>                             create,
    std::function<void(const std::vector<UserRef> &)> people
) {
    auto d = std::make_unique<ChannelFinder>(
        ctx, avatars, std::move(channel), std::move(person), std::move(create), std::move(people)
    );
    auto *raw = d.get();
    if (tab == 1)
        raw->selectTab(1);
    w.showPopup(std::move(d));
    raw->search().edit().focus();
    return raw;
}

Popup *showCreateChannel(
    Window &w, const std::string &workspaceName, std::function<void(const std::string &, bool)> done
) {
    auto  d   = std::make_unique<CreateChannel>(workspaceName, std::move(done));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->name().edit().focus();
    return raw;
}

} // namespace shell
