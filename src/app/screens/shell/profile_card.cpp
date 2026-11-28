#include "screens/shell/profile_card.h"

#include "app/mrkdwn/emoji.h"
#include "app/screens/common/avatar_initial.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/shell/shell_text.h"

#include <algorithm>
#include <cmath>

using namespace ui;
using gfx::Icon;
using i18n::tr;

namespace shell {

namespace {

// The card's metrics.
constexpr float kShadow = 8, kRadius = 8, kPad = 16, kAv = 72, kAvRadius = 8, kAvGap = 14;
constexpr float kBtnH = 36, kGap = 6, kDot = 8, kDotGap = 8;
constexpr int   kCopiedMs = 1200;

// The theme colours the card uses, light then dark.
enum Tone : uint8_t {
    Raised,
    BorderStrong,
    Divider,
    Highlight,
    Primary,
    Secondary,
    Tertiary,
    IconDef,
    AccentDef,
    Online,
    NTones
};
constexpr C kTones[NTones] = {
    C::FormBg,
    C::FormDividerStrong,
    C::FormDivider,
    C::FormHighlight,
    C::FormText,
    C::FormTextMuted,
    C::FormTextFaint,
    C::FormIcon,
    C::Accent,
    C::Online,
};

Color tone(Tone t) {
    return color(kTones[t]);
}

// These are sized in pixels (fonts.xl 16, fonts.md 13); scaled with the
// text-size preference like every other role.
text::Style px(float size, bool bold, Tone t) {
    return pxFont(size, bold ? text::Weight::Bold : text::Weight::Regular, tone(t));
}

uint32_t nowMs() {
    return uint32_t(app()->nowMs());
}

} // namespace

ProfileCard::ProfileCard(
    screens::Context &ctx, Avatars &avatars, ProfileCards &owner, model::UserRef user
)
    : _ctx(ctx), _owner(owner), _user(user) {
    setCard(false);  // painted here
    setModal(false); // a hover card: no focus grab, no outside-press close
    setPaintOutset(uint8_t(kShadow + 4));
    _avatar  = avatars.get(ctx.store().user(user).avatar, 144);
    _message = true; // hidden only for deactivated accounts
}

ProfileCard::~ProfileCard() {
    if (_copiedTimer)
        app()->cancelTimer(_copiedTimer);
}

void ProfileCard::placeBeside(RectF t) {
    // Above, left edge at the target's minus the padding (so the avatars
    // line up); Popup flips it below when there is no room.
    setAnchor({t.x - kPad, t.y - (kGap - 4), t.w, t.h + 2 * (kGap - 4)}, Place::Above);
}

void ProfileCard::styleChanged() {
    _name.reset();
    _initialLayout.reset();
    update();
}

// The email line ("Copied" for a moment after a click), in its hover look.
void ProfileCard::buildEmail() {
    const model::User &u      = _ctx.store().user(_user);
    const bool         copied = _copiedUntil && int32_t(_copiedUntil - nowMs()) > 0;
    if (u.email.empty()) {
        _email = nullptr;
        return;
    }
    text::Style s = px(13, false, copied || _emailHover ? AccentDef : Primary);
    s.underline   = _emailHover && !copied;
    _email        = oneLineLayout(
        copied ? std::string(tr("Copied")) : u.email, s, kWidth - 2 * kPad - 24, windowScale()
    );
}

// The email line alone (its hover colour, "Copied"): the rest stays as built.
// Nothing built yet: build() makes it with the rest.
void ProfileCard::refreshEmail() {
    if (_name)
        buildEmail();
}

void ProfileCard::build() {
    const model::User &u     = _ctx.store().user(_user);
    const float        k     = windowScale();
    const float        textW = kWidth - 2 * kPad - kAv - kAvGap;
    const char        *role  = u.owner   ? tr("Workspace Owner")
                               : u.admin ? tr("Workspace Admin")
                               : u.bot   ? tr("App")
                                         : nullptr;
    _role = role ? oneLineLayout(role, px(13, true, Primary), kWidth - 2 * kPad, k) : nullptr;
    _name = oneLineLayout(u.label(), px(16, true, Primary), textW - kDotGap - kDot, k);
    std::string status = emoji::toUnicode(u.statusEmoji);
    if (!u.statusText.empty())
        status = status.empty() ? u.statusText : str::concat({status, " ", u.statusText});
    _status = status.empty() ? nullptr : oneLineLayout(status, px(13, false, Secondary), textW, k);
    _title = u.title.empty() ? nullptr : oneLineLayout(u.title, px(13, false, Secondary), textW, k);
    buildEmail();
    if (u.hasTz) {
        const int64_t now   = base::nowSecs();
        const int64_t local = now + u.tzOffset - base::localTime(now).utcOffset;
        _clock              = oneLineLayout(
            i18n::arg(tr("%1 local time"), base::formatTime(local)),
            px(13, false, Primary),
            kWidth,
            k
        );
    } else {
        _clock = nullptr;
    }
    _btn = oneLineLayout(tr("Message"), px(13, true, Primary), kWidth, k);

    const float detailH = std::ceil(px(13, false, Primary).size * 1.4f);
    _headerH            = _role ? 34 : 0;
    const float statusH = _status ? detailH + 4 : 0, titleH = _title ? detailH + 4 : 0;
    const float textColH = std::ceil(_name->height()) + statusH + titleH;
    _bodyH               = kPad + std::max(kAv, textColH) + kPad;
    _emailH              = _email ? 24 : 0;
    _clockH              = _clock ? 24 : 0;
    float bottomH        = 0;
    if (_emailH > 0 || _clockH > 0 || _message) {
        bottomH = 12 + _emailH + (_clockH > 0 ? (_emailH > 0 ? 6 : 0) + _clockH : 0) + 12;
        if (_message)
            bottomH += ((_emailH > 0 || _clockH > 0) ? 10 : 0) + kBtnH;
    }
    _cardH = _headerH + _bodyH + (bottomH > 0 ? 1 + bottomH : 0);
}

SizeF ProfileCard::measureContent(float, float) {
    if (!_name)
        build();
    return {kWidth, _cardH};
}

RectF ProfileCard::emailRow() const {
    if (_emailH <= 0)
        return {};
    return {kPad, _headerH + _bodyH + 1 + 12, kWidth - 2 * kPad, _emailH};
}

RectF ProfileCard::messageButton() const {
    if (!_message || !_btn)
        return {};
    float rows = _headerH + _bodyH + 1 + 12 + _emailH;
    if (_clockH > 0)
        rows += (_emailH > 0 ? 6 : 0) + _clockH;
    rows += (_emailH > 0 || _clockH > 0) ? 10 : 0;
    return {kPad, rows, std::ceil(12 + 16 + 8 + _btn->width() + 12), kBtnH};
}

void ProfileCard::paint(gfx::Painter &p) {
    if (!_name)
        build();
    const model::User &u = _ctx.store().user(_user);
    const RectF        b = bounds();
    p.dropShadow(b, kRadius, kShadow, {0, 4}, color(C::Shadow)); // dy 4
    p.fillRoundRect(b, kRadius, tone(Raised));
    float y = 0;
    if (_role) { // the role strip
        p.save();
        p.clipRoundRect(b, kRadius);
        p.fillRect({0, 0, kWidth, _headerH}, tone(Highlight));
        p.restore();
        p.fillRect({0, _headerH, kWidth, 1}, tone(Divider));
        _role->paint(p, snapPx({kPad, std::floor((_headerH - _role->height()) / 2)}));
        y = _headerH;
    }
    // Avatar, then name + presence, status and title centred beside it.
    const RectF av{kPad, y + kPad, kAv, kAv};
    if (_avatar && !_avatar->empty()) { // empty: still downloading
        p.save();
        p.clipRoundRect(av, kAvRadius);
        // Shrunk once for the pixels it lands on (drawBitmap would
        // area-average on every paint), as ui::Image does.
        const gfx::Bitmap *src = _avatar.get();
        int                dw = 0, dh = 0;
        p.snappedSize(av, &dw, &dh);
        if (dw > 0 && dh > 0 && (src->width() > dw || src->height() > dh)) {
            if (_shrunk.src != src->pixels() || _shrunk.sw != src->width() ||
                _shrunk.sh != src->height() || _shrunk.bmp.width() != dw ||
                _shrunk.bmp.height() != dh)
                _shrunk = {
                    src->pixels(), src->width(), src->height(), gfx::resize(src->view(), dw, dh)
                };
            src = &_shrunk.bmp;
        }
        p.drawBitmap(src->view(), av, gfx::Sampling::Smooth);
        p.restore();
    } else { // the initial-letter tile on presence.away
        screens::paintInitial(
            p,
            *this,
            av,
            kAvRadius,
            ui::color(C::PresenceAway),
            screens::avatarInitial(_ctx.store().user(_user).label()),
            _initialLayout
        );
    }
    const float detailH = std::ceil(px(13, false, Primary).size * 1.4f);
    const float textX   = kPad + kAv + kAvGap;
    const float colH =
        std::ceil(_name->height()) + (_status ? detailH + 4 : 0) + (_title ? detailH + 4 : 0);
    float ty = av.y + std::max(0.f, std::floor((kAv - colH) / 2));
    _name->paint(p, snapPx({textX, ty}));
    const float  cy = ty + _name->height() / 2;
    const PointF dot{textX + _name->width() + kDotGap + kDot / 2, cy};
    if (u.dnd) { // presence.away with a surface.raised dash across
        p.fillCircle(dot, kDot / 2, ui::color(C::PresenceAway));
        p.fillRoundRect(
            {dot.x - kDot / 2 + 2, dot.y - 0.75f, kDot - 4, 1.5f}, 0.75f, ui::color(C::FormBg)
        );
    } else if (u.active)
        p.fillCircle(dot, kDot / 2, tone(Online));
    else
        p.strokeCircle(dot, kDot / 2 - 0.6f, 1.2f, tone(Tertiary));
    ty += std::ceil(_name->height());
    for (const text::Layout *l : {_status.get(), _title.get()})
        if (l) {
            l->paint(p, snapPx({textX, ty + std::floor((detailH + 4 - l->height()) / 2)}));
            ty += detailH + 4;
        }
    if (_emailH <= 0 && _clockH <= 0 && !_message)
        return;
    const float div = _headerH + _bodyH;
    p.fillRect({0, div, kWidth, 1}, tone(Divider));
    if (_email) {
        const RectF r      = emailRow();
        const bool  copied = _copiedUntil && int32_t(_copiedUntil - nowMs()) > 0;
        gfx::drawIcon(
            p,
            copied ? Icon::Check : Icon::Mail,
            {r.x, r.y + (r.h - 16) / 2, 16, 16},
            copied ? tone(AccentDef) : tone(IconDef)
        );
        _email->paint(p, snapPx({r.x + 24, r.y + std::floor((r.h - _email->height()) / 2)}));
    }
    if (_clock) {
        const float cyTop = div + 1 + 12 + (_emailH > 0 ? _emailH + 6 : 0);
        gfx::drawIcon(p, Icon::Clock, {kPad, cyTop + (_clockH - 16) / 2, 16, 16}, tone(IconDef));
        _clock->paint(p, snapPx({kPad + 24, cyTop + std::floor((_clockH - _clock->height()) / 2)}));
    }
    if (_message) {
        const RectF r = messageButton();
        if (_btnHover)
            p.fillRoundRect(r, 8, tone(Highlight));
        p.strokeRoundRect(r, 8, 1, tone(BorderStrong));
        gfx::drawIcon(
            p, Icon::MessageSquare, {r.x + 12, r.y + (kBtnH - 16) / 2, 16, 16}, tone(Primary)
        );
        _btn->paint(p, snapPx({r.x + 36, r.y + std::floor((kBtnH - _btn->height()) / 2)}));
    }
    p.strokeRoundRect(b, kRadius, 1, tone(BorderStrong));
}

bool ProfileCard::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerEnter:
        _owner.cancelHide();
        return false;
    case EventType::PointerLeave:
        if (_btnHover || _emailHover) {
            _btnHover = _emailHover = false;
            refreshEmail();
            update();
        }
        _owner.scheduleHide();
        return false;
    case EventType::PointerDown:
        return true;
    case EventType::PointerMove: {
        const bool btn = messageButton().contains(e.pos), mail = emailRow().contains(e.pos);
        if (btn != _btnHover || mail != _emailHover) {
            _btnHover   = btn;
            _emailHover = mail;
            if (_email)
                refreshEmail(); // the email line's colour
            update();
        }
        setCursor(btn || mail ? plat::Cursor::Hand : plat::Cursor::Arrow);
        return true;
    }
    case EventType::PointerUp:
        if (messageButton().contains(e.pos)) {
            // "Message": the DM (opened or
            // created), or a teammate's page.
            if (_ctx.messageUser)
                _ctx.messageUser(_user);
            _owner.hideNow(); // deferred destruction: `this` stays valid here
        } else if (emailRow().contains(e.pos)) {
            _ctx.app.platform().setClipboardText(_ctx.store().user(_user).email);
            _copiedUntil = nowMs() + kCopiedMs;
            refreshEmail();
            update();
            app()->cancelTimer(_copiedTimer);
            _copiedTimer = app()->addTimer(kCopiedMs + 20, false, [this] {
                _copiedTimer = 0;
                refreshEmail();
                update();
            });
        }
        return true;
    default:
        return Popup::onEvent(e);
    }
}

// ── ProfileCards ────────────────────────────────────────────────────────────

namespace {
constexpr int kShowDelayMs = 300; // hover before the card shows
constexpr int kHideDelayMs = 260; // grace before it hides
} // namespace

ProfileCards::ProfileCards(screens::Context &ctx, Window &win, Avatars &avatars)
    : _ctx(ctx), _win(win), _avatars(avatars) {}

ProfileCards::~ProfileCards() {
    app()->cancelTimer(_showTimer);
    app()->cancelTimer(_hideTimer);
    if (_card)
        _card->onClosed = nullptr;
}

void ProfileCards::hover(model::UserRef u, RectF anchor, int mode) {
    if (mode == Leave) {
        app()->cancelTimer(_showTimer);
        _showTimer = 0;
        _pending   = model::kNoUser;
        scheduleHide();
        return;
    }
    if (_card && _card->user() == u) {
        cancelHide();
        return;
    }
    if (mode == Click) { // no hover delay
        app()->cancelTimer(_showTimer);
        _showTimer = 0;
        show(u, anchor);
        return;
    }
    if (_pending == u && _showTimer)
        return;
    _pending   = u;
    _pendingAt = anchor;
    app()->cancelTimer(_showTimer);
    _showTimer = app()->addTimer(kShowDelayMs, false, [this] {
        _showTimer = 0;
        show(_pending, _pendingAt);
    });
}

void ProfileCards::show(model::UserRef u, RectF anchor) {
    if (u >= _ctx.store().userCount())
        return;
    hideNow();
    cancelHide();
    auto p = std::make_unique<ProfileCard>(_ctx, _avatars, *this, u);
    if (anchor.w > 0 || anchor.h > 0) {
        p->placeBeside(anchor);
    } else {
        const SizeF ws = _win.size();
        p->setAnchor({(ws.w - ProfileCard::kWidth) / 2, 80, ProfileCard::kWidth, 0});
    }
    ProfileCard *raw = p.get();
    raw->onClosed    = [this, raw] {
        if (_card == raw)
            _card = nullptr;
    };
    _card = raw;
    _win.showPopup(std::move(p));
    _win.requestFrame();
}

void ProfileCards::hideNow() {
    app()->cancelTimer(_hideTimer);
    _hideTimer = 0;
    if (ProfileCard *c = _card) {
        _card = nullptr;
        c->close();
    }
}

void ProfileCards::cancelHide() {
    app()->cancelTimer(_hideTimer);
    _hideTimer = 0;
}

void ProfileCards::scheduleHide() {
    if (!_card || _hideTimer)
        return;
    _hideTimer = app()->addTimer(kHideDelayMs, false, [this] {
        _hideTimer = 0;
        hideNow();
        _win.requestFrame();
    });
}

} // namespace shell
