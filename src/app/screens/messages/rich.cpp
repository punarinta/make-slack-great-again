#include "app/screens/messages/rich.h"

#include "app/model/jobs.h"
#include "app/mrkdwn/emoji.h"
#include "app/mrkdwn/link_labels.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"
#include "app/screens/common/message_text.h"
#include "gfx/icons_generated.h"
#include "base/i18n.h"
#include "base/process.h"
#include "base/str.h"
#include "net/net.h"
#include "ui/controls.h"

#include <algorithm>
#include <cmath>

namespace screens {

using mrkdwn::Kind;

// A conversation's place label: "#name", a DM peer's name, a group DM's own name or
// "group message"; "" for a conversation we can't see.
std::string placeLabel(const Store &st, std::string_view convId) {
    const ConvRef c = st.findConversation(convId);
    if (c == model::kNoConv)
        return {};
    const model::Conversation &cv = st.conversation(c);
    if (cv.kind == model::ConvKind::Dm)
        return cv.dmUser != model::kNoUser ? std::string(st.user(cv.dmUser).label())
                                           : std::string();
    if (cv.kind == model::ConvKind::Group) {
        // A group DM's own name: never Slack's internal "mpdm-a--b-1".
        if (!cv.localName.empty())
            return cv.localName;
        if (!cv.name.empty() && !str::startsWith(cv.name, "mpdm-"))
            return cv.name;
        return i18n::tr("group message");
    }
    return cv.name.empty() ? std::string() : "#" + cv.name;
}

// A message link's label: "Author in #channel", the author, the place, or
// "message". The author is known only from a rich_text message_mention.
std::string messageLinkLabel(const Store &st, const mrkdwn::MessageRef &ref) {
    const std::string place = placeLabel(st, ref.conv);
    model::UserRef    a =
        ref.author.empty() ? st.linkedAuthor(ref.conv, ref.ts) : st.findUser(ref.author);
    const std::string who =
        a != model::kNoUser && !st.user(a).placeholder ? std::string(st.user(a).label()) : "";
    if (!who.empty() && !place.empty())
        return i18n::arg(i18n::tr("%1 in %2"), who, place);
    if (!who.empty())
        return who;
    if (!place.empty())
        return place;
    return i18n::tr("message");
}

namespace {

// What a message link chip's icon box stands for in a label's image list
// (paintEmojiBoxes draws the icon; no image has this name).
constexpr const char *kMessageLinkIcon = "\x01icon:message";

// Flattens mrkdwn runs into styled text, resolving entities through the Store.
struct Builder {
    Context                       &ctx;
    const mrkdwn::Rich            &r;
    text::Style                    base;
    std::vector<RichLabel::Target> targets;
    std::vector<std::string>       images;
    std::vector<mrkdwn::Run>       runs;
    int                            lastEntity = -1;

    // self: the own-mention background (a mention of me, @here/@channel, my group).
    text::Style pill(text::Style s, bool self = false) const {
        s.color      = ui::themed(ui::C::MentionText);
        s.background = ui::themed(self ? ui::C::MentionSelfBg : ui::C::MentionBg);
        s.weight     = text::Weight::Medium;
        return s;
    }

    uint32_t target(Kind k, const std::string &data) {
        targets.push_back({k, data});
        return uint32_t(targets.size());
    }

    void append(text::AttributedText &t, uint32_t a, uint32_t b) {
        runs.clear();
        mrkdwn::runs(r, a, b, runs);
        const Store &store = ctx.store;
        for (const mrkdwn::Run &run : runs) {
            const std::string_view slice =
                std::string_view(r.text).substr(run.start, run.end - run.start);
            text::Style s = base;
            if (run.style & mrkdwn::StyleBold)
                s.weight = text::Weight::Bold;
            if (run.style & mrkdwn::StyleItalic)
                s.italic = true;
            if (run.style & mrkdwn::StyleUnderline)
                s.underline = true;
            if (run.style & mrkdwn::StyleStrike)
                s.strike = true;
            if (run.style & mrkdwn::StyleCode) {
                s.mono       = true;
                s.size       = std::max(10.f, base.size - 2);
                s.color      = ui::themed(ui::C::CodeText);
                s.background = ui::themed(ui::C::CodeBg);
            }
            if (run.entity < 0) {
                t.append(slice, s);
                continue;
            }
            const mrkdwn::Entity  &e     = r.entities[size_t(run.entity)];
            const std::string_view label = std::string_view(r.text).substr(e.start, e.length);
            // Link labels: a GIPHY media link is a "GIF" badge; a
            // label Slack shortened ("host/…/…") is rebuilt from the URL.
            const bool             giphy = e.kind == Kind::Link && mrkdwn::isGiphyMediaUrl(e.data);
            const bool             shortened =
                e.kind == Kind::Link && mrkdwn::isShortenedUrlLabel(label, e.data);
            // Mentions, emoji and rewritten links are replaced as a whole:
            // emit them once even when a style change split the entity into
            // several runs.
            // A permalink (it has a host; the app's own thread links don't) is
            // a chip.
            const bool chip =
                e.kind == Kind::MessageLink && !mrkdwn::refFromToken(e.data).host.empty();
            const bool atomic =
                (e.kind != Kind::Link && e.kind != Kind::MessageLink) || giphy || shortened || chip;
            if (atomic && run.entity == lastEntity)
                continue;
            lastEntity = run.entity;
            switch (e.kind) {
            case Kind::Link:
            case Kind::MessageLink:
                s.color  = ui::themed(ui::C::Link);
                s.linkId = target(e.kind, e.data);
                if (chip) {
                    // The message icon, then where it is, on the mention tint.
                    s.background   = ui::themed(ui::C::MentionBg);
                    text::Style is = s;
                    images.push_back(kMessageLinkIcon);
                    is.inlineBoxId = uint32_t(images.size());
                    is.boxWidth = is.boxHeight = std::round(s.size * 11.f / 15.f);
                    t.append(" ", is);
                    t.append("\xC2\xA0" + messageLinkLabel(store, mrkdwn::refFromToken(e.data)), s);
                } else if (giphy) {
                    // A pill like a mention: "GIF", then the link's own title.
                    text::Style ps = pill(s);
                    ps.linkId      = s.linkId;
                    ps.weight      = text::Weight::Bold;
                    t.append(i18n::tr("GIF"), ps);
                    ps.weight =
                        s.weight == text::Weight::Bold ? text::Weight::Bold : text::Weight::Medium;
                    if (!mrkdwn::isUrlLabel(label, e.data))
                        t.append(std::string(" \xC2\xB7 ") + std::string(label), ps);
                } else if (shortened) {
                    constexpr size_t kMaxLinkLabelChars = 96;
                    t.append(mrkdwn::expandedLabel(e.data, kMaxLinkLabelChars), s);
                } else {
                    t.append(slice, s);
                }
                break;
            case Kind::User: {
                const model::UserRef u    = store.findUser(e.data);
                const std::string    name = userMentionText(store, u);
                text::Style          ps   = pill(s, u != model::kNoUser && u == store.me);
                ps.linkId                 = target(e.kind, e.data);
                t.append(name.empty() ? std::string(slice) : name, ps);
                break;
            }
            case Kind::Channel: {
                s.color  = ui::themed(ui::C::Link);
                s.linkId = target(e.kind, e.data);
                // One the roster doesn't list: its name is looked up once.
                if (store.findConversation(e.data) == model::kNoConv && !store.channelName(e.data))
                    ctx.backend().resolveChannel(e.data);
                const std::string name = entityText(store, e);
                t.append(name.empty() ? std::string(slice) : name, s);
                break;
            }
            case Kind::Usergroup: {
                const bool mine = std::find(store.myGroups.begin(), store.myGroups.end(), e.data) !=
                                  store.myGroups.end();
                const std::string name = entityText(store, e);
                t.append(name.empty() ? std::string(slice) : name, pill(s, mine));
                break;
            }
            case Kind::Here:
            case Kind::ChannelCmd:
                t.append(slice, pill(s, true));
                break;
            case Kind::Emoji: {
                const Store::EmojiGlyph g = store.emojiFor(e.data);
                if (!g.unicode.empty()) {
                    t.append(emojiText(g.unicode, run.skinTone), s);
                } else if (!g.image.empty()) {
                    images.push_back(g.image);
                    s.inlineBoxId = uint32_t(images.size());
                    s.boxWidth = s.boxHeight = std::round(s.size * 1.35f);
                    t.append(slice, s);
                } else {
                    t.append(slice, s);
                }
                break;
            }
            default:
                t.append(slice, s);
                break;
            }
        }
    }
};

void appendEdited(text::AttributedText &t) {
    text::Style s = ui::font(ui::Font::Small);
    s.color       = ui::themed(ui::C::TextFaint);
    t.append(" ", s);
    t.append(i18n::tr("(edited)"), s);
}

} // namespace

// ── RichLabel ───────────────────────────────────────────────────────────────

RichLabel::RichLabel(Context &ctx, ui::View *waiter) : _ctx(ctx), _waiter(waiter ? waiter : this) {
    onLink = [this](uint32_t id) { activate(id); };
}

RichLabel::~RichLabel() {
    _ctx.app.cancelTimer(_animTimer);
}

void RichLabel::windowChanged() {
    if (!window()) {
        _ctx.app.cancelTimer(_animTimer);
        _animTimer = 0;
    }
}

uint8_t RichLabel::cursorAt(ui::PointF local) const {
    // Code copies on a click: the hand over it.
    if (!linkAt(local) && !codeAt(mapToWindow(local)).empty())
        return uint8_t(plat::Cursor::Hand);
    if (_selectable && !linkAt(local))
        return uint8_t(plat::Cursor::IBeam);
    return ui::Label::cursorAt(local);
}

void RichLabel::wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const {
    const text::Layout *l = textLayout();
    *from                 = l ? l->wordStart(offset) : offset;
    *to                   = l ? l->wordEnd(offset) : offset;
}

// The visual line under the point.
void RichLabel::lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const {
    const text::Layout *l = textLayout();
    if (!l) {
        *from = *to = 0;
        return;
    }
    const float y = local.y - textOrigin().y;
    *from         = l->hitTest({-1e6f, y}).offset;
    *to           = l->hitTest({1e6f, y}).offset;
}

std::string RichLabel::codeAt(ui::PointF wp) const {
    if (_codeBlock)
        return windowRect().contains(wp) ? text() : std::string();
    const text::Layout         *l = textLayout();
    const text::AttributedText *t = richText();
    if (!l || !t)
        return {};
    // Inline code: each run of mono spans (a style change inside code
    // splits it), hit by its highlight boxes.
    const ui::PointF o = textOrigin();
    const ui::PointF p = mapFromWindow(wp);
    const ui::PointF at{p.x - o.x, p.y - o.y};
    for (size_t i = 0; i < t->spans.size();) {
        if (!t->spans[i].style.mono) {
            ++i;
            continue;
        }
        const uint32_t from = t->spans[i].start;
        while (i + 1 < t->spans.size() && t->spans[i + 1].style.mono)
            ++i;
        const uint32_t to = t->spans[i++].end;
        for (const gfx::RectF &r : l->selectionRects(from, to))
            if (r.contains(at))
                return t->text.substr(from, to - from);
    }
    return {};
}

void RichLabel::setContent(
    text::AttributedText t, std::vector<Target> targets, std::vector<std::string> images
) {
    _targets = std::move(targets);
    _images  = std::move(images);
    _hover.clear();
    setUnderlinedLink(0);
    setRichText(std::move(t));
}

ui::RectF RichLabel::tooltipAnchor() const {
    return _hover.anchor();
}

void RichLabel::activate(uint32_t id) {
    if (id && id <= _targets.size())
        activateTarget(_ctx, _targets[id - 1], window(), _hover.at(), windowRect());
}

const RichLabel::Target *RichLabel::targetAt(ui::PointF local) const {
    const uint32_t id = linkAt(local);
    return id && id <= _targets.size() ? &_targets[id - 1] : nullptr;
}

bool RichLabel::onEvent(ui::Event &e) {
    switch (e.type) {
    case ui::EventType::PointerMove:
    case ui::EventType::PointerLeave: {
        // One hit test per move; the user looked up when the link changes.
        const uint32_t id    = e.type == ui::EventType::PointerMove ? linkAt(e.pos) : 0;
        const auto     shown = [&] {
            std::string s;
            if (const text::AttributedText *t = richText())
                for (const text::Span &sp : t->spans)
                    if (sp.style.linkId == id)
                        s.append(t->text, sp.start, sp.end - sp.start);
            return s;
        };
        if (_hover.move(_ctx, _targets, id, e.windowPos, windowRect(), shown)) {
            setUnderlinedLink(_hover.underlined());
            if (window())
                window()->rearmTooltip();
        }
        return ui::Label::onEvent(e);
    }
    // A click on code copies it; on message text the list's selection
    // handles the press instead (MessageList), so a drag still selects.
    case ui::EventType::PointerDown:
        if (!_selectable && e.button == plat::Button::Left && !linkAt(e.pos)) {
            _pressedCode = codeAt(e.windowPos);
            if (!_pressedCode.empty())
                return true;
        }
        return ui::Label::onEvent(e);
    case ui::EventType::PointerUp:
        if (!_pressedCode.empty()) {
            std::string code = std::move(_pressedCode);
            _pressedCode.clear();
            if (codeAt(e.windowPos) == code)
                copyCode(_ctx, code, window(), e.windowPos);
            return true;
        }
        return ui::Label::onEvent(e);
    case ui::EventType::PointerCancel:
        _pressedCode.clear();
        return ui::Label::onEvent(e);
    case ui::EventType::ContextMenu: {
        // The link menu, for links only (mentions, channels: nothing), and
        // only from a right click (raw set) — no Menu key or long press.
        const Target *tg = e.raw ? targetAt(e.pos) : nullptr;
        return tg && window() && showTargetMenu(_ctx, *tg, *window(), e.windowPos);
    }
    default:
        return ui::Label::onEvent(e);
    }
}

// ── Link targets ────────────────────────────────────────────────────────────

namespace {

// A message link's permalink ("" for the app's own links, or a
// conversation this workspace can't see).
std::string messageLinkUrl(Context &ctx, const std::string &token) {
    const mrkdwn::MessageRef ref = mrkdwn::refFromToken(token);
    const model::ConvRef     c   = ctx.store().findConversation(ref.conv);
    if (c == model::kNoConv || ref.host.empty())
        return {};
    return ctx.store().permalink(c, model::parseTs(ref.ts), model::parseTs(ref.threadTs));
}

} // namespace

bool TargetHover::move(
    Context                            &ctx,
    const std::vector<LinkTarget>      &targets,
    uint32_t                            id,
    ui::PointF                          at,
    ui::RectF                           card,
    const std::function<std::string()> &shown
) {
    _at                       = at;
    const bool           same = id && id == _link;
    const LinkTarget    *tg   = id && id <= targets.size() ? &targets[id - 1] : nullptr;
    // Hovering a mention or a name: the profile card after a delay.
    const model::UserRef u    = same                           ? _user
                                : tg && tg->kind == Kind::User ? ctx.store().findUser(tg->data)
                                                               : model::kNoUser;
    if (u != _user && ctx.profileHover) {
        if (_user != model::kNoUser)
            ctx.profileHover(_user, _card, 0);
        if (u != model::kNoUser)
            ctx.profileHover(u, card, 1);
    }
    if (u != _user)
        _card = card;
    _user = u;
    if (id == _link)
        return false;
    _link       = id;
    // Every piece of the hovered URL link
    // underlines together (mentions, channels, message links never).
    _underlined = tg && tg->kind == Kind::Link ? id : 0;
    // The URL tooltip: links and message links (their permalink), unless the
    // text already is the URL.
    std::string url;
    if (tg && tg->kind == Kind::Link)
        url = net::percentDecode(tg->data);
    else if (tg && tg->kind == Kind::MessageLink)
        url = messageLinkUrl(ctx, tg->data);
    _tip = url.empty() || url == shown() ? std::string() : url;
    return true;
}

void activateTarget(
    Context &ctx, const LinkTarget &tg, ui::Window *w, ui::PointF at, ui::RectF card
) {
    switch (tg.kind) {
    case Kind::Link:
        openLink(ctx, tg.data, w, at);
        break;
    case Kind::MessageLink: {
        const mrkdwn::MessageRef ref    = mrkdwn::refFromToken(tg.data);
        const model::ConvRef     c      = ctx.store().findConversation(ref.conv);
        const model::Ts          thread = model::parseTs(ref.threadTs);
        if (!ref.host.empty()) {
            // Only this workspace's conversations can be jumped to; a
            // link into another team (or one we can't see) is still a link.
            if (c == model::kNoConv) {
                if (ctx.openUrl)
                    ctx.openUrl(mrkdwn::messagePermalink(ref));
            } else if (ctx.openMessage) {
                ctx.openMessage(c, model::parseTs(ref.ts), thread);
            }
            break;
        }
        if (c == model::kNoConv)
            break;
        if (thread && ctx.openThread)
            ctx.openThread(c, thread);
        else if (ctx.openConversation)
            ctx.openConversation(c);
        break;
    }
    case Kind::User: { // a click opens the profile card at once
        const model::UserRef u = ctx.store().findUser(tg.data);
        if (u != model::kNoUser && ctx.profileHover)
            ctx.profileHover(u, card, 2);
        else if (u != model::kNoUser && ctx.openProfile)
            ctx.openProfile(u);
        break;
    }
    case Kind::Channel: {
        const model::ConvRef c = ctx.store().findConversation(tg.data);
        if (c != model::kNoConv && ctx.openConversation)
            ctx.openConversation(c);
        break;
    }
    default:
        break;
    }
}

bool showTargetMenu(Context &ctx, const LinkTarget &tg, ui::Window &w, ui::PointF windowPos) {
    const std::string url = tg.kind == Kind::Link          ? tg.data
                            : tg.kind == Kind::MessageLink ? messageLinkUrl(ctx, tg.data)
                                                           : std::string();
    if (url.empty())
        return false;
    showLinkMenu(ctx, w, windowPos, url);
    return true;
}

void RichLabel::paint(gfx::Painter &p) {
    ui::Label::paint(p);
    const text::Layout *l = textLayout();
    if (_images.empty() || !l)
        return;
    const double next = paintEmojiBoxes(_ctx, p, *l, textOrigin(), _images, _waiter);
    if (next >= 0 && !_animTimer && window())
        _animTimer = _ctx.app.addTimer(int(std::ceil(next)), false, [this] {
            _animTimer = 0;
            update();
        });
}

double paintEmojiBoxes(
    Context                        &ctx,
    gfx::Painter                   &p,
    const text::Layout             &l,
    ui::PointF                      o,
    const std::vector<std::string> &images,
    ui::View                       *waiter
) {
    const float  scale = waiter ? waiter->windowScale() : 1.f;
    // Animated custom emoji (Settings → Animate emoji) all run on one clock,
    // so every copy of one shows the same frame; the caller repaints when
    // the soonest of them changes.
    const bool   anim  = ctx.images.animateEmoji() && !ui::app()->reducedMotion();
    const double now   = ctx.app.nowMs();
    double       next  = -1;
    for (const text::InlineBox &b : l.boxes()) {
        if (b.id == 0 || b.id > images.size())
            continue;
        if (images[b.id - 1] == kMessageLinkIcon) { // a message link chip's icon
            gfx::drawIcon(
                p,
                gfx::Icon::MessageSquare,
                {b.rect.x + o.x, b.rect.y + o.y, b.rect.w, b.rect.h},
                ui::color(ui::C::Link)
            );
            continue;
        }
        const ImageCache::Ref r{
            images[b.id - 1], int(std::lround(b.rect.w * scale)), int(std::lround(b.rect.h * scale))
        };
        const ui::RectF    dst{b.rect.x + o.x, b.rect.y + o.y, b.rect.w, b.rect.h};
        ImageCache::Bitmap still;
        const gfx::Bitmap *bmp = nullptr;
        ImageCache::Frames frames;
        if (anim && (frames = ctx.images.frames(r, waiter)) && frames->size() > 1) {
            double total = 0;
            for (const gfx::AnimFrame &f : *frames)
                total += std::max(20, f.delayMs);
            double t = std::fmod(now, total);
            for (const gfx::AnimFrame &f : *frames) {
                const double d = std::max(20, f.delayMs);
                if (t < d) {
                    bmp               = &f.frame;
                    const double left = d - t;
                    next              = next < 0 ? left : std::min(next, left);
                    break;
                }
                t -= d;
            }
        } else if (frames) {
            bmp = &(*frames)[0].frame;
        } else if (!anim) {
            still = ctx.images.get(r, waiter);
            bmp   = still.get();
        }
        if (bmp)
            p.drawBitmap(bmp->view(), dst, gfx::Sampling::Nearest);
    }
    return next;
}

EmojiFrameTimer::~EmojiFrameTimer() {
    _ctx.app.cancelTimer(_id);
}

void EmojiFrameTimer::schedule(double ms) {
    if (ms < 0 || _id || !_view.window())
        return;
    _id = _ctx.app.addTimer(int(std::ceil(ms)), false, [this] {
        _id = 0;
        _view.update();
    });
}

// ── Bodies ──────────────────────────────────────────────────────────────────

namespace {

// RichOptions::cutChars/cutLines over the parsed text `s`: the byte offset
// where the preview ends, or UINT32_MAX when all of it fits.
uint32_t previewCut(std::string_view s, int *maxChars, int *maxLines) {
    int lines = 1;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] & 0xc0) == 0x80)
            continue; // a continuation byte: the same character
        if (*maxChars <= 0 || (s[i] == '\n' && ++lines > *maxLines)) {
            *maxChars = *maxLines = 0;
            return uint32_t(i);
        }
        --*maxChars;
    }
    *maxLines -= lines - 1;
    return UINT32_MAX;
}

// buildBody and bodyTexts in one: with a column, the labels are made; with
// none, only their texts are collected (the same ones, in the same order).
uint32_t walkBody(
    Context                  &ctx,
    ui::View                 *column,
    std::string_view          text,
    const RichOptions        &o,
    ui::View                 *waiter,
    std::vector<std::string> *texts
) {
    const mrkdwn::Rich               r      = mrkdwn::parse(text);
    const std::vector<mrkdwn::Block> blocks = mrkdwn::blocks(r);
    const uint32_t cut = o.cutChars ? previewCut(r.text, o.cutChars, o.cutLines) : UINT32_MAX;
    Builder        b{ctx, r, ui::font(o.font), {}, {}, {}};
    b.base.color = ui::themed(o.color);
    b.base.size *= o.scale;
    auto label = [&](ui::View *parent) -> RichLabel * {
        if (!parent)
            return nullptr;
        auto *l = parent->add<RichLabel>(ctx, waiter);
        if (o.maxLines > 0)
            l->setMaxLines(o.maxLines);
        if (o.labels) {
            l->setSelectable(true);
            o.labels->push_back(l);
        }
        return l;
    };
    auto finish = [&](RichLabel *l, text::AttributedText &t, bool last) {
        if (last && o.edited)
            appendEdited(t);
        if (texts)
            texts->push_back(t.text);
        if (l)
            l->setContent(std::move(t), std::move(b.targets), std::move(b.images));
        b.targets = {};
        b.images  = {};
    };
    auto edited = [&] {
        text::AttributedText e;
        appendEdited(e);
        if (texts)
            texts->push_back(e.text);
        if (RichLabel *l = label(column))
            l->setContent(std::move(e), {}, {});
    };
    ui::View *quote = nullptr; // content column of the current quoted run
    for (size_t i = 0; i < blocks.size(); ++i) {
        mrkdwn::Block bl = blocks[i];
        if (bl.start >= cut)
            break;
        const bool cutHere = bl.end > cut;
        if (cutHere)
            bl.end = cut;
        const bool last   = i + 1 == blocks.size() || cutHere;
        ui::View  *parent = column;
        if (bl.quoted && column) {
            if (!quote) {
                auto *q = column->add<ui::View>();
                q->style().row().spacing(10);
                auto *bar = q->add<ui::View>();
                bar->style().width(4).noShrink();
                bar->setBackground(ui::C::BorderStrong, 2);
                quote = q->add<ui::View>();
                quote->style().flex(1).spacing(2).padding(0, 1);
            }
            parent = quote;
        } else {
            quote = nullptr;
        }
        text::AttributedText t;
        switch (bl.kind) {
        case mrkdwn::BlockKind::Paragraph: {
            RichLabel *l = label(parent);
            b.append(t, bl.start, bl.end);
            if (cutHere)
                t.append("\xE2\x80\xA6", b.base);
            finish(l, t, last);
            break;
        }
        case mrkdwn::BlockKind::ListItem: {
            RichLabel *l = nullptr;
            if (parent) {
                auto *row = parent->add<ui::View>();
                row->style().row().margins(float(bl.indent) * 22, 0, 0, 0);
                std::string marker = bl.ordinal ? str::number(bl.ordinal) + "." : "•";
                auto       *m      = row->add<ui::Label>(std::move(marker), o.font, o.color);
                // At the top: stretched to a wrapped item's height, the label
                // would centre the bullet halfway down it.
                m->style().width(22).noShrink().alignSelf(ui::Align::Start);
                l = label(row);
                l->style().flex(1);
            }
            b.append(t, std::min(bl.markerEnd, bl.end), bl.end);
            if (cutHere)
                t.append("\xE2\x80\xA6", b.base);
            finish(l, t, last);
            break;
        }
        case mrkdwn::BlockKind::Code: {
            ui::View *box = nullptr;
            if (parent) {
                box = parent->add<ui::View>();
                box->setBackground(ui::C::CodeBg, 4);
                box->setBorder(ui::C::Border);
                box->style().margins(0, 2, 0, 2);
            }
            text::Style mono = ui::font(ui::Font::Mono);
            mono.color       = ui::themed(o.color);
            t.append(std::string_view(r.text).substr(bl.start, bl.end - bl.start), mono);
            RichLabel *l = label(box);
            if (l) {
                // The padding is the label's: a click anywhere on the box copies.
                l->style().padding(10, 8);
                l->setCodeBlock(true);
            }
            finish(l, t, false);
            if (last && o.edited)
                edited();
            break;
        }
        }
        if (cutHere)
            break;
    }
    if (blocks.empty() && o.edited)
        edited();
    return cut;
}

} // namespace

uint32_t buildBody(
    Context &ctx, ui::View *column, std::string_view text, const RichOptions &o, ui::View *waiter
) {
    return walkBody(ctx, column, text, o, waiter, nullptr);
}

text::AttributedText richText(
    Context                  &ctx,
    std::string_view          text,
    const RichOptions        &o,
    std::vector<std::string> *images,
    std::vector<LinkTarget>  *targets
) {
    const mrkdwn::Rich r = mrkdwn::parse(text);
    Builder            b{ctx, r, ui::font(o.font), {}, {}, {}};
    b.base.color = ui::themed(o.color);
    b.base.size *= o.scale;
    if (images)
        b.images = std::move(*images); // box ids continue after the ones given
    if (targets)
        b.targets = std::move(*targets); // link ids likewise
    text::AttributedText t;
    b.append(t, 0, uint32_t(r.text.size()));
    if (targets)
        *targets = std::move(b.targets);
    else
        for (text::Span &sp : t.spans)
            sp.style.linkId = 0; // no targets kept
    if (images)
        *images = std::move(b.images);
    return t;
}

std::vector<std::string> bodyTexts(Context &ctx, std::string_view text, const RichOptions &o) {
    std::vector<std::string> out;
    RichOptions              x = o;
    x.labels                   = nullptr;
    walkBody(ctx, nullptr, text, x, nullptr, &out);
    return out;
}

void flashCopied(Context &ctx, ui::Button *copy, std::weak_ptr<char> alive) {
    copy->setLabel(i18n::tr("Copied"));
    ctx.app.addTimer(1400, false, [copy, alive = std::move(alive)] {
        if (!alive.expired())
            copy->setLabel(i18n::tr("Copy"));
    });
}

void copyCode(Context &ctx, const std::string &code, ui::Window *w, ui::PointF at) {
    ctx.app.platform().setClipboardText(code);
    if (w)
        showClickToast(ctx, *w, i18n::tr("Copied"), 1200, at);
}

// ── Links ───────────────────────────────────────────────────────────────────

namespace {

#if defined(__linux__)
// Whether a mail app handles mailto:. On Linux the opener reports
// nothing back, so ask xdg-mime whether anything takes mailto: (once; the
// answer is remembered). 1 = yes, 0 = no, -1 = not asked yet.
int g_mailHandler = -1;
#endif

void copyAddress(Context &ctx, const std::string &url, ui::Window *w, ui::PointF at) {
    // "mailto:a@b?subject=hi" → "a@b".
    std::string addr = url.substr(7);
    addr             = addr.substr(0, addr.find('?'));
    ctx.app.platform().setClipboardText(addr);
    if (w)
        showClickToast(ctx, *w, i18n::tr("No email app \xE2\x80\x94 address copied"), 1800, at);
}

} // namespace

void openLink(Context &ctx, const std::string &url, ui::Window *w, ui::PointF at) {
    if (url.size() <= 7 || !str::iequals(std::string_view(url).substr(0, 7), "mailto:")) {
        if (ctx.openUrl)
            ctx.openUrl(url);
        return;
    }
#if defined(__linux__)
    if (g_mailHandler < 0) {
        // The query runs a process: on a worker, then the click goes on.
        auto found = std::make_shared<bool>(true);
        model::runInBackground(
            ctx.app.platform(),
            [found] {
                const std::string exe = base::findExecutable("xdg-mime");
                if (exe.empty())
                    return; // can't tell: assume a handler and just try
                base::RunOptions o;
                o.mergeStderr = false;
                o.timeoutMs   = 1500;
                const base::RunResult r =
                    base::run(exe, {"query", "default", "x-scheme-handler/mailto"}, o);
                if (r.started && !r.timedOut) {
                    bool any = false;
                    for (char c : r.output)
                        any = any || (c != ' ' && c != '\n' && c != '\t' && c != '\r');
                    *found = any;
                }
            },
            [&ctx, url, w, at, found] {
                g_mailHandler = *found ? 1 : 0;
                openLink(ctx, url, w, at);
            }
        );
        return;
    }
    if (g_mailHandler == 0) {
        copyAddress(ctx, url, w, at);
        return;
    }
#endif
    if (!ctx.app.platform().openUrl(url))
        copyAddress(ctx, url, w, at);
}

} // namespace screens
