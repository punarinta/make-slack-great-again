#include "screens/shell/saved_page.h"

#include "base/i18n.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/shell/message_search.h"

#include <algorithm>

using namespace ui;
using i18n::arg;
using i18n::tr;
using model::ConvRef;
using model::Ts;
using SavedItem = model::Store::SavedItem;

namespace shell {

namespace {

std::string dueLabel(const SavedItem &s) {
    if (s.due <= 0)
        return tr("Saved for later");
    return arg(tr("Reminder set for %1"), base::formatDateTime(s.due));
}

} // namespace

// One saved message: the conversation header outside the bordered body,
// the message row, the due footer.
class SavedPage::Card final : public View {
public:
    Card(SavedPage &page, const SavedItem &item) : _page(page), _item(item) {
        screens::Context   &ctx = page._ctx;
        const model::Store &st  = ctx.store;
        style().spacing(12);
        cardHeader(this, st, item.conv, [this] {
            if (_page.onOpenChannel)
                _page.onOpenChannel(_item.conv);
        });
        auto *body = cardBody(this);
        // The message, chat-style; a click anywhere jumps to it.
        auto *row  = body->add<Clickable>();
        row->setLook({C::None, C::None, C::None, C::None, 0});
        row->setCursor(plat::Cursor::Hand);
        row->onClick           = [this] { open(); };
        const model::User &u   = st.user(item.author);
        // A reminder set from another client carries no author.
        const std::string  who = !item.botName.empty()           ? item.botName
                                 : item.author != model::kNoUser ? std::string(u.label())
                                                                 : std::string(tr("Message"));
        View              *col = fillMessageRow(
            row,
            page._avatars,
            who,
            !item.botAvatar.empty() ? item.botAvatar : u.avatar,
            base::dateTimeLabel(model::tsSecs(item.ts), ctx.backend().nowSecs())
        );
        const std::string text = searchPreview(st, item.text);
        col->add<Label>(text.empty() ? std::string(tr("No preview available")) : text, Font::Body)
            ->setMaxLines(1);

        // Overdue reminders already alarmed: the clock line in the mention
        // badge's colour. A plain bookmark shows a bookmark, not the clock.
        const bool reminder = item.due > 0;
        const bool overdue  = reminder && item.due <= base::nowSecs();
        const C    dueCol   = overdue ? C::Badge : C::TextMuted;
        auto      *footer   = body->add<View>();
        footer->style().row().spacing(8).items(Align::Center);
        footer->add<IconView>(reminder ? gfx::Icon::AlarmClock : gfx::Icon::Bookmark, 13, dueCol);
        footer->add<Label>(dueLabel(item), Font::Caption, dueCol)->style().flex(1);
        auto *removeBtn    = footer->add<TextLink>(tr("Remove"), Font::Control, C::Link, false);
        removeBtn->onClick = [this] { remove(); };
    }

    void open() {
        if (_page.onOpenMessage)
            _page.onOpenMessage(_item.conv, _item.ts, _item.thread);
    }
    // The saved item goes, reminder or not.
    void remove() {
        if (_item.due > 0)
            _page._ctx.backend().setReminder(_item.conv, _item.ts, 0);
        else
            _page._ctx.backend().setSaved(_item.conv, _item.ts, false);
    }

private:
    SavedPage &_page;
    SavedItem  _item;
};

SavedPage::SavedPage(screens::Context &ctx, Avatars &avatars)
    : OverviewPage(ctx, avatars, tr("Saved messages")) {}

// Follows set / remove / server sync while the page is on screen; open()
// rebuilds anyway when it comes back.
void SavedPage::onChange(const model::Change &ch) {
    if (!visible() || rebuildQueued())
        return;
    bool mine = false;
    if (ch.kind == model::ChangeKind::Reset || ch.kind == model::ChangeKind::Roster)
        mine = true;
    else if (ch.kind == model::ChangeKind::Update || ch.kind == model::ChangeKind::Remove)
        mine = _ctx.store().findSaved(ch.conv, ch.ts) ||
               std::any_of(_items.begin(), _items.end(), [&](const SavedItem &s) {
                   return s.conv == ch.conv && s.ts == ch.ts;
               });
    if (mine)
        rebuildSoon();
}

void SavedPage::open() {
    _tried.clear();
    rebuild();
}

void SavedPage::clear() {
    _list->clearChildren();
    _cards.clear();
    _items.clear();
    _tried.clear();
    setStatus({});
}

void SavedPage::rebuild() {
    _list->clearChildren();
    _cards.clear();
    _items.clear();
    for (const SavedItem &s : _ctx.store().savedList()) {
        if (s.conv >= _ctx.store().conversationCount())
            continue;
        _cards.push_back(_list->add<Card>(*this, s));
        _items.push_back(s);
    }
    setStatus(
        _cards.empty()
            ? std::string(tr("Messages you save for later or set reminders on will appear here."))
            : std::string()
    );
    resolvePreviews();
}

// A card whose message was never seen (saved
// from another client, not in the loaded history) fetches it once; the
// answer lands in the Store, which rebuilds the page.
void SavedPage::resolvePreviews() {
    for (const SavedItem &s : _items) {
        if (s.previewed)
            continue;
        const auto key = std::make_pair(s.conv, s.ts);
        if (std::find(_tried.begin(), _tried.end(), key) != _tried.end())
            continue;
        _tried.push_back(key);
        std::weak_ptr<int> alive = _alive;
        const std::string  ws    = _ctx.store().workspaceId;
        _ctx.backend().loadMessage(s.conv, s.ts, [this, alive, ws, key](bool ok, model::Message m) {
            if (alive.expired() || _ctx.store().workspaceId != ws)
                return;
            _ctx.store().setSavedPreview(key.first, key.second, ok ? &m : nullptr);
        });
    }
}

ui::View *SavedPage::card(size_t i) const {
    return _cards[i];
}

std::string SavedPage::dueText(size_t i) const {
    return i < _items.size() ? dueLabel(_items[i]) : std::string();
}

void SavedPage::remove(size_t i) {
    if (i < _cards.size())
        _cards[i]->remove();
}

void SavedPage::activate(size_t i) {
    if (i < _cards.size())
        _cards[i]->open();
}

} // namespace shell
