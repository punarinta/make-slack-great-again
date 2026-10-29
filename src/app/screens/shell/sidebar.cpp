#include "screens/shell/sidebar.h"

#include "screens/common/tag_badge.h"

#include "base/crypto.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/header.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/sidebar_footer.h"
#include "screens/shell/teammate_page.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvKind;
using model::ConvRef;
using model::kNoConv;
using model::tsSecs;

namespace shell {

namespace {
// The list's metrics: one row height for everything (scaled with
// the text size), a 6 px inset above the first row, pills inset 8 px from the
// list's edges; icons at kPadH, labels and conversation content at 32.
constexpr float kRowHBase = 30, kTopPad = 6, kPill = 8, kPadH = 12, kContentX = 32;
constexpr int   kAvatarPx = 40; // 20 logical, sharp at 2x

float rowH() {
    return std::round(kRowHBase * (app() ? app()->userTextScale() : 1.f));
}

// A row's label and icon: the selected pill's ink, bright, or dim.
C ink(bool selected, bool bright) {
    return selected ? C::SidebarSelectedText : bright ? C::SidebarText : C::SidebarTextMuted;
}

// A row on the list's pill: inset kPill from the edges, the content at
// kContentX; selected, the selected pill.
void pillRow(Clickable *r) {
    r->setLook({C::None, C::SidebarHover, C::SidebarHover, C::SidebarSelected, 6});
    r->setRole(Role::ListItem);
    r->style().row().height(rowH()).margins(kPill, 0, kPill, 0).items(Align::Center);
    r->style().padding(kContentX - kPill, 0, 6, 0);
}

// The 20 px rounded avatar rows show, its placeholder the away grey.
Avatar *smallAvatar(View *parent) {
    auto *a = parent->add<Avatar>();
    a->style().size(20, 20);
    a->setRadius(5);
    a->setPlaceholder(C::PresenceAway);
    return a;
}

// Rows that change colour on hover (repainted bright).
class HoverRow : public Clickable {
public:
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave) {
            refreshLook();
            return false;
        }
        return Clickable::onEvent(e);
    }
    virtual void refreshLook() { update(); }
};

// "Threads" / "Saved messages": a pill like a conversation row, the icon
// optically one notch smaller centred in the 14 px slot, the section face.
class NavRow final : public HoverRow {
public:
    NavRow(Icon i, const char *text) : icon(i) {
        pillRow(this);
        label = add<Label>(text, Font::Section, C::SidebarTextMuted);
        label->setMaxLines(1);
    }
    void refreshLook() override {
        label->setColor(ink(checked(), hovered() || unread));
        update();
    }
    void paint(gfx::Painter &p) override {
        HoverRow::paint(p);
        const float s = 13, y = snapPx((height() - s) / 2);
        gfx::drawIcon(
            p, icon, {kPadH - kPill + 0.5f, y, s, s}, color(ink(checked(), hovered() || unread))
        );
    }
    Icon   icon;
    Label *label  = nullptr;
    // Threads: followed threads with unread replies (Store::unreadThreads);
    // Activity: unread items in the feed (Store::unreadActivity).
    bool   unread = false;
};

// The "+" and the "⋮" (More actions) on a section header: shown while the
// header is hovered. `at` is where the press was (the menus open at the
// cursor); a right click on it does the same as a left one.
class HeaderButton final : public Clickable {
public:
    HeaderButton(Icon i, const char *tip) : icon(i) {
        style().size(14, 14).noShrink();
        setLook({C::None, C::None, C::None, C::None, 0});
        setTooltip(tip);
    }
    void paint(gfx::Painter &p) override {
        gfx::drawIcon(p, icon, bounds(), color(hovered() ? C::SidebarText : C::SidebarTextMuted));
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave)
            update();
        if (e.type == EventType::PointerDown)
            at = e.windowPos;
        if (e.type == EventType::ContextMenu && onRightClick) {
            at = e.windowPos;
            onRightClick();
            return true;
        }
        return Clickable::onEvent(e);
    }
    Icon                  icon;
    PointF                at;
    std::function<void()> onRightClick;
};

// A count centred in the view by its ink, not by a Label: centring the line
// box and the advance width puts the digits' ink up to two device pixels off
// (uneven ascent/descent and side bearings, macOS's system font most), and
// hinting rounds the digits' height away from the font's cap height. So the
// rasterised ink box is centred, on whole device pixels, ties going right
// and down.
class CentredCount : public View {
public:
    explicit CentredCount(Font f) : _font(f) {}

    // "" paints nothing.
    void setText(std::string text) {
        if (text == _text)
            return;
        _text = std::move(text);
        _layout.reset();
        invalidateLayout();
        update();
    }

    SizeF measureContent(float, float) override {
        const text::Layout *l = textLayout();
        return l ? SizeF{std::ceil(l->width()), 0} : SizeF{0, 0};
    }

    void paint(gfx::Painter &p) override {
        View::paint(p);
        const text::Layout *l = textLayout();
        if (!l)
            return;
        const float       s = p.scale();
        const gfx::PointF o = p.toPhysical({0, 0});
        const float x = std::floor(o.x + (width() - _ink.w) * s / 2 - (_ink.x + _lean) * s + 0.5f);
        const float y = std::floor(o.y + (height() - _ink.h) * s / 2 - _ink.y * s + 0.5f);
        l->paint(p, {(x - o.x) / s, (y - o.y) / s});
    }

private:
    const text::Layout *textLayout() {
        if (_text.empty())
            return nullptr;
        const float      scale = windowScale();
        const gfx::Color fg    = color(C::BadgeText);
        if (!_layout || _scale != scale || _fg != fg) {
            text::AttributedText t;
            t.append(_text, font(_font, C::BadgeText));
            _layout = text::Layout::build(std::move(t), {}, scale);
            _ink    = _layout->inkBounds();
            _lean   = _layout->inkLean();
            _scale  = scale;
            _fg     = fg;
        }
        return _layout.get();
    }

    Font                          _font;
    std::string                   _text;
    std::unique_ptr<text::Layout> _layout;
    RectF                         _ink;
    float                         _lean  = 0;
    float                         _scale = 0;
    gfx::Color                    _fg    = 0;
};

// A red pill with a count (a circle for one digit), or the 8 px blue dot.
class CountBadge final : public CentredCount {
public:
    CountBadge() : CentredCount(Font::CountBadge) {
        style().noShrink();
        setVisible(false);
    }
    void set(int count, bool dot) {
        const bool show = count > 0 || dot;
        setVisible(show);
        if (!show)
            return;
        setText(
            count <= 0   ? std::string()
            : count > 99 ? std::string("99+")
                         : str::number(int64_t(count))
        );
        if (count > 0) {
            style().size(kAuto, 20).padding(5, 0).minW = 20;
            setBackground(C::Badge, 10);
        } else {
            style().size(8, 8).padding(0).minW = 0;
            setBackground(C::BadgeActivity, 4);
        }
    }
};

// Slack's group DM tile: the number of other people in it.
class GroupTile final : public CentredCount {
public:
    explicit GroupTile(size_t others) : CentredCount(Font::TileBold) {
        style().size(20, 20).noShrink();
        setBackground(C::PresenceAway, 5);
        setText(str::number(int64_t(std::max<size_t>(1, others))));
    }
};

bool looksLikeUserId(std::string_view s) {
    if (s.size() < 9 || (s[0] != 'U' && s[0] != 'W'))
        return false;
    for (char ch : s.substr(1))
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
            return false;
    return true;
}

} // namespace

bool deadDm(const model::Store &store, const model::Conversation &c) {
    if (c.kind != ConvKind::Dm || c.dmUser == model::kNoUser)
        return false;
    const model::User &u = store.user(c.dmUser);
    return !u.placeholder &&
           (u.deleted || u.displayName == "deactivateduser" || looksLikeUserId(u.label()));
}

// ── Rows ────────────────────────────────────────────────────────────────────

class SectionHeader final : public HoverRow {
public:
    SectionHeader(Sidebar &sb, int k, Icon i, std::string text)
        : sidebar(sb), kind(k), icon(i), title(text) {
        setLook({C::None, C::SidebarHover, C::SidebarHover, C::None, 0}); // full width
        setRole(Role::Button);
        style().row().height(rowH()).padding(kContentX, 0, kPadH, 0).items(Align::Center);
        label = add<Label>(std::move(text), Font::Section, C::SidebarTextMuted);
        label->setMaxLines(1);
        label->style().shrink = 1;
        add<View>()->style().flex(1);
        // Channels: "+" → find or create one; Direct messages: the browse
        // dialog's People tab; Sessions: find or create one (a right click
        // too); Team: add a teammate.
        const bool agents = sb._ctx.backend.capabilities().agentSessions;
        if (k == 1 || k == 2 || k == 4) {
            plus = add<HeaderButton>(
                Icon::Plus,
                k == 1   ? tr("Add channels")
                : k == 4 ? tr("Add teammate")
                : agents ? tr("Add sessions")
                         : tr("Open a direct message")
            );
            plus->setVisible(false);
            plus->onClick = [this, k, agents] {
                if (k == 1)
                    sidebar.addChannelsMenu(plus->at);
                else if (k == 4) {
                    if (sidebar.onAddTeammate)
                        sidebar.onAddTeammate();
                } else if (agents) {
                    if (sidebar.onSessionMenu)
                        sidebar.onSessionMenu(plus->at);
                } else if (sidebar.onBrowsePeople) {
                    sidebar.onBrowsePeople();
                }
            };
            if (k == 2 && agents)
                plus->onRightClick = [this] {
                    if (sidebar.onSessionMenu)
                        sidebar.onSessionMenu(plus->at);
                };
        }
        // More actions: the section menu (sort, the user's sections); not
        // on the Team, which lists no conversations.
        if (k != 4) {
            more = add<HeaderButton>(Icon::EllipsisVertical, tr("More actions"));
            more->style().margins(plus ? 6 : 0, 0, 0, 0);
            more->setVisible(false);
            more->onClick      = [this] { sidebar.sectionMenu(this, more->at); };
            more->onRightClick = more->onClick;
        }
    }
    void setUnread(bool on) {
        unread = on;
        label->setFont(on ? Font::SectionBold : Font::Section);
        label->setColor(labelInk());
        update();
    }
    // The label's and icon's colour: the selected pill's ink under a drop.
    C labelInk() const {
        return dropTarget ? C::SidebarSelectedText : unread ? C::SidebarText : C::SidebarTextMuted;
    }
    // A toggle rebuilds the rows, which forgets the hover until the
    // pointer moves again.
    bool lit() const { return hovered() && !stale; }
    void activate() override {
        stale = true;
        refreshLook();
        HoverRow::activate();
    }
    bool onEvent(Event &e) override {
        if ((e.type == EventType::PointerMove && stale) || e.type == EventType::PointerLeave) {
            stale = false;
            refreshLook();
        }
        // More actions, at the pointer.
        if (e.type == EventType::ContextMenu && more) {
            sidebar.sectionMenu(this, e.windowPos);
            return true;
        }
        return HoverRow::onEvent(e);
    }
    // A custom section's emoji, drawn where the icon goes ("" = the icon).
    void setEmoji(std::string glyph) {
        emoji = std::move(glyph);
        emojiLayout.reset();
    }
    void refreshLook() override {
        const C bg = dropTarget ? C::SidebarSelected : lit() ? C::SidebarHover : C::None;
        setLook({bg, bg, C::SidebarHover, C::None, 0});
        label->setColor(labelInk());
        if (plus)
            plus->setVisible(lit());
        if (more)
            more->setVisible(lit());
        update();
    }
    void paint(gfx::Painter &p) override {
        HoverRow::paint(p);
        // The section's icon; hovered, the chevron previewing the click.
        const Icon  i = !lit() ? icon : collapsed ? Icon::ChevronRight : Icon::ChevronDown;
        const float y = snapPx((height() - 14) / 2);
        if (!lit() && !emoji.empty()) {
            const float scale = windowScale();
            if (!emojiLayout || emojiScale != scale) {
                text::AttributedText t;
                t.append(emoji, font(Font::Body, C::SidebarText));
                emojiLayout = text::Layout::build(std::move(t), {}, scale);
                emojiScale  = scale;
            }
            emojiLayout->paint(
                p, {kPadH + (14 - emojiLayout->width()) / 2, (height() - emojiLayout->height()) / 2}
            );
            return;
        }
        gfx::drawIcon(p, i, {kPadH, y, 14, 14}, color(labelInk()));
    }
    Sidebar                      &sidebar;
    // 0 starred, 1 channels, 2 direct messages, 3 agents & apps, 4 team,
    // 5 a custom section (sectionId)
    int                           kind;
    Icon                          icon;
    std::string                   title;
    std::string                   sectionId; // kind 5: Store::Section::id
    std::string                   emoji;     // kind 5: its glyph ("" = the icon)
    std::unique_ptr<text::Layout> emojiLayout;
    float                         emojiScale = 0;
    Label                        *label      = nullptr;
    HeaderButton                 *plus       = nullptr;
    HeaderButton                 *more       = nullptr;
    std::vector<ConvRow *>        rows;
    std::vector<View *>           extras; // "N more channels", "Add channels"
    bool                          collapsed = false, unread = false, stale = false;
    bool                          dropTarget = false; // a dragged row would land here
};

namespace {

// "N more channels" and "Add channels": full-width hover, dim → bright.
class ActionRow final : public HoverRow {
public:
    ActionRow(std::string text, bool withPlus) : _plus(withPlus) {
        setLook({C::None, C::SidebarHover, C::SidebarHover, C::None, 0});
        setRole(Role::Button);
        style().row().height(rowH()).items(Align::Center);
        style().padding(withPlus ? kContentX + 14 + 6 : kContentX, 0, kPadH, 0);
        label = add<Label>(std::move(text), Font::Body, C::SidebarTextMuted);
        label->setMaxLines(1);
    }
    void refreshLook() override {
        label->setColor(hovered() ? C::SidebarText : C::SidebarTextMuted);
        update();
    }
    void paint(gfx::Painter &p) override {
        HoverRow::paint(p);
        if (_plus)
            gfx::drawIcon(
                p,
                Icon::Plus,
                {kContentX, snapPx((height() - 14) / 2), 14, 14},
                color(hovered() ? C::SidebarText : C::SidebarTextMuted)
            );
    }
    Label *label = nullptr;

private:
    bool _plus;
};

// The sidebar's huddle pill: the first participant's avatar, then an
// accent pill (headphones, the participant count) — a click joins instead
// of opening the conversation. kHuddlePad / kHuddleIcon / kHuddleGap.
class HuddlePill final : public Clickable {
public:
    HuddlePill() {
        setLook({C::None, C::None, C::None, C::None, 0});
        style().row().items(Align::Center).spacing(6).margins(6, 0, 0, 0).noShrink();
        avatar = smallAvatar(this);
        avatar->setHitTransparent(true);
        pill = add<View>();
        pill->style().row().height(18).padding(6, 0, 6, 0).items(Align::Center).noShrink();
        pill->setBackground(C::Accent, 9);
        pill->setHitTransparent(true);
        pill->add<IconView>(Icon::Headphones, 13, C::AccentText)->setHitTransparent(true);
        count = pill->add<Label>("", Font::CountBadge, C::AccentText);
        count->style().margins(4, 0, 0, 0);
        count->setHitTransparent(true);
    }
    Avatar *avatar = nullptr;
    View   *pill   = nullptr;
    Label  *count  = nullptr;
};

} // namespace

// What a conversation row and a teammate row share: selection on press
// (not on release), the row's context menu, a hover repaint (the presence
// dot's ring takes the hover colour), and the full name over a truncated one
// (a tooltip).
class SidebarRow : public Clickable {
public:
    bool onEvent(Event &e) override {
        if (e.type == EventType::ContextMenu && showMenu(e.windowPos))
            return true;
        if (e.type == EventType::PointerDown && e.button == plat::Button::Left) {
            activate();
            return true;
        }
        if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave)
            refresh();
        return Clickable::onEvent(e);
    }
    std::string tooltip() const override {
        auto *l = const_cast<Label *>(label);
        return l->measure(kInf, kInf).w > label->width() + 0.5f ? label->text() : std::string();
    }

protected:
    virtual void refresh()                 = 0;
    virtual bool showMenu(PointF windowAt) = 0; // false: no menu here
    Label       *label                     = nullptr;
};

class ConvRow final : public SidebarRow {
public:
    ConvRow(Sidebar &sb, ConvRef c) : sidebar(sb), conv(c) {
        pillRow(this);
        const auto &store = sb._ctx.store();
        const auto &cv    = store.conversation(c);
        if (cv.kind == ConvKind::Dm) {
            avatar = smallAvatar(this);
            avatar->style().margins(0, 0, 8, 0);
            avatar->setBitmap(sb._avatars.get(store.user(cv.dmUser).avatar, kAvatarPx));
            avatar->setInitial(store.user(cv.dmUser).label()); // while it downloads
        } else if (cv.kind == ConvKind::Group) {
            size_t others = 0;
            for (model::UserRef u : cv.members)
                others += u != store.me;
            add<GroupTile>(others)->style().margins(0, 0, 8, 0);
        } else {
            glyph = add<IconView>(convIcon(cv), 14, C::SidebarTextMuted);
            glyph->style().margins(0, 0, 6, 0);
        }
        label = add<Label>(store.displayName(c), Font::Body, C::SidebarTextMuted);
        label->setMaxLines(1);
        label->style().shrink = 1;
        if (cv.kind == ConvKind::Dm) {
            const auto &u = store.user(cv.dmUser);
            // The "EXT" pill after a Slack Connect peer's name.
            if (u.stranger)
                screens::addTagBadge(this, true, true)->style().margins(6, 0, 0, 0);
            if (!u.statusEmoji.empty()) {
                const model::Store::EmojiGlyph g = store.emojiFor(u.statusEmoji);
                if (!g.unicode.empty()) {
                    auto *s = add<Label>(g.unicode, Font::Body);
                    s->style().margins(4, 0, 0, 0).noShrink();
                } else if (!g.image.empty()) {
                    auto *s = add<Image>();
                    s->style().size(15, 15).margins(4, 0, 0, 0).noShrink();
                    s->setFit(Image::Fit::Contain);
                    s->setBitmap(sb._avatars.get(g.image, 30));
                }
            }
            if (cv.dmUser == store.me && store.me != model::kNoUser) {
                you = add<Label>(tr("you"), Font::YouLabel, C::SidebarTextMuted);
                you->style().margins(4, 0, 0, 0).noShrink();
            }
        }
        add<View>()->style().flex(1);
        // The badge and the huddle pill (in that order, last) are made the
        // first time they show: most rows never need either.
        onClick = [this] {
            if (sidebar._ctx.openConversation)
                sidebar._ctx.openConversation(conv);
        };
    }

    bool showMenu(PointF at) override {
        if (sidebar._menus)
            sidebar._menus->showChat(conv, at);
        return sidebar._menus != nullptr;
    }

    // A press that travels drags the row onto another section (the row it
    // pressed stays open, as a click opens it on the press).
    bool onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerDown:
            pressAt  = e.windowPos;
            pressing = e.button == plat::Button::Left;
            break;
        case EventType::PointerMove:
            if (pressing &&
                (sidebar._dragRow == this ||
                 std::abs(e.windowPos.x - pressAt.x) + std::abs(e.windowPos.y - pressAt.y) >=
                     kDragStart)) {
                sidebar.dragRow(this, e.windowPos);
                return true;
            }
            break;
        case EventType::PointerUp:
        case EventType::PointerCancel:
            pressing = false;
            if (sidebar._dragRow == this) {
                sidebar.dropRow(e.type == EventType::PointerUp);
                return true;
            }
            break;
        default:
            break;
        }
        return SidebarRow::onEvent(e);
    }
    static constexpr float kDragStart = 10; // px of travel before a press becomes a drag
    PointF                 pressAt;
    bool                   pressing = false;

    CountBadge *makeBadge() {
        if (!badge) { // before the huddle pill, when there is one
            badge = new CountBadge();
            adopt(std::unique_ptr<View>(badge), huddle ? int(childCount()) - 1 : -1);
        }
        return badge;
    }
    HuddlePill *makeHuddle() {
        if (!huddle) {
            huddle          = add<HuddlePill>();
            huddle->onClick = [this] {
                if (sidebar._ctx.openUrl)
                    sidebar._ctx.openUrl(huddleJoinUrl(sidebar._ctx.store, conv));
            };
        }
        return huddle;
    }

    // The DM's presence dot (a users change that moved nothing else).
    void refreshPresence() {
        if (!avatar)
            return;
        const auto &store   = sidebar._ctx.store();
        const auto &cv      = store.conversation(conv);
        const bool  sel     = sidebar._selected == conv;
        const bool  phantom = cv.dmUser == store.me && store.me != model::kNoUser &&
                              sidebar._ctx.backend.selfPresence().phantomAway();
        avatar->setPresence(
            Avatar::presenceOf(
                &store.user(cv.dmUser), sidebar._ctx.backend.capabilities().presence, phantom
            ),
            sel         ? C::SidebarSelected
            : hovered() ? C::SidebarHover
                        : C::Sidebar,
            sel
        );
    }

    // Everything that depends on Store state.
    void refresh() override {
        const auto &store = sidebar._ctx.store();
        const auto &cv    = store.conversation(conv);
        const auto  caps  = sidebar._ctx.backend.capabilities();
        const bool  sel   = sidebar._selected == conv;
        const bool  quiet = sidebar.muted(cv);
        const bool  stale =
            cv.latest && tsSecs(cv.latest) < sidebar._ctx.backend.nowSecs() - kMaxNotifyAgeSecs;
        unread        = sidebar.paintsUnread(cv);
        // DMs count every unread message (an agent session only what needs
        // me: Session counts those as mentions); channels their mentions.
        const int red = cv.isDirect() && !caps.agentSessions ? int(cv.unread) : int(cv.mentions);
        count         = !stale && !quiet && red > 0 ? red : 0;
        dot = !stale && !quiet && sidebar.level(cv) == model::NotifyLevel::All && !cv.isDirect() &&
              cv.mentions == 0 && cv.unread > 0;
        const C text = ink(sel, unread);
        label->setText(store.displayName(conv));
        label->setFont(unread ? Font::BodySemibold : Font::Body);
        label->setColor(text);
        if (glyph)
            glyph->setTint(text);
        if (you)
            you->setColor(sel ? text : C::SidebarTextMuted);
        refreshPresence();
        if (count > 0 || dot || badge)
            makeBadge()->set(count, count == 0 && dot);
        // A live huddle: who is in it and how many (no count before anyone joined).
        const bool live = caps.huddles && cv.huddleActive;
        if (live || huddle)
            makeHuddle()->setVisible(live);
        huddleCount = live ? std::max<int>(1, int(cv.huddleParticipants.size())) : 0;
        if (live) {
            const auto &ps = cv.huddleParticipants;
            huddle->avatar->setVisible(!ps.empty());
            if (!ps.empty()) {
                const auto &u = store.user(ps.front());
                huddle->avatar->setBitmap(sidebar._avatars.get(u.avatar, kAvatarPx));
                huddle->avatar->setInitial(u.label());
            }
            huddle->count->setText(ps.empty() ? std::string() : str::number(int64_t(ps.size())));
            huddle->count->setVisible(!ps.empty());
        }
        setChecked(sel);
    }

    Sidebar       &sidebar;
    ConvRef        conv;
    Avatar        *avatar  = nullptr;
    IconView      *glyph   = nullptr;
    Label         *you     = nullptr;
    CountBadge    *badge   = nullptr;
    HuddlePill    *huddle  = nullptr;
    SectionHeader *section = nullptr;
    size_t         base    = 0; // its place in the section as listed (before the sort)
    bool           unread = false, dot = false;
    int            count = 0, huddleCount = 0;
};

// A teammate in the Team section: the pill of a
// conversation row, its user's avatar and presence, its name — bright while
// any of its sessions has something unread (the sessions carry the badges).
class TeammateRow final : public SidebarRow {
public:
    TeammateRow(Sidebar &sb, const model::Backend::AgentRole &mate) : sidebar(sb), role(mate.id) {
        pillRow(this);
        const auto &store = sb._ctx.store();
        user              = mate.user;
        avatar            = smallAvatar(this);
        avatar->style().margins(0, 0, 8, 0);
        const std::string &pic = user < store.userCount() && !store.user(user).avatar.empty()
                                     ? store.user(user).avatar
                                     : mate.avatar;
        avatar->setBitmap(sb._avatars.get(pic, kAvatarPx));
        avatar->setInitial(mate.name);
        label = add<Label>(mate.name, Font::Body, C::SidebarTextMuted);
        label->setMaxLines(1);
        label->style().shrink = 1;
        add<View>()->style().flex(1);
        onClick = [this] {
            // Again on a re-click: back to the page from anywhere in it.
            sidebar.selectTeammate(role);
            if (sidebar.onTeammate)
                sidebar.onTeammate(role);
        };
    }
    bool showMenu(PointF at) override {
        if (sidebar._menus)
            sidebar._menus->showTeammate(role, at);
        return sidebar._menus != nullptr;
    }
    // `unread` as Sidebar::refreshTeammates found it.
    void refresh() override {
        const auto &store = sidebar._ctx.store();
        const bool  sel   = sidebar._selectedTeammate == role;
        label->setFont(unread ? Font::BodySemibold : Font::Body);
        label->setColor(
            sel      ? C::SidebarSelectedText
            : unread ? C::SidebarText
                     : C::SidebarTextMuted
        );
        const model::User *u = user < store.userCount() ? &store.user(user) : nullptr;
        avatar->setPresence(
            Avatar::presenceOf(u, sidebar._ctx.backend.capabilities().presence),
            sel         ? C::SidebarSelected
            : hovered() ? C::SidebarHover
                        : C::Sidebar,
            sel
        );
        setChecked(sel);
    }

    Sidebar       &sidebar;
    std::string    role;
    model::UserRef user   = model::kNoUser;
    Avatar        *avatar = nullptr;
    bool           unread = false; // any of its sessions is (set by the sidebar)
};

// ── Sidebar ─────────────────────────────────────────────────────────────────

Sidebar::Sidebar(screens::Context &ctx, Avatars &avatars) : _ctx(ctx), _avatars(avatars) {
    _scroll = add<ScrollView>();
    // A click focuses the list, and then its keys scroll it (they never change the selection). Tab
    // already stops on each row here, so the list itself is click-focus only.
    _scroll->setClickFocus(true);
    _scroll->setThinThumb(C::SidebarScrollbar);
    _scroll->style().flex(1);
    _items = _scroll->content();
    _items->style().padding(0, kTopPad, 0, 0).spacing(0);
    _footer = add<SidebarFooter>(*this, ctx, avatars);

    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        switch (ch.kind) {
        case model::ChangeKind::Roster:
            rebuild();
            break;
        case model::ChangeKind::Users: // names, status emoji, presence
            usersSoon();
            break;
        case model::ChangeKind::Append:
        case model::ChangeKind::Prepend:
        case model::ChangeKind::Insert:
            // History that makes a hidden conversation relevant lists it
            // (the rows are re-derived whenever conversations change).
            if (ch.thread == 0 && ch.conv < _ctx.store().conversationCount() && !rowFor(ch.conv) &&
                _ctx.store().conversation(ch.conv).member && relevant(ch.conv))
                rebuildSoon();
            else if (ch.thread == 0 && rowFor(ch.conv)) // most recent first may move it
                resortSoon();
            [[fallthrough]];
        case model::ChangeKind::Update:
        case model::ChangeKind::Remove:
            // A saved item added or gone shows or hides "Saved messages".
            refreshNav();
            break;
        case model::ChangeKind::Meta: {
            // A star toggle moves the row between sections, leaving/closing
            // removes it, reopening a DM adds it, and the unreads-only filter
            // may list or drop it: rebuild; else restyle.
            if (ch.conv == model::kNoConv && _threadsRow) { // the Threads entry's unread state
                auto *r   = static_cast<NavRow *>(_threadsRow);
                r->unread = _ctx.store().unreadThreads() > 0;
                r->refreshLook();
            }
            if (ch.conv == model::kNoConv && _activityRow) { // the Activity entry's
                auto *r   = static_cast<NavRow *>(_activityRow);
                r->unread = _ctx.store().unreadActivity() > 0;
                r->refreshLook();
            }
            // The scheduled list came or went: show or hide its entry. The
            // sections changed: the rows move.
            if (ch.conv == model::kNoConv) {
                refreshNav();
                if (_ctx.store().sectionsRevision() != _sectionsRev)
                    rebuildSoon(false);
            }
            if (ch.conv >= _ctx.store().conversationCount())
                break;
            const auto &cv = _ctx.store().conversation(ch.conv);
            ConvRow    *r  = rowFor(ch.conv);
            // Under unreads-only a row comes or goes when its bold state
            // flips (the selected row and Starred stay regardless).
            const bool  filterFlip =
                _filters.unreadsOnly && ch.conv != _selected && !cv.starred &&
                !(r && r->section->kind == 5) &&
                (r ? r->unread != paintsUnread(cv) : cv.member && paintsUnread(cv));
            const bool starFlip =
                r && r->section->kind != 5 && (r->section->kind == 0) != cv.starred;
            if (!r != !cv.member || starFlip || filterFlip)
                rebuildSoon(false); // once per burst; the list stays where it was scrolled
            else {
                refresh(ch.conv);
                sectionsSoon(); // the headers, its sessions' teammate: once per burst
                resortSoon();   // unreads on top, a new name or activity may move it
            }
            break;
        }
        default:
            break;
        }
    });
    rebuild();
}

Sidebar::~Sidebar() {
    _ctx.store.unobserve(_observer);
    for (plat::TimerId t : {_rebuildTimer, _usersTimer, _sectionsTimer})
        if (t)
            _ctx.app.platform().cancelTimer(t);
}

void Sidebar::paint(gfx::Painter &p) {
    paintNavGradient(*this, p, C::Sidebar);
}

void Sidebar::setFilters(const Filters &f) {
    if (f == _filters)
        return;
    _filters = f;
    rebuild();
}

bool Sidebar::muted(const model::Conversation &c) const {
    return level(c) == model::NotifyLevel::Nothing;
}

// The effective level: Default follows the global level.
model::NotifyLevel Sidebar::level(const model::Conversation &c) const {
    return c.effectiveNotify(_filters.defaultLevel);
}

// The one rule for bold rows and the unreads-only filter.
bool Sidebar::paintsUnread(const model::Conversation &c) const {
    if (c.unread == 0 || muted(c))
        return false;
    return !(
        level(c) == model::NotifyLevel::Mentions && !_filters.highlightMentionsOnly &&
        !c.isDirect() && c.mentions == 0
    );
}

bool Sidebar::isApp(const model::Conversation &c) const {
    return c.kind == ConvKind::Dm && c.dmUser != model::kNoUser && _ctx.store().user(c.dmUser).bot;
}

void Sidebar::setVisited(std::unordered_map<std::string, int64_t> stamps) {
    _visited = std::move(stamps);
    rebuild();
}

void Sidebar::clearVisited() {
    _visited.clear();
    // The rebuild re-seeds from what the Store knows (unreads): a fresh
    // first-launch view without a restart.
    rebuild();
    if (onVisitedChanged)
        onVisitedChanged();
}

// Relevant: unread, open, visited or active within the window, or a
// channel nothing is known about yet. Agent sessions are always listed.
bool Sidebar::relevant(ConvRef ref) const {
    const auto &c = _ctx.store().conversation(ref);
    if (c.unread > 0 || ref == _selected || _ctx.backend.capabilities().agentSessions)
        return true;
    const int64_t cutoff =
        _ctx.backend.nowSecs() - int64_t(std::max(1, _filters.relevantDays)) * 86400;
    const auto v = _visited.find(c.id);
    if (v != _visited.end() && v->second >= cutoff)
        return true;
    const model::Ts activity = std::max(c.latest, c.lastRead);
    if (activity)
        return tsSecs(activity) >= cutoff;
    return !c.isDirect();
}

void Sidebar::rebuild() {
    for (auto *h : _sections) { // survives the rebuild
        if (h->kind == 5)
            _collapsedCustom[h->sectionId] = h->collapsed;
        else
            _collapsed[h->kind] = h->collapsed;
    }
    _dragRow = nullptr; // a drag under way ends with its row
    _items->clearChildren();
    _rows.clear();
    _rowOf.assign(_ctx.store().conversationCount(), nullptr);
    _teamRows.clear();
    _sections.clear();
    _nav.clear();
    _navTitles.clear();
    _savedRow         = nullptr;
    _threadsRow       = nullptr;
    _scheduledRow     = nullptr;
    _activityRow      = nullptr;
    const auto &store = _ctx.store();
    const auto  caps  = _ctx.backend.capabilities();

    auto nav = [&](Icon icon, const char *text, std::function<void()> *hook) {
        auto *r    = _items->add<NavRow>(icon, text);
        r->onClick = [hook] {
            if (*hook)
                (*hook)();
        };
        r->refreshLook();
        _nav.push_back(r);
        _navTitles.push_back(text);
        return r;
    };
    if (caps.activityView) {
        auto *r      = nav(Icon::Bell, tr("Activity"), &onActivity);
        r->unread    = store.unreadActivity() > 0;
        _activityRow = r;
    }
    if (caps.threadsView) {
        auto *r     = nav(Icon::Split, tr("Threads"), &onThreads);
        r->unread   = store.unreadThreads() > 0;
        _threadsRow = r;
    }
    if (caps.messageReminders)
        _savedRow = nav(Icon::Bookmark, tr("Saved messages"), &onSavedMessages);
    if (caps.scheduledSend)
        _scheduledRow = nav(Icon::Clock, tr("Scheduled messages"), &onScheduled);
    // The open page's entry, if this workspace has one.
    if (!navRow(_navSelected))
        _navSelected = Nav::None;
    showNavSelection();

    // Starred first (any kind), then channels, direct messages, agents &
    // apps; fixture (= server) order inside each.
    std::vector<ConvRef> lists[4], hiddenCh;
    const auto           listed = [&](ConvRef c) {
        return !_filters.unreadsOnly || c == _selected || paintsUnread(store.conversation(c));
    };
    // The seed: a conversation with unread messages is stamped, so it stays
    // listed for the whole window once read.
    const int64_t now    = _ctx.backend.nowSecs();
    const int64_t cutoff = now - int64_t(std::max(1, _filters.relevantDays)) * 86400;
    bool          seeded = false;
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const auto &cv = store.conversation(c);
        if (!cv.member || cv.unread <= 0 || cv.id.empty())
            continue;
        int64_t &at = _visited[cv.id];
        if (at < cutoff) {
            at     = now;
            seeded = true;
        }
    }
    if (seeded && onVisitedChanged)
        onVisitedChanged();
    // The user's own sections, under Starred: each lists its conversations
    // in its own order, like Starred regardless of the filters, and takes
    // them out of the other sections (Starred too).
    const bool                        own  = caps.customSections;
    const auto                       &secs = store.sections();
    std::vector<std::vector<ConvRef>> custom(own ? secs.size() : 0);
    std::vector<char>                 inCustom(store.conversationCount(), 0);
    for (size_t s = 0; s < custom.size(); ++s)
        for (const std::string &id : secs[s].convs) {
            const ConvRef c = store.findConversation(id);
            if (c == kNoConv || inCustom[c])
                continue;
            inCustom[c] = 1;
            if (store.conversation(c).member && !deadDm(store, store.conversation(c)))
                custom[s].push_back(c);
        }
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const auto &cv = store.conversation(c);
        if (!cv.member || deadDm(store, cv) || inCustom[c]) // left channels, closed or dead DMs
            continue;
        if (cv.starred)
            lists[0].push_back(c);
        else if (isApp(cv)) {
            if (listed(c) && (_filters.showAgentsApps || c == _selected))
                lists[3].push_back(c);
        } else if (cv.isDirect()) {
            if (relevant(c) && listed(c))
                lists[2].push_back(c);
        } else if (relevant(c) && listed(c))
            lists[1].push_back(c);
        else
            hiddenCh.push_back(c);
    }
    _hiddenChannels = _showAllChannels ? 0 : int(hiddenCh.size());
    if (_showAllChannels)
        lists[1].insert(lists[1].end(), hiddenCh.begin(), hiddenCh.end());
    const bool agents = caps.agentSessions;
    _team = agents ? _ctx.backend.agentRoles() : std::vector<model::Backend::AgentRole>{};
    struct Def {
        Icon        icon;
        const char *title;
    };
    const Def defs[] = {
        {Icon::Star, tr("Starred")},
        {Icon::Hash, tr("Channels")},
        {Icon::MessagesSquare, agents ? tr("Sessions") : tr("Direct messages")},
        {Icon::Bot, tr("Agents & apps")},
    };
    // A header and its rows (a click on the header collapses them).
    const auto section = [&](int                         kind,
                             Icon                        icon,
                             std::string                 title,
                             const std::vector<ConvRef> &rows,
                             const std::string          &id = {}) {
        auto *h      = _items->add<SectionHeader>(*this, kind, icon, std::move(title));
        h->collapsed = kind < 5 && _collapsed[kind];
        _sections.push_back(h);
        h->onClick = [this, h] {
            h->collapsed = !h->collapsed;
            applyCollapse(h);
        };
        h->sectionId = id;
        for (size_t i : sortedOrder(sortOf(h), rows)) {
            const ConvRef c   = rows[i];
            auto         *row = _items->add<ConvRow>(*this, c);
            row->section      = h;
            row->base         = i;
            h->rows.push_back(row);
            _rows.push_back(row);
            if (c < _rowOf.size())
                _rowOf[c] = row;
        }
        return h;
    };
    for (int s = 0; s < 4; ++s) {
        if (s == 1) // the user's own sections, between Starred and Channels
            for (size_t i = 0; i < custom.size(); ++i) {
                auto *h = section(5, Icon::List, secs[i].name, custom[i], secs[i].id);
                if (!secs[i].emoji.empty())
                    h->setEmoji(store.emojiFor(secs[i].emoji).unicode);
                const auto was = _collapsedCustom.find(secs[i].id);
                h->collapsed   = was != _collapsedCustom.end() && was->second;
                applyCollapse(h);
            }
        // No Starred section until something is starred, no Agents & apps
        // without an app DM, and an agent workspace drops an empty Channels.
        if (lists[s].empty() && (s == 0 || s == 3 || (s == 1 && agents && hiddenCh.empty())))
            continue;
        auto *h = section(s, defs[s].icon, defs[s].title, lists[s]);
        if (s == 1 && _hiddenChannels > 0) {
            auto *more = _items->add<ActionRow>(
                i18n::arg(
                    tr("%1 more %2"),
                    str::number(int64_t(_hiddenChannels)),
                    _hiddenChannels == 1 ? tr("channel") : tr("channels")
                ),
                false
            );
            more->onClick = [this] { showAllChannels(); };
            h->extras.push_back(more);
        }
        if (s == 1 || (s == 2 && agents)) {
            auto *add =
                _items->add<ActionRow>(s == 1 ? tr("Add channels") : tr("Add sessions"), true);
            add->onClick = [this, add, s] {
                if (s == 1) {
                    addChannelsMenu(add);
                } else if (onSessionMenu) { // Sessions: find one or create one
                    const RectF r = add->windowRect();
                    onSessionMenu({r.x + kContentX, r.y + r.h / 2});
                }
            };
            h->extras.push_back(add);
        }
        applyCollapse(h);
        // The team, under the sessions (the fourth section): the roles sessions
        // are started with.
        if (s == 2 && !_team.empty()) {
            auto *t      = _items->add<SectionHeader>(*this, 4, Icon::Users, tr("Team"));
            t->collapsed = _collapsed[4];
            _sections.push_back(t);
            t->onClick = [this, t] {
                t->collapsed = !t->collapsed;
                applyCollapse(t);
            };
            for (const auto &mate : _team) {
                auto *row = _items->add<TeammateRow>(*this, mate);
                t->extras.push_back(row);
                _teamRows.push_back(row);
            }
            applyCollapse(t);
        }
    }
    refreshTeammates();
    refreshAll();
    _footer->refresh();
    noteShape(userShape());
    _sectionsRev = store.sectionsRevision();
    ++_rebuilds;
    invalidateLayout();
}

// Everything rebuild() reads from the users: which DMs are dead or apps (they
// leave the list or change section), and what a row is built with but
// refresh() doesn't restyle (avatar, EXT pill, status emoji, "you"), the
// team. Equal → a users change only needs the rows restyled.
uint64_t Sidebar::userShape() const {
    const auto &store = _ctx.store();
    uint64_t    h     = crypto::kFnvOffset;
    const auto  mix   = [&h](std::string_view v) {
        h = crypto::fnv1a(v, h);
        h = crypto::fnv1a(std::string_view("\x1f", 1), h);
    };
    std::string bits;
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const auto &cv = store.conversation(c);
        if (cv.kind == ConvKind::Dm)
            bits += char('0' + (deadDm(store, cv) ? 1 : 0) + (isApp(cv) ? 2 : 0));
    }
    mix(bits);
    mix(str::number(int64_t(store.me)));
    for (const ConvRow *r : _rows) {
        const auto &cv = store.conversation(r->conv);
        if (cv.kind != ConvKind::Dm)
            continue;
        const model::User &u = store.user(cv.dmUser);
        mix(u.avatar);
        mix(u.label());
        mix(u.stranger ? "1" : "0");
        mix(u.statusEmoji);
        if (!u.statusEmoji.empty()) {
            const model::Store::EmojiGlyph g = store.emojiFor(u.statusEmoji);
            mix(g.unicode);
            mix(g.image);
        }
    }
    if (_ctx.backend.capabilities().agentSessions)
        for (const auto &mate : _ctx.backend.agentRoles()) {
            mix(mate.id);
            mix(mate.name);
            mix(mate.avatar);
            mix(str::number(int64_t(mate.user)));
            if (mate.user < store.userCount())
                mix(store.user(mate.user).avatar);
        }
    return h;
}

// A burst of users changes (a presence round, a users.list page) → one pass:
// restyle the rows in place, rebuild only when the list's shape changed.
void Sidebar::usersSoon() {
    if (_usersTimer)
        return;
    std::weak_ptr<int> alive = _alive;
    _usersTimer              = _ctx.app.platform().addTimer(0, false, [this, alive] {
        if (alive.expired())
            return;
        _usersTimer = 0;
        if (_rebuildTimer)
            return; // the pending rebuild reads the users anyway
        // No profile (nor custom emoji) changed since the shape was taken:
        // a presence round, which only moves the dots. An agent workspace
        // (its team may change with its users) always takes the long way.
        const auto &st = _ctx.store();
        if (!_ctx.backend.capabilities().agentSessions && _shapeStore == &st && _shapeMe == st.me &&
            _shapeProfileRev == st.profileRevision() && _shapeTextRev == st.textRevision()) {
            for (ConvRow *r : _rows)
                r->refreshPresence();
            _footer->refresh();
            return;
        }
        const uint64_t shape = userShape();
        if (shape != _userShape) {
            rebuild();
            return;
        }
        noteShape(shape);
        refreshTeammates();
        refreshAll();
        _footer->refresh();
    });
}

void Sidebar::noteShape(uint64_t shape) {
    const auto &st   = _ctx.store();
    _userShape       = shape;
    _shapeStore      = &st;
    _shapeMe         = st.me;
    _shapeProfileRev = st.profileRevision();
    _shapeTextRev    = st.textRevision();
}

void Sidebar::sectionsSoon() {
    if (_sectionsTimer)
        return;
    std::weak_ptr<int> alive = _alive;
    _sectionsTimer           = _ctx.app.platform().addTimer(0, false, [this, alive] {
        if (alive.expired())
            return;
        _sectionsTimer = 0;
        refreshSections();
        refreshTeammates();
        if (std::exchange(_resort, false) && !sortsHold())
            rebuildSoon(false);
    });
}

void Sidebar::resortSoon() {
    if (_sorts.empty())
        return;
    _resort = true;
    sectionsSoon();
}

bool Sidebar::sortsHold() const {
    for (const SectionHeader *h : _sections) {
        const SectionSort sort = sortOf(h);
        if (sort.plain() || h->rows.size() < 2)
            continue;
        std::vector<ConvRef> listed(h->rows.size());
        for (const ConvRow *r : h->rows)
            listed[r->base] = r->conv;
        const std::vector<size_t> order = sortedOrder(sort, listed);
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] != h->rows[i]->base)
                return false;
    }
    return true;
}

std::vector<size_t> Sidebar::sortedOrder(SectionSort sort, const std::vector<ConvRef> &rows) const {
    std::vector<size_t> order(rows.size());
    std::iota(order.begin(), order.end(), size_t(0));
    if (sort.plain())
        return order;
    const auto &store = _ctx.store();
    struct Key {
        bool        unread = false;
        model::Ts   recent = 0;
        std::string name; // case-folded
    };
    std::vector<Key> keys(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &cv = store.conversation(rows[i]);
        keys[i].unread = sort.unreadsFirst && paintsUnread(cv);
        if (sort.by == SectionSort::By::Recent)
            keys[i].recent = std::max(cv.latest, cv.lastRead);
        else if (sort.by == SectionSort::By::Alpha)
            keys[i].name = utf8::foldCase(store.displayName(rows[i]));
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Key &x = keys[a], &y = keys[b];
        if (x.unread != y.unread)
            return x.unread;
        switch (sort.by) {
        case SectionSort::By::Alpha:
            return x.name < y.name;
        case SectionSort::By::Recent:
            return x.recent > y.recent;
        case SectionSort::By::Default:
            break;
        }
        return false;
    });
    return order;
}

// ── Sorting and the section menu ────────────────────────────────────────────

std::string SectionSort::encode() const {
    std::string out = by == By::Alpha ? "alpha" : by == By::Recent ? "recent" : "";
    if (unreadsFirst)
        out += out.empty() ? "unreads" : ",unreads";
    return out;
}

SectionSort SectionSort::decode(std::string_view s) {
    SectionSort sort;
    for (std::string_view part : str::split(s, ','))
        if (part == "alpha")
            sort.by = By::Alpha;
        else if (part == "recent")
            sort.by = By::Recent;
        else if (part == "unreads")
            sort.unreadsFirst = true;
    return sort;
}

std::string Sidebar::sectionKey(int kind, const std::string &id) {
    switch (kind) {
    case 0:
        return "#starred";
    case 1:
        return "#channels";
    case 2:
        return "#direct";
    case 3:
        return "#apps";
    case 5:
        return id;
    }
    return {};
}

SectionSort Sidebar::sortOf(const SectionHeader *h) const {
    const auto it = _sorts.find(sectionKey(h->kind, h->sectionId));
    return it == _sorts.end() ? SectionSort{} : it->second;
}

void Sidebar::setSorts(Sorts sorts) {
    std::erase_if(sorts, [](const auto &e) { return e.first.empty() || e.second.plain(); });
    if (sorts == _sorts)
        return;
    _sorts = std::move(sorts);
    rebuild();
}

void Sidebar::setSort(const std::string &key, SectionSort sort) {
    Sorts next = _sorts;
    if (sort.plain())
        next.erase(key);
    else
        next[key] = sort;
    if (next == _sorts)
        return;
    _sorts = std::move(next);
    rebuildSoon(false); // not from inside the menu's own pick
    if (onSortsChanged)
        onSortsChanged();
}

void Sidebar::sectionMenu(SectionHeader *h, PointF at) {
    if (!_menus)
        return;
    const std::string key = sectionKey(h->kind, h->sectionId);
    _menus->showSection(
        h->kind == 5 ? h->sectionId : std::string(), sortOf(h), at, [this, key](SectionSort sort) {
            setSort(key, sort);
        }
    );
}

// Collapsed, a section lists nothing under its header.
void Sidebar::applyCollapse(SectionHeader *h) {
    for (ConvRow *r : h->rows)
        r->setVisible(!h->collapsed);
    for (View *v : h->extras)
        v->setVisible(!h->collapsed);
    h->update();
}

// ── Dragging a row onto a section ───────────────────────────────────────────

// The section under the pointer: its header, its rows, the rows' extras.
SectionHeader *Sidebar::sectionAt(PointF windowPos) const {
    for (SectionHeader *h : _sections) {
        if (h->kind == 4) // the Team lists teammates, not conversations
            continue;
        RectF span = h->windowRect();
        for (View *v : h->rows)
            if (v->visible())
                span.h = std::max(span.h, v->windowRect().bottom() - span.y);
        for (View *v : h->extras)
            if (v->visible())
                span.h = std::max(span.h, v->windowRect().bottom() - span.y);
        if (windowPos.y >= span.y && windowPos.y < span.y + span.h)
            return h;
    }
    return nullptr;
}

// Where the dragged row would land (null: nowhere new): a custom section,
// Starred, or out of its custom section / Starred via the other built-ins.
static bool accepts(const SectionHeader *to, const SectionHeader *from) {
    if (!to || to == from)
        return false;
    if (to->kind == 5 || to->kind == 0)
        return true;
    return from->kind == 5 || from->kind == 0; // back to its own section
}

void Sidebar::dragRow(ConvRow *row, PointF windowPos) {
    if (!_ctx.backend.capabilities().customSections)
        return;
    _dragRow           = row;
    SectionHeader *to  = sectionAt(windowPos);
    SectionHeader *hit = accepts(to, row->section) ? to : nullptr;
    for (SectionHeader *h : _sections)
        if (h->dropTarget != (h == hit)) {
            h->dropTarget = h == hit;
            h->refreshLook();
        }
    // Near the list's edges it scrolls along.
    const RectF v = _scroll->windowRect();
    if (windowPos.y < v.y + rowH())
        _scroll->scrollBy(-rowH() / 2);
    else if (windowPos.y > v.bottom() - rowH())
        _scroll->scrollBy(rowH() / 2);
}

void Sidebar::dropRow(bool drop) {
    ConvRow       *row = std::exchange(_dragRow, nullptr);
    SectionHeader *to  = nullptr;
    for (SectionHeader *h : _sections)
        if (h->dropTarget) {
            to            = h;
            h->dropTarget = false;
            h->refreshLook();
        }
    if (drop && row && to)
        moveInto(row->conv, to); // rebuilds once the Store says so
}

void Sidebar::moveInto(ConvRef conv, SectionHeader *to) {
    model::Backend &be = _ctx.backend;
    if (to->kind == 5)
        be.moveToSection(conv, to->sectionId);
    else if (to->kind == 0)
        be.setStarred(conv, true); // takes it out of its section too
    else {
        be.moveToSection(conv, {});
        if (_ctx.store().conversation(conv).starred)
            be.setStarred(conv, false);
    }
}

bool Sidebar::dropOnSection(ConvRef conv, std::string_view title) {
    const ConvRow *r = rowFor(conv);
    for (SectionHeader *h : _sections)
        if (r && h->title == title && accepts(h, r->section)) {
            moveInto(conv, h);
            return true;
        }
    return false;
}

void Sidebar::showAllChannels() {
    _showAllChannels = true;
    rebuildSoon(); // not from inside the row's own click
}

void Sidebar::rebuildSoon(bool reveal) {
    _revealOnRebuild = _revealOnRebuild || reveal;
    if (_rebuildTimer)
        return;
    std::weak_ptr<int> alive = _alive;
    _rebuildTimer            = _ctx.app.platform().addTimer(0, false, [this, alive] {
        if (alive.expired())
            return;
        _rebuildTimer = 0;
        rebuild();
        if (ConvRow *r = rowFor(_selected); r && std::exchange(_revealOnRebuild, false))
            _scroll->ensureVisible(r, 8);
        _revealOnRebuild = false;
    });
}

void Sidebar::addChannelsMenu(View *row) {
    const RectF r = row->windowRect();
    addChannelsMenu({r.x + kContentX, r.y + r.h / 2});
}

void Sidebar::addChannelsMenu(PointF at) {
    Window *w = window();
    if (!w)
        return;
    std::vector<MenuItem> items(2);
    items[0].id      = 1;
    items[0].label   = tr("Find a channel");
    items[0].enabled = bool(onFindChannel);
    items[1].id      = 2;
    items[1].label   = tr("Create a channel");
    items[1].enabled = bool(onCreateChannel);
    Menu::popupAt(*w, at, std::move(items), [this](int id) {
        auto &hook = id == 1 ? onFindChannel : onCreateChannel;
        if (hook)
            hook();
    });
}

void Sidebar::refreshAll() {
    for (ConvRow *r : _rows)
        r->refresh();
    refreshSections();
}

void Sidebar::refreshTeammates() {
    if (_teamRows.empty())
        return;
    // The roles with an unread session, in one pass over the conversations.
    const auto              &store = _ctx.store();
    std::vector<std::string> roles;
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const auto &cv = store.conversation(c);
        if (cv.member && paintsUnread(cv))
            if (std::string role = _ctx.backend.agentSessionRole(c);
                std::find(roles.begin(), roles.end(), role) == roles.end())
                roles.push_back(std::move(role));
    }
    for (TeammateRow *r : _teamRows) {
        r->unread = std::find(roles.begin(), roles.end(), r->role) != roles.end();
        r->refresh();
    }
}

void Sidebar::selectTeammate(const std::string &role) {
    if (!role.empty()) {
        select(kNoConv);
        if (_navSelected != Nav::None)
            selectNav(Nav::None);
    }
    _selectedTeammate = role;
    refreshTeammates();
    for (TeammateRow *r : _teamRows)
        if (r->role == role) {
            for (SectionHeader *h : _sections)
                if (h->kind == 4 && h->collapsed) {
                    h->collapsed = false;
                    applyCollapse(h);
                }
            _scroll->ensureVisible(r, 8);
        }
}

std::vector<std::string> Sidebar::teammates() const {
    std::vector<std::string> out;
    for (const TeammateRow *r : _teamRows)
        out.push_back(r->role);
    return out;
}

Sidebar::TeammateState Sidebar::teammateState(const std::string &role) const {
    TeammateState s;
    for (const TeammateRow *r : _teamRows)
        if (r->role == role) {
            s.exists   = true;
            s.bold     = r->unread;
            s.selected = r->checked();
            s.presence = int(r->avatar->presence());
        }
    return s;
}

int Sidebar::conversationPresence(ConvRef conv) const {
    const ConvRow *r = rowFor(conv);
    return r && r->avatar ? int(r->avatar->presence()) : 0;
}

void Sidebar::refresh(ConvRef conv) {
    if (ConvRow *r = rowFor(conv))
        r->refresh();
}

void Sidebar::refreshSections() {
    // A section header is bright and bold while anything in it is.
    for (SectionHeader *h : _sections) {
        bool any = false;
        for (ConvRow *r : h->rows)
            any |= r->unread;
        h->setUnread(any);
    }
    refreshNav();
}

void Sidebar::refreshNav() {
    // "Saved messages" shows while the saved list is not empty.
    if (_savedRow)
        _savedRow->setVisible(_ctx.store().hasSaved());
    // "Scheduled messages" only while something waits to be posted.
    if (_scheduledRow)
        _scheduledRow->setVisible(_ctx.store().hasScheduled());
}

bool Sidebar::scheduledShown() const {
    return _scheduledRow && _scheduledRow->visible();
}

const ui::View *Sidebar::rowView(ConvRef conv) const {
    return rowFor(conv);
}

ConvRow *Sidebar::rowFor(ConvRef conv) const {
    return conv < _rowOf.size() ? _rowOf[conv] : nullptr;
}

View *Sidebar::navRow(Nav n) const {
    switch (n) {
    case Nav::Activity:
        return _activityRow;
    case Nav::Threads:
        return _threadsRow;
    case Nav::Saved:
        return _savedRow;
    case Nav::Scheduled:
        return _scheduledRow;
    case Nav::None:
        break;
    }
    return nullptr;
}

void Sidebar::selectNav(Nav n) {
    _navSelected = navRow(n) ? n : Nav::None;
    if (_navSelected != Nav::None) {
        select(kNoConv);
        selectTeammate({});
    }
    showNavSelection();
}

void Sidebar::showNavSelection() {
    for (Nav k : {Nav::Activity, Nav::Threads, Nav::Saved, Nav::Scheduled})
        if (auto *r = static_cast<NavRow *>(navRow(k))) {
            r->setChecked(_navSelected == k);
            r->refreshLook();
        }
}

void Sidebar::select(ConvRef conv) {
    const ConvRef old = _selected;
    _selected         = conv;
    if (conv != kNoConv && _navSelected != Nav::None)
        selectNav(Nav::None);                            // the overview page is left
    if (conv != kNoConv && !_selectedTeammate.empty()) { // the teammate's page is left
        _selectedTeammate.clear();
        refreshTeammates();
    }
    if (conv < _ctx.store().conversationCount()) { // a visit keeps it relevant for the whole window
        if (const std::string &id = _ctx.store().conversation(conv).id; !id.empty()) {
            _visited[id] = _ctx.backend.nowSecs();
            if (onVisitedChanged)
                onVisitedChanged();
        }
    }
    refresh(old);
    // A conversation opened from elsewhere must have a row: selecting it
    // lists it (relevance, unreads-only, apps) and
    // expands its section. Rebuilt after the click that got here.
    ConvRow *r = rowFor(conv);
    if (r && r->section->collapsed) {
        r->section->collapsed = false;
        applyCollapse(r->section);
    }
    if ((conv != kNoConv && !r) || (_filters.unreadsOnly && old != kNoConv && old != conv))
        rebuildSoon(); // the one left may drop out of unreads-only
    if (r) {
        r->refresh();
        _scroll->ensureVisible(r, 8);
    }
    refreshSections();
}

std::vector<ConvRef> Sidebar::order() const {
    std::vector<ConvRef> out;
    for (ConvRow *r : _rows)
        if (r->visible())
            out.push_back(r->conv);
    return out;
}

std::vector<std::string> Sidebar::sectionTitles() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < _nav.size(); ++i)
        if (_nav[i]->visible())
            out.push_back(_navTitles[i]);
    for (const SectionHeader *h : _sections)
        out.push_back(h->title);
    return out;
}

std::string Sidebar::rowSection(ConvRef conv) const {
    const ConvRow *r = rowFor(conv);
    return r && r->section->kind == 5 ? r->section->sectionId : std::string();
}

bool Sidebar::toggleSection(std::string_view title) {
    for (SectionHeader *h : _sections)
        if (h->title == title) {
            h->activate();
            return true;
        }
    return false;
}

Sidebar::RowState Sidebar::rowState(ConvRef conv) const {
    RowState       s;
    const ConvRow *r = rowFor(conv);
    if (!r)
        return s;
    s.exists   = true;
    s.visible  = r->visible();
    s.bold     = r->unread;
    s.dot      = r->count == 0 && r->dot;
    s.badge    = r->count;
    s.selected = r->checked();
    s.huddle   = r->huddleCount;
    return s;
}

bool Sidebar::threadsUnread() const {
    return _threadsRow && static_cast<NavRow *>(_threadsRow)->unread;
}

bool Sidebar::activityUnread() const {
    return _activityRow && static_cast<NavRow *>(_activityRow)->unread;
}

bool Sidebar::joinHuddle(ConvRef conv) {
    ConvRow *r = rowFor(conv);
    if (!r || !r->huddle || !r->huddle->visible())
        return false;
    r->huddle->onClick();
    return true;
}

int Sidebar::attentionCount() const {
    return workspaceAttention(_ctx.store(), _filters.defaultLevel, _ctx.backend.nowSecs())
        .important;
}

Attention workspaceAttention(const model::Store &st, model::NotifyLevel fallback, int64_t nowSecs) {
    Attention a;
    for (ConvRef c = 0; c < st.conversationCount(); ++c) {
        const auto &cv = st.conversation(c);
        if (!cv.member || (cv.latest && tsSecs(cv.latest) < nowSecs - kMaxNotifyAgeSecs))
            continue; // left or closed; or all of it older than the notification window
        const model::NotifyLevel level = cv.effectiveNotify(fallback);
        if (level == model::NotifyLevel::Nothing)
            continue;
        if (st.answersAreMentions) // an agent session: only what needs you
            a.important += int(cv.mentions);
        else if (cv.isDirect())
            a.important += int(cv.unread);
        else {
            a.important += int(cv.mentions);
            a.unread = a.unread || (level == model::NotifyLevel::All && cv.unread > cv.mentions);
        }
    }
    return a;
}

} // namespace shell
