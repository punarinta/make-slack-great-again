// The sidebar's "Activity" page (like the official client's Activity view):
// what notified me, newest first — mentions, thread replies, reactions to
// my messages, direct messages, posts in channels on "All new posts". Under
// the title the tabs All | DMs | Mentions | Threads, then the "Unread
// only" checkbox, then one bordered list: per item the avatar, the name
// and time, the message on two lines and where it happened ("Mention in
// #design", "Thread in #design", "Reacted 👍 to your message in #design").
// An unread item is bold with a dot (or its count of new messages); a
// hovered one offers "Mark as read" / "Mark as unread". A click marks it
// read and opens it: a thread in the thread panel, else the message in its
// conversation. Data comes from Backend::loadActivity, so the page exists
// only where Capabilities::activityView does; the feed has no push, so
// every open() and every tab reloads it.
#pragma once

#include "screens/shell/overview_page.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

class TabStrip;

class ActivityPage : public OverviewPage {
public:
    class Row;
    using Filter = model::Backend::ActivityFilter;
    using Item   = model::Backend::ActivityItem;

    ActivityPage(screens::Context &ctx, Avatars &avatars);

    // (Re)loads the first page of the current tab. Every time the page is
    // brought to front.
    void open() override;
    // Drops the items and any load in flight (workspace switch).
    void clear() override;

    // A thread item: the thread (channel + thread panel). Any other: the
    // message, inside its thread for a reply.
    std::function<void(model::ConvRef, model::Ts root)>                 onOpenThread;
    std::function<void(model::ConvRef, model::Ts ts, model::Ts thread)> onOpenMessage;

    // The tabs and the checkbox (a click does the same).
    void      setFilter(Filter f);
    Filter    filter() const { return _filter; }
    void      setUnreadOnly(bool on);
    bool      unreadOnly() const { return _unreadOnly; }
    TabStrip &tabs() const { return *_tabs; }

    // Tests: what the page shows.
    size_t      itemCount() const { return _items.size(); }
    const Item &item(size_t i) const { return _items[i]; }
    Row        &row(size_t i) const { return *_rows[i]; }
    void        activate(size_t i);   // a click on the item
    void        toggleRead(size_t i); // its "Mark as read" / "Mark as unread"
    void        showMore();           // "Show more"

private:
    void reload();
    void loadPage(std::string cursor);
    void rebuild() override;
    void onChange(const model::Change &ch) override;

    TabStrip          *_tabs   = nullptr;
    ui::CheckBox      *_unread = nullptr; // the "Unread only" filter
    ui::View          *_card   = nullptr; // the bordered list
    ui::Button        *_more   = nullptr;
    std::vector<Item>  _items;
    std::vector<Row *> _rows;
    Filter             _filter     = Filter::All;
    bool               _unreadOnly = false;
    std::string        _nextCursor;
    bool               _loading    = false;
    uint32_t           _generation = 0; // the load an answer belongs to
};

// One item of the list.
class ActivityPage::Row : public ui::Clickable {
public:
    Row(ActivityPage &page, size_t index);

    bool        unread() const;
    std::string context() const; // "Mention in #design" (tests)
    ui::View   *readButton() const { return _readBtn; }

    bool onEvent(ui::Event &e) override;
    void refreshLook(); // the unread mark, the read toggle

private:
    ActivityPage &_page;
    size_t        _index;
    ui::Label    *_context = nullptr;
    ui::Badge    *_badge   = nullptr;
    ui::Button   *_readBtn = nullptr;
};

} // namespace shell
