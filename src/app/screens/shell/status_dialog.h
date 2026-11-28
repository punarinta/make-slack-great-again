// "Set a status" (from the footer's "Manage status"):
// the emoji + text input, "Clear after", the workspace's five suggestions,
// "Clear status" while one is set, Cancel / Save. Saving goes through
// Backend::setStatus (text, emoji shortcode, expiry).
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

namespace shell {

class StatusDialog : public ui::Dialog {
public:
    explicit StatusDialog(screens::Context &ctx);
    static StatusDialog *show(ui::Window &w, screens::Context &ctx);

    void               setEmoji(std::string shortcode); // "" = the smiley placeholder
    const std::string &emoji() const { return _emoji; }
    ui::TextEdit      &text() const { return *_text; }
    ui::Dropdown      &clearAfter() const { return *_clearAfter; }
    void               applyPreset(int i); // a suggestion row
    void               save();
    void               clearStatus();
    // The expiry "Clear after" picks, from `now` (epoch secs); 0 = never.
    int64_t            expiry(int64_t now) const;
    // A set or clear the service refused:
    // "Could not set status: …", with the re-sign-in hint a missing scope
    // needs. Outlives the dialog (it is closed by then).
    std::function<void(const std::string &message)> onError;

private:
    void apply(std::string emoji, std::string text, int64_t expiry);

    screens::Context &_ctx;
    ui::View         *_inputBox   = nullptr;
    ui::Clickable    *_emojiBtn   = nullptr;
    ui::Label        *_emojiGlyph = nullptr;
    ui::IconView     *_smiley     = nullptr;
    ui::TextEdit     *_text       = nullptr;
    ui::Dropdown     *_clearAfter = nullptr;
    std::string       _emoji;
};

// "Profile" (from "Manage profile"): the avatar (a
// click picks a new photo), Name / Email / Phone, Cancel / "Save changes";
// only changed fields are saved.
class ProfileDialog : public ui::Dialog {
public:
    ProfileDialog(screens::Context &ctx, Avatars &avatars);
    static ProfileDialog *show(ui::Window &w, screens::Context &ctx, Avatars &avatars);
    ui::TextField        &name() const { return *_name; }
    ui::TextField        &email() const { return *_email; }
    ui::TextField        &phone() const { return *_phone; }
    void                  save();
    void                  pickPhoto();

private:
    void status(std::string text, bool error);

    screens::Context         &_ctx;
    Avatars                  &_avatars;
    ui::Image                *_avatar = nullptr;
    ui::TextField            *_name = nullptr, *_email = nullptr, *_phone = nullptr;
    ui::Label                *_status = nullptr;
    ui::Button               *_save   = nullptr;
    model::Backend::MyProfile _loaded;
    bool                      _loadedOk = false;
    std::shared_ptr<int>      _alive    = std::make_shared<int>(0);
};

} // namespace shell
