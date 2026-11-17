#include "screens/shell/scheduled_page.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/shell/message_search.h"

using namespace ui;
using i18n::arg;
using i18n::tr;
using model::ConvRef;
using ScheduledItem = model::Store::ScheduledItem;

namespace shell {

namespace {

std::string whenLabel(const ScheduledItem &s) {
    return arg(tr("Sends %1"), base::formatDateTime(s.at));
}

} // namespace

// One scheduled message: the conversation header outside the bordered body,
// the message as I'll post it, the send-time footer with its two actions.
class ScheduledPage::Card final : public View {
public:
    Card(ScheduledPage &page, const ScheduledItem &item) : _page(page), _item(item) {
        const model::Store &st = page._ctx.store;
        style().spacing(12);
        View *nameRow = cardHeader(this, st, item.conv, [this] {
            if (_page.onOpenChannel)
                _page.onOpenChannel(_item.conv);
        });
        if (item.thread)
            nameRow->add<Label>(tr("in a thread"), Font::Small, C::TextMuted);

        auto              *body = cardBody(this);
        const model::User &me   = st.user(st.me);
        const std::string  who  = st.me != model::kNoUser ? std::string(me.label()) : std::string();
        View *col = fillMessageRow(body->add<View>(), page._avatars, who, me.avatar, {});
        const std::string text = searchPreview(st, item.text);
        col->add<Label>(text.empty() ? std::string(tr("No preview available")) : text, Font::Body)
            ->setMaxLines(3);

        auto *footer = body->add<View>();
        footer->style().row().spacing(12).items(Align::Center);
        footer->add<IconView>(gfx::Icon::Clock, 13, C::TextMuted);
        footer->add<Label>(whenLabel(item), Font::Caption, C::TextMuted)->style().flex(1);
        // Send now needs the thread a reply goes to; a list that doesn't
        // say (a token workspace's) leaves only Cancel.
        if (item.threadKnown) {
            auto *now    = footer->add<TextLink>(tr("Send now"), Font::Control, C::Link, false);
            now->onClick = [this] { sendNow(); };
        }
        auto *cancelBtn    = footer->add<TextLink>(tr("Cancel"), Font::Control, C::Link, false);
        cancelBtn->onClick = [this] { cancel(); };
    }

    bool canSendNow() const { return _item.threadKnown; }

    // Unscheduled, then posted the usual way (the pending copy shows in the
    // conversation). Only once the service let go of it: never twice.
    void sendNow() {
        if (_busy || !_item.threadKnown)
            return;
        _busy                     = true;
        screens::Context   &ctx   = _page._ctx;
        std::weak_ptr<int>  alive = _page._alive;
        const std::string   ws    = ctx.store().workspaceId;
        const ScheduledItem item  = _item;
        ScheduledPage      *page  = &_page;
        ctx.backend().cancelScheduled(
            item.id, [page, &ctx, alive, ws, item](bool ok, const std::string &) {
                if (alive.expired() || ctx.store().workspaceId != ws)
                    return;
                if (!ok)
                    return page->reset(); // the banner said why; the card again
                ctx.backend().send(item.conv, item.text, item.thread, nullptr);
            }
        );
    }
    void cancel() {
        if (_busy)
            return;
        _busy                    = true;
        std::weak_ptr<int> alive = _page._alive;
        ScheduledPage     *page  = &_page;
        _page._ctx.backend().cancelScheduled(_item.id, [page, alive](bool ok, const std::string &) {
            if (!ok && !alive.expired())
                page->reset();
        });
    }
    const ScheduledItem &item() const { return _item; }

private:
    ScheduledPage &_page;
    ScheduledItem  _item;
    bool           _busy = false; // a click went out (the list answers by rebuilding)
};

ScheduledPage::ScheduledPage(screens::Context &ctx, Avatars &avatars)
    : OverviewPage(ctx, avatars, tr("Scheduled messages")) {}

void ScheduledPage::onChange(const model::Change &ch) {
    if (!visible() || rebuildQueued())
        return;
    // The list fires Meta without a conversation; names come with the roster.
    const bool mine = (ch.kind == model::ChangeKind::Meta && ch.conv == model::kNoConv) ||
                      ch.kind == model::ChangeKind::Reset || ch.kind == model::ChangeKind::Roster ||
                      ch.kind == model::ChangeKind::Users;
    if (mine)
        rebuildSoon();
}

void ScheduledPage::open() {
    rebuild();
    _ctx.backend().refreshScheduled(); // ones scheduled or sent elsewhere
}

void ScheduledPage::reset() {
    _items.clear();
    _cards.clear();
    rebuild();
}

void ScheduledPage::clear() {
    _list->clearChildren();
    _cards.clear();
    _items.clear();
    _names.clear();
    setStatus({});
}

std::string ScheduledPage::namesKey() const {
    const model::Store &st = _ctx.store();
    std::string key = st.me != model::kNoUser
                          ? str::concat({st.user(st.me).label(), "\n", st.user(st.me).avatar})
                          : std::string();
    for (const ScheduledItem &s : st.scheduled())
        if (s.conv < st.conversationCount())
            key += str::concat({"\n", st.displayName(s.conv)});
    return key;
}

void ScheduledPage::rebuild() {
    // Unchanged (an unrelated Meta): keep the cards and their busy state.
    // A new name (mine, a conversation's) is a change.
    const auto &now  = _ctx.store().scheduled();
    const auto  same = [](const ScheduledItem &a, const ScheduledItem &b) {
        return a.id == b.id && a.version == b.version && a.at == b.at && a.text == b.text &&
               a.conv == b.conv && a.thread == b.thread;
    };
    std::string names = namesKey();
    if (!_cards.empty() && names == _names &&
        std::equal(_items.begin(), _items.end(), now.begin(), now.end(), same))
        return;
    _names = std::move(names);
    _list->clearChildren();
    _cards.clear();
    _items.clear();
    for (const ScheduledItem &s : now) {
        if (s.conv >= _ctx.store().conversationCount())
            continue;
        _cards.push_back(_list->add<Card>(*this, s));
        _items.push_back(s);
    }
    setStatus(
        _cards.empty()
            ? std::string(tr("Messages you schedule will appear here until they're sent."))
            : std::string()
    );
}

std::string ScheduledPage::whenText(size_t i) const {
    return i < _items.size() ? whenLabel(_items[i]) : std::string();
}

bool ScheduledPage::canSendNow(size_t i) const {
    return i < _cards.size() && _cards[i]->canSendNow();
}

void ScheduledPage::sendNow(size_t i) {
    if (i < _cards.size())
        _cards[i]->sendNow();
}

void ScheduledPage::cancel(size_t i) {
    if (i < _cards.size())
        _cards[i]->cancel();
}

} // namespace shell
