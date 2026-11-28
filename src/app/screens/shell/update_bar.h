// The update bar: a thin bar right under the title bar once an update is
// downloaded — "A new version of msga has been downloaded. Restart to
// apply." with "Restart now" (Linux, Windows), or "…ready to install." with
// "Open installer" (macOS, where the DMG waits in Downloads). Hidden until
// showUpdateReady().
#pragma once

#include "ui/ui.h"

#include <functional>

namespace shell {

class UpdateBar final : public ui::View {
public:
    UpdateBar();
    void                  showUpdateReady();
    std::function<void()> onRestart; // the button
    ui::Button           *button() const { return _btn; }
    void                  paint(gfx::Painter &p) override;

private:
    ui::Label  *_label = nullptr;
    ui::Button *_btn   = nullptr;
};

} // namespace shell
