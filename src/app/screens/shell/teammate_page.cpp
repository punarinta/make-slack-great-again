#include "screens/shell/teammate_page.h"

#include "app/claude/common.h"
#include "app/screens/common/file_dialogs.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "screens/shell/recent_folders.h"
#include "screens/shell/shell_text.h"

#include <algorithm>

using namespace ui;
using i18n::tr;
using model::ConvRef;
using model::kNoConv;

namespace shell {

namespace {

constexpr float kAvatar = 56, kAvatarRadius = 12;

// Last activity of a conversation, epoch micros: the later of the latest message and the last read.
model::Ts activityOf(const model::Conversation &c) {
    return std::max(c.latest, c.lastRead);
}

} // namespace

TeammatePage::TeammatePage(
    screens::Context &ctx, Avatars &avatars, Settings &settings, std::function<void()> saveSettings
)
    : _ctx(ctx), _avatars(avatars), _settings(settings), _saveSettings(std::move(saveSettings)) {
    style().column().flex(1);

    // ── Who it is ──
    auto *header = add<View>();
    header->style().row().spacing(16).padding(24, 24, 24, 16);
    _avatar = header->add<Avatar>();
    _avatar->style().size(kAvatar, kAvatar).noShrink().alignSelf(Align::Start);
    _avatar->setRadius(kAvatarRadius);
    _avatar->setPlaceholder(C::PresenceAway);
    auto *who = header->add<View>();
    who->style().flex(1).spacing(4).justifyContent(Justify::Center);
    who->style().shrink = 1;
    _name               = who->add<Label>();
    _description        = who->add<Label>();
    auto *edit =
        header->add<Button>(tr("Edit teammate…"), Button::Kind::Secondary, Button::Form::Small);
    edit->setFocusable(false);
    edit->style().alignSelf(Align::Start).noShrink();
    edit->onClick = [this] {
        if (!_mate.id.empty() && onEdit)
            onEdit(_mate.id);
    };

    // ── Its sessions ──
    styledLabel(this, tr("Sessions"), pxFont(16, text::Weight::Bold, themed(C::Text)))
        ->style()
        .padding(24, 12, 24, 4);
    auto *stack = add<View>();
    stack->style().stack().flex(1);
    _empty = stack->add<Label>(
        tr("No sessions yet. Write below to start one."), Font::Body, C::TextMuted
    );
    _empty->setAlign(text::LayoutOptions::Align::Center);
    _empty->style().alignSelf(Align::Center).padding(24);
    _list = stack->add<BrowseList>(avatars);
    _list->setOnContentSurface(true);
    _list->setAvatarRadius(8);
    _list->setVisible(false);
    _list->onActivated = [this](const std::string &id) {
        const ConvRef c = _ctx.store().findConversation(id);
        if (c != kNoConv && onOpenSession)
            onOpenSession(c);
    };

    // ── Where a new one starts ──
    add<Separator>(false, C::FormDivider);
    auto *footer = add<View>();
    // No bottom padding: the composer below opens with its own top margin.
    footer->style().row().items(Align::Center).spacing(8).padding(24, 8, 24, 0);
    _folderLabel = footer->add<Label>();
    _folderLabel->style().flex(1);
    _folderLabel->style().shrink = 1;
    _folderLabel->setMaxLines(1);
    _folderBtn = footer->add<Button>(tr("Change folder"), Button::Kind::Ghost, Button::Form::Small);
    _folderBtn->setFocusable(false);
    _folderBtn->style().noShrink();
    _folderBtn->onClick = [this] { showFolderMenu(); };

    // Sessions come, go, start and finish working: follow them while shown.
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        using K = model::ChangeKind;
        if (ch.kind == K::Meta || ch.kind == K::Roster || ch.kind == K::Users)
            rebuildSoon();
    });
}

TeammatePage::~TeammatePage() {
    _ctx.store.unobserve(_observer);
    if (_timer)
        _ctx.app.platform().cancelTimer(_timer);
}

void TeammatePage::paint(gfx::Painter &p) {
    p.fillRect(bounds(), color(C::Surface));
    View::paint(p);
}

void TeammatePage::visibilityChanged(bool on) {
    if (on)
        rebuildSoon();
}

void TeammatePage::open(const model::Backend::AgentRole &mate) {
    _mate = mate;
    setStyledText(_name, mate.name, pxFont(24, text::Weight::Bold, themed(C::Text)));
    setStyledText(
        _description, mate.description, pxFont(15, text::Weight::Regular, themed(C::TextMuted))
    );
    _description->setVisible(!mate.description.empty());
    _avatar->setInitial(mate.name);
    _avatar->setBitmap(_avatars.get(mate.avatar, int(kAvatar * 2)));
    const std::string home  = _ctx.app.platform().standardDir(plat::StandardDir::Home);
    const std::string dir   = recent_folders::teammateFolder(_settings, mate.id, home);
    // Moved or deleted since: start in what is left of it, and stop offering it.
    const auto        isDir = [](const std::string &p) { return file::isDir(p); };
    std::string       here  = dir;
    if (!isDir(dir)) {
        here = recent_folders::existingFolder(dir, home, isDir);
        recent_folders::forgetFolder(_settings, dir, here);
        if (_saveSettings)
            _saveSettings();
    }
    setFolder(here);
    rebuild(true);
}

void TeammatePage::clear() {
    _mate = {};
    _list->setItems({});
    _list->setVisible(false);
    _empty->setVisible(true);
}

void TeammatePage::rebuildSoon() {
    if (_timer || !visible())
        return; // open() rebuilds when the page comes back
    std::weak_ptr<int> alive = _alive;
    _timer                   = _ctx.app.platform().addTimer(0, false, [this, alive] {
        if (alive.expired())
            return;
        _timer = 0;
        if (visible())
            rebuild();
    });
}

void TeammatePage::rebuild(bool fresh) {
    if (_mate.id.empty())
        return;
    const model::Store &st = _ctx.store;
    struct Row {
        BrowseList::Item item;
        bool             working  = false;
        model::Ts        activity = 0;
    };
    std::vector<Row> rows;
    const int64_t    now = _ctx.backend().nowSecs();
    for (ConvRef c = 0; c < st.conversationCount(); ++c) {
        const model::Conversation &cv = st.conversation(c);
        if (!cv.member || cv.kind != model::ConvKind::Dm || cv.dmUser >= st.userCount() ||
            _ctx.backend().agentSessionRole(c) != _mate.id)
            continue;
        const model::User &u = st.user(cv.dmUser);
        Row                r;
        r.item.id     = cv.id;
        r.item.avatar = u.avatar.empty() ? _mate.avatar : u.avatar;
        r.item.title  = st.displayName(c);
        r.working     = u.active;
        r.activity    = activityOf(cv);
        std::vector<std::string> sub;
        if (!cv.topic.empty())
            sub.push_back(cv.topic); // the folder (and "no permission checks")
        if (r.activity > 0)
            sub.push_back(base::relativeTime(model::tsSecs(r.activity), now));
        for (size_t i = 0; i < sub.size(); ++i)
            r.item.subtitle += (i ? " \xC2\xB7 " : "") + sub[i];
        // What needs a look first: news, else what it's doing right now.
        if (cv.mentions > 0) {
            r.item.badge       = i18n::trn("%n new", "%n new", int64_t(cv.mentions));
            r.item.badgeStrong = true;
        } else if (!u.statusText.empty()) {
            r.item.badge = u.statusText;
        }
        r.item.badgeCheck = false;
        rows.push_back(std::move(r));
    }
    // Working ones on top, then the most recent.
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.working != b.working)
            return a.working;
        return a.activity > b.activity;
    });
    std::vector<BrowseList::Item> items;
    items.reserve(rows.size());
    for (Row &r : rows)
        items.push_back(std::move(r.item));
    const bool none = items.empty();
    if (fresh)
        _list->setItems(std::move(items));
    else
        _list->refreshItems(std::move(items)); // a change: where it was scrolled to
    _list->setVisible(!none);
    _empty->setVisible(none);
}

void TeammatePage::setFolder(const std::string &dir) {
    _folder                  = dir;
    _blocker                 = _ctx.backend().agentSessionBlocker(dir);
    // "Start new session in <b>~/src/x</b>".
    const std::string    fmt = tr("Start new session in %1");
    const size_t         at  = fmt.find("%1");
    const text::Style    reg = pxFont(13, text::Weight::Regular, themed(C::TextMuted));
    text::Style          b   = reg;
    text::AttributedText t;
    b.weight = text::Weight::Bold;
    t.append(fmt.substr(0, at), reg);
    t.append(claude::homeRelative(dir), b);
    if (at != std::string::npos)
        t.append(fmt.substr(at + 2), reg);
    _folderLabel->setRichText(std::move(t));
    if (onFolderChanged)
        onFolderChanged();
}

// The folders sessions were last started in or work in, newest first, then
// "Browse…" for any other. Built on every open: sessions come and go.
std::vector<MenuItem> TeammatePage::folderMenuItems(std::vector<std::string> *paths) const {
    const model::Store                        &st = _ctx.store;
    std::vector<recent_folders::SessionFolder> sessions;
    for (ConvRef c = 0; c < st.conversationCount(); ++c) {
        const model::Conversation &cv = st.conversation(c);
        if (cv.kind != model::ConvKind::Dm)
            continue;
        const std::string dir = _ctx.backend().agentSessionFolder(c);
        if (!dir.empty())
            sessions.push_back({dir, model::tsSecs(activityOf(cv))});
    }
    const auto        isDir   = [](const std::string &p) { return file::isDir(p); };
    auto              choices = recent_folders::rank(_settings.claudeRecentDirs, sessions, isDir);
    // The current one is always there to see (e.g. home, never picked).
    const std::string current = recent_folders::normalized(_folder);
    if (!current.empty() && isDir(current) &&
        std::none_of(choices.begin(), choices.end(), [&](const auto &c) {
            return c.path == current;
        }))
        choices.insert(choices.begin(), recent_folders::Choice{current});

    std::vector<MenuItem> items;
    const int64_t         now = _ctx.backend().nowSecs();
    for (const auto &c : choices) {
        MenuItem it;
        it.id      = int(items.size()) + 1;
        it.label   = claude::homeRelative(c.path);
        it.checked = c.path == current;
        std::vector<std::string> hint;
        if (c.sessions > 0)
            hint.push_back(i18n::trn("%n session", "%n sessions", c.sessions));
        if (c.lastUsed > 0)
            hint.push_back(base::relativeTime(c.lastUsed, now));
        for (size_t i = 0; i < hint.size(); ++i)
            it.hint += (i ? " \xC2\xB7 " : "") + hint[i];
        items.push_back(std::move(it));
        if (paths)
            paths->push_back(c.path);
    }
    items.push_back(MenuItem::separatorItem());
    MenuItem browse;
    browse.id    = -1;
    browse.label = tr("Browse…");
    items.push_back(std::move(browse));
    return items;
}

void TeammatePage::showFolderMenu() {
    Window *w = window();
    if (!w)
        return;
    std::vector<std::string> paths;
    auto                     items = folderMenuItems(&paths);
    const RectF              r     = _folderBtn->windowRect();
    std::weak_ptr<int>       alive = _alive;
    Menu::show(*w, {r.x, r.y + r.h + 2, 0, 0}, std::move(items), [this, alive, paths](int id) {
        if (alive.expired())
            return;
        if (id == -1) // after the menu is gone: the dialog runs on its own
            _ctx.app.platform().post([this, alive] {
                if (!alive.expired())
                    chooseFolder();
            });
        else if (id >= 1 && size_t(id) <= paths.size())
            pickFolder(paths[size_t(id) - 1]);
    });
}

void TeammatePage::chooseFolder() {
    plat::FileDialogDesc d;
    d.mode                   = plat::FileDialogDesc::Mode::PickFolder;
    d.title                  = i18n::arg(tr("Folder for new sessions with the %1"), _mate.name);
    d.initialDir             = _folder;
    std::weak_ptr<int> alive = _alive;
    screens::fileDialog(_ctx, std::move(d), [this, alive](std::vector<std::string> paths) {
        if (!alive.expired() && !paths.empty() && !paths.front().empty())
            pickFolder(paths.front());
    });
}

// Today's default for this teammate, and the most recent pick for all of them.
void TeammatePage::pickFolder(const std::string &dir) {
    if (_mate.id.empty())
        return;
    recent_folders::pickTeammateFolder(_settings, _mate.id, dir);
    if (_saveSettings)
        _saveSettings();
    setFolder(dir);
}

} // namespace shell
