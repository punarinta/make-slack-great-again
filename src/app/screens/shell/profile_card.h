// The profile card: a
// role strip for owners/admins/apps, the picture, name with presence, status
// and title, then email (click copies), local time and a "Message" button.
//
// It is a hover card: pointing at an avatar, a name or a mention
// shows it after 300 ms (clicking a mention or a name at once), and leaving
// both the target and the card hides it after a 260-ms grace period.
// ProfileCards owns that behaviour; Context::profileHover drives it.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <memory>
#include <string>

namespace shell {

class ProfileCards;

class ProfileCard final : public ui::Popup {
public:
    static constexpr float kWidth = 320;
    ProfileCard(screens::Context &ctx, Avatars &avatars, ProfileCards &owner, model::UserRef user);
    ~ProfileCard() override;
    model::UserRef user() const { return _user; }
    // Above `target` (falling back to below), its left edge where the
    // avatar's is, next to an avatar or a mention.
    void           placeBeside(ui::RectF target);

    ui::SizeF measureContent(float availW, float availH) override;
    void      paint(gfx::Painter &p) override;
    bool      onEvent(ui::Event &e) override;
    void      styleChanged() override;
    bool      showsBitmap(const gfx::Bitmap *b) const override { return _avatar.get() == b; }

    // Geometry (tests): the button and the email row, local coordinates.
    ui::RectF messageButton() const;
    ui::RectF emailRow() const;

private:
    void build();
    void buildEmail();
    void refreshEmail();

    screens::Context                  &_ctx;
    ProfileCards                      &_owner;
    model::UserRef                     _user;
    std::shared_ptr<const gfx::Bitmap> _avatar;
    std::unique_ptr<text::Layout>      _initialLayout; // the placeholder letter
    std::unique_ptr<text::Layout>      _role, _name, _status, _title, _email, _clock, _btn;
    float         _headerH = 0, _bodyH = 0, _emailH = 0, _clockH = 0, _cardH = 0;
    uint32_t      _copiedUntil = 0; // ms tick; "Copied" shows until then
    plat::TimerId _copiedTimer = 0;
    bool          _btnHover = false, _emailHover = false, _message = false;

    // _avatar shrunk to the pixels it is painted at.
    struct Shrunk {
        const uint32_t *src = nullptr; // the source's pixels (filled in place: new ones)
        int             sw = 0, sh = 0;
        gfx::Bitmap     bmp;
    } _shrunk;
};

class ProfileCards {
public:
    enum Mode : int { Leave = 0, Hover = 1, Click = 2 };
    ProfileCards(screens::Context &ctx, ui::Window &win, Avatars &avatars);
    ~ProfileCards();

    // The pointer entered (Hover) or left (Leave) something that stands for
    // `u`, or clicked it (Click); `anchor` is its window rect.
    void         hover(model::UserRef u, ui::RectF anchor, int mode);
    ProfileCard *card() const { return _card; }
    void         hideNow();

    // Internal (the card): the pointer entered / left it.
    void cancelHide();
    void scheduleHide();

private:
    void show(model::UserRef u, ui::RectF anchor);

    screens::Context &_ctx;
    ui::Window       &_win;
    Avatars          &_avatars;
    ProfileCard      *_card      = nullptr;
    plat::TimerId     _showTimer = 0, _hideTimer = 0;
    model::UserRef    _pending = model::kNoUser;
    ui::RectF         _pendingAt;
};

} // namespace shell
