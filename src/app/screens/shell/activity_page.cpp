#include "screens/shell/activity_page.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "app/screens/common/message_rules.h"
#include "screens/shell/header.h"
#include "screens/shell/message_search.h"

using namespace ui;
using i18n::arg;
using i18n::tr;
using model::ConvRef;
using model::Ts;
using Kind = model::Backend::ActivityItem::Kind;

namespace shell {

namespace {

// The tabs, in the strip's order.
constexpr ActivityPage::Filter kTabs[] = {
    ActivityPage::Filter::All,
    ActivityPage::Filter::Dms,
    ActivityPage::Filter::Mentions,
    ActivityPage::Filter::Threads,
};

gfx::Icon kindIcon(Kind k) {
    switch (k) {
    case Kind::Mention:
        return gfx::Icon::AtSign;
    case Kind::Thread:
        return gfx::Icon::MessagesSquare;
    case Kind::Reaction:
        return gfx::Icon::Smile;
    case Kind::Dm:
        return gfx::Icon::MessageCircle;
    case Kind::Channel:
        return gfx::Icon::Hash;
    }
    return gfx::Icon::AtSign;
}

// Where it happened, in words.
std::string contextText(const model::Store &st, const ActivityPage::Item &it) {
    const std::string where = searchConvLabel(st, it.conv);
    switch (it.kind) {
    case Kind::Mention:
        return arg(tr("Mention in %1"), where);
    case Kind::Thread:
        return arg(tr("Thread in %1"), where);
    case Kind::Reaction:
        return arg(
            tr("Reacted %1 to your message in %2"),
            searchPreview(st, ":" + it.reaction + ":"),
            where
        );
    case Kind::Dm:
        return st.conversation(it.conv).kind == model::ConvKind::Group
                   ? std::string(tr("Group message"))
                   : std::string(tr("Direct message"));
    case Kind::Channel:
        return arg(tr("New messages in %1"), where);
    }
    return where;
}

} // namespace

// ── Row ─────────────────────────────────────────────────────────────────────

ActivityPage::Row::Row(ActivityPage &page, size_t index) : _page(page), _index(index) {
    screens::Context   &ctx = page._ctx;
    const model::Store &st  = ctx.store;
    const Item         &it  = page._items[index];
    setLook({C::None, C::SurfaceHover, C::SurfaceHover, C::None, 0});
    setCursor(plat::Cursor::Hand);
    onClick = [this] { _page.activate(_index); };

    // A reaction is the reactor's; the rest, the message's author. A
    // message that couldn't be loaded: the conversation.
    const model::Message &m = it.message;
    std::string           who, picture;
    if (it.kind == Kind::Reaction && it.actor != model::kNoUser) {
        const model::User &u = st.user(it.actor);
        who                  = std::string(u.label());
        picture              = u.avatar;
    } else if (m.ts) {
        who     = std::string(screens::authorName(st, m));
        picture = screens::authorAvatar(st, m);
    } else {
        who = searchConvLabel(st, it.conv);
    }
    View *col = fillMessageRow(
        this,
        page._avatars,
        who,
        picture,
        base::dateTimeLabel(model::tsSecs(it.ts), ctx.backend.nowSecs())
    );
    style().padding(16, 12, 16, 12);
    const std::string text = m.ts ? searchPreview(st, m.text) : std::string();
    auto             *body = col->add<Label>(
        text.empty() ? std::string(tr("No preview available")) : text,
        Font::Body,
        m.ts ? C::Text : C::TextMuted
    );
    body->setMaxLines(2);
    auto *line = col->add<View>();
    line->style().row().spacing(6).items(Align::Center).margins(0, 4, 0, 0);
    line->add<IconView>(kindIcon(it.kind), 13, C::TextMuted);
    _context = line->add<Label>(contextText(st, it), Font::Small, C::TextMuted);
    _context->setMaxLines(1);
    _context->style().shrink = 1;

    // The unread mark (its count of new messages, else a dot), and in its
    // place while hovered the read toggle.
    auto *side = add<View>();
    side->style().row().items(Align::Center).noShrink();
    _badge   = side->add<Badge>(0, C::Accent, C::AccentText);
    _readBtn = side->add<Button>(gfx::Icon::Check, std::string(tr("Mark as read")));
    _readBtn->setFocusable(false);
    _readBtn->onClick = [this] { _page.toggleRead(_index); };
    refreshLook();
}

bool ActivityPage::Row::unread() const {
    return _page._items[_index].unread;
}

std::string ActivityPage::Row::context() const {
    return _context->text();
}

bool ActivityPage::Row::onEvent(Event &e) {
    if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave) {
        const bool r = Clickable::onEvent(e);
        refreshLook();
        return r;
    }
    return Clickable::onEvent(e);
}

void ActivityPage::Row::refreshLook() {
    const Item &it = _page._items[_index];
    _badge->setCount(it.unread && it.count > 1 ? it.count : 0);
    _badge->setDot(it.unread && it.count <= 1);
    _badge->setVisible(it.unread && !hovered());
    _readBtn->setIcon(it.unread ? gfx::Icon::Check : gfx::Icon::Mail);
    _readBtn->setTooltip(it.unread ? tr("Mark as read") : tr("Mark as unread"));
    _readBtn->setVisible(hovered());
    update();
}

// ── Page ────────────────────────────────────────────────────────────────────

ActivityPage::ActivityPage(screens::Context &ctx, Avatars &avatars)
    : OverviewPage(ctx, avatars, tr("Activity")) {
    // The tabs go right under the title, in the conversation tabs' place
    // (their own divider replaces the title's).
    auto tabs = std::make_unique<TabStrip>();
    _tabs     = tabs.get();
    adopt(std::move(tabs), 1);
    _rule->setVisible(false);
    _tabs->addTab(tr("All"));
    _tabs->addTab(gfx::Icon::MessageCircle, tr("DMs"));
    _tabs->addTab(gfx::Icon::AtSign, tr("Mentions"));
    _tabs->addTab(gfx::Icon::MessagesSquare, tr("Threads"));
    _tabs->onSelect = [this](int i) {
        _filter = kTabs[i];
        reload();
    };

    // The toolbar over the list: the unread filter.
    View *content = _scroll->content();
    auto  bar     = std::make_unique<View>();
    bar->style().row().items(Align::Center);
    _unread = bar->add<Button>(tr("Unread messages"), Button::Kind::Secondary, Button::Form::Small);
    _unread->setFocusable(false);
    _unread->onClick = [this] { setUnreadOnly(!_unreadOnly); };
    content->adopt(std::move(bar), 0);

    // One bordered card holds the rows (hairlines between them).
    _card = _list->add<View>();
    _card->setBackground(C::Surface, 8);
    _card->setBorder(C::Border);
    _card->setClipChildren(true);
    _card->setVisible(false);
    _more = _list->add<Button>(tr("Show more"), Button::Kind::Secondary, Button::Form::Small);
    _more->setFocusable(false);
    _more->style().alignSelf(Align::Start);
    _more->setVisible(false);
    _more->onClick = [this] { showMore(); };
}

void ActivityPage::setFilter(Filter f) {
    for (int i = 0; i < int(std::size(kTabs)); ++i)
        if (kTabs[i] == f && _tabs->active() != i) {
            _tabs->setActive(i);
            _filter = f;
            reload();
        }
}

void ActivityPage::setUnreadOnly(bool on) {
    if (on == _unreadOnly)
        return;
    _unreadOnly = on;
    _unread->setChecked(on);
    // On: the accent's tint, as the official client shows it.
    _unread->setTextColor(on ? C::Link : C::Text);
    reload();
}

void ActivityPage::open() {
    reload();
}

void ActivityPage::clear() {
    ++_generation; // an answer in flight is dropped
    _loading = false;
    _nextCursor.clear();
    _items.clear();
    rebuild();
    setStatus({});
}

void ActivityPage::reload() {
    _loading = false; // a tab switch outranks the load in flight
    loadPage({});
}

void ActivityPage::showMore() {
    if (!_nextCursor.empty())
        loadPage(_nextCursor);
}

void ActivityPage::loadPage(std::string cursor) {
    if (_loading)
        return;
    _loading = true;
    _more->setEnabled(false);
    const bool first = cursor.empty();
    if (first) {
        _items.clear();
        _nextCursor.clear();
        rebuild();
        setStatus(tr("Loading activity…"));
    }
    const uint32_t     gen   = ++_generation;
    std::weak_ptr<int> alive = _alive;
    _ctx.backend.loadActivity(
        _filter,
        _unreadOnly,
        std::move(cursor),
        [this, alive, gen](bool ok, model::Backend::ActivityFeed page) {
            if (alive.expired() || gen != _generation)
                return;
            _loading = false;
            _more->setEnabled(true);
            if (!ok) {
                // Slack's code with it: the one clue a report can carry.
                if (_items.empty())
                    setStatus(
                        page.error.empty()
                            ? std::string(tr("Couldn't load activity. Try again later."))
                            : str::concat(
                                  {tr("Couldn't load activity. Try again later."),
                                   " (",
                                   page.error,
                                   ")"}
                              )
                    );
                return;
            }
            for (Item &it : page.items)
                if (it.conv < _ctx.store().conversationCount())
                    _items.push_back(std::move(it));
            _nextCursor = std::move(page.nextCursor);
            rebuild();
            setStatus(
                !_items.empty() ? std::string()
                : _unreadOnly   ? std::string(tr("You're all caught up."))
                                : std::string(
                                      tr("Mentions, replies, reactions and direct "
                                         "messages will appear here.")
                                  )
            );
        }
    );
}

void ActivityPage::rebuild() {
    _card->clearChildren();
    _rows.clear();
    for (size_t i = 0; i < _items.size(); ++i) {
        if (i)
            _card->add<Separator>(false, C::Border);
        _rows.push_back(_card->add<Row>(*this, i));
    }
    _card->setVisible(!_items.empty());
    _more->setVisible(!_nextCursor.empty());
}

// Names and avatars that arrive later (users resolved after the page
// came) redraw the rows.
void ActivityPage::onChange(const model::Change &ch) {
    if (ch.kind == model::ChangeKind::Users && visible() && !_items.empty())
        rebuildSoon();
}

void ActivityPage::activate(size_t i) {
    if (i >= _items.size())
        return;
    Item &it = _items[i];
    if (it.unread) {
        _ctx.backend.setActivityRead(it, true);
        it.unread = false;
    }
    const ConvRef conv = it.conv;
    if (it.kind == Kind::Thread) {
        if (onOpenThread)
            onOpenThread(conv, it.thread);
    } else if (onOpenMessage) {
        onOpenMessage(conv, it.ts, it.thread);
    }
}

void ActivityPage::toggleRead(size_t i) {
    if (i >= _items.size())
        return;
    Item &it  = _items[i];
    it.unread = !it.unread;
    _ctx.backend.setActivityRead(it, !it.unread);
    if (i < _rows.size())
        _rows[i]->refreshLook();
}

} // namespace shell
