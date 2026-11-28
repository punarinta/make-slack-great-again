// Shared fixture for the ui tests: one App on plat's headless backend, a
// window wrapper that pumps frames, and input helpers that go through plat's
// TestHooks (the same path real input takes) or, for events the hooks cannot
// synthesise (IME preedit, exact click counts), straight into Window::handle.
#pragma once

#include "support/test.h"
#include "plat/testing.h"
#include "ui/ui.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace plat::testing_internal {
Cursor headlessCursor(Window &w);
int    headlessFrames(Window &w);
} // namespace plat::testing_internal

namespace uitest {

inline ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p) {
            std::fprintf(stderr, "ui::App::create failed: %s\n", err.c_str());
            std::abort();
        }
        return p;
    }();
    return *a;
}

inline plat::TestHooks &hooks() {
    return *app().platform().testHooks();
}

// A leaf with a fixed content size that counts its layouts and paints.
struct Box : ui::View {
    ui::SizeF content{0, 0};
    int       layouts = 0, paints = 0;
    explicit Box(float w = 0, float h = 0) : content{w, h} {}
    ui::SizeF measureContent(float, float) override { return content; }
    void      layout() override {
        ++layouts;
        View::layout();
    }
    void paint(gfx::Painter &p) override {
        ++paints;
        View::paint(p);
    }
};

struct Win {
    std::unique_ptr<ui::Window> w;

    explicit Win(int width = 400, int height = 300) {
        app();
        plat::WindowDesc d;
        d.title = "ui test";
        d.size  = {width, height};
        w       = std::make_unique<ui::Window>(d);
        frame();
    }
    ui::View     &root() { return w->root(); }
    plat::Window &native() { return w->native(); }

    // Runs posted work, due timers and pending frames.
    void frame(int n = 3) {
        for (int i = 0; i < n; ++i)
            app().pump(0);
    }
    // Pumps until `done()` or ~2 s (animations run on real time).
    template <class F>
    bool until(F done, int maxIters = 20000) {
        for (int i = 0; i < maxIters; ++i) {
            if (done())
                return true;
            app().pump(1);
        }
        return done();
    }

    void move(float x, float y) {
        hooks().injectPointerMove(native(), {x, y});
        frame(1);
    }
    void press(plat::Button b = plat::Button::Left) {
        hooks().injectButton(native(), b, true);
        frame(1);
    }
    void release(plat::Button b = plat::Button::Left) {
        hooks().injectButton(native(), b, false);
        frame(1);
    }
    void click(float x, float y, plat::Button b = plat::Button::Left) {
        move(x, y);
        press(b);
        release(b);
    }
    void key(plat::Key k) {
        hooks().injectKey(native(), k, true);
        hooks().injectKey(native(), k, false);
        frame(1);
    }
    // Mod keys held around k (ModShift/ModCtrl/ModAlt/ModSuper bits).
    void chord(uint32_t mods, plat::Key k) {
        auto hold = [&](bool down) {
            if (mods & plat::ModShift)
                hooks().injectKey(native(), plat::Key::ShiftLeft, down);
            if (mods & plat::ModCtrl)
                hooks().injectKey(native(), plat::Key::ControlLeft, down);
            if (mods & plat::ModAlt)
                hooks().injectKey(native(), plat::Key::AltLeft, down);
            if (mods & plat::ModSuper)
                hooks().injectKey(native(), plat::Key::SuperLeft, down);
        };
        hold(true);
        hooks().injectKey(native(), k, true);
        hooks().injectKey(native(), k, false);
        hold(false);
        frame(1);
    }
    // Letters, digits and spaces through the key path (headless types US text).
    void type(const char *s) {
        for (; *s; ++s) {
            const char c = *s;
            if (c == ' ')
                key(plat::Key::Space);
            else if (c >= 'a' && c <= 'z')
                key(plat::Key(int(plat::Key::A) + (c - 'a')));
            else if (c >= '0' && c <= '9')
                key(plat::Key(int(plat::Key::Num0) + (c - '0')));
            else
                text(std::string(1, c));
        }
    }
    // Committed text straight in (what an IME or a non-US layout delivers).
    void text(std::string s) {
        plat::Event e;
        e.type   = plat::EventType::TextInput;
        e.window = &native();
        e.text   = std::move(s);
        w->handle(e);
        frame(1);
    }
    void preedit(std::string s, int cursor = -1) {
        plat::Event e;
        e.type               = plat::EventType::TextPreedit;
        e.window             = &native();
        e.text               = std::move(s);
        e.preeditCursorBegin = e.preeditCursorEnd = cursor;
        w->handle(e);
        frame(1);
    }
    // A press/release pair with an exact click count, bypassing the
    // headless double-click timer.
    void clickN(float x, float y, int clicks, uint32_t mods = 0) {
        plat::Event e;
        e.window = &native();
        e.pos    = {x, y};
        e.mods   = mods;
        e.type   = plat::EventType::PointerMove;
        w->handle(e);
        e.type   = plat::EventType::PointerDown;
        e.clicks = clicks;
        w->handle(e);
        e.type = plat::EventType::PointerUp;
        w->handle(e);
        frame(1);
    }
    // Sum of the last frame's damage area in physical pixels.
    long damageArea() const {
        long a = 0;
        for (const plat::Rect &r : w->stats().lastDamage)
            a += long(r.w) * r.h;
        return a;
    }
};

inline bool near(float a, float b, float eps = 0.5f) {
    return a > b - eps && a < b + eps;
}

} // namespace uitest
