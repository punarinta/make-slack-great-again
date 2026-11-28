// The scripted walkthrough of the demo workspace (`msga --demo demo
// --demo-tour demo/tour.json`), the recording behind the README video. The
// tour drives the real views: it moves the real pointer through plat's
// input injection (XTest on X11: the screen capture draws it) and hands the
// same pointer and key input to the window, so it clicks and types into
// whatever is under it — menus and pickers are popups — and calls the
// shell's own entry points otherwise. No screen
// coordinates in the script: views are found by what they show.
// Compiled only into demo builds (-DMSGA_DEMO=ON).
#pragma once

#include "app/model/types.h"
#include "screens/shell/demo/tour_script.h"
#include "ui/ui.h"

#include <functional>
#include <memory>

namespace fake {
class FakeBackend;
}
namespace screens {
struct Context;
}

namespace shell {
class Shell;
}

namespace demo {

class Tour {
public:
    // `backend`: the Slack demo's fake backend (Post needs it); null on the
    // Claude Code demo, which runs the real workspace.
    Tour(
        screens::Context  &ctx,
        shell::Shell      &sh,
        ui::Window        &win,
        fake::FakeBackend *backend,
        TourScript         script
    );
    ~Tour();

    // Sizes the window, waits for it to settle, prints "tour: start" (the
    // recording script trims to it), then runs the steps; "tour: end" and
    // the shell's quit follow the Quit step or the end of the list.
    void start();

private:
    using Done = std::function<void()>;

    void runNext();
    void run(const TourStep &step, Done done);

    // Primitives — every one ends by calling `done` from the event loop.
    // Positions are window coordinates (logical).
    void moveTo(ui::PointF to, Done done);
    void click(ui::PointF at, Done done, plat::Button b = plat::Button::Left);
    void typeText(std::string text, double cps, Done done);
    void pressKey(plat::Key k, Done done);
    void after(int ms, Done done);
    void park(Done done); // a neutral spot over the messages
    // One input event at the pointer, straight to the window.
    void send(
        plat::EventType type, plat::Button b = plat::Button::Left, plat::Key k = plat::Key::Unknown
    );

    ui::View      *findText(std::string_view fragment, ui::View *in = nullptr) const;
    ui::View      *findName(std::string_view name, ui::View *in = nullptr) const;
    ui::PointF     centerOf(const ui::View *v) const;
    // The message's text on screen; when it isn't, the list jumps to it and
    // `retry` runs once that has settled (*retrying, null returned).
    ui::View      *messageView(const TourStep &step, Done retry, model::Ts *ts, bool *retrying);
    model::ConvRef conv(const std::string &id) const;
    void           finish();

    screens::Context     &_ctx;
    shell::Shell         &_sh;
    ui::Window           &_win;
    fake::FakeBackend    *_backend;
    TourScript            _script;
    size_t                _index  = 0;
    ui::PointF            _pos    = {640, 400};
    bool                  _jumped = false, _done = false;
    std::shared_ptr<char> _alive = std::make_shared<char>(0);
};

} // namespace demo
