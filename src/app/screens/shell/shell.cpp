#include "screens/shell/shell.h"

#include "app/llm/service.h"
#include "app/llm/voice_input.h"
#include "app/media/sounds.h"
#include "app/mrkdwn/emoji.h"
#include "app/spell/spell.h"
#include "base/log.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/common/avatar_initial.h"
#include "screens/common/file_dialogs.h"
#include "screens/common/remote_images.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/canvas_page.h"
#include "screens/shell/canvas_viewer.h"
#include "screens/shell/channel_dialogs.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/header.h"
#include "screens/shell/huddle_banner.h"
#include "screens/shell/message_search.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/profile_card.h"
#include "screens/shell/shell_dialogs.h"
#include "screens/shell/quick_switcher.h"
#include "screens/shell/session_status_dialog.h"
#include "screens/shell/teammate_page.h"
#include "screens/shell/overview_page.h"
#include "screens/shell/saved_page.h"
#include "screens/shell/scheduled_page.h"
#include "screens/shell/threads_page.h"
#include "screens/shell/shortcuts.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/sidebar_footer.h"
#include "screens/shell/standin.h"
#include "screens/shell/status_dialog.h"
#include "screens/shell/typing_indicator.h"
#ifdef MSGA_SELF_UPDATE
#include "screens/shell/update_bar.h"
#endif
#include "app/model/jobs.h"
#ifdef MSGA_SELF_UPDATE
#include "app/update/updater.h"
#endif
#include "base/process.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"
#include "app/screens/messages/thread_panel.h"
#endif

#include <algorithm>
#include <tuple>
#include <cmath>
#include <cstring>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::Ts;

namespace shell {

namespace {

#ifdef __APPLE__
// The unified header (NSWindowToolbarStyleUnifiedCompact), where AppKit
// does not say how tall its title bar is.
constexpr float kMacTitleBarH = 38;
#else
constexpr float kTitleBarH = 22; // the compact title strip (Linux/Windows)
#endif
constexpr float kRailW = 64;
constexpr float kListW = 240, kListMinW = 160, kListMaxW = 400;
constexpr float kResizeEdge          = 6;
// Settings changes are written this long after the first of a burst.
constexpr int   kSettingsSaveDelayMs = 500;

} // namespace

// An attached workspace (attachWorkspace): its Store's observer and the
// counts its rail tile shows.
struct Shell::Running {
    std::string              key;
    model::Store            *store    = nullptr;
    model::Backend          *backend  = nullptr;
    model::Store::ObserverId observer = 0;
    bool                     live     = false;
    bool                     dirty    = true; // att needs a recount
    Attention                att;
};

// ── Small views ─────────────────────────────────────────────────────────────

// The workspace bubble on the rail: the icon in
// a 40 px rounded square (the name's first letter until there is one), a
// white ring while active or hovered, the attention dot at the corner — red
// for mentions and DMs, blue for other unread.
class RailTile final : public Clickable {
public:
    static constexpr float kSize = 40, kRadius = 10;

    RailTile() {
        style().size(kSize, kSize);
        setLook({C::None, C::None, C::None, C::None, kRadius});
        setPaintOutset(4);
        setHoverRepaint(true);
        setCursor(plat::Cursor::Hand);
    }
    void setDot(int kind) { // 0 none, 1 activity, 2 mention
        if (kind != dot) {
            dot = kind;
            update();
        }
    }
    void setBitmap(std::shared_ptr<const gfx::Bitmap> b) {
        if (b != bitmap) {
            bitmap = std::move(b);
            scaled.reset();
            update();
        }
    }
    void setActive(bool on) {
        if (on != active) {
            active = on;
            update();
        }
    }
    // While lifted the rail paints this bubble above the others (paintBubble).
    void setLifted(bool on) {
        lifted = on;
        update();
    }
    // A press that turned into a drag ends without a click.
    void release() {
        setFlag(Pressed, false);
        setFlag(UserFlag1, false);
        update();
    }
    bool onEvent(Event &e) override {
        if (filter && filter(e))
            return true;
        if (e.type == EventType::ContextMenu && onMenu) {
            onMenu(e.windowPos);
            return true;
        }
        return Clickable::onEvent(e);
    }
    // The workspace bubble without its icon (not there yet, or none): the
    // name's first letter, white bold 17 px.
    void setName(std::string_view name) {
        std::string l = screens::avatarInitial(name.empty() ? std::string_view("?") : name);
        if (l != letter) {
            letter = std::move(l);
            layout.reset();
            update();
        }
    }
    void styleChanged() override {
        layout.reset();
        Clickable::styleChanged();
    }
    bool showsBitmap(const gfx::Bitmap *b) const override { return bitmap.get() == b; }
    void paint(gfx::Painter &p) override {
        if (!lifted)
            paintBubble(p, bounds(), hovered() && !pressed());
    }
    // The bubble in `r` (local to whoever paints it: this tile, or the rail
    // while it is lifted).
    void paintBubble(gfx::Painter &p, RectF r, bool hover) {
        const float radius = kRadius * r.w / kSize; // the shape kept under the lift
        const bool  photo  = bitmap && !bitmap->empty();
        if (photo) {
            // Shrunk once per size (drawBitmap would area-average every frame).
            const int px = int(std::lround(r.w * windowScale()));
            if (!scaled || scaled->width() != px)
                scaled = std::make_shared<gfx::Bitmap>(gfx::resize(bitmap->view(), px, px));
            p.save();
            p.clipRoundRect(r, radius);
            p.drawBitmap(scaled->view(), r, gfx::Sampling::Smooth);
            p.restore();
        } else {
            p.fillRoundRect(r, radius, color(C::Accent));
            screens::paintInitial(p, *this, r, radius, 0, letter, layout, 17);
        }
        if (active || hover) {
            const float in = 0.75f;
            p.strokeRoundRect(
                {r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in},
                radius - in,
                active ? 2 : 1.5f,
                active ? 0xc8ffffffU : 0x64ffffffU
            );
        }
        if (dot) {
            const PointF c{r.x + r.w - 3, r.y + r.h - 3};
            p.fillCircle(c, 5 + 1.25f, color(C::Rail));
            p.fillCircle(c, 5 - 1.25f, color(dot == 2 ? C::Badge : C::BadgeActivity));
        }
    }

    std::string                        key;
    std::function<bool(Event &)>       filter; // the rail's drag handling, first
    std::function<void(PointF)>        onMenu; // window position
    std::shared_ptr<const gfx::Bitmap> bitmap;
    std::shared_ptr<gfx::Bitmap>       scaled;
    int                                dot    = 0;
    bool                               active = false, lifted = false;
    std::string                        letter;
    std::unique_ptr<text::Layout>      layout;
};

// The workspace switcher: the workspaces' bubbles kGap apart, the white
// bar left of the active one. A press that moves picks a bubble up (a
// little larger, with a shadow); the others glide out of its way, and the
// drop reports the new order.
class WorkspaceStack final : public View {
public:
    static constexpr float kGap        = 8;
    static constexpr float kStep       = RailTile::kSize + kGap;
    static constexpr float kDragStart  = 10;    // px of travel before a press becomes a drag
    static constexpr float kLiftScale  = 1.06f; // the picked-up bubble's scale
    static constexpr float kAnimFactor = 0.35f; // per 16 ms: ~95% of the way in ~130 ms

    std::function<void(const std::string &key)>               onClick;
    std::function<void(const std::string &key, PointF)>       onMenu;
    std::function<void(const std::vector<std::string> &keys)> onReorder;

    WorkspaceStack() { style().width(kRailW).noShrink(); }

    size_t    count() const { return _entries.size(); }
    RailTile *tile(size_t i) const { return _entries[i].tile; }
    RailTile *find(const std::string &key) const {
        for (const Entry &e : _entries)
            if (e.tile->key == key)
                return e.tile;
        return nullptr;
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        for (const Entry &e : _entries)
            out.push_back(e.tile->key);
        return out;
    }
    bool dragging() const { return _dragging; }

    // The workspaces in order: tiles are kept per key (with their place, so
    // one that moved glides to its new slot), new ones made, gone ones dropped.
    void setKeys(const std::vector<std::string> &order) {
        if (_dragging) // a rebuild mid-drag: the drag is abandoned
            cancelDrag();
        _press = -1;
        std::vector<Entry> next;
        for (const std::string &k : order) {
            auto it = std::find_if(_entries.begin(), _entries.end(), [&](const Entry &e) {
                return e.tile->key == k;
            });
            if (it != _entries.end()) {
                next.push_back(*it);
                _entries.erase(it);
                continue;
            }
            auto *t    = add<RailTile>();
            t->key     = k;
            t->filter  = [this, t](Event &e) { return tileEvent(*t, e); };
            t->onClick = [this, t] {
                if (onClick) {
                    const std::string key = t->key; // a handler may rebuild the rail
                    onClick(key);
                }
            };
            t->onMenu = [this, t](PointF at) {
                if (onMenu) {
                    const std::string key = t->key;
                    onMenu(key, at);
                }
            };
            next.push_back({t, slotY(next.size())});
        }
        for (const Entry &gone : _entries)
            remove(gone.tile);
        _entries = std::move(next);
        invalidateLayout();
        startTicking();
    }

    SizeF measureContent(float, float) override {
        const float n = float(_entries.size());
        return {kRailW, n > 0 ? n * kStep - kGap : 0};
    }
    void layout() override {
        const float x = (kRailW - RailTile::kSize) / 2;
        for (const Entry &e : _entries)
            e.tile->setFrame({x, e.y, RailTile::kSize, RailTile::kSize});
    }
    void paint(gfx::Painter &p) override {
        for (const Entry &e : _entries)
            if (e.tile->active && !e.tile->lifted) {
                const float barH = 28;
                p.fillRoundRect(
                    {0, e.y + (RailTile::kSize - barH) / 2, 3, barH}, 1.5f, 0xffffffffU
                );
            }
    }
    void paintOver(gfx::Painter &p) override {
        if (!_dragging)
            return;
        // The lifted bubble: a soft shadow and a little larger, above the rest.
        const float x    = (kRailW - RailTile::kSize) / 2;
        const float grow = (kLiftScale - 1) * RailTile::kSize / 2;
        const RectF r{
            x - grow, _dragY - grow, RailTile::kSize + 2 * grow, RailTile::kSize + 2 * grow
        };
        p.fillRoundRect(
            {r.x - 1, r.y + 3 - 1, r.w + 2, r.h + 2}, RailTile::kRadius + 2, 0x5a000000U
        );
        _entries[size_t(_drag)].tile->paintBubble(p, r, false);
    }
    bool tick(double nowMs) override {
        const double dt    = _lastTick > 0 ? std::min(nowMs - _lastTick, 100.0) : 16.0;
        _lastTick          = nowMs;
        const float f      = 1 - std::pow(1 - kAnimFactor, float(dt / 16.0));
        bool        moving = false;
        for (size_t i = 0; i < _entries.size(); ++i) {
            if (_dragging && int(i) == _drag)
                continue;
            float      &y = _entries[i].y;
            const float d = slotY(i) - y;
            if (std::abs(d) < 0.5f) {
                y = slotY(i);
            } else {
                y += d * f;
                moving = true;
            }
        }
        layout();
        update();
        if (!moving)
            _lastTick = 0;
        return moving;
    }

private:
    struct Entry {
        RailTile *tile;
        float     y; // the bubble's top, animated toward its slot
    };
    static float slotY(size_t i) { return float(i) * kStep; }
    int          indexOf(const RailTile &t) const {
        for (size_t i = 0; i < _entries.size(); ++i)
            if (_entries[i].tile == &t)
                return int(i);
        return -1;
    }

    // The tile's pointer events, before its own click handling.
    bool tileEvent(RailTile &t, Event &e) {
        switch (e.type) {
        case EventType::PointerDown:
            if (e.button == plat::Button::Left) {
                _press    = indexOf(t);
                _pressPos = e.windowPos;
            }
            return false;
        case EventType::PointerMove:
            if (_dragging) {
                moveDrag(e.windowPos);
                return true;
            }
            if (_press >= 0 && t.pressed() && _entries.size() > 1 &&
                std::abs(e.windowPos.x - _pressPos.x) + std::abs(e.windowPos.y - _pressPos.y) >=
                    kDragStart) {
                beginDrag(e.windowPos);
                return true;
            }
            return false;
        case EventType::PointerUp:
            _press = -1;
            if (_dragging) {
                endDrag();
                t.release();
                return true;
            }
            return false;
        case EventType::PointerCancel:
            _press = -1;
            if (_dragging)
                cancelDrag();
            return false;
        default:
            return false;
        }
    }
    void beginDrag(PointF windowPos) {
        _dragging   = true;
        _drag       = _press;
        _grab       = mapFromWindow(_pressPos).y - _entries[size_t(_drag)].y;
        _dragY      = _entries[size_t(_drag)].y;
        _startOrder = keys();
        RailTile *t = _entries[size_t(_drag)].tile;
        t->setLifted(true);
        t->setCursor(plat::Cursor::Grabbing);
        moveDrag(windowPos);
    }
    void moveDrag(PointF windowPos) {
        const size_t n = _entries.size();
        _dragY         = std::clamp(mapFromWindow(windowPos).y - _grab, slotY(0), slotY(n - 1));
        const int idx  = std::clamp(int(std::lround(_dragY / kStep)), 0, int(n) - 1);
        if (idx != _drag) {
            Entry e = _entries[size_t(_drag)];
            _entries.erase(_entries.begin() + _drag);
            _entries.insert(_entries.begin() + idx, e);
            _drag = idx;
            startTicking(); // the displaced bubbles glide to their new slots
        }
        _entries[size_t(_drag)].y = _dragY;
        layout();
        update();
    }
    void endDrag() {
        RailTile *t = _entries[size_t(_drag)].tile;
        _dragging   = false;
        _drag       = -1;
        t->setLifted(false);
        t->setCursor(plat::Cursor::Hand);
        startTicking(); // it settles from where it was dropped
        update();
        if (onReorder) {
            const std::vector<std::string> now = keys();
            if (now != _startOrder)
                onReorder(now);
        }
    }
    void cancelDrag() {
        RailTile *t = _entries[size_t(_drag)].tile;
        _dragging   = false;
        _drag       = -1;
        t->setLifted(false);
        t->release();
        t->setCursor(plat::Cursor::Hand);
        startTicking();
        update();
    }

    std::vector<Entry>       _entries;
    int                      _press = -1, _drag = -1;
    PointF                   _pressPos;
    bool                     _dragging = false;
    float                    _dragY = 0, _grab = 0;
    std::vector<std::string> _startOrder;
    double                   _lastTick = 0;
};

// The rail: the nav gradient behind the workspaces, "+" and the gear.
class Rail final : public View {
public:
    void paint(gfx::Painter &p) override { paintNavGradient(*this, p, C::Rail); }
};

// The list's resize handle: 4 px right of the list, in the list's gradient at
// rest and the link blue under the pointer; a drag resizes the list.
class ListHandle final : public View {
public:
    ListHandle(View &list) : _list(list) {
        style().width(4).noShrink();
        setCursor(plat::Cursor::ResizeH);
        setHoverRepaint(true);
    }
    void paint(gfx::Painter &p) override {
        if (hovered())
            p.fillRect(bounds(), color(C::Link));
        else
            paintNavGradient(*this, p, C::Sidebar);
    }
    bool onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerDown:
            _startX = e.windowPos.x;
            _startW = _list.width();
            return true;
        case EventType::PointerMove:
            if (window() && window()->capture() == this)
                _list.style().width(
                    std::clamp(_startW + e.windowPos.x - _startX, kListMinW, kListMaxW)
                );
            return true;
        default:
            return false;
        }
    }

private:
    View &_list;
    float _startX = 0, _startW = 0;
};

// The windowed frame: the right panel keeps 4 px off the window's right and
// bottom edges unless maximized or full screen, and the rail colour shows
// there (a nav-coloured margin).
class InsetPane final : public View {
public:
    void paint(gfx::Painter &p) override {
        const auto &pad = currentStyle().pad;
        if (pad.r > 0 || pad.b > 0)
            p.fillRect(bounds(), color(C::Rail));
        p.fillRect({0, 0, width() - pad.r, height() - pad.b}, color(C::Surface));
    }
    void layout() override {
        const bool edge =
            window() && (window()->native().isMaximized() || window()->native().isFullscreen());
        const float want = edge ? 0 : 4;
        if (currentStyle().pad.r != want || currentStyle().pad.b != want) {
            style().pad.r = want;
            style().pad.b = want;
        }
        View::layout();
    }
};

// The splitter handle between the messages and the thread panel: 1 px, with the panel's soft shadow
// cast 6 px onto the chat.
class Splitter final : public View {
public:
    explicit Splitter(std::function<void(float dx)> onDrag) : _onDrag(std::move(onDrag)) {
        style().width(1).noShrink();
        setCursor(plat::Cursor::ResizeH);
        setPaintOutset(6);
    }
    void paint(gfx::Painter &p) override {
        p.fillRect(bounds(), color(C::Surface));
        p.fillRectGradient({-5, 0, 6, height()}, {-5, 0}, 0, {1, 0}, 0x1c000000U);
    }
    bool onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerDown:
            _last = e.windowPos.x;
            return true;
        case EventType::PointerMove:
            if (window() && window()->capture() == this) {
                _onDrag(e.windowPos.x - _last);
                _last = e.windowPos.x;
            }
            return true;
        default:
            return false;
        }
    }

private:
    std::function<void(float)> _onDrag;
    float                      _last = 0;
};

// The right-hand thread panel: the messages screens' ThreadPanel with our
// composer in its slot — or, without them, a stand-in with the same parts.
class ThreadArea final : public View {
public:
    ThreadArea(screens::Context &ctx, Avatars &avatars, DraftStash &drafts) : _ctx(ctx) {
        setBackground(C::Surface);
#ifdef MSGA_HAVE_MESSAGES
        (void)avatars;
        panel = add<screens::ThreadPanel>(ctx);
        panel->style().flex(1);
        typing = static_cast<TypingIndicator *>(
            panel->setTyping(std::make_unique<TypingIndicator>(ctx))
        );
        composer =
            static_cast<Composer *>(panel->setComposer(std::make_unique<Composer>(ctx, drafts)));
        composer->setThreadMode(true);
        // "Also send to channel": one reply's worth; an edit or attachments
        // untick it until they are gone.
        composer->broadcastWanted      = [this] { return panel->broadcastWanted(); };
        composer->onSent               = [this] { panel->setBroadcastWanted(false); };
        composer->onCompositionChanged = [this] {
            panel->setBroadcastBlocked(composer->editing() || !composer->attachments().empty());
        };
#else
        auto *head = add<View>();
        head->style().row().height(56).padding(20, 0, 12, 0).spacing(8).items(Align::Center);
        head->add<Label>(tr("Thread"), Font::Title);
        where = head->add<Label>("", Font::Small, C::TextMuted);
        where->setMaxLines(1);
        where->style().flex(1);
        auto *close    = head->add<IconButton>(Icon::X, tr("Close thread"));
        close->onClick = [this] {
            if (_ctx.closeThread)
                _ctx.closeThread();
        };
        add<Separator>();
        list = add<MessageStandIn>(ctx, avatars);
        list->style().flex(1);
        composer = add<Composer>(ctx, drafts);
#endif
    }
    void show(ConvRef conv, Ts root) {
        this->conv = conv;
        this->root = root;
#ifdef MSGA_HAVE_MESSAGES
        panel->show(conv, root);
        typing->setTarget(conv, root);
#else
        where->setText(convTitle(_ctx.store(), conv));
        list->setTarget(conv, root);
#endif
        composer->setTarget(conv, root);
    }
    // Showing nothing: the workspace it showed is going away.
    void clear() {
        conv = kNoConv;
        root = 0;
#ifdef MSGA_HAVE_MESSAGES
        panel->list().clear();
        typing->setTarget(kNoConv, 0);
#else
        list->setTarget(kNoConv, 0);
#endif
        composer->setTarget(kNoConv, 0);
    }
    screens::Context &_ctx;
#ifdef MSGA_HAVE_MESSAGES
    screens::ThreadPanel *panel  = nullptr;
    TypingIndicator      *typing = nullptr;
#endif
    Label          *where    = nullptr;
    MessageStandIn *list     = nullptr;
    Composer       *composer = nullptr;
    ConvRef         conv     = kNoConv;
    Ts              root     = 0;
};

// The welcome screen: "Keyboard shortcuts", generated from the table
// (shortcuts.h inHelp rows, in table order), each row an action name and its
// keys as chips. Rows that do not fit drop from the bottom (the table lists
// them in descending usefulness) instead of being clipped.
class ShortcutsPanel final : public View {
public:
    ShortcutsPanel() {
        setBackground(C::Surface);
        style().dir = Dir::None;
        _content    = add<View>();
        _content->style().column();
        auto *title = _content->add<Label>(tr("Keyboard shortcuts"), Font::Title, C::TextMuted);
        title->style().margins(0, 0, 0, 16);
        auto *rule = _content->add<Separator>();
        rule->style().margins(0, 0, 0, 12);
        rebuild();
    }
    void rebuild() {
        while (_content->childCount() > 2)
            _content->remove(_content->child(2));
        _rows.clear();
        for (size_t i = 0; i < shortcuts::tableSize(); ++i) {
            const shortcuts::Def &d = shortcuts::table()[i];
            if (!d.inHelp)
                continue;
            auto *row = _content->add<View>();
            row->style().row().height(36).spacing(8).items(Align::Center);
            row->add<Label>(tr(d.label), Font::Body)->style().flex(1);
            bool first = true;
            for (const std::string &k : shortcuts::keyChips(d.id)) {
                if (!first)
                    row->add<Label>("+", Font::Small, C::Border);
                first      = false;
                auto *chip = row->add<View>();
                chip->setBackground(C::Hover, 5);
                chip->setBorder(C::Border);
                chip->style().padding(9, 3);
                chip->add<Label>(k, Font::Caption);
            }
            _rows.push_back(row);
        }
        invalidateLayout();
    }
    void layout() override {
        constexpr float kMargin = 48;
        const float     w       = std::clamp(width() - 2 * kMargin, 300.f, 420.f);
        const float     room    = std::max(0.f, height() - 2 * kMargin);
        for (View *r : _rows)
            r->setVisible(true);
        float       h    = _content->measure(w, 1e6f).h;
        // A dropped row takes its height and the gap above it along: one
        // measure for all of them, and one of what is left.
        const float gap  = _content->currentStyle().gap;
        size_t      keep = _rows.size();
        while (keep > 0 && h > room)
            h -= _rows[--keep]->measure(w, 1e6f).h + gap;
        if (keep < _rows.size()) {
            for (size_t i = keep; i < _rows.size(); ++i)
                _rows[i]->setVisible(false);
            h = _content->measure(w, 1e6f).h;
        }
        const float y = std::max(kMargin, std::round((height() - h) / 2));
        _content->setFrame({std::round((width() - w) / 2), y, w, h});
        _content->layout();
    }

private:
    View               *_content = nullptr;
    std::vector<View *> _rows;
};

// The logged-out page: no workspace signed in — the
// logo, "MSGA", the tagline with its initials picked out, and "Log in to
// workspace" in a 300 px column centred on the content surface.
class SignedOutPage final : public View {
public:
    SignedOutPage() {
        setBackground(C::Surface);
        style().column().padding(32).items(Align::Center).justifyContent(Justify::Center);
        auto *col = add<View>();
        col->style().column().width(300).items(Align::Center);
        col->add<IconView>(Icon::Logo, 72, C::Text);
        auto *title = col->add<Label>("MSGA");
        title->style().margins(0, 16 + 4, 0, 0);
        _title   = title;
        _tagline = col->add<Label>("[make slack great again]");
        _tagline->style().margins(0, 8, 0, 0);
        login = col->add<Button>(
            tr("Log in to workspace"), Button::Kind::Primary, Button::Form::Normal
        );
        login->style().margins(0, 16 + 12, 0, 0);
        styleChanged();
    }
    // Theme and text-size changes restyle the rich labels (sentinel colours
    // follow the theme by themselves; the sizes follow the text scale).
    void styleChanged() override {
        View::styleChanged();
        setStyledText(_title, "MSGA", pxFont(24, text::Weight::Semibold, themed(C::Text)));
        const text::Style    dim = pxFont(11, text::Weight::Regular, themed(C::TextFaint));
        const text::Style    hi  = pxFont(11, text::Weight::Regular, themed(C::Text));
        text::AttributedText g;
        g.append("[", dim);
        for (const char *w : {"make ", "slack ", "great ", "again"}) {
            g.append(std::string_view(w, 1), hi);
            g.append(w + 1, dim);
        }
        g.append("]", dim);
        _tagline->setRichText(std::move(g));
    }
    Button *login = nullptr;

private:
    Label *_title = nullptr, *_tagline = nullptr;
};

// ── Shell ───────────────────────────────────────────────────────────────────

Shell::Shell(screens::Context &ctx, Window &win, Settings &settings, std::string settingsPath)
    : _ctx(ctx), _win(win), _settings(settings), _settingsPath(std::move(settingsPath)),
      _avatars(ctx.images) {
    // Decoded avatars fill the bitmaps the views already hold: repaint.
    _avatars.setRemote(ctx.remote);
    // Only the views showing a picture that landed (every view drawing an
    // Avatars bitmap answers View::showsBitmap).
    _avatars.onLoaded = [this](const std::vector<Avatars::Picture> &landed) {
        _win.damageShowing(landed);
    };
    shortcuts::setCtrlEnterSends(settings.ctrlEnterSends);
    setMarkdownFormatting(settings.markdownFormatting);
    _drafts.setStoreOf([this](const std::string &key) { return storeFor(key); });
    ctx.openConversation = [this](ConvRef c) { open(c); };
    ctx.openThread       = [this](ConvRef c, Ts root) { openThread(c, root); };
    ctx.closeThread      = [this] { closeThread(); };
    ctx.openProfile      = [this](model::UserRef u) { showProfile(u); };
    ctx.openUrl          = [this](const std::string &url) { _ctx.app.platform().openUrl(url); };
    // A channel we never joined is joined first.
    ctx.openMessage      = [this](ConvRef c, Ts ts, Ts thread) {
        const model::Conversation *cv =
            c < _ctx.store().conversationCount() ? &_ctx.store().conversation(c) : nullptr;
        if (!cv || cv->member || cv->isDirect()) {
            jumpToMessage(c, ts, thread);
            return;
        }
        std::weak_ptr<int> alive = _agentAlive;
        _ctx.backend.joinChannel(
            c, [this, alive, ts, thread](ConvRef joined, const std::string &err) {
                if (alive.expired())
                    return;
                if (joined == kNoConv)
                    showError(err);
                else
                    jumpToMessage(joined, ts, thread);
            }
        );
    };
    ctx.window      = &win; // file dialogs' parent
    ctx.attachFiles = [this](std::vector<std::string> paths, bool thread) {
        if (Composer *c = thread ? threadComposer() : _composer)
            c->addAttachments(paths);
    };

    auto *screen = win.root().add<View>();
    screen->style().column();
    buildTitleBar(screen);
#ifdef MSGA_SELF_UPDATE
    _updateBar            = screen->add<UpdateBar>();
    _updateBar->onRestart = [this] { applyUpdate(); };
#endif
    auto *body = screen->add<View>();
    body->style().row().flex(1);
    buildRail(body);
    _sidebar = body->add<Sidebar>(ctx, _avatars);
    _sidebar->style().width(kListW).noShrink();
    // The visit stamps come back from the settings and go there again 1.5 s
    // after the last change (coalesced).
    _sidebar->setVisited({_settings.visitedAt.begin(), _settings.visitedAt.end()});
    _sidebar->onVisitedChanged = [this] {
        _ctx.app.cancelTimer(_visitedTimer);
        _visitedTimer = _ctx.app.addTimer(1500, false, [this] {
            _visitedTimer = 0;
            storeVisited();
            saveSettingsSoon();
        });
    };
    loadSorts();
    _sidebar->onSortsChanged = [this] {
        storeSorts();
        saveSettingsSoon();
    };
    _listHandle = body->add<ListHandle>(*_sidebar);
    buildMain(body);
    // With no workspace signed in (setSignedIn(false)) this takes the
    // sidebar's and the main pane's place.
    _signedOut = body->add<InsetPane>();
    _signedOut->style().flex(1);
    auto *page = _signedOut->add<SignedOutPage>();
    page->style().flex(1);
    page->login->onClick = [this, b = page->login] {
        const RectF r = b->windowRect(); // the button's bottom-left
        addWorkspace({r.x, r.y + r.h});
    };
    _signedOut->setVisible(false);

    // Context menus (context_menus.h). One workspace for now: nothing to
    // reorder, and signing out needs sign-in (Menus::hooks stay unset).
    _menus           = std::make_unique<Menus>(*this, ctx, win);
    _profiles        = std::make_unique<ProfileCards>(ctx, win, _avatars);
    ctx.profileHover = [this](model::UserRef u, RectF r, int mode) {
        _profiles->hover(u, r, mode);
    };
    _sidebar->setMenus(_menus.get());
    _sidebar->footer().onManageStatus = [this] {
        StatusDialog::show(_win, _ctx)->onError = [this](const std::string &message) {
            showError(message);
        };
    };
    _sidebar->footer().onManageProfile = [this] { ProfileDialog::show(_win, _ctx, _avatars); };
    _sidebar->footer().onError         = [this](const std::string &message) { showError(message); };
    _sidebar->onFindChannel            = [this] { openBrowseDialog(0); };
    _sidebar->onBrowsePeople           = [this] { openBrowseDialog(1); };
    _sidebar->onCreateChannel          = [this] { openCreateChannel(); };
    ctx.forwardMessage                 = [this](ConvRef c, Ts ts, const std::string &file) {
        // With two or more workspaces signed in, a picker chooses
        // where it goes (starting on this one).
        std::vector<ForwardWorkspace> targets;
        for (const LiveWorkspace &w : liveWorkspaces())
            targets.push_back(
                {w.key, w.name, w.store, w.backend, [this, key = w.key, st = w.store] {
                     const Running *r = findRunning(key);
                     return r && r->store == st;
                 }}
            );
        _forward = showForwardDialog(
            _ctx,
            _win,
            c,
            ts,
            file,
            [this](const std::string &message) { showError(message); },
            std::move(targets),
            [this](
                const std::string       &key,
                const std::string       &role,
                std::string              text,
                std::vector<std::string> paths
            ) {
                // Its workspace first (as a notification's click does).
                if (!key.empty()) {
                    if (!(key == _activeKey && _signedIn) && onSwitchWorkspace)
                        onSwitchWorkspace(key);
                    const Running *r = findRunning(key);
                    if (!r || r->store != &_ctx.store() || !_signedIn)
                        return; // logged out meanwhile
                }
                prefillTeammate(role, std::move(text), std::move(paths));
            }
        );
        if (_forward)
            _forward->onClosed = [this, f = _forward] {
                if (_forward == f)
                    _forward = nullptr;
            };
    };
    ctx.openAiSettings = [this] { openAiSettings(); };
    // What the screens report through the backend they hold (main's proxy:
    // a failed move, a preview removal) reaches the banner too.
    if (!ctx.backend.onError) {
        ctx.backend.onError = [this](const std::string &message) { showError(message); };
        _ownsBackendError   = true;
    }
    // A canvas file in a message: the canvas viewer over the window.
    ctx.openCanvas = [this](ConvRef c, const model::File &f) {
        showCanvasViewer(_ctx, _win, c, f, [this](const std::string &message) {
            showError(message);
        });
    };
    ctx.messageUser = [this](model::UserRef u) {
        // A teammate has no DM of its own; its page is
        // where writing to it starts a session.
        if (_ctx.backend.capabilities().agentSessions)
            for (const model::Backend::AgentRole &r : _ctx.backend.agentRoles())
                if (r.user != model::kNoUser && r.user == u) {
                    openTeammate(r.id);
                    return;
                }
        std::weak_ptr<int> alive = _agentAlive;
        _ctx.backend.openDm(u, [this, alive](ConvRef c) {
            if (!alive.expired() && c != kNoConv)
                open(c);
        });
    };
    _menus->hooks.changeWorkspaceIcon = [this](const std::string &key) {
        const auto done = [this](const std::string &) { refreshWorkspaceIcon(); };
        for (const Workspace &w : _workspaces)
            if (w.key == key && !(key == _activeKey && _signedIn)) {
                showWorkspaceIconDialog(_ctx, _win, _avatars, w.id, w.icon, done);
                return;
            }
        showWorkspaceIconDialog(_ctx, _win, _avatars, done); // the open workspace's
    };
#ifdef MSGA_HAVE_MESSAGES
    // "Edit message": the composer under the list edits in place.
    static_cast<screens::MessageList *>(_messages)->onEdit = [this](ConvRef c, Ts ts) {
        if (c == _current)
            _composer->beginEdit(ts);
    };
    _thread->panel->list().onEdit = [this](ConvRef, Ts ts) { _thread->composer->beginEdit(ts); };
#endif
    wireAgentUi();

    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        onChange(ch);
    });

    // Keyboard: the window-scope shortcuts (shortcuts.h), and the
    // app-wide Shift+Del filter.
    using shortcuts::Id;
    shortcuts::install(win, Id::OpenSettings, [this] { openSettings(); });
    shortcuts::install(win, Id::NavBack, [this] { navigateHistory(true); });
    shortcuts::install(win, Id::NavForward, [this] { navigateHistory(false); });
    shortcuts::install(win, Id::CloseFrontmost, [this] { closeFrontmost(); });
    shortcuts::install(win, Id::SearchMessages, [this] { openSearch(); });
    shortcuts::install(win, Id::QuickSwitch, [this] { showQuickSwitcher(); });
    win.keyFilter = [this](const Event &e) {
        noteActivity();
        return removeIdleSession(e);
    };
    // The mouse's side buttons and trackpad swipes walk the same history.
    win.inputFilter = [this](const plat::Event &e) {
        if (e.type == plat::EventType::PointerDown || e.type == plat::EventType::Scroll)
            noteActivity();
        return navInput(e);
    };

    win.native().setHitTest([this](plat::Point p) { return hitTest(p); });
#ifdef MSGA_HAVE_MESSAGES
    // The emoji picker's "Frequently used" and skin tone.
    screens::EmojiPicker::restoreState(_settings.emojiRecent, _settings.emojiSkinTone);
    screens::EmojiPicker::setStateObserver(
        [this](const std::vector<std::string> &recent, int tone) {
            _settings.emojiRecent   = recent;
            _settings.emojiSkinTone = tone;
            saveSettingsSoon();
        }
    );
#endif
    applySettings(); // creates the tray icon too
    // The saved size was measured on whatever monitor was attached last time.
    fitToScreen();
}

// Coalesced: an emoji pick, a toggle or a dialog's changes are one write a
// moment later, made on a worker (Settings::saveInBackground).
void Shell::saveSettingsSoon() {
    if (_saveTimer)
        return;
    _saveTimer = _ctx.app.addTimer(kSettingsSaveDelayMs, false, [this] {
        _saveTimer = 0;
        _settings.saveInBackground(_ctx.app.platform(), _settingsPath);
    });
}

void Shell::saveSettingsAsync() {
    if (_saveTimer)
        _ctx.app.cancelTimer(std::exchange(_saveTimer, 0));
    _settings.saveInBackground(_ctx.app.platform(), _settingsPath);
}

void Shell::saveSettingsNow() {
    if (_saveTimer)
        _ctx.app.cancelTimer(std::exchange(_saveTimer, 0));
    if (!_settings.save(_settingsPath) && !_settingsPath.empty())
        LOG_WARN("shell", "could not save %s", _settingsPath.c_str());
}

// ── Claude Code UI ──────────────────────────────────────────────────────────

void Shell::wireAgentUi() {
    // ↑ in a Claude Code session: its prompt history, as in Claude Code
    // itself (the thread composer has none).
    // On a teammate's page, before any session: the prompts of the folder
    // the new session starts in. (A command typed there is the new
    // session's first prompt: the page's onSendRequest takes the send.)
    _composer->historySource = [this] {
        if (teammateOpen())
            return _ctx.backend.folderPromptHistory(teammatePage()->folder());
        return _current == kNoConv ? std::vector<std::string>()
                                   : _ctx.backend.promptHistory(_current);
    };
    _composer->onCommand = [this](const std::string &name, const std::string &args) {
        runCommand(_current, 0, name, args);
    };
    // Zen mode: remembered per workspace in Settings; the open chat reloads
    // to match.
    SidebarFooter &footer = _sidebar->footer();
    footer.setZenOn(_settings.zenMode(_activeKey));
    footer.onZenToggled = [this](bool on) {
        _settings.setZenMode(_activeKey, on);
        saveSettingsSoon();
        _ctx.backend.setZenMode(on);
#ifdef MSGA_HAVE_MESSAGES
        if (_current != kNoConv)
            static_cast<screens::MessageList *>(_messages)->showConversation(_current);
#endif
    };
#ifdef MSGA_HAVE_MESSAGES
    // A command typed in a thread runs as in the channel, in the thread:
    // what it posts is a reply there.
    _thread->composer->onCommand = [this](const std::string &name, const std::string &args) {
        runCommand(_thread->composer->conv(), _thread->composer->thread(), name, args);
    };
    _thread->composer->setPopupArea(_thread->panel); // its popups stay inside the panel
    // "Open as session": a branched agent thread moves to the list.
    _thread->panel->onOpenAsSession = [this](ConvRef conv, Ts root) {
        const ConvRef session = _ctx.backend.openThreadAsSession(conv, root);
        if (session == kNoConv)
            return;
        closeThread();
        open(session);
    };
#endif
}

// A conversation nobody can post to from here
// (Conversation::readOnly, e.g. a session a terminal drives) locks the
// composer and says why; re-run on every change, since it flips while the
// chat is open. The suggested reply is handed on only when it changes: a
// send drops it, and the service may still report it until the next turn.
void Shell::applyComposerAccess() {
    if (_current == kNoConv || _current >= _ctx.store().conversationCount())
        return;
    const std::string suggestion = _ctx.backend.promptSuggestion(_current);
    if (suggestion != _composerSuggestion) {
        _composerSuggestion = suggestion;
        _composer->setSuggestion(suggestion);
    }
    // Unlocked, the placeholder names the chat: a rename goes in too.
    const std::string &reason = _ctx.store().conversation(_current).readOnly;
    if (reason == _composerLock && !reason.empty())
        return;
    _composerLock = reason;
    _composer->setLockReason(reason);
}

// A command msga runs itself (an agent session's
// /status, /clear) — nothing is sent.
void Shell::runCommand(ConvRef conv, Ts thread, const std::string &name, const std::string &args) {
    if (conv == kNoConv)
        return;
    const auto r = _ctx.backend.runLocalCommand(conv, thread, name, args);
    if (!r.error.empty())
        showError(r.error);
    if (!r.status.empty())
        SessionStatusDialog::show(_win, r.status);
    if (r.open != kNoConv)
        open(r.open);
}

Shell::~Shell() {
    _ctx.app.cancelTimer(_errorTimer);
    for (plat::TimerId id : _resolveTimers)
        _ctx.app.cancelTimer(id);
    _ctx.app.cancelTimer(_attentionTimer);
    for (const auto &r : _running)
        r->store->unobserve(r->observer);
    _ctx.app.cancelTimer(_visitedTimer);
    if (_saveTimer) // a change still waiting is written now
        saveSettingsNow();
#ifdef MSGA_SELF_UPDATE
    // The updater outlives us (main owns it): no event or check may reach
    // a dead shell.
    _ctx.app.cancelTimer(_updateTimer);
    if (_updater) {
        _updater->unlisten(_updateListener);
        _updater->onChecked = nullptr;
    }
#endif
    // The views die with us, not later with the window: the composers stash
    // their drafts into the store from their destructors, so nothing of ours
    // may answer a store change once the tree below is cleared.
    _ctx.store.unobserve(_observer);
    _win.closeAllPopups();
    _win.keyFilter = nullptr;
    _win.root().clearChildren();
    _ctx.window         = nullptr;
    _ctx.attachFiles    = nullptr;
    _ctx.openAiSettings = nullptr;
#ifdef MSGA_HAVE_MESSAGES
    screens::EmojiPicker::setStateObserver(nullptr);
#endif
    _ctx.messageUser = nullptr;
    if (_ownsBackendError)
        _ctx.backend.onError = nullptr;
}

// The title strip. It continues the rail's window-wide nav gradient, so the
// two meet without a seam. On macOS it is the unified header instead: the
// content surface over a subtle rule, as tall as AppKit's title bar, with
// the conversation header (or the workspace's name) in it and the native
// traffic lights over its left end; its context menu holds "Pin
// window on top" (no pin button beside the traffic lights).
class TitleStrip final : public View {
public:
    void paint(gfx::Painter &p) override {
#ifdef __APPLE__
        p.fillRect(bounds(), color(C::Surface));
        p.fillRect({0, height() - 1, width(), 1}, color(C::DividerSubtle));
#else
        // A custom theme's pinned top_nav_bg is a flat title bar colour.
        if (palette(app() && app()->dark()) == Palette::Custom && customPalette().titleBarBg)
            p.fillRect(bounds(), color(C::TitleBar));
        else
            paintNavGradient(*this, p, C::Rail);
#endif
    }
    bool onEvent(Event &e) override {
        if (e.type != EventType::ContextMenu || !onContextMenu)
            return View::onEvent(e);
        onContextMenu(e.windowPos);
        return true;
    }
    void styleChanged() override {
        View::styleChanged();
        if (onStyleChanged)
            onStyleChanged();
    }
    std::function<void(PointF)> onContextMenu;
    std::function<void()>       onStyleChanged; // a theme or palette switch
};

void Shell::buildTitleBar(View *parent) {
    auto *strip = parent->add<TitleStrip>();
    _titleBar   = strip;
#ifdef __APPLE__
    // As tall as the title bar AppKit lays the traffic lights out in (the
    // compact unified toolbar's: 38 pt before macOS 26, 40 on it), so
    // they sit centred in the header whatever the OS version makes of it.
    const double native = _win.native().titleBarHeight();
    _titleBarH          = native > 0 ? float(std::round(native)) : kMacTitleBarH;
    _titleBar->style().stack().height(_titleBarH).noShrink();
    // The fallback title while no conversation header shows: the
    // workspace, centred in the window, clear of the traffic lights.
    _titleLabel = _titleBar->add<Label>(tr("msga"), Font::UnifiedTitle);
    _titleLabel->setMaxLines(1);
    _titleLabel->setAlign(text::LayoutOptions::Align::Center);
    _titleLabel->setHitTransparent(true);
    _titleLabel->style().margins(112, 0, 112, 1);
#else
    _titleBarH = kTitleBarH;
    _titleBar->style().row().height(kTitleBarH).noShrink().items(Align::Center);
    _titleBar->add<View>()->style().flex(1);
#endif
    strip->onStyleChanged = [this] { syncChrome(); };
    syncChrome();
#ifndef __APPLE__ // macOS keeps its traffic lights
    auto chrome = [&](Icon icon, const char *tip, bool danger) {
        auto *b = _titleBar->add<IconButton>(icon, tip);
        b->style().size(40, kTitleBarH);
        b->setIconSize(12);
        b->setTextColor(C::TitleBarControl);
        b->setLook(
            {C::None,
             danger ? C::Danger : C::SidebarHover,
             danger ? C::Danger : C::SidebarHover,
             C::None,
             0}
        );
        b->setFocusable(false);
        return b;
    };
    // The pin: not on Wayland, where a client can't keep itself on top.
    if (_win.native().supportsAlwaysOnTop()) {
        _pinBtn          = chrome(Icon::PinOff, tr("Pin window on top"), false);
        _pinBtn->onClick = [this] { togglePin(); };
    }
    _minBtn          = chrome(Icon::WcMinimize, tr("Minimize"), false);
    _maxBtn          = chrome(Icon::WcMaximize, tr("Maximize"), false);
    _closeBtn        = chrome(Icon::WcClose, tr("Close"), true);
    _minBtn->onClick = [this] { minimize(); };
    _maxBtn->onClick = [this] {
        const bool maxed = !_win.native().isMaximized();
        _win.native().setMaximized(maxed);
        _maxBtn->setIcon(maxed ? Icon::WcRestore : Icon::WcMaximize);
    };
    _closeBtn->onClick = [this] {
        if (_win.onCloseRequested)
            _win.onCloseRequested();
    };
#else
    strip->onContextMenu = [this](PointF at) {
        if (!_win.native().supportsAlwaysOnTop())
            return;
        MenuItem pin{1, tr("Pin window on top")};
        pin.checked = _win.native().isAlwaysOnTop();
        Menu::popupAt(_win, at, {pin}, [this](int) { togglePin(); });
    };
#endif
    // The window manager may change these by itself (a double-click on the
    // caption, its own "Always on top").
    _win.onEvent = [this](const plat::Event &e) {
        if (e.type == plat::EventType::StateChanged) {
            updateTitleButtons();
            syncWindowVisible(); // minimised or back
        } else if (e.type == plat::EventType::FocusIn || e.type == plat::EventType::FocusOut)
            updateReading();
        // Dragged onto another monitor: re-fit.
        if (e.type == plat::EventType::Moved) {
            const uint64_t on = _win.native().monitor();
            if (on && on != _fitMonitor)
                fitToScreen();
        }
    };
}

// The native chrome (macOS's title-bar rim and
// traffic lights) dark when the content surface is, whatever the system's
// own appearance — a dark theme on a light-mode Mac would otherwise get a
// bright rim along the top edge. Custom palettes included.
void Shell::syncChrome() {
    const Color c = color(C::Surface);
    const int   r = int((c >> 16) & 0xff), g = int((c >> 8) & 0xff), b = int(c & 0xff);
    const int   hsl = std::max({r, g, b}) + std::min({r, g, b}); // HSL lightness * 2
    _win.native().setDarkChrome(hsl < 255);
}

void Shell::togglePin() {
    plat::Window &w = _win.native();
    w.setAlwaysOnTop(!w.isAlwaysOnTop());
    updateTitleButtons();
}

// The pin red while pinned, its
// tooltip the action a click takes; the restore glyph while maximized.
void Shell::updateTitleButtons() {
    if (_maxBtn)
        _maxBtn->setIcon(_win.native().isMaximized() ? Icon::WcRestore : Icon::WcMaximize);
    if (_pinBtn) {
        const bool pinned = _win.native().isAlwaysOnTop();
        _pinBtn->setIcon(pinned ? Icon::Pin : Icon::PinOff);
        _pinBtn->setTextColor(pinned ? C::Danger : C::SidebarTextMuted);
        _pinBtn->setTooltip(pinned ? tr("Unpin window") : tr("Pin window on top"));
    }
}

// The workspace switcher: the bubble 16 px from the top, the ghost "+" 16
// under it, the gear 14 px above the bottom; tooltips to the right.
void Shell::buildRail(View *parent) {
    _rail = parent->add<Rail>();
    _rail->style().width(kRailW).noShrink().padding(0, 16, 0, 14).items(Align::Center);
    _wsStack          = _rail->add<WorkspaceStack>();
    _wsStack->onClick = [this](const std::string &key) {
        if (key != _activeKey && onSwitchWorkspace)
            onSwitchWorkspace(key);
    };
    _wsStack->onMenu    = [this](const std::string &key, PointF at) { showWorkspaceMenu(key, at); };
    _wsStack->onReorder = [this](const std::vector<std::string> &keys) {
        // The list follows the rail; the tray menu lists them in that order.
        std::vector<Workspace> sorted;
        for (const std::string &k : keys)
            for (const Workspace &w : _workspaces)
                if (w.key == k)
                    sorted.push_back(w);
        _workspaces = std::move(sorted);
        rebuildTrayMenu();
        if (onReorderWorkspaces)
            onReorderWorkspaces(keys);
    };
    auto *add    = _rail->add<GhostButton>(Icon::Plus, tr("Add workspace"), true);
    _addBtn      = add; // its top margin: setWorkspaces
    add->onClick = [this, add] {
        const RectF r = add->windowRect(); // the menu opens at its top-right
        addWorkspace({r.x + r.w, r.y});
    };
    _rail->add<View>()->style().flex(1);
    auto *prefs    = _rail->add<GhostButton>(Icon::Settings2, tr("Settings"));
    _prefsBtn      = prefs;
    prefs->onClick = [this] { openSettings(); };
}

// The right panel: the header and the tab strip span the pane; below
// them the messages (list, typing indicator, composer) and the thread panel
// sit on a 1 px splitter.
void Shell::buildMain(View *parent) {
    _mainPane = parent->add<InsetPane>();
    _mainPane->style().flex(1);
    _mainPane->style().minW = 360;

#ifdef __APPLE__
    // The header is the unified title bar's.
    _header = _titleBar->add<ConvHeader>(_ctx, _avatars);
#else
    _header = _mainPane->add<ConvHeader>(_ctx, _avatars);
#endif
    _header->onSearch     = [this] { openSearch(); };
    _tabs                 = _mainPane->add<ConvTabs>();
    _tabs->onSelect       = [this](int tab) { showCanvas(tab == 1); };
    // The huddle banner: under the tabs, over the messages.
    _huddleBanner         = _mainPane->add<HuddleBanner>();
    _huddleBanner->onJoin = [this] {
        if (_current != kNoConv && _ctx.openUrl)
            _ctx.openUrl(huddleJoinUrl(_ctx.store, _current));
    };
    // The error banner: under the tabs, across the pane, danger.icon on
    // surface.raised text, 6/12 padding, fonts.md, centred; hidden after 5 s.
    _errorBanner = _mainPane->add<Label>("", Font::Control, C::FormBg);
    _errorBanner->setBackground(C::FormError);
    _errorBanner->setAlign(text::LayoutOptions::Align::Center);
    _errorBanner->setMaxLines(1);
    _errorBanner->style().padding(12, 6).noShrink();
    _errorBanner->setVisible(false);
    // The parallel-usage banner: right under it, the same danger bar with
    // light text, a "How to solve this?" link to the setup docs and a close
    // button (xl/xs/sm/xs margins, md spacing); hidden until raised.
    _parallelBanner = _mainPane->add<View>();
    _parallelBanner->style().row().padding(16, 4, 8, 4).spacing(12).noShrink();
    _parallelBanner->setBackground(C::FormError);
    {
        static constexpr const char *kSetupDocsUrl =
            "https://github.com/punarinta/make-slack-great-again/blob/master/docs/SETUP_SLACK.md";
        const std::string lead =
            tr("The same app keys are running on another device and keep "
               "interrupting your Slack connection.");
        const std::string how = tr("How to solve this?");
        text::Style       st  = font(Font::Control, C::FormBg);
        st.color              = themed(C::FormBg);
        text::AttributedText t;
        t.append(lead + " ", st);
        text::Style ls = st;
        ls.underline   = true;
        ls.linkId      = 1;
        t.append(how, ls);
        auto *label = _parallelBanner->add<Label>(lead + " " + how, Font::Control, C::FormBg);
        label->setRichText(std::move(t));
        label->style().flex(1).alignSelf(Align::Center);
        label->onLink = [this](uint32_t) {
            if (_ctx.openUrl)
                _ctx.openUrl(kSetupDocsUrl);
        };
        auto *close = _parallelBanner->add<GlyphButton>(Icon::X, 20, 14, C::FormBg);
        close->style().alignSelf(Align::Start);
        close->onClick = [this] { _parallelBanner->setVisible(false); };
    }
    _parallelBanner->setVisible(false);

    auto *body = _mainPane->add<View>();
    body->style().row().flex(1);
    // The message area, with the search overlay over all of it.
    auto *areaHost = body->add<View>();
    areaHost->style().stack().flex(1);
    areaHost->style().minW = 360;
    auto *area             = areaHost->add<View>();
    auto *stack            = area->add<View>();
    stack->style().stack().flex(1);
#ifdef MSGA_HAVE_MESSAGES
    _messages = stack->add<screens::MessageList>(_ctx);
    static_cast<screens::MessageList *>(_messages)->setTypingRow(false);
#else
    _messages = stack->add<MessageStandIn>(_ctx, _avatars);
#endif
    // Until a conversation is open: the shortcuts panel in the list's place.
    _welcome = stack->add<ShortcutsPanel>();
    _messages->setVisible(false);
    _canvas = stack->add<CanvasPage>(_ctx);
    _canvas->setVisible(false);
    _canvas->onError        = [this](const std::string &message) { showError(message); };
    _canvas->onTitleChanged = [this](const std::string &title) {
        if (_current != kNoConv)
            _tabs->setCanvas(true, true, title);
    };
    buildTeammatePage(stack); // an agent workspace's teammate page (shell_agents.cpp)
    buildThreadsPage(stack);
    buildSavedPage(stack);
    buildScheduledPage(stack);
    _swipeBadge = stack->add<SwipeIndicator>(); // above the list, the panel and the canvas
    _typing     = area->add<TypingIndicator>(_ctx);
    _composer   = area->add<Composer>(_ctx, _drafts);
    _composer->setEnabled(false); // until a conversation is open
    _search           = areaHost->add<MessageSearch>(_ctx);
    // A result: the conversation opened the usual way, then the
    // jump (it waits for the first page if that is still loading).
    _search->onResult = [this](ConvRef conv, Ts ts) { jumpToMessage(conv, ts, 0); };
    _search->onHidden = [this] {
        if (_current != kNoConv)
            _composer->edit().focus();
    };

    _splitter = body->add<Splitter>([this](float dx) {
        // The splitter: the panel at least 100 px, the messages 200.
        const float total = _thread->parent()->width();
        _settings.threadWidth =
            int(std::clamp(_thread->width() - dx, 100.f, std::max(100.f, total - 200)));
        _thread->style().width(float(_settings.threadWidth));
    });
    _thread   = body->add<ThreadArea>(_ctx, _avatars, _drafts);
    for (Composer *c : {_composer, _thread->composer})
        setupComposer(*c);
    applyScheduleSend(); // the demo's backend is in place already
    _thread->style().width(float(_settings.threadWidth)).noShrink();
    _splitter->setVisible(false);
    _thread->setVisible(false);
    _header->setVisible(false);
    _tabs->setVisible(false);
    _huddleBanner->setVisible(false);
}

void Shell::setSignedIn(bool on) {
    _signedIn = on;
    refreshRail(); // signed out, the rail stays, no workspace selected
    _sidebar->setVisible(on);
    _listHandle->setVisible(on);
    _mainPane->setVisible(on);
    _signedOut->setVisible(!on);
    // A workspace opening with nothing loaded yet: the waiting state.
    setWaiting(on && _ctx.store().conversationCount() == 0);
}

void Shell::setLive(bool on) {
    _live = on;
    if (on)
        setWaiting(false); // connected: whatever arrived is all there is
}

// A workspace opening with no cache, until the conversations' first arrival:
// the conversation column (sidebar, its resize handle) hidden, header, tabs
// and composer hidden, and the message list in its loading state; then the
// column back, and the welcome panel unless a conversation was opened.
void Shell::setWaiting(bool on) {
    if (on == _waiting)
        return;
    _waiting = on;
    _sidebar->setVisible(_signedIn && !on);
    _listHandle->setVisible(_signedIn && !on);
    if (on) {
        leaveTeammate();
        leavePages();
        _header->setVisible(false);
        _tabs->setVisible(false);
        _huddleBanner->setVisible(false);
        _composer->setVisible(false);
        _typing->setVisible(false);
        _canvas->setVisible(false);
        _welcome->setVisible(false);
        _messages->setVisible(true);
    } else if (_current == kNoConv) {
        _messages->setVisible(false);
        _welcome->setVisible(true);
    }
#ifdef MSGA_HAVE_MESSAGES
    static_cast<screens::MessageList *>(_messages)->setWaiting(on);
#endif
}

void Shell::addWorkspace(PointF anchor) {
    // The services to sign in to (the accounts
    // controller shows them).
    if (onAddWorkspace)
        onAddWorkspace(anchor);
}

void Shell::giveBackUnsent(
    const std::string              &key,
    const model::Store             &store,
    ConvRef                         conv,
    Ts                              thread,
    const std::string              &text,
    const std::vector<std::string> &files
) {
    // The composer holding that conversation's input, shown or not, takes it:
    // its draft is what it holds.
    if (key == _drafts.scope())
        for (Composer *c : {_composer, threadComposer()})
            if (c && c->conv() == conv && c->thread() == thread) {
                c->giveBack(text, files);
                return;
            }
    TextEdit edit;
    loadMrkdwn(edit, store, text);
    _drafts.putBack(key, {conv, thread}, edit.empty() ? std::string() : edit.html(), files);
}

// Only when the signed-out page that follows is
// out of sight (hidden to the tray; plat cannot tell "minimized" on every
// OS, so a minimized window is not nudged) — the app telling the user
// it stopped working, so not gated on the notification settings. A click
// raises the window (an id we don't track).
void Shell::showError(const std::string &message) {
    // The banner shows the latest message for 5 s.
    LOG_WARN("shell", "%s", message.c_str());
    _errorBanner->setText(message);
    _errorBanner->setVisible(true);
    _ctx.app.cancelTimer(_errorTimer);
    _errorTimer = _ctx.app.addTimer(5000, false, [this] {
        _errorTimer = 0;
        _errorBanner->setVisible(false);
    });
}

void Shell::showParallelUsage() {
    _parallelBanner->setVisible(true);
}

// Real input anywhere (a press, a key, a
// wheel) feeds the presence link, at most once per 20 s here (the link
// throttles its tickles further; the WhileUsing clock is 30 min).
void Shell::noteActivity() {
    constexpr int64_t kActivityNoteGapMs = 20'000;
    const int64_t     now                = base::monotonicMs();
    if (_lastActivityNote && now - _lastActivityNote < kActivityNoteGapMs)
        return;
    _lastActivityNote = now;
    // Every running workspace's link: presence is per workspace, the user is the same.
    if (_running.empty())
        _ctx.backend.noteUserActivity();
    for (const auto &r : _running)
        r->backend->noteUserActivity();
}

// Hidden to the tray or minimised, backends poll
// only what notifies (the open chat slowly, no presence rounds); shown again,
// they catch up at once.
void Shell::syncWindowVisible() {
    const bool visible = !_hidden && !_win.native().isMinimized();
    if (visible == _windowVisible)
        return;
    _windowVisible = visible;
    if (_running.empty())
        _ctx.backend.setWindowVisible(visible);
    for (const auto &r : _running)
        r->backend->setWindowVisible(visible);
}

// The Settings dialog's sample notification with the workspace
// icon (illustrative only); the outcome goes back to the dialog.
void Shell::showSampleNotification(
    plat::Notification n, std::function<void(const std::string &)> result
) {
#ifdef __APPLE__
    result(tr("Submitting notification to macOS\xE2\x80\xA6"));
#endif
    _sampleResult = std::move(result);
    notificationImage({workspaceIconPath()}, [this, n = std::move(n)](plat::Image img) mutable {
        n.image = std::move(img);
        sampleNotificationReady(n);
    });
}

void Shell::sampleNotificationReady(const plat::Notification &n) {
    plat::App &pa       = _ctx.app.platform();
    _sampleNotification = pa.notificationsAvailable() ? pa.notify(n) : 0;
#ifdef __APPLE__
    if (_sampleResult)
        _sampleResult(
            _sampleNotification ? tr("Accepted by macOS. If no banner appears, check Focus and "
                                     "notification settings.")
                                : tr("The macOS notification service is unavailable.")
        );
#endif
}

void Shell::notifySessionExpired(const std::string &workspace) {
    plat::App &pa = _ctx.app.platform();
    if (!_hidden || !pa.notificationsAvailable())
        return;
    plat::Notification n;
    n.title = tr("Session expired");
    n.body = workspace.empty()
                 ? std::string(tr("Your session has expired. Click to sign in again."))
                 : i18n::arg(tr("Your %1 session has expired. Click to sign in again."), workspace);
    n.timeoutMs = 10000;
    n.silent    = sounds::kSilentNotifications;
    notificationImage({workspaceIconPath()}, [this, n = std::move(n)](plat::Image img) mutable {
        n.image = std::move(img);
        _ctx.app.platform().notify(n);
    });
}

// Schedule-send is Slack's (chat.scheduleMessage): both composers' chevrons
// follow the open workspace. The shell is built before any workspace is
// behind the proxy, so this runs again on every switch.
void Shell::applyScheduleSend() {
    const bool on = _ctx.backend.capabilities().scheduledSend;
    _composer->setScheduleVisible(on);
    _thread->composer->setScheduleVisible(on);
}

void Shell::workspaceChanged() {
    applyScheduleSend();
    refreshWorkspaceIcon();
    rebuildTrayMenu();
    updateHeader();
    _sidebar->footer().setZenOn(_settings.zenMode(_activeKey)); // its own
    loadSorts();
}

void Shell::open(ConvRef conv) {
    if (!_signedIn || conv == kNoConv || conv >= _ctx.store().conversationCount())
        return;
    leaveTeammate(); // the teammate page keeps what was typed to it
    leavePages();
    _search->hideNow();
    if (conv != _current && threadOpen())
        closeThread(); // a leave path: the thread composer stashes its draft
    _current = conv;
    // Back/forward history: a jump being applied keeps the forward stack
    // (editor undo/redo), every direct open discards it.
    if (_navApplying) {
        _nav.setCurrent(here(conv));
    } else {
        _nav.recordOpen(here(conv));
        _pendingNav = {}; // a direct open wins over a jump still waiting
    }
    _welcome->setVisible(false);
    _header->setVisible(true);
    _tabs->setVisible(true);
    updateHuddleBanner();
    _composer->setTarget(conv, 0);
    _composer->setEnabled(conv != model::kNoConv); // disabled until one is open
    _composerLock.clear();
    _composerSuggestion = _ctx.backend.promptSuggestion(conv); // setTarget gave it
    applyComposerAccess();
    _typing->setTarget(conv, 0);
    _sidebar->select(conv);
    showCanvas(false);
    updateHeader();
    // The peer's presence now, not on the
    // poll's next round.
    if (const auto &c = _ctx.store().conversation(conv);
        c.kind == model::ConvKind::Dm && c.dmUser != model::kNoUser)
        _ctx.backend.requestPresence(c.dmUser);
    lookUpCanvas(conv);
    _ctx.backend.setActiveConversation(conv, 0);
#ifdef MSGA_HAVE_MESSAGES
    // The list loads history and marks read once the newest message shows.
    static_cast<screens::MessageList *>(_messages)->showConversation(conv);
#else
    static_cast<MessageStandIn *>(_messages)->setTarget(conv, 0);
    _ctx.backend.loadHistory(conv, 0, nullptr);
    const auto &c = _ctx.store().conversation(conv);
    if (c.latest && c.latest > c.lastRead)
        _ctx.backend.markRead(conv, c.latest);
#endif
    _composer->edit().focus();
}

void Shell::leaveWorkspace() {
    leaveTeammate();
    leavePages();
    if (_threadsPage)
        _threadsPage->clear(); // its cards point into the old workspace
    if (_savedPage)
        _savedPage->clear();
    if (_scheduledPage)
        _scheduledPage->clear();
    _search->reset(); // another workspace: the query and results go
    if (threadOpen())
        closeThread(); // a leave path: the thread composer stashes its draft
    if (_thread)
        _thread->clear();
    _current = kNoConv;
    if (!_navSwitching)
        _pendingNav = {}; // a manual switch cancels a jump
    _composer->setTarget(kNoConv, 0);
    _composer->setEnabled(false);
    _composerLock.clear();
    _composerSuggestion.clear();
    _typing->setTarget(kNoConv, 0);
    _sidebar->select(kNoConv);
    showCanvas(false);
    _canvas->clear();
#ifdef MSGA_HAVE_MESSAGES
    static_cast<screens::MessageList *>(_messages)->clear();
#else
    static_cast<MessageStandIn *>(_messages)->setTarget(kNoConv, 0);
#endif
    _header->show(kNoConv);
    _header->setVisible(false);
    _tabs->setVisible(false);
    _huddleBanner->setVisible(false);
    _messages->setVisible(false);
    _welcome->setVisible(true);
}

void Shell::purgeHistory(const std::string &key) {
    _nav.purge(key);
    if (_pendingNav.key == key)
        _pendingNav = {};
}

NavLocation Shell::here(ConvRef conv) const {
    return {
        _activeKey,
        conv < _ctx.store().conversationCount() ? _ctx.store().conversation(conv).id : std::string()
    };
}

bool Shell::navigateHistory(bool back) {
    // The open workspace's entries must still be listed (left since?); another
    // workspace must still be running, its entry is checked when its
    // conversations are there.
    const auto valid = [this](const NavLocation &l) {
        if (l.key == _activeKey && _signedIn) {
            const ConvRef c = _ctx.store().findConversation(l.conv);
            return c != kNoConv && _sidebar->rowState(c).exists;
        }
        return findRunning(l.key) != nullptr && onSwitchWorkspace;
    };
    const NavLocation target = back ? _nav.goBack(valid) : _nav.goForward(valid);
    if (!target.valid())
        return false;
    _pendingNav = target;
    if (target.key != _activeKey || !_signedIn) {
        // The workspace first; its last chat must not
        // count as a direct open, the jump's target replaces it.
        _navSwitching = _navApplying = true;
        onSwitchWorkspace(target.key);
        _navSwitching = _navApplying = false;
    }
    applyPendingNav();
    return true;
}

// The jump's conversation, once its workspace is the open one and its
// conversations are there; one that is gone leaves the last chat open.
void Shell::applyPendingNav() {
    if (!_pendingNav.valid() || _pendingNav.key != _activeKey || !_signedIn)
        return;
    const model::Store &st = _ctx.store();
    const ConvRef       c  = st.findConversation(_pendingNav.conv);
    if (c == kNoConv || !(_sidebar->rowState(c).exists || st.conversation(c).member)) {
        if (st.conversationCount() > 0)
            _pendingNav = {};
        return;
    }
    _pendingNav      = {};
    const bool outer = _navApplying;
    _navApplying     = true;
    open(c);
    _navApplying = outer;
}

// Side buttons anywhere in the window
// navigate; a swipe that navigates flashes the arrow badge over the chat.
bool Shell::navInput(const plat::Event &e) {
    using T = plat::EventType;
    if ((e.type == T::PointerDown || e.type == T::PointerUp) &&
        (e.button == plat::Button::Back || e.button == plat::Button::Forward)) {
        if (e.type == T::PointerDown)
            navigateHistory(e.button == plat::Button::Back);
        return true;
    }
    using A = SwipeNav::Action;
    switch (const A act = _swipe.feed(e, base::monotonicMs())) {
    case A::Pass:
        return false;
    case A::Back:
    case A::Forward:
        if (navigateHistory(act == A::Back))
            _swipeBadge->flash(act == A::Back);
        [[fallthrough]];
    case A::Swallow:
        return true;
    }
    return false;
}

// The topmost dialog: a modal dialog (a Dialog covers the window) or the
// quick switcher; not the Settings panel, which is kept apart.
Popup *Shell::topDialog() const {
    return _win.topPopup([this](const Popup &p) {
        return &p == _switcher || (p.place() == Popup::Place::Fill && &p != _settingsDlg);
    });
}

void Shell::closeFrontmost() {
    // A dialog first (like macOS closing a sheet before its window), then the
    // Settings panel; with nothing open the window closes, which hides it to
    // the tray exactly like the title bar's close button.
    if (Popup *d = topDialog()) {
        d->close();
        return;
    }
    if (_settingsDlg) {
        _settingsDlg->close();
        return;
    }
    if (_win.onCloseRequested)
        _win.onCloseRequested();
}

void Shell::openSearch() {
    if (_signedIn)
        _search->toggle();
}

// Shift+Del on an idle Claude Code session: "Remove from msga". Filtered
// before the focused view sees the key, because the composer keeps focus; a
// text field with a selection keeps the key (Cut there).
bool Shell::removeIdleSession(const Event &e) {
    if (!shortcuts::matches(shortcuts::Id::RemoveIdleSession, e) || topDialog() || _settingsDlg)
        return false;
    if (View *f = _win.focusView();
        f && f->role() == Role::TextInput && static_cast<const TextEdit *>(f)->hasSelection())
        return false;
    const ConvRef c = _sidebar->selected();
    if (c >= _ctx.store().conversationCount() || !_ctx.backend.isAgentSession(c))
        return false;
    const model::Conversation &cv = _ctx.store().conversation(c);
    // Exactly what the row paints: an idle session has a gray dot — neither
    // working (green) nor there-but-unreachable (yellow).
    if (cv.kind != model::ConvKind::Dm || cv.dmUser >= _ctx.store().userCount() ||
        _ctx.store().user(cv.dmUser).active || _ctx.store().user(cv.dmUser).unavailable)
        return false;
    _menus->run(Menus::kLeave, c);
    return true;
}

void Shell::openThread(ConvRef conv, Ts root) {
    if (conv != _current)
        open(conv);
    _thread->show(conv, root); // stashes the previous thread's draft
#ifdef MSGA_HAVE_MESSAGES
    // Its reply bar in the list says "Close thread".
    static_cast<screens::MessageList *>(_messages)->setOpenThreadRoot(root);
#endif
    if (!_thread->visible()) { // the saved width, clamped to what fits
        const float total = _thread->parent()->width();
        _thread->style().width(
            std::clamp(float(_settings.threadWidth), 100.f, std::max(100.f, total - 200))
        );
    }
    _thread->setVisible(true);
    _splitter->setVisible(true);
    _ctx.backend.loadThread(conv, root, nullptr);
    _ctx.backend.setActiveConversation(conv, root);
    // The focus stays where it was (the reply bar that opened it).
}

void Shell::closeThread() {
    if (!threadOpen())
        return;
    _thread->composer->stashNow();
    _thread->setVisible(false);
#ifdef MSGA_HAVE_MESSAGES
    static_cast<screens::MessageList *>(_messages)->setOpenThreadRoot(0);
#endif
    _splitter->setVisible(false);
    _ctx.backend.setActiveConversation(_current, 0);
    _composer->edit().focus();
}

void Shell::setupComposer(Composer &c) {
    c.setAvatars(&_avatars);
    c.gifKey    = [this] { return _settings.effectiveGiphyKey(); };
    c.setGifKey = [this](const std::string &k) {
        _settings.giphyKey = k;
        saveSettingsSoon();
    };
    // The paperclip's chooser starts where the last attach was picked.
    c.attachDir    = [this] { return _settings.lastAttachDir; };
    c.setAttachDir = [this](const std::string &dir) {
        if (dir == _settings.lastAttachDir)
            return;
        _settings.lastAttachDir = dir;
        saveSettingsSoon();
    };
    c.refreshTips();
}

// ── The Threads page ────────────────────────────────────────────────────────

void Shell::buildThreadsPage(View *stack) {
    _threadsPage =
        stack->add<ThreadsPage>(_ctx, _avatars, _drafts, [this](Composer &c) { setupComposer(c); });
    _threadsPage->setVisible(false);
    // A thread or channel: the channel (and
    // the thread panel) the usual way.
    _threadsPage->onOpenThread  = [this](ConvRef c, Ts root) { openThread(c, root); };
    _threadsPage->onOpenChannel = [this](ConvRef c) { open(c); };
    _sidebar->onThreads         = [this] { showPage(Page::Threads); };
}

// ── The overview pages ──────────────────────────────────────────────────────

OverviewPage *Shell::pageView(Page p) const {
    switch (p) {
    case Page::Threads:
        return _threadsPage;
    case Page::Saved:
        return _savedPage;
    case Page::Scheduled:
        return _scheduledPage;
    case Page::None:
        break;
    }
    return nullptr;
}

bool Shell::pageOpen(Page p) const {
    const OverviewPage *v = pageView(p);
    return v && v->visible();
}

// Leaving the conversation for the overview: the conversation's chrome and
// the composer go, the open chat's draft is stashed.
void Shell::showPage(Page p) {
    OverviewPage *page = pageView(p);
    const auto    caps = _ctx.backend.capabilities();
    const bool    has  = p == Page::Threads ? caps.threadsView
                         : p == Page::Saved ? caps.messageReminders
                                            : caps.scheduledSend;
    if (!page || !_signedIn || !has)
        return;
    leaveTeammate();
    for (Page o : {Page::Threads, Page::Saved, Page::Scheduled})
        if (o != p)
            leavePage(o);
    leaveConversationChrome(false);
    _sidebar->selectNav(p);
    page->setVisible(true);
    page->open();
}

void Shell::leaveConversationChrome(bool keepComposer) {
    _search->hideNow();
    if (threadOpen())
        closeThread();
    _composer->setTarget(kNoConv, 0); // stashes the conversation's draft
    if (!keepComposer) {
        _composer->setVisible(false);
        _composer->setEnabled(false);
    }
    _current = kNoConv;
    _header->setVisible(false);
    _tabs->setVisible(false);
    _huddleBanner->setVisible(false);
    _messages->setVisible(false);
    _welcome->setVisible(false);
    _canvas->flushPendingSave();
    _canvas->setVisible(false);
    _typing->setTarget(kNoConv, 0);
    _typing->setVisible(false);
    _ctx.backend.setActiveConversation(kNoConv, 0);
}

void Shell::leavePage(Page p) {
    if (!pageOpen(p))
        return;
    pageView(p)->setVisible(false);
    if (_sidebar->selectedNav() == p)
        _sidebar->selectNav(Page::None);
    _composer->setVisible(true);
}

void Shell::leavePages() {
    for (Page p : {Page::Threads, Page::Saved, Page::Scheduled})
        leavePage(p);
}

// ── The Saved messages page ─────────────────────────────────────────────────

void Shell::buildSavedPage(View *stack) {
    _savedPage = stack->add<SavedPage>(_ctx, _avatars);
    _savedPage->setVisible(false);
    // The conversation opened the usual way, then
    // the jump — inside its thread for a reply (history never lists those).
    _savedPage->onOpenMessage = [this](ConvRef c, Ts ts, Ts thread) {
        jumpToMessage(c, ts, thread);
    };
    _savedPage->onOpenChannel = [this](ConvRef c) { open(c); };
    _sidebar->onSavedMessages = [this] { showPage(Page::Saved); };
}

// ── The Scheduled messages page ─────────────────────────────────────────────

void Shell::buildScheduledPage(View *stack) {
    _scheduledPage = stack->add<ScheduledPage>(_ctx, _avatars);
    _scheduledPage->setVisible(false);
    _scheduledPage->onOpenChannel = [this](ConvRef c) { open(c); };
    _sidebar->onScheduled         = [this] { showPage(Page::Scheduled); };
}

void Shell::jumpToMessage(ConvRef conv, Ts ts, Ts thread) {
    if (conv == kNoConv || !ts)
        return;
    if (conv != _current)
        open(conv);
    if (conv != _current)
        return;
#ifdef MSGA_HAVE_MESSAGES
    if (!thread) {
        static_cast<screens::MessageList *>(_messages)->jumpTo(ts);
        return;
    }
    openThread(conv, thread);
    if (screens::ThreadPanel *p = static_cast<screens::ThreadPanel *>(threadPanel()))
        p->list().jumpTo(ts);
#else
    (void)ts;
    if (thread)
        openThread(conv, thread);
#endif
}

bool Shell::threadOpen() const {
    return _thread && _thread->visible();
}

ui::View *Shell::threadPanel() const {
#ifdef MSGA_HAVE_MESSAGES
    return _thread ? _thread->panel : nullptr;
#else
    return nullptr;
#endif
}

Composer *Shell::threadComposer() {
    return _thread ? _thread->composer : nullptr;
}

// The tabs: Messages, or the canvas page (the conversation's canvas, or
// "Add canvas": a blank page the first save turns into one).
void Shell::showCanvas(bool on) {
    on = on && _current != kNoConv;
    if (!on)
        _canvas->flushPendingSave(); // on every tab / conversation switch
    _canvas->setVisible(on);
    _messages->setVisible(!on && _current != kNoConv);
    _composer->setVisible(!on);
    _typing->setVisible(!on && !_typing->text().empty());
    _tabs->setActive(on ? 1 : 0);
    if (on)
        _canvas->open(_current);
}

// While the open conversation (its header on
// screen) has a live huddle, where the service has huddles.
void Shell::updateHuddleBanner() {
    const bool on = _current != kNoConv && _header->visible() &&
                    _ctx.backend.capabilities().huddles &&
                    _ctx.store().conversation(_current).huddleActive;
    if (_huddleBanner->visible() != on)
        _huddleBanner->setVisible(on);
}

void Shell::updateHeader() {
    if (_current == kNoConv)
        return;
    updateHuddleBanner();
    const auto &c = _ctx.store().conversation(_current);
    _header->show(_current);
    // Canvases on every conversation but an app's DM (the tab is hidden for
    // bots, whose canvases are app-owned), where the service has them.
    const bool app = c.kind == model::ConvKind::Dm && _ctx.store().user(c.dmUser).bot;
    _tabs->setCanvas(
        _ctx.backend.capabilities().canvases && !app,
        !c.canvasId.empty() || !c.canvasTitle.empty(),
        c.canvasTitle
    );
}

// The roster may not say whether a channel has a
// canvas (conversations.list carries no properties), so look it up, and its
// title for the tab, each time a conversation opens.
void Shell::lookUpCanvas(ConvRef conv) {
    const auto &c   = _ctx.store().conversation(conv);
    const bool  app = c.kind == model::ConvKind::Dm && _ctx.store().user(c.dmUser).bot;
    if (!_ctx.backend.capabilities().canvases || app)
        return;
    _ctx.backend.loadChannelCanvas(conv, [this, conv](std::string fileId) {
        if (fileId.empty() || conv != _current)
            return;
        _ctx.backend.loadCanvasMeta(fileId, nullptr); // the Store gets the title
    });
}

void Shell::onChange(const model::Change &ch) {
    using K = model::ChangeKind;
    if (ch.kind == K::Roster) { // the workspace arrived (or changed): its icon, the tray menu
        refreshWorkspaceIcon();
        rebuildTrayMenu();
        if (_waiting && _ctx.store().conversationCount() > 0)
            setWaiting(false); // the column shows the moment real data arrives
        if (teammateOpen())    // another workspace: the page follows (or goes)
            refreshTeammates();
        applyPendingNav(); // a back/forward jump waiting for these conversations
    }
    bool attached = false;
    for (const auto &r : _running)
        attached = attached || r->store == &_ctx.store();
    if (ch.kind == K::Users && _current != kNoConv) {
        // A name resolved later, a presence or DND flip, my phantom state —
        // the open chat's header and "Message …" follow.
        _header->refresh();
        _composer->refreshPlaceholder();
        return;
    }
    if (ch.kind == K::Meta || ch.kind == K::Roster) {
        if (ch.kind == K::Meta && !attached && _live)
            huddleChanged(_ctx.store(), {}, ch.conv);
        if (ch.conv == _current)
            updateHeader();
        if (ch.conv == _current || ch.kind == K::Roster)
            applyComposerAccess(); // the lock flips while the chat is open
        // A poll or a roster reload brings a burst of Meta: count once after
        // it (each count walks every conversation of every workspace). A
        // Roster was counted already (refreshWorkspaceIcon → refreshRail).
        if (ch.kind == K::Meta)
            attentionSoon();
        return;
    }
    if ((ch.kind != K::Append && ch.kind != K::Arrived) || !_live || ch.conv == kNoConv)
        return;
    // New messages on screen are read; the others notify (an attached
    // workspace's own observer does that, open or not).
    model::Store &st = _ctx.store();
    if (const std::vector<model::Message> *list =
            ch.thread ? st.replies(ch.conv, ch.thread) : &st.conversation(ch.conv).messages;
        ch.kind == K::Append && list && reading() && ch.conv == _current && !ch.thread)
        for (size_t i = list->size() - std::min<size_t>(ch.count, list->size()); i < list->size();
             ++i)
            if (const model::Message &m = (*list)[i]; m.user != st.me && !m.pending)
                _ctx.backend.markRead(ch.conv, m.ts);
    if (!attached)
        messagesArrived(st, {}, ch);
}

// The tray menu: one item per workspace, Settings, Reset window
// size (a frameless window pushed off-screen has no other way back), Quit.
void Shell::rebuildTrayMenu() {
    if (!_tray)
        return;
    using K = plat::MenuItem::Kind;
    std::vector<plat::MenuItem> items;
    if (_haveWorkspaces) {
        // In the rail's order; the open one by the Store's (freshest) name.
        for (size_t i = 0; i < _workspaces.size(); ++i) {
            const Workspace   &w    = _workspaces[i];
            const bool         open = w.key == _activeKey && _signedIn;
            const std::string &name =
                open && !_ctx.store().workspaceName.empty() ? _ctx.store().workspaceName : w.name;
            items.push_back({K::Action, uint32_t(kTrayWorkspace + i), name.empty() ? w.id : name});
        }
    } else if (!_ctx.store().workspaceId.empty()) {
        const std::string &name = _ctx.store().workspaceName;
        items.push_back(
            {K::Action, kTrayWorkspace, name.empty() ? _ctx.store().workspaceId : name}
        );
    }
    items.push_back({K::Separator, 0, ""});
    items.push_back({K::Action, kTraySettings, tr("Settings")});
    items.push_back({K::Action, kTrayResetSize, tr("Reset window size")});
    items.push_back({K::Separator, 0, ""});
    items.push_back({K::Action, kTrayQuit, tr("Quit")});
    _tray->setMenu(std::move(items));
}

// The default size, centred on the window's monitor's work area.
namespace {

// The monitor the window overlaps most, else the one
// the platform names (no positions on Wayland), else the primary.
const plat::Monitor *monitorFor(const plat::Window &w, const std::vector<plat::Monitor> &ms) {
    const plat::Monitor *best = nullptr;
    if (const auto p = w.position()) {
        const plat::Size s     = w.size();
        double           bestA = 0;
        for (const plat::Monitor &m : ms) {
            const plat::Rect &b = m.bounds;
            const double iw = std::min(p->x + s.w, double(b.x + b.w)) - std::max(p->x, double(b.x));
            const double ih = std::min(p->y + s.h, double(b.y + b.h)) - std::max(p->y, double(b.y));
            if (iw > 0 && ih > 0 && iw * ih > bestA) {
                bestA = iw * ih;
                best  = &m;
            }
        }
    }
    for (const plat::Monitor &m : ms)
        if (!best && m.id == w.monitor())
            best = &m;
    for (const plat::Monitor &m : ms)
        if (!best && m.primary)
            best = &m;
    return best ? best : ms.empty() ? nullptr : &ms.front();
}

} // namespace

// Tray rescue: back to the default size (fitted), centred on the current
// monitor. Unlike fitToScreen a deliberate request: it may grow the window.
void Shell::resetWindowGeometry() {
    auto &w = _win.native();
    if (w.isFullscreen())
        w.setFullscreen(false);
    if (w.isMaximized())
        w.setMaximized(false);
    w.setSize({1200, 800});
    fitToScreen();
    const std::vector<plat::Monitor> ms = _ctx.app.platform().monitors();
    if (const plat::Monitor *m = monitorFor(w, ms)) {
        const plat::Rect &a  = m->workArea;
        const plat::Size  sz = w.size();
        w.setPosition({a.x + (a.w - sz.w) / 2.0, a.y + (a.h - sz.h) / 2.0});
    }
}

void Shell::fitToScreen() {
    plat::Window &w = _win.native();
    // Maximised and fullscreen geometry is the window system's business.
    if (w.isMaximized() || w.isFullscreen())
        return;
    const std::vector<plat::Monitor> ms = _ctx.app.platform().monitors();
    const plat::Monitor             *m  = monitorFor(w, ms);
    if (!m || m->workArea.w <= 0 || m->workArea.h <= 0)
        return;
    _fitMonitor         = m->id;
    const plat::Rect &a = m->workArea;
    // The minimum gives way first: a floor bigger than the screen makes the
    // window unshrinkable whatever we do to its size.
    w.setMinSize({std::min(kMinWindowSize.w, a.w), std::min(kMinWindowSize.h, a.h)});
    const plat::Size s = w.size();
    const plat::Size want{std::min(s.w, a.w), std::min(s.h, a.h)};
    if (want.w != s.w || want.h != s.h)
        w.setSize(want);
    // Only a window hanging off the work area moves (nothing on Wayland,
    // where the compositor keeps it on screen and the resize does the work).
    if (const auto p = w.position()) {
        const double x = std::clamp(p->x, double(a.x), double(std::max(a.x, a.x + a.w - want.w)));
        const double y = std::clamp(p->y, double(a.y), double(std::max(a.y, a.y + a.h - want.h)));
        if (x != p->x || y != p->y)
            w.setPosition({x, y});
    }
}

bool Shell::hideToTray() {
    // Closing the window keeps msga running in the tray
    // for badges and notifications — but only while a tray host actually
    // shows our icon (none with Settings → Use tray off), or there would be
    // no way back.
    return hideWindow();
}

void Shell::minimize() {
    // Settings → Minimize to tray: the window leaves the taskbar too. Only our
    // own title bar's button can do this — the OS minimizes (a compositor
    // shortcut, the taskbar) without telling us, as on Wayland.
    if (!(_settings.minimizeToTray && hideWindow()))
        _win.native().minimize();
}

bool Shell::hideWindow() {
    if (!_tray || !_tray->isVisible())
        return false;
    saveState();
    _hiddenMaximized = _win.native().isMaximized();
    _win.native().hide();
    _hidden = true;
    updateReading();
    syncWindowVisible();
    return true;
}

bool Shell::reading() const {
    return _win.isActive() && !_hidden;
}

// In the background the open chat
// accrues unreads and notifies; back in front it is marked read.
void Shell::updateReading() {
#ifdef MSGA_HAVE_MESSAGES
    static_cast<screens::MessageList *>(_messages)->setReading(reading());
#endif
}

// ── Updates and restarts ────────────────────────────────────────────────────

#ifdef MSGA_SELF_UPDATE
void Shell::setUpdater(update::Updater *u) {
    if (_updater)
        _updater->unlisten(_updateListener);
    _updater = u;
    if (!u)
        return;
    _updateListener = u->listen([this](const update::Updater::Event &e) {
        using K = update::Updater::Event::Kind;
        if (e.kind == K::Ready)
            _updateBar->showUpdateReady();
        else if (e.kind == K::Failed)
            showError(e.message); // the error banner
    });
    u->onChecked    = [this](int64_t when) {
        _settings.lastUpdateCheck = when; // when the last check ran
        saveSettingsSoon();
    };
    // A silent check 5 s after the start (none with auto-checks off).
    _ctx.app.cancelTimer(_updateTimer);
    _updateTimer = _ctx.app.addTimer(5000, false, [this] {
        _updateTimer = 0;
        if (_updater)
            _updater->checkInBackground(_settings.autoUpdates);
    });
}

// The replaced binary starts again; on macOS
// the DMG opens instead.
void Shell::applyUpdate() {
#ifdef __APPLE__
    if (_updater && !_updater->downloadedPath().empty())
        _ctx.app.platform().openUrl(file::toFileUrl(_updater->downloadedPath()));
#else
    restart();
#endif
}
#endif

void Shell::restart() {
    base::relaunchOnExit(restartArgs);
    quit();
}

void Shell::quit() {
    saveState();
    if (onQuit)
        onQuit();
}

void Shell::restore(const std::string &token) {
    auto &w = _win.native();
    if (_hidden) {
        _hidden = false;
        w.show();
        if (_hiddenMaximized)
            w.setMaximized(true);
        updateReading();
        fitToScreen(); // on show: monitors may have changed meanwhile
    }
    syncWindowVisible();
    // Un-minimising is the backend's job (Wayland remaps the window).
    if (!token.empty())
        w.activateWithToken(token);
    else
        w.activate();
}

void Shell::handleAppEvent(const plat::Event &e) {
    using T    = plat::EventType;
    auto raise = [&] { restore(e.activationToken); };
    switch (e.type) {
    case T::MonitorsChanged: // added, removed, resized, rescaled, a panel appeared
        fitToScreen();
        break;
    case T::QuitRequested: // macOS Cmd+Q, the Dock's Quit: the tray's Quit
        quit();
        break;
    case T::TrayActivated:
        // Like Telegram: restore the window when it is
        // tucked away in the tray or minimised; otherwise do nothing.
        if (_hidden || !_win.native().isActive())
            raise();
        break;
    case T::TrayMenuItem:
        switch (e.id) {
        case kTraySettings:
            raise();
            openSettings();
            break;
        case kTrayResetSize:
            raise();
            resetWindowGeometry();
            break;
        case kTrayQuit:
            quit();
            break;
        default:
            // A workspace: restore, then switch to it.
            if (e.id >= kTrayWorkspace) {
                raise();
                const size_t i = e.id - kTrayWorkspace;
                if (i < _workspaces.size() && _workspaces[i].key != _activeKey &&
                    onSwitchWorkspace) {
                    const std::string key = _workspaces[i].key;
                    onSwitchWorkspace(key);
                }
            }
            break;
        }
        break;
    case T::NotificationFailed:
        if (e.id && e.id == _sampleNotification && _sampleResult)
            _sampleResult(i18n::arg(tr("Notification status: %1"), e.text));
        break;
    case T::NotificationActivated:
        if (e.id && e.id == _sampleNotification && e.action == "join") {
            // The sample huddle's "Join": the open conversation's huddle link.
            _ctx.app.platform().openUrl(
                _current != kNoConv ? huddleJoinUrl(_ctx.store, _current)
                                    : std::string("https://app.slack.com")
            );
            break;
        }
        if (const auto it = _notified.find(e.id); it != _notified.end()) {
            const Notified to = it->second;
            _notified.erase(it);
            // A huddle's "Join": the browser, the window left where it is.
            if (e.action == "join" && !to.join.empty()) {
                if (_ctx.openUrl)
                    _ctx.openUrl(to.join);
                else
                    _ctx.app.platform().openUrl(to.join);
                break;
            }
            // Another workspace's: switch to it first, then the chat if it is the one shown —
            // a reply's thread with it, a reminder's message flashed.
            if (!to.key.empty() && !(to.key == _activeKey && _signedIn) && onSwitchWorkspace)
                onSwitchWorkspace(to.key);
            const Running *r = to.key.empty() ? nullptr : findRunning(to.key);
            if (to.key.empty() || (r && r->store == &_ctx.store() && _signedIn)) {
                raise();
                if (to.ts)
                    jumpToMessage(to.conv, to.ts, to.root);
                else {
                    open(to.conv);
                    if (to.root && to.conv == _current)
                        openThread(to.conv, to.root);
                }
                break;
            }
        }
        raise();
        break;
    case T::NotificationClosed:
        _notified.erase(e.id);
        break;
    case T::InstanceActivated:
        raise();
        break; // a second launch: show this one
    default:
        break;
    }
}

// Unread badges and the tray icon: each workspace's own dot on
// its rail tile; the launcher badge and the tray dot sum every workspace
// except the muted ones (their tiles still show).
void Shell::updateAttention() {
    if (_attentionTimer) {
        _ctx.app.cancelTimer(_attentionTimer);
        _attentionTimer = 0;
    }
    const model::Store      &open     = _ctx.store();
    const model::NotifyLevel fallback = defaultLevel();
    int                      n        = 0;
    bool                     unread   = false;
    bool                     attached = false;
    for (const auto &r : _running) {
        if (r->store == &open) {
            attached = true;
            r->dirty = true; // the open one is recounted on every call, as before
        }
        if (r->dirty) {
            r->att   = workspaceAttention(*r->store, fallback, r->backend->nowSecs());
            r->dirty = false;
        }
        if (RailTile *t = _wsStack->find(r->key))
            t->setDot(r->att.dot());
        if (!r->store->workspaceMuted) {
            n += r->att.important;
            unread = unread || r->att.unread;
        }
    }
    if (!attached && _signedIn) { // the demo, tests: the open Store alone
        const Attention a = workspaceAttention(open, fallback, _ctx.backend.nowSecs());
        if (RailTile *t = _wsStack->find(_activeKey))
            t->setDot(a.dot());
        if (!open.workspaceMuted) {
            n += a.important;
            unread = unread || a.unread;
        }
    }
    unread = unread || n > 0;
    if (n != _lastBadge)
        _ctx.app.platform().setBadgeCount(n);
    const int tray = n > 0 ? 2 : unread ? 1 : 0;
    if (n != _lastBadge || tray != _lastTray)
        refreshTrayIcon(n, unread);
    _lastBadge = n;
    _lastTray  = tray;
}

void Shell::refreshTrayIcon(int mentions, bool unread) {
    if (!_tray)
        return;
    // The paper plane (gfx/icon_tray.svg, compiled as Icon::Tray), with a
    // dot in the corner: red while a DM or mention waits,
    // blue for plain unread activity. Muted conversations count for neither.
#ifdef __APPLE__
    const gfx::Color plane = 0xff000000U; // a template's alpha is all that counts
    const int        px[]  = {18, 36};
#else
    const gfx::Color plane = 0xffffffffU; // panels and taskbars are dark by default
    const int        px[]  = {16, 22, 32, 44};
#endif
    // Settings → Tray icon: the user's own picture instead of the plane.
    // Fitted into a square and, by default, a white silhouette like the
    // built-in plane.
    const std::string custom =
        _settings.customTrayIcon && !_settings.trayIconPath.empty()
            ? _settings.trayIconPath + (_settings.trayMonochrome ? "\x01m" : "")
            : std::string();
    _trayMentions = mentions;
    _trayUnread   = unread;
    if (custom != _trayImagePath) {
        _trayImagePath = custom;
        _trayImage.reset();
        _trayDecoding = !custom.empty();
        if (_trayDecoding) { // read and decoded on a worker, not here
            auto out = std::make_shared<std::shared_ptr<const gfx::Bitmap>>();
            model::runInBackground(
                _ctx.app.platform(),
                [out, path = _settings.trayIconPath, mono = _settings.trayMonochrome] {
                    std::string bytes;
                    gfx::Bitmap bmp;
                    if (file::readAll(path, &bytes) && decodeTrayPicture(bytes, &bmp))
                        *out = std::make_shared<gfx::Bitmap>(trayPicture(bmp, mono));
                },
                [this, alive = std::weak_ptr<int>(_agentAlive), out, custom] {
                    if (alive.expired() || custom != _trayImagePath)
                        return; // the shell is gone, or another picture is wanted
                    _trayImage    = std::move(*out);
                    _trayDecoding = false;
                    refreshTrayIcon(_trayMentions, _trayUnread);
                }
            );
        }
    }
    if (_trayDecoding)
        return;
#ifdef __APPLE__
    // An NSImage template, so the menu bar tints it — the plane, or a
    // monochrome custom picture; a colour picture keeps its colours. In a
    // template the dot is a white mask too, with a clear halo cut around it.
    const bool templ = !_trayImage || _settings.trayMonochrome;
#else
    const bool templ = false;
#endif
    std::vector<plat::Image> sizes;
    for (int n : px) {
        gfx::Bitmap b(n, n);
        {
            gfx::Painter p(b.view(), 1.f);
            if (_trayImage)
                p.drawBitmap(_trayImage->view(), {0, 0, float(n), float(n)});
            else
                gfx::drawIcon(p, Icon::Tray, {0, 0, float(n), float(n)}, plane);
        }
        if (mentions > 0 || unread) {
            const float d = std::max(6.f, float(n) * 0.3f);
            const float c = float(n) - d / 2;
#ifdef __APPLE__
            if (templ) // a clear halo, so the dot stays apart from the plane's wing
                gfx::clearDisc(b, c, c, d / 2 + float(n) * 4 / 128);
#endif
            gfx::Painter p(b.view(), 1.f);
            p.fillCircle(
                {c, c},
                d / 2,
                templ          ? 0xffffffffU
                : mentions > 0 ? 0xffcd2553U
                               : 0xff1d9bd1U
            );
        }
        sizes.push_back(toPlatImage(b));
    }
    _tray->setTemplate(templ);
    _tray->setIcon(sizes);
}

void Shell::openSettings() {
    openSettingsAt(uint8_t(settings::SettingsDialog::Page::Appearance));
}

void Shell::openAiSettings() {
    // An open dialog turns to the page.
    if (_settingsDlg)
        _settingsDlg->showPage(settings::SettingsDialog::Page::Ai);
    else
        openSettingsAt(uint8_t(settings::SettingsDialog::Page::Ai));
}

void Shell::openSettingsAt(uint8_t page) {
    if (_settingsDlg)
        return;
    settings::SettingsDialog::Hooks hooks;
    // What the dialog changes is on disk at once (its pages say so).
    hooks.changed = [this] {
        saveSettingsAsync();
        applySettings();
    };
#ifdef MSGA_SELF_UPDATE
    hooks.updater = _updater;
#endif
    hooks.restart = [this] { restart(); };
    hooks.testNotification =
        [this](plat::Notification n, std::function<void(const std::string &)> result) {
            showSampleNotification(std::move(n), std::move(result));
        };
    hooks.pickTrayIcon = [this](std::function<void()> after) {
        Popup *d = showTrayIconDialog(
            _ctx,
            _win,
            _settings.customTrayIcon ? _settings.trayIconPath : std::string(),
            _settings.trayMonochrome,
            [this, after](const std::string &path, bool mono) {
                _settings.trayMonochrome = mono;
                _settings.trayIconPath   = path;
                // A picture saved turns the custom icon on; "Use default" off.
                _settings.customTrayIcon = !path.empty();
                saveSettingsAsync();
                applySettings();
                after();
            }
        );
        d->onClosed = after;
    };
    hooks.clearState = [this] {
        // Re-seeded from what is known now.
        _sidebar->clearVisited();
        storeVisited();
        saveSettingsAsync();
    };
    hooks.importSlackSession   = onImportSlackSession;
    hooks.convertToSession     = onConvertToSession;
    hooks.oauthSlackWorkspaces = oauthSlackWorkspaces ? oauthSlackWorkspaces() : 0;
    // The theme's source: the open workspace if it can read the account's
    // Slack theme, else the first running one that can (by key: it may stop
    // while the dialog is open).
    std::string themeKey;
    for (const auto &r : _running)
        if (r->backend->capabilities().sidebarTheme &&
            (themeKey.empty() || (r->store == &_ctx.store() && _signedIn)))
            themeKey = r->key;
    if (!themeKey.empty() || (_running.empty() && _ctx.backend.capabilities().sidebarTheme))
        hooks.fetchSlackTheme = [this,
                                 themeKey](settings::SettingsDialog::Hooks::SlackThemeDone done) {
            const Running  *r = themeKey.empty() ? nullptr : findRunning(themeKey);
            model::Backend *b = r ? r->backend : themeKey.empty() ? &_ctx.backend : nullptr;
            if (!b) {
                done({}, {}, "no_workspace");
                return;
            }
            b->loadSidebarTheme([done](model::Backend::SidebarTheme t, std::string err) {
                done(std::move(t.iaTheme), std::move(t.legacyValues), std::move(err));
            });
        };
    auto *dlg = new settings::SettingsDialog(
        _ctx, _settings, std::move(hooks), settings::SettingsDialog::Page(page)
    );
    dlg->onClosed = [this, dlg] {
        if (_settingsDlg == dlg)
            _settingsDlg = nullptr;
#ifdef __APPLE__
        _header->setEnabled(true);
#endif
    };
    _settingsDlg = dlg;
#ifdef __APPLE__
    // Settings covers the body, but the unified header is in the title bar:
    // its actions are blocked while Settings is open.
    _header->setEnabled(false);
#endif
    _win.showPopup(std::unique_ptr<Popup>(dlg));
    dlg->sections().focus();
}

std::string Shell::workspaceIconPath() const {
    // The chosen picture, else the server's.
    return workspaceIcon(_ctx.app.platform(), _ctx.store().workspaceId, _ctx.store().workspaceIcon);
}

void Shell::refreshWorkspaceIcon() {
    refreshRail();
}

void Shell::setWorkspaces(std::vector<Workspace> list, std::string activeKey) {
    _haveWorkspaces = true;
    _workspaces     = std::move(list);
    _activeKey      = std::move(activeKey);
    _drafts.setScope(_activeKey); // its drafts, not another workspace's
    refreshRail();
    rebuildTrayMenu();
}

std::vector<std::string> Shell::railOrder() const {
    return _wsStack->keys();
}

View *Shell::workspaceTile(const std::string &key) const {
    return _wsStack->find(key);
}

int Shell::workspaceDot(const std::string &key) const {
    const RailTile *t = _wsStack->find(key);
    return t ? t->dot : -1;
}

void Shell::refreshRail() {
    // Without the accounts controller (the demo, tests) the rail shows the
    // Store's workspace alone while signed in.
    std::vector<std::string> keys;
    if (_haveWorkspaces)
        for (const Workspace &w : _workspaces)
            keys.push_back(w.key);
    else if (_signedIn)
        keys.push_back(_activeKey);
    if (!_wsStack->dragging() && keys != _wsStack->keys())
        _wsStack->setKeys(keys);
    plat::App &pa = _ctx.app.platform();
    for (size_t i = 0; i < _wsStack->count(); ++i) {
        RailTile        *t      = _wsStack->tile(i);
        const bool       active = t->key == _activeKey && _signedIn;
        const Workspace *w      = nullptr;
        for (const Workspace &x : _workspaces)
            if (x.key == t->key)
                w = &x;
        // The chosen picture, else the server's.
        std::string name, icon;
        if (active || !w) {
            name = _ctx.store().workspaceName;
            icon = workspaceIconPath();
        } else {
            name = w->name;
            icon = workspaceIcon(pa, w->id, w->icon);
        }
        t->setBitmap(icon.empty() ? nullptr : _avatars.get(icon, 80));
        t->setName(name);
        t->setTooltip(name);
        t->setActive(active);
        if (!active && !findRunning(t->key))
            t->setDot(0); // not running: nothing is known (updateAttention sets the rest)
    }
    // kGap under the last bubble; with none the "+"
    // takes the first bubble's slot right under the top pad.
    _addBtn->style().margins(0, _wsStack->count() ? 16 : 0, 0, 0);
    _wsStack->update();
    updateAttention();
    // The title: the open workspace's name, "msga" without one.
    if (_titleLabel) {
        const std::string &name = _ctx.store().workspaceName;
        _titleLabel->setText(_signedIn && !name.empty() ? name : std::string(tr("msga")));
    }
}

// ── Running workspaces ──────────────────────────────────────────────────────

Shell::Running *Shell::findRunning(const std::string &key) {
    for (const auto &r : _running)
        if (r->key == key)
            return r.get();
    return nullptr;
}

bool Shell::showWorkspaceOf(const model::Store *st) {
    if (!st)
        return false;
    if (st == &_ctx.store() && _signedIn)
        return true;
    for (const auto &r : _running)
        if (r->store == st) {
            if (onSwitchWorkspace)
                onSwitchWorkspace(r->key);
            break;
        }
    return st == &_ctx.store() && _signedIn;
}

void Shell::attachWorkspace(const std::string &key, model::Store &store, model::Backend &backend) {
    detachWorkspace(key);
    auto r      = std::make_unique<Running>();
    r->key      = key;
    r->store    = &store;
    r->backend  = &backend;
    // On the Store itself, not the screens' slot: it stays with this
    // workspace whichever one is open.
    r->observer = store.observe(model::Store::kAnyConv, [this, key](const model::Change &ch) {
        onWorkspaceChange(key, ch);
    });
    _running.push_back(std::move(r));
    attentionSoon();
}

void Shell::detachWorkspace(const std::string &key) {
    const auto it = std::find_if(_running.begin(), _running.end(), [&](const auto &r) {
        return r->key == key;
    });
    if (it == _running.end())
        return;
    (*it)->store->unobserve((*it)->observer);
    _running.erase(it);
    std::erase_if(_notified, [&](const auto &n) { return n.second.key == key; });
    // Dialogs that reach every workspace let it go.
    if (_forward)
        _forward->close();
    if (_switcher)
        _switcher->close();
    if (RailTile *t = _wsStack->find(key); t && key != _activeKey)
        t->setDot(0);
    updateAttention();
}

model::Backend &Shell::backendFor(const model::Store &st) {
    for (const auto &r : _running)
        if (r->store == &st)
            return *r->backend;
    return _ctx.backend;
}

model::Store *Shell::storeFor(const std::string &key) const {
    for (const auto &r : _running)
        if (r->key == key)
            return r->store;
    return key == _activeKey ? &_ctx.store() : nullptr; // the open one: attached or not
}

bool Shell::storeAlive(const model::Store *st) const {
    if (st == &_ctx.store())
        return true;
    for (const auto &r : _running)
        if (r->store == st)
            return true;
    return false;
}

void Shell::setWorkspaceLive(const std::string &key, bool live) {
    if (Running *r = findRunning(key))
        r->live = live;
}

std::vector<Shell::LiveWorkspace> Shell::liveWorkspaces() const {
    std::vector<LiveWorkspace> out;
    for (const Workspace &w : _workspaces)
        for (const auto &r : _running)
            if (r->key == w.key) {
                const bool  open = r->store == &_ctx.store() && _signedIn;
                std::string name =
                    open && !r->store->workspaceName.empty() ? r->store->workspaceName : w.name;
                out.push_back(
                    {w.key,
                     name.empty() ? w.id : name,
                     workspaceIcon(_ctx.app.platform(), w.id, w.icon),
                     r->store,
                     r->backend}
                );
            }
    return out;
}

void Shell::onWorkspaceChange(const std::string &key, const model::Change &ch) {
    using K    = model::ChangeKind;
    Running *r = findRunning(key);
    if (!r)
        return;
    if (ch.kind == K::Meta || ch.kind == K::Roster) {
        r->dirty = true;
        if (r->store != &_ctx.store())
            attentionSoon(); // the open one's view observer counts at once
        if (ch.kind == K::Meta && r->live)
            huddleChanged(*r->store, key, ch.conv);
        return;
    }
    if ((ch.kind == K::Append || ch.kind == K::Arrived) && r->live && ch.conv != kNoConv)
        messagesArrived(*r->store, key, ch);
}

// A workspace's unread counts changed: recount once the burst (a poll's
// worth of conversations) is over.
void Shell::attentionSoon() {
    if (_attentionTimer)
        return;
    _attentionTimer = _ctx.app.addTimer(50, false, [this] {
        _attentionTimer = 0;
        updateAttention();
    });
}

void Shell::showWorkspaceMenu(const std::string &key, PointF at) {
    Menus::Workspace w;
    w.key    = key;
    w.active = key == _activeKey && _signedIn;
    if (const Running *r = findRunning(key))
        w.store = r->store; // a live workspace: whether you administer it
    for (const Workspace &x : _workspaces)
        if (x.key == key) {
            w.name  = x.name;
            w.muted = x.muted;
        }
    _menus->showWorkspace(w, at);
}

void Shell::showProfile(model::UserRef u, RectF anchor) {
    _profiles->hover(u, anchor, ProfileCards::Click);
}

void Shell::renameConversation(ConvRef c) {
    showRenameDialog(_ctx, _win, c);
}

void Shell::openBrowseDialog(int tab) {
    std::weak_ptr<int> alive = _agentAlive;
    showFindChannel(
        _ctx,
        _win,
        _avatars,
        tab,
        [this, alive](ConvRef c) {
            // Already a member: just open it (even if the sidebar hides it).
            if (_ctx.store().conversation(c).member) {
                open(c);
                return;
            }
            _ctx.backend.joinChannel(c, [this, alive](ConvRef joined, const std::string &err) {
                if (alive.expired())
                    return;
                if (joined == kNoConv)
                    showError(err);
                else
                    open(joined);
            });
        },
        [this, alive](model::UserRef u) {
            // The DM, created by conversations.open if never messaged.
            _ctx.backend.openDm(u, [this, alive](ConvRef c) {
                if (!alive.expired() && c != kNoConv)
                    open(c);
            });
        },
        [this, alive] {
            _ctx.app.platform().post([this, alive] {
                if (!alive.expired())
                    openCreateChannel();
            });
        }
    );
}

void Shell::openCreateChannel() {
    std::weak_ptr<int> alive = _agentAlive;
    showCreateChannel(
        _win, _ctx.store().workspaceName, [this, alive](const std::string &name, bool priv) {
            // Created, and the list shows it; nothing opens.
            _ctx.backend.createChannel(name, priv, [this, alive](ConvRef, const std::string &err) {
                if (!alive.expired() && !err.empty())
                    showError(err);
            });
        }
    );
}

void Shell::applySettings() {
    // The send key: the composers read the table on every key; the tooltips
    // and the shortcuts panel show it.
    if (_settings.ctrlEnterSends != shortcuts::ctrlEnterSends()) {
        shortcuts::setCtrlEnterSends(_settings.ctrlEnterSends);
        _composer->refreshTips();
        if (_thread)
            _thread->composer->refreshTips();
        static_cast<ShortcutsPanel *>(_welcome)->rebuild();
    }
    // The formatting the composers are typed in: what is typed and every
    // draft keep their meaning — rewritten into the new formatting first.
    if (_settings.markdownFormatting != markdownFormatting()) {
        const bool md = _settings.markdownFormatting;
        for (Composer *c : {_composer, threadComposer()})
            if (c)
                c->redialect(md);
        std::vector<model::Store *> stores{&_ctx.store()};
        for (const auto &r : _running)
            if (r->store != &_ctx.store())
                stores.push_back(r->store);
        for (model::Store *st : stores) {
            const std::vector<model::Store::Draft> drafts = st->drafts();
            for (const model::Store::Draft &d : drafts) {
                std::string html = redialectDraft(*st, d.html, md);
                if (html != d.html)
                    st->setDraft(d.conv, d.thread, std::move(html), d.files);
            }
        }
        setMarkdownFormatting(md);
    }
    base::setUse24h(_settings.use24h);
    // Dates follow a language change at once (UI text at the next start).
    if (_settings.language != _builtLanguage)
        base::setDateLanguage(_settings.language);
    _ctx.linkPreviews   = _settings.linkPreviews;
    _ctx.threadsInline  = _settings.threadsInline;
    // Settings → System → Presence: global, applied to every running
    // workspace (Accounts hands it to every new one).
    const auto presence = model::Backend::PresenceMode(_settings.presence);
    if (_running.empty())
        _ctx.backend.setPresenceMode(presence);
    for (const auto &r : _running)
        r->backend->setPresenceMode(presence);
    // Settings → Appearance → Names: the same, per workspace Store.
    const auto names = model::Backend::NamesMode(_settings.names);
    if (_running.empty())
        _ctx.backend.setNamesMode(names);
    for (const auto &r : _running)
        r->backend->setNamesMode(names);
    // Settings → Check spelling: loads (or frees) the dictionaries; the
    // composers re-check when it is ready.
    spell::Checker::instance().configure(
        _ctx.app.platform(), _settings.spellCheck, _settings.spellLanguages
    );
    // Settings → AI assistance into the LLM layer: the providers, the default
    // and the language AI features answer in (the app's until one is picked).
    if (_ctx.ai) {
        std::vector<llm::Provider> providers;
        for (const AiProvider &p : _settings.ai)
            providers.push_back(llm::fromSettings(p.id, p.name, p.url, p.key, p.model, p.sttModel));
        _ctx.ai->setProviders(std::move(providers), _settings.aiDefault);
        _ctx.ai->setLanguage(_settings.effectiveAiLanguage());
        // …and Voice input's glossary (one term per line) and clean-up.
        std::vector<std::string> glossary;
        for (std::string_view rest = _settings.voiceGlossary; !rest.empty();) {
            const size_t nl = rest.find('\n');
            if (const std::string_view t = str::trim(rest.substr(0, nl)); !t.empty())
                glossary.emplace_back(t);
            rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        }
        _ctx.ai->voice().setGlossary(std::move(glossary));
        _ctx.ai->voice().setCleanup(_settings.voiceCleanup);
    }
#ifdef MSGA_HAVE_MESSAGES
    // The cache limit bounds the decoded-image cache too, which never grows
    // past its own default.
    constexpr size_t kImageBudget = size_t(48) << 20;
    _ctx.images.setBudget(std::min(kImageBudget, size_t(_settings.cacheLimitMb) << 20));
    _ctx.images.setAnimate(_settings.animateMedia);
    _ctx.images.setAnimateEmoji(_settings.animateEmoji);
    static_cast<screens::MessageList *>(_messages)->setThreadsInline(_settings.threadsInline);
#endif
    // …and the downloaded pictures on disk (the cache cap).
    if (_ctx.remote)
        _ctx.remote->setLimitMb(_settings.cacheLimitMb);
    // Rows bake the time format and link previews in: rebuild what is shown.
    if (_builtPreviews != _settings.linkPreviews || _built24h != _settings.use24h ||
        _builtAnimate != _settings.animateMedia || _builtEmoji != _settings.animateEmoji ||
        _builtLanguage != _settings.language) {
        _builtPreviews = _settings.linkPreviews;
        _built24h      = _settings.use24h;
        _builtLanguage = _settings.language;
        _builtAnimate  = _settings.animateMedia;
        _builtEmoji    = _settings.animateEmoji;
        if (_current != kNoConv) {
#ifdef MSGA_HAVE_MESSAGES
            static_cast<screens::MessageList *>(_messages)->showConversation(_current);
#else
            static_cast<MessageStandIn *>(_messages)->setTarget(_current, 0);
#endif
            if (threadOpen())
                _thread->show(_thread->conv, _thread->root);
        }
    }
    // Settings → Appearance (relevant days, Agents & apps, unreads only) and
    // Notifications (the default level, "Highlight mentions-only channels").
    Sidebar::Filters f;
    f.relevantDays          = _settings.relevantDays;
    f.showAgentsApps        = _settings.showAgentsApps;
    f.unreadsOnly           = _settings.unreadsOnly;
    f.highlightMentionsOnly = _settings.boldMentionsOnly;
    f.defaultLevel          = defaultLevel();
    _sidebar->setFilters(f);
    for (const auto &r : _running)
        r->dirty = true; // the default level decides what counts
    // Settings → System → Use tray: the icon comes and goes at once. Without
    // it the window's close quits and minimizing leaves it on the taskbar.
    if (_settings.useTray && !_tray) {
        _tray = _ctx.app.platform().createTray();
        if (_tray) {
            _tray->setTooltip("MSGA");
            rebuildTrayMenu();
        }
    } else if (!_settings.useTray && _tray) {
        if (_hidden)
            restore(); // never left hidden with no icon to bring it back
        _tray.reset();
    }
    _lastTray = -1; // the tray picture may have changed
    updateAttention();
}

void Shell::showQuickSwitcher() {
    // Not over another dialog (nor over itself): the switcher navigates the
    // window behind the backdrop.
    if (topDialog())
        return;
    // One tab per running workspace, in the rail's order, each listing its
    // member conversations by name (the visit stamps are app-wide).
    std::vector<QuickSwitchTab> tabs;
    for (const LiveWorkspace &w : liveWorkspaces()) {
        tabs.push_back({w.key, w.name, w.icon, w.store, {}});
        if (w.store != &_ctx.store()) // the open one's comes in below
            tabs.back().order = quickSwitchOrder(*w.store, _sidebar->visited());
    }
    auto p = std::make_unique<QuickSwitcher>(
        _ctx, _avatars, quickSwitchOrder(_ctx.store(), _sidebar->visited()), std::move(tabs)
    );
    auto *raw       = p.get();
    raw->onChooseIn = [this](const std::string &key, ConvRef conv) {
        // The same path as a notification's click: the workspace, then the chat.
        if (onSwitchWorkspace)
            onSwitchWorkspace(key);
        const Running *r = findRunning(key);
        if (r && r->store == &_ctx.store() && _signedIn)
            open(conv);
    };
    const SizeF ws = _win.size();
    p->setAnchor({(ws.w - 520) / 2, 90, 520, 0}, Popup::Place::Below);
    p->onClosed = [this, raw] {
        if (_switcher == raw)
            _switcher = nullptr;
    };
    _switcher = raw;
    _win.showPopup(std::move(p));
    raw->field().focus();
}

plat::HitArea Shell::hitTest(plat::Point p) const {
    using H           = plat::HitArea;
    plat::Window &nat = _win.native();
    const SizeF   sz  = _win.size();
#ifdef __APPLE__
    // AppKit resizes from its own edge zone (on and just outside the frame);
    // ours would only leave a dead band along the unified header's edges,
    // where the whole title bar drags.
    const bool resizable = p.y >= _titleBarH;
#else
    const bool resizable = true;
#endif
    if (resizable && !nat.isMaximized() && !nat.isFullscreen()) {
        const bool l = p.x < kResizeEdge, r = p.x >= sz.w - kResizeEdge;
        const bool t = p.y < kResizeEdge, b = p.y >= sz.h - kResizeEdge;
        if (t && l)
            return H::ResizeTopLeft;
        if (t && r)
            return H::ResizeTopRight;
        if (b && l)
            return H::ResizeBottomLeft;
        if (b && r)
            return H::ResizeBottomRight;
        if (l)
            return H::ResizeLeft;
        if (r)
            return H::ResizeRight;
        if (b)
            return H::ResizeBottom;
        if (t && !(_closeBtn && _closeBtn->windowRect().contains({float(p.x), float(p.y)})))
            return H::ResizeTop;
    }
    if (p.y >= _titleBarH)
        return H::Client;
    const PointF pt{float(p.x), float(p.y)};
#ifdef __APPLE__
    // The unified header: its controls (and whatever a popup puts over it)
    // take their presses; the title, the empty space and a dialog's backdrop
    // drag the window. The traffic lights never get
    // here: AppKit keeps their presses (PlatNSWindow).
    View *v = _win.viewAt(pt);
    if (!v || _win.topPopup([v](const Popup &d) {
            return static_cast<const View *>(&d) == v && d.place() == Popup::Place::Fill;
        }))
        return H::Caption;
    for (View *a = v; a; a = a->parent()) {
        if (a == _titleBar)
            return H::Caption;
        if (a->focusable() || a->role() == Role::Button)
            return H::Client;
    }
    return H::Client; // in a popup over the band (a menu, a card)
#endif
    if (_pinBtn && _pinBtn->windowRect().contains(pt))
        return H::Client;
    if (_minBtn && _minBtn->windowRect().contains(pt))
        return H::MinimizeButton;
    if (_maxBtn && _maxBtn->windowRect().contains(pt))
        return H::MaximizeButton;
    if (_closeBtn && _closeBtn->windowRect().contains(pt))
        return H::CloseButton;
    return H::Caption;
}

// The sidebar's stamps into the settings, without those older than twice the
// relevance window (pruned, so the store never grows).
void Shell::storeVisited() {
    const int64_t horizon =
        _ctx.backend.nowSecs() - int64_t(std::max(1, _settings.relevantDays)) * 86400 * 2;
    _settings.visitedAt.clear();
    for (const auto &[id, at] : _sidebar->visited())
        if (at >= horizon)
            _settings.visitedAt.emplace_back(id, at);
    std::sort(_settings.visitedAt.begin(), _settings.visitedAt.end()); // a stable file
}

void Shell::loadSorts() {
    Sidebar::Sorts sorts;
    for (const Settings::SectionSortPref &p : _settings.sidebarSorts)
        if (p.workspace == _activeKey)
            sorts[p.section] = SectionSort::decode(p.sort);
    _sidebar->setSorts(std::move(sorts));
}

void Shell::storeSorts() {
    std::erase_if(_settings.sidebarSorts, [this](const Settings::SectionSortPref &p) {
        return p.workspace == _activeKey;
    });
    for (const auto &[section, sort] : _sidebar->sorts())
        _settings.sidebarSorts.push_back({_activeKey, section, sort.encode()});
    std::sort( // a stable file
        _settings.sidebarSorts.begin(),
        _settings.sidebarSorts.end(),
        [](const Settings::SectionSortPref &a, const Settings::SectionSortPref &b) {
            return std::tie(a.workspace, a.section) < std::tie(b.workspace, b.section);
        }
    );
}

void Shell::saveState() {
    _composer->stashNow();
    storeVisited();
    if (_thread)
        _thread->composer->stashNow();
    plat::Window &nat   = _win.native();
    _settings.maximized = nat.isMaximized();
    if (!_settings.maximized && !nat.isFullscreen()) {
        const plat::Size s = nat.size();
        _settings.width    = s.w;
        _settings.height   = s.h;
        if (const auto pos = nat.position()) {
            _settings.hasPosition = true;
            _settings.x           = int(pos->x);
            _settings.y           = int(pos->y);
        }
    }
    saveSettingsNow();
}

} // namespace shell
