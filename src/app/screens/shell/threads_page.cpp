#include "screens/shell/threads_page.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "app/screens/common/message_rules.h"
#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/rich.h"
#endif

#include <algorithm>

using namespace ui;
using i18n::arg;
using i18n::tr;
using model::ConvRef;
using model::Ts;
using FollowedThread = model::Backend::FollowedThread;

namespace shell {

namespace {

// One message inside a card: avatar, name and time
// over the rich body, a one-line summary of attached files. A click outside
// a link opens the real thread.
class MessageRow final : public Clickable {
public:
    MessageRow(screens::Context &ctx, Avatars &avatars, const model::Message &m) {
        setLook({C::None, C::None, C::None, C::None, 0});
        setCursor(plat::Cursor::Hand); // the whole row opens the thread
        const model::Store &st  = ctx.store();
        View               *col = fillMessageRow(
            this,
            avatars,
            std::string(screens::authorName(st, m)),
            screens::authorAvatar(st, m),
            base::dateTimeLabel(model::tsSecs(m.ts), ctx.backend.nowSecs())
        );
        auto *body = col->add<View>();
        body->style().spacing(2);
#ifdef MSGA_HAVE_MESSAGES
        screens::RichOptions o;
        o.edited = m.edited;
        screens::buildBody(ctx, body, m.text, o, this);
#else
        body->add<Label>(m.text, Font::Body);
#endif
        const auto &files = m.files();
        if (!files.empty()) {
            auto *line = col->add<View>();
            line->style().row().spacing(4).items(Align::Center);
            line->add<IconView>(gfx::Icon::Paperclip, 12, C::TextMuted);
            line->add<Label>(
                files.size() == 1 ? files.front().name
                                  : arg(tr("%1 files"), str::number(int64_t(files.size()))),
                Font::Body,
                C::TextMuted
            );
        }
    }
};

} // namespace

std::string threadParticipants(const model::Store &store, const model::Message &root) {
    std::vector<model::UserRef> ids{root.user};
    for (model::UserRef u : root.replyUsers)
        if (std::find(ids.begin(), ids.end(), u) == ids.end())
            ids.push_back(u);
    bool                     includesMe = false;
    std::vector<std::string> names;
    for (model::UserRef u : ids) {
        if (store.me != model::kNoUser && u == store.me) {
            includesMe = true;
            continue;
        }
        if (u == model::kNoUser)
            continue;
        if (const std::string_view name = store.user(u).label(); !name.empty())
            names.emplace_back(name);
    }
    // A bot's root has no author: its name.
    if (names.empty() && !includesMe && root.extra && !root.extra->botName.empty())
        names.push_back(root.extra->botName);
    const auto join = [&](size_t n) {
        std::string s;
        for (size_t i = 0; i < n; ++i)
            s += (i ? ", " : "") + names[i];
        return s;
    };
    constexpr size_t kMaxNames = 3;
    if (names.size() > kMaxNames)
        return arg(
            tr("%1 and %2 others"), join(kMaxNames), str::number(int64_t(names.size() - kMaxNames))
        );
    if (includesMe)
        return names.empty() ? std::string(tr("you")) : arg(tr("%1 and you"), join(names.size()));
    return join(names.size());
}

// ── Card ────────────────────────────────────────────────────────────────────

ThreadsPage::Card::Card(ThreadsPage &page, FollowedThread item)
    : _page(page), _item(std::move(item)) {
    screens::Context &ctx = page._ctx;
    // Like the official client: the channel header (name over the
    // participants) sits on the page's grey, outside the white card; only
    // the messages and the reply box live in the bordered body.
    style().spacing(12);
    auto *head = add<View>();
    head->style().spacing(2);
    View *nameRow = cardHeader(head, ctx.store(), _item.conv, [this] {
        if (_page.onOpenChannel)
            _page.onOpenChannel(_item.conv);
    });
    auto *pill    = nameRow->add<Label>(tr("New"), Font::PlateName, C::AccentText);
    pill->setBackground(C::Badge, 8);
    pill->style().padding(8, 1).noShrink();
    pill->setVisible(unread());
    _newPill = pill;
    head->add<Label>(participants(), Font::Small, C::TextMuted)->setMaxLines(1);

    _body                                                           = cardBody(this);
    _body->add<MessageRow>(ctx, page._avatars, _item.root)->onClick = [this] { openThread(); };
    // "Show N more replies": the rest lives in the thread panel.
    const int hidden = int(_item.root.replyCount) - int(_item.latestReplies.size());
    if (hidden > 0) {
        auto *more = _body->add<TextLink>(
            hidden == 1 ? std::string(tr("Show 1 more reply"))
                        : arg(tr("Show %1 more replies"), str::number(int64_t(hidden))),
            Font::Control,
            C::Link,
            true
        );
        more->onClick = [this] { openThread(); };
    }
    _replies = _body->add<View>();
    _replies->style().spacing(4);
    rebuildReplies();
    _latest = _item.root.latestReply;
    for (const model::Message &m : _item.latestReplies)
        _latest = std::max(_latest, m.ts);
    _replyBtn =
        _body->add<Button>(tr("Reply in thread…"), Button::Kind::Secondary, Button::Form::Small);
    _replyBtn->setFocusable(false);
    _replyBtn->style().alignSelf(Align::Start).margins(0, 4, 0, 0);
    _replyBtn->onClick = [this] { showComposer(); };
}

bool ThreadsPage::Card::unread() const {
    return _item.root.latestReply && _item.root.latestReply > _item.lastRead;
}

std::string ThreadsPage::Card::participants() const {
    return threadParticipants(_page._ctx.store, _item.root);
}

void ThreadsPage::Card::rebuildReplies() {
    _replies->clearChildren();
    for (const model::Message &m : _item.latestReplies)
        _replies->add<MessageRow>(_page._ctx, _page._avatars, m)->onClick = [this] {
            openThread();
        };
}

void ThreadsPage::Card::markRead() {
    const Ts upTo = _latest ? _latest : _item.root.ts;
    _page._ctx.backend.markThreadRead(_item.conv, _item.root.ts, upTo);
    _item.lastRead = upTo;
    _newPill->setVisible(false);
}

void ThreadsPage::Card::openThread() {
    markRead();
    if (_page.onOpenThread)
        _page.onOpenThread(_item.conv, _item.root.ts);
}

// A real thread composer, made on first use (one per card up front would be
// needlessly heavy); it replaces the button.
void ThreadsPage::Card::showComposer() {
    if (_composer) {
        _composer->edit().focus();
        return;
    }
    _composer = static_cast<Composer *>(
        _body->adopt(std::make_unique<Composer>(_page._ctx, _page._drafts))
    );
    _composer->setThreadMode(true);
    _composer->setScheduleVisible(false);
    // In line with the avatars: the card's padding is the gutter the chat
    // footer's margins otherwise give (flush horizontal margins).
    _composer->style().padding(0, 8, 0, 0);
    if (_page._setupComposer)
        _page._setupComposer(*_composer);
    _composer->setTarget(_item.conv, _item.root.ts);
    _composer->onSent = [this] { markRead(); };
    _replyBtn->setVisible(false);
    _composer->edit().focus();
}

void ThreadsPage::Card::onChange(const model::Change &ch) {
    using K                    = model::ChangeKind;
    const model::Store &store  = _page._ctx.store;
    auto               &shown  = _item.latestReplies;
    const auto          findTs = [&](Ts ts) {
        return std::find_if(shown.begin(), shown.end(), [&](const model::Message &m) {
            return m.ts == ts;
        });
    };
    bool changed = false;
    if (ch.kind == K::Append || ch.kind == K::Insert) {
        // Live replies: a realtime one, or our own pending send.
        const std::vector<model::Message> *list = store.replies(_item.conv, _item.root.ts);
        if (!list)
            return;
        const size_t n = ch.kind == K::Append ? std::min<size_t>(ch.count, list->size()) : 0;
        std::vector<const model::Message *> added;
        for (size_t i = list->size() - n; i < list->size(); ++i)
            added.push_back(&(*list)[i]);
        if (ch.kind == K::Insert)
            if (const model::Message *m = store.findMessage(_item.conv, ch.ts))
                added.push_back(m);
        for (const model::Message *m : added) {
            if (findTs(m->ts) != shown.end())
                continue;
            shown.insert(
                std::upper_bound(
                    shown.begin(),
                    shown.end(),
                    m->ts,
                    [](Ts ts, const model::Message &x) { return ts < x.ts; }
                ),
                m->clone()
            );
            ++_item.root.replyCount;
            _latest = std::max(_latest, m->ts);
            changed = true;
        }
    } else if (ch.kind == K::Update) {
        const auto it = findTs(ch.ts);
        if (it == shown.end())
            return;
        if (const model::Message *m = store.findMessage(_item.conv, ch.ts)) {
            *it     = m->clone();
            changed = true;
        }
    } else if (ch.kind == K::Remove) {
        const auto it = findTs(ch.ts);
        if (it == shown.end())
            return;
        shown.erase(it);
        _item.root.replyCount = _item.root.replyCount ? _item.root.replyCount - 1 : 0;
        changed               = true;
    }
    if (changed)
        rebuildReplies();
}

// ── Page ────────────────────────────────────────────────────────────────────

ThreadsPage::ThreadsPage(
    screens::Context               &ctx,
    Avatars                        &avatars,
    DraftStash                     &drafts,
    std::function<void(Composer &)> setup
)
    : OverviewPage(ctx, avatars, tr("Threads")), _drafts(drafts), _setupComposer(std::move(setup)) {
    _more = _scroll->content()->add<Button>(
        tr("Show more threads"), Button::Kind::Secondary, Button::Form::Small
    );
    _more->setFocusable(false);
    _more->style().alignSelf(Align::Start);
    _more->setVisible(false);
    _more->onClick = [this] { showMore(); };
}

void ThreadsPage::onChange(const model::Change &ch) {
    if (!ch.thread || ch.conv == model::kNoConv)
        return;
    for (Card *c : _cards)
        if (c->conv() == ch.conv && c->root() == ch.thread) {
            c->onChange(ch);
            break;
        }
}

void ThreadsPage::open() {
    loadPage({});
}

void ThreadsPage::showMore() {
    if (!_nextCursor.empty())
        loadPage(_nextCursor);
}

void ThreadsPage::clear() {
    ++_generation; // an answer in flight is dropped
    _loading = false;
    _nextCursor.clear();
    _list->clearChildren();
    _cards.clear();
    _more->setVisible(false);
    setStatus({});
}

void ThreadsPage::loadPage(std::string cursor) {
    if (_loading)
        return;
    _loading = true;
    _more->setEnabled(false);
    const bool first = cursor.empty();
    if (first)
        setStatus(tr("Loading threads…"));
    const uint32_t     gen   = ++_generation;
    std::weak_ptr<int> alive = _alive;
    _ctx.backend.loadThreadsView(
        std::move(cursor), [this, alive, gen, first](bool ok, model::Backend::ThreadsView page) {
            if (alive.expired() || gen != _generation)
                return;
            _loading = false;
            _more->setEnabled(true);
            if (!ok) {
                // Slack's code with it: the one clue a report can carry.
                if (_cards.empty())
                    setStatus(
                        page.error.empty()
                            ? std::string(tr("Couldn't load threads. Try again later."))
                            : str::concat(
                                  {tr("Couldn't load threads. Try again later."),
                                   " (",
                                   page.error,
                                   ")"}
                              )
                    );
                return;
            }
            if (first) {
                _list->clearChildren();
                _cards.clear();
            }
            for (FollowedThread &t : page.threads) {
                if (t.conv >= _ctx.store().conversationCount())
                    continue;
                _cards.push_back(_list->add<Card>(*this, std::move(t)));
            }
            _nextCursor = page.hasMore ? page.nextCursor : std::string();
            _more->setVisible(page.hasMore);
            setStatus(
                _cards.empty() ? std::string(tr("Threads you're following will appear here."))
                               : std::string()
            );
        }
    );
}

} // namespace shell
