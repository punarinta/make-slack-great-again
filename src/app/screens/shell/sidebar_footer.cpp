#include "screens/shell/sidebar_footer.h"

#include "base/i18n.h"
#include "gfx/icons_generated.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/sidebar.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace ui;
using gfx::Icon;
using i18n::tr;

namespace shell {

namespace {
constexpr float kHeight = 64, kPad = 12, kBtn = 40, kBottomPad = 14;
constexpr int   kConfirmMs  = 5000; // the optimistic icon reverts after this
constexpr float kTaskGap    = 8;    // between the task spinner and the toggle
// The spinner's pace: kSpinDegStep per kSpinTickMs, a full turn in ~2 s.
// Here the frame clock paces it; the angle follows the elapsed time.
constexpr float kSpinTickMs = 16, kSpinDegStep = 3;
constexpr float kTipPadH = 10, kTipPadV = 5, kTipRadius = 6, kTipArrowW = 7, kTipArrowH = 6;
constexpr float kTipHeaderGapV = 5, kTipMaxW = 320;

std::string tasksHeader(size_t n) {
    return i18n::trn("%n background task running", "%n background tasks running", int64_t(n));
}

// The task list: the tooltip chip above the spinner, a
// small dimmed count header, then one running job per line (elided at 320).
class TaskListPopup final : public Popup {
public:
    TaskListPopup() {
        setModal(false);
        setCard(false);
        setRole(Role::Tooltip);
        setPaintOutset(6);
    }
    View *hitTest(PointF) override { return nullptr; } // never blocks the pointer
    // The jobs changed: the count and the list follow.
    void  reload() {
        _lines.clear();
        invalidateLayout();
        update();
    }
    SizeF measureContent(float, float) override {
        build();
        float w = 0, h = 0;
        for (const auto &l : _lines) {
            w = std::max(w, l->width());
            h += l->height();
        }
        if (!_lines.empty())
            h += kTipHeaderGapV;
        return {
            std::ceil(std::min(w, kTipMaxW)) + 2 * kTipPadH,
            std::ceil(h) + 2 * kTipPadV + kTipArrowH
        };
    }
    void styleChanged() override { reload(); }
    void paint(gfx::Painter &p) override {
        build();
        const RectF w     = windowRect();
        const bool  below = w.y > anchor().y;
        const RectF body{0, below ? kTipArrowH : 0, width(), height() - kTipArrowH};
        const Color bg = color(C::TooltipBg);
        p.dropShadow(body, kTipRadius, 5, {0, 1}, 0x30000000U);
        p.fillRoundRect(body, kTipRadius, bg);
        const float cx =
            std::clamp(anchor().x + anchor().w / 2 - w.x, kTipArrowW, width() - kTipArrowW);
        gfx::Path a;
        if (below) {
            a.moveTo(cx - kTipArrowW, body.y + 3);
            a.lineTo(cx + kTipArrowW, body.y + 3);
            a.lineTo(cx, 0);
        } else {
            a.moveTo(cx - kTipArrowW, body.bottom() - 3);
            a.lineTo(cx + kTipArrowW, body.bottom() - 3);
            a.lineTo(cx, body.bottom() + kTipArrowH);
        }
        a.close();
        p.fillPath(a, bg);
        float y = body.y + kTipPadV;
        for (size_t i = 0; i < _lines.size(); ++i) {
            _lines[i]->paint(p, snapPx({kTipPadH, y}));
            y += _lines[i]->height() + (i == 0 ? kTipHeaderGapV : 0);
        }
    }
    // The header, then the jobs, one per line.
    std::string accessibleName() const override {
        std::string out = tasksHeader(model::jobs().count());
        for (const std::string &d : model::jobs().descriptions())
            out += "\n" + d;
        return out;
    }

private:
    void build() {
        if (!_lines.empty())
            return;
        const float k    = windowScale();
        text::Style head = font(Font::Body);
        head.size *= 0.82f;
        head.color = (color(C::TooltipText) & 0x00ffffffu) | 0xa0000000u; // onDark at 160
        _lines.push_back(oneLineLayout(tasksHeader(model::jobs().count()), head, kTipMaxW, k));
        text::Style st = font(Font::Body);
        st.weight      = text::Weight::Medium;
        st.color       = color(C::TooltipText);
        for (const std::string &d : model::jobs().descriptions())
            _lines.push_back(oneLineLayout(d, st, kTipMaxW, k));
    }
    std::vector<std::unique_ptr<text::Layout>> _lines;
};

} // namespace

// The background-task spinner: the ghost chrome with a turning cog. A
// status indicator, not a button (the arrow cursor, no focus); it turns only
// while jobs run.
class TaskSpinner final : public GhostButton {
public:
    TaskSpinner() : GhostButton(Icon::Cog, {}) {
        setFocusable(false);
        setCursor(plat::Cursor::Arrow);
        setRole(Role::Image);
    }
    std::function<void(bool on)> onHover;
    void                         run() {
        if (!_running)
            _last = -1;
        _running = true;
        startTicking(); // a no-op while it ticks
    }
    void stop() { _running = false; } // the next tick ends the animation
    bool tick(double nowMs) override {
        if (!_running || !visible() || model::jobs().count() == 0) {
            _running = false;
            return false;
        }
        if (_last >= 0)
            _angle = std::fmod(_angle + float(nowMs - _last) * kSpinDegStep / kSpinTickMs, 360.f);
        _last = nowMs;
        update();
        return true;
    }
    void paint(gfx::Painter &p) override {
        paintChrome(p);
        paintGlyphTurned(p, Icon::Cog, _angle);
    }
    bool onEvent(Event &e) override {
        if ((e.type == EventType::PointerEnter || e.type == EventType::PointerLeave) && onHover)
            onHover(e.type == EventType::PointerEnter);
        return GhostButton::onEvent(e);
    }
    std::string accessibleName() const override { return tasksHeader(model::jobs().count()); }
    bool        running() const { return _running; }

private:
    double _last    = -1;
    float  _angle   = 0;
    bool   _running = false;
};

// The ghost button with a ~100 ms cross-fade between the two icons.
class PresenceToggle final : public GhostButton {
public:
    PresenceToggle() : GhostButton(Icon::CircleUserRound, {}) {}
    void show(bool hidden, bool animate) {
        if (hidden == _hidden && _t >= 1)
            return;
        _from   = _hidden;
        _hidden = hidden;
        _t      = animate ? 0 : 1;
        update();
        if (animate)
            startTicking();
    }
    bool hidden() const { return _hidden; }
    bool tick(double nowMs) override {
        if (_start < 0)
            _start = nowMs;
        _t = float(std::min(1.0, (nowMs - _start) / 100.0));
        update();
        if (_t < 1)
            return true;
        _start = -1;
        return false;
    }
    void paint(gfx::Painter &p) override {
        paintChrome(p);
        const Icon to = _hidden ? Icon::HatGlasses : Icon::CircleUserRound;
        if (_t >= 1) {
            paintGlyph(p, to);
            return;
        }
        p.save();
        p.setOpacity(1 - _t);
        paintGlyph(p, _from ? Icon::HatGlasses : Icon::CircleUserRound);
        p.restore();
        p.save();
        p.setOpacity(_t);
        paintGlyph(p, to);
        p.restore();
    }

private:
    double _start  = -1;
    float  _t      = 1;
    bool   _hidden = false, _from = false;
};

// The avatar: a left or right click opens the profile/status menu.
class FooterAvatar final : public Clickable {
public:
    bool onEvent(Event &e) override {
        if (e.type == EventType::ContextMenu && onClick) {
            onClick();
            return true;
        }
        return Clickable::onEvent(e);
    }
};

SidebarFooter::SidebarFooter(Sidebar &, screens::Context &ctx, Avatars &avatars)
    : _ctx(ctx), _avatars(avatars) {
    style().row().height(kHeight).noShrink().padding(kPad, 0, kPad, kBottomPad);
    style().items(Align::End);
    _avatarBtn = add<FooterAvatar>();
    _avatarBtn->style().size(kBtn, kBtn).noShrink();
    _avatarBtn->setLook({C::None, C::None, C::None, C::None, 10});
    _avatarBtn->setRole(Role::Button);
    _avatar = _avatarBtn->add<Avatar>();
    _avatar->style().size(kBtn, kBtn);
    _avatar->setRadius(10);
    _avatar->setPlaceholder(C::PresenceAway);
    _avatar->setHitTransparent(true);
    _avatarBtn->onClick = [this] { showAvatarMenu(); };
    add<View>()->style().flex(1);
    // Left of the toggle (kTaskGap apart; refresh()), at the right edge
    // without one.
    _tasks = add<TaskSpinner>();
    _tasks->setVisible(false);
    _tasks->onHover = [this](bool on) {
        if (on)
            showTasksPopup();
        else
            hideTasksPopup();
    };
    _toggle          = add<PresenceToggle>();
    _toggle->onClick = [this] { togglePresence(); };
    _zen             = add<GhostButton>(Icon::Eye, std::string());
    _zen->onClick    = [this] { toggleZen(); };
    _zen->setVisible(false);
    setZenOn(false);
    _jobsObserver = model::jobs().observe([this] { jobsChanged(); });
    jobsChanged();
}

View *SidebarFooter::tasksIndicator() const {
    return _tasks;
}

bool SidebarFooter::tasksTurning() const {
    return _tasks->running();
}

void SidebarFooter::jobsChanged() {
    const bool running = model::jobs().count() > 0;
    if (running != _tasks->visible())
        _tasks->setVisible(running);
    if (!running) {
        _tasks->stop();
        hideTasksPopup();
        return;
    }
    _tasks->run();
    if (_tasksPopup)
        static_cast<TaskListPopup *>(_tasksPopup)->reload();
}

void SidebarFooter::showTasksPopup() {
    Window *w = window();
    if (!w || !_tasks->visible() || model::jobs().count() == 0)
        return;
    if (!_tasksPopup) {
        auto p      = std::make_unique<TaskListPopup>();
        p->onClosed = [this] { _tasksPopup = nullptr; };
        _tasksPopup = w->showPopup(std::move(p));
    }
    _tasksPopup->setAnchor(_tasks->windowRect(), Popup::Place::Tip);
}

void SidebarFooter::hideTasksPopup() {
    if (Popup *p = std::exchange(_tasksPopup, nullptr)) {
        p->onClosed = nullptr;
        p->close();
    }
}

void SidebarFooter::setZenOn(bool on) {
    _zenOn = on;
    _zen->setIcon(on ? Icon::Leaf : Icon::Eye);
    _zen->setTooltip(
        on ? tr("Zen mode is on: tool calls are hidden. Click to show everything.")
           : tr("Zen mode is off. Click to hide tool calls for easier reading.")
    );
    _zen->update();
}

void SidebarFooter::toggleZen() {
    setZenOn(!_zenOn);
    if (onZenToggled)
        onZenToggled(_zenOn);
}

void SidebarFooter::toggleRail() {
    _railShown = !_railShown;
    if (onRailToggled)
        onRailToggled(_railShown);
}

Clickable *SidebarFooter::zenToggle() const {
    return _zen;
}

SidebarFooter::~SidebarFooter() {
    model::jobs().unobserve(_jobsObserver);
    hideTasksPopup();
    if (_confirm)
        _ctx.app.platform().cancelTimer(_confirm);
}

void SidebarFooter::refresh() {
    const auto &store = _ctx.store();
    const auto  caps  = _ctx.backend.capabilities();
    const auto  sp    = _ctx.backend.selfPresence();
    const auto &me    = store.user(store.me);
    // Presence is supported with presence && self presence: a zen-mode service
    // (Claude Code) has presence dots but no self presence to toggle.
    const bool  self  = caps.presence && !caps.zenMode;
    _avatar->setBitmap(_avatars.get(me.avatar, 80));
    _avatar->setInitial(me.label());
    using P           = Avatar::Presence;
    // phantomAway() is false while manually away: an explicit "hidden" shows
    // the hollow ring rather than the amber tint.
    const bool active = sp.loaded ? sp.active : me.active;
    // The footer never draws my own DND.
    _avatar->setPresence(
        !self              ? P::None
        : active           ? P::Active
        : sp.phantomAway() ? P::Phantom
                           : P::Away,
        C::Sidebar
    );
    _avatarBtn->setTooltip(caps.selfStatus ? tr("Profile & status") : tr("Profile"));
    _toggle->setVisible(self);
    _zen->setVisible(!self && caps.zenMode);
    _tasks->style().margins(0, 0, self || caps.zenMode ? kTaskGap : 0, 0);
    if (!_confirm)
        _toggle->show(sp.manualAway, false);
    // The presence tooltip: while away only for want of a connected client,
    // it says why the presence link isn't (yet) fixing that.
    using L          = model::Backend::PresenceLink;
    const L     link = _ctx.backend.presenceLink();
    const char *tip  = nullptr;
    if (_toggle->hidden())
        tip =
            tr("Hidden \xE2\x80\x94 you appear away to everyone. Click to use automatic "
               "presence.");
    else if (sp.phantomAway() && link == L::Idle)
        tip =
            tr("Away \xE2\x80\x94 you haven't used MSGA for a while. Any click or keystroke "
               "makes you active again (Settings \xE2\x86\x92 System \xE2\x86\x92 Presence).");
    else if (sp.phantomAway() && (link == L::Connecting || link == L::Active))
        tip =
            tr("Visible \xE2\x80\x94 connecting so you appear active without the official "
               "Slack app\xE2\x80\xA6"); // Active: Slack registers the socket a beat later
    else if (sp.phantomAway() && link == L::Unavailable)
        tip =
            tr("Visible \xE2\x80\x94 but you appear away while no official Slack app is "
               "connected. MSGA can't hold your presence on this workspace.");
    else if (sp.phantomAway())
        tip =
            tr("Visible \xE2\x80\x94 but you appear away while no official Slack app is "
               "connected. MSGA can keep you active: Settings \xE2\x86\x92 System "
               "\xE2\x86\x92 Presence.");
    else if (link == L::Active)
        tip = tr("Visible \xE2\x80\x94 MSGA keeps you active. Click to appear hidden.");
    else
        tip = tr("Visible \xE2\x80\x94 using automatic presence. Click to appear hidden.");
    _toggle->setTooltip(tip);
}

Clickable *SidebarFooter::toggle() const {
    return _toggle;
}

bool SidebarFooter::showsHidden() const {
    return _toggle->hidden();
}

void SidebarFooter::togglePresence() {
    // Optimistic: flip the icon now, ask the service, settle on its answer
    // (or, without one, back on the truth after kConfirmMs).
    const bool target = !_toggle->hidden();
    _toggle->show(target, true);
    auto &pa = _ctx.app.platform();
    if (_confirm)
        pa.cancelTimer(_confirm);
    _confirm = pa.addTimer(kConfirmMs, false, [this] {
        _confirm = 0;
        _toggle->show(_ctx.backend.selfPresence().manualAway, true);
        refresh();
    });
    _ctx.backend.setPresence(target, [this](bool ok, const std::string &err) {
        // A failure says so (with the re-sign-in
        // hint a missing scope needs).
        if (!ok && onError)
            onError(
                i18n::arg(tr("Could not change presence: %1"), err) +
                (err == "missing_scope"
                     ? tr(" \xE2\x80\x94 sign in to this workspace again to grant the new "
                          "permission")
                     : std::string())
            );
        if (_confirm) {
            _ctx.app.platform().cancelTimer(_confirm);
            _confirm = 0;
        }
        _toggle->show(_ctx.backend.selfPresence().manualAway, true);
        refresh();
    });
}

void SidebarFooter::showAvatarMenu() {
    Window *w = window();
    if (!w)
        return;
    std::vector<MenuItem> items(1);
    items[0].id      = 1;
    items[0].label   = tr("Manage profile");
    items[0].icon    = uint16_t(Icon::CircleUserRound);
    items[0].enabled = bool(onManageProfile);
    if (_ctx.backend.capabilities().selfStatus) {
        MenuItem st;
        st.id      = 2;
        st.label   = tr("Manage status");
        st.icon    = uint16_t(Icon::Smile);
        st.enabled = bool(onManageStatus);
        items.push_back(std::move(st));
    }
    MenuItem rail;
    rail.id    = 3;
    rail.label = _railShown ? tr("Hide workspaces") : tr("Show workspaces");
    rail.icon  = uint16_t(_railShown ? Icon::PanelLeftClose : Icon::PanelLeftOpen);
    items.push_back(std::move(rail));
    const RectF r = _avatarBtn->windowRect();
    Menu::show(
        *w,
        {r.x, r.y, 0, 0},
        std::move(items),
        [this](int id) {
            if (id == 3) {
                toggleRail();
                return;
            }
            auto &hook = id == 1 ? onManageProfile : onManageStatus;
            if (hook)
                hook();
        },
        Popup::Place::Above
    );
}

} // namespace shell
