// End-to-end check of whichever real backend App::create() picks: opens a
// window, presents a known pattern and reads it back from the screen, drives
// input through the OS (plat/testing.h), and round-trips the clipboard.
// scripts/plat-selftest.sh runs it under Xvfb, a headless wlroots compositor,
// and Wine; on macOS/Windows run it in a logged-in desktop session.
//
// Output is one PASS/FAIL/SKIP line per case; exit status 1 on any FAIL.
#include "plat/plat.h"
#include "plat/testing.h"
#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

using namespace plat;
using plat_test::runCase;
using plat_test::skip;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kCaption = 0xff20a040; // top 30 logical px
constexpr uint32_t kLeft    = 0xffd02020;
constexpr uint32_t kRight   = 0xff2040d0;

std::unique_ptr<App>    g_app;
std::unique_ptr<Window> g_win;
std::vector<Event>      g_events;
int                     g_frames    = 0;
bool                    g_skipPaint = false; // Frames present nothing, ask for another

bool pumpUntil(const std::function<bool()> &pred, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && Clock::now() < end)
        g_app->pump(10);
    return pred();
}
void pumpFor(int ms) {
    pumpUntil([] { return false; }, ms);
}

const Event *find(EventType t, const std::function<bool(const Event &)> &extra = nullptr) {
    for (auto &e : g_events)
        if (e.type == t && (!extra || extra(e)))
            return &e;
    return nullptr;
}

void paint(Window &w) {
    if (g_skipPaint) {
        ++g_frames;
        w.requestFrame(); // from inside the Frame, as an animating app does
        return;
    }
    Canvas    c    = w.beginPaint();
    const int capH = int(std::lround(30 * c.scale));
    for (int y = 0; y < c.height; ++y) {
        uint32_t *row = c.pixels + size_t(y) * c.stride;
        for (int x = 0; x < c.width; ++x)
            row[x] = y < capH ? kCaption : (x < c.width / 2 ? kLeft : kRight);
    }
    w.endPaint({});
    ++g_frames;
}

bool sameRgb(uint32_t a, uint32_t b) {
    auto ch = [](uint32_t v, int s) { return int((v >> s) & 0xff); };
    for (int s : {0, 8, 16})
        if (std::abs(ch(a, s) - ch(b, s)) > 8) // tolerate colour management / dithering
            return false;
    return true;
}

TestHooks *hooks() {
    return g_app->testHooks();
}
bool inputDisabled() {
    return std::getenv("PLAT_SELFTEST_NO_INPUT") != nullptr;
}

// ── cases ───────────────────────────────────────────────────────────────────

void caseFirstFrame() {
    CHECK(pumpUntil([] { return g_frames > 0; }, 5000));
    CHECK(g_win->size().w > 0);
    const double s = g_win->scale();
    CHECK(s >= 0.5 && s <= 4.0);
    std::printf("    size %dx%d scale %.3f\n", g_win->size().w, g_win->size().h, s);
}

// An app that asks for a frame from every Frame but presents nothing (no
// frame callback paces it on Wayland) gets about the display rate, not a
// busy loop.
void caseUnpresentedFramePacing() {
    // The backends that pace such Frames themselves (core::frameIntervalMs).
    const std::string be = g_app->backendName();
    if (be != "x11" && be != "wayland" && be != "win32") {
        skip("this backend does not pace unpresented frames itself");
        return;
    }
    pumpFor(100);
    g_skipPaint      = true;
    const int before = g_frames;
    g_win->requestFrame();
    pumpFor(500);
    g_skipPaint = false;
    const int n = g_frames - before;
    std::printf("    %d unpresented frames in 500 ms\n", n);
    // 60 Hz is 30; allow 20..75 Hz displays and a slow server.
    CHECK(n >= 8 && n <= 40);
    g_win->requestFrame(); // repaint for the next cases
    pumpFor(100);
}

void casePresentReadback() {
    if (!hooks()) {
        skip("backend has no test hooks");
        return;
    }
    pumpFor(300); // let the compositor/server catch up
    const double s = g_win->scale();
    const int    w = int(std::lround(g_win->size().w * s));
    const int    y = int(std::lround(120 * s));
    uint32_t     l = 0, r = 0, cap = 0;
    if (!hooks()->readPixel(*g_win, w / 4, y, &l)) {
        skip("backend cannot read screen pixels");
        return;
    }
    if (!hooks()->readsComposited()) {
        skip("backend can only read back its own buffer, not the composited screen");
        return;
    }
    CHECK(hooks()->readPixel(*g_win, w * 3 / 4, y, &r));
    CHECK(hooks()->readPixel(*g_win, w / 2, int(std::lround(10 * s)), &cap));
    std::printf("    left %08x right %08x caption %08x\n", l, r, cap);
    CHECK(sameRgb(l, kLeft));
    CHECK(sameRgb(r, kRight));
    CHECK(sameRgb(cap, kCaption));
}

void caseTimer() {
    const auto start = Clock::now();
    bool       fired = false;
    g_app->addTimer(50, false, [&] { fired = true; });
    CHECK(pumpUntil([&] { return fired; }, 2000));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::printf("    fired after %lld ms\n", (long long)ms);
    CHECK(ms >= 45 && ms < 1000);
}

void casePostFromThread() {
    bool        ran = false;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        g_app->post([&] { ran = true; });
    });
    // pump(-1) blocks with no timeout: only the post's wake-up can return it.
    const auto  start = Clock::now();
    while (!ran && Clock::now() - start < std::chrono::seconds(3))
        g_app->pump(-1);
    t.join();
    CHECK(ran);
}

void caseWatchFd() {
#if defined(_WIN32)
    skip("fd watches are POSIX-only");
#else
    int fds[2];
    CHECK(pipe(fds) == 0);
    bool           readable = false;
    const uint64_t id =
        g_app->watchFd(fds[0], FdRead, [&](uint32_t ev) { readable = (ev & FdRead) != 0; });
    if (id == 0) {
        skip("backend has no fd watches");
    } else {
        CHECK(write(fds[1], "x", 1) == 1);
        CHECK(pumpUntil([&] { return readable; }, 1000));
        g_app->unwatchFd(id);
    }
    close(fds[0]);
    close(fds[1]);
#endif
}

void caseClipboard() {
    const std::string text = "plat clipboard ✓ ü 日本";
    g_app->setClipboardText(text);
    std::optional<std::string> got;
    bool                       done = false;
    g_app->requestClipboard("text/plain;charset=utf-8", [&](auto v) {
        got  = v;
        done = true;
    });
    CHECK(pumpUntil([&] { return done; }, 3000));
    CHECK(got.has_value());
    if (got)
        CHECK(*got == text);
}

void casePointerMove() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    g_events.clear();
    if (!hooks()->injectPointerMove(*g_win, {160, 120})) {
        skip("backend cannot inject pointer motion");
        return;
    }
    const Event *e = nullptr;
    CHECK(pumpUntil(
        [&] {
            e = find(EventType::PointerMove, [](auto &e) {
                return std::abs(e.pos.x - 160) <= 1 && std::abs(e.pos.y - 120) <= 1;
            });
            return e != nullptr;
        },
        2000
    ));
    if (!e && !g_events.empty())
        std::printf("    last event type %d\n", int(g_events.back().type));
}

void caseClick() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    g_events.clear();
    if (!hooks()->injectButton(*g_win, Button::Left, true)) {
        skip("backend cannot inject buttons");
        return;
    }
    hooks()->injectButton(*g_win, Button::Left, false);
    CHECK(pumpUntil([] { return find(EventType::PointerUp) != nullptr; }, 2000));
    const Event *down = find(EventType::PointerDown);
    CHECK(down != nullptr);
    if (down) {
        CHECK(down->button == Button::Left);
        CHECK(down->clicks >= 1);
    }
}

void caseScroll() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    g_events.clear();
    if (!hooks()->injectScroll(*g_win, 0, 1)) {
        skip("backend cannot inject scroll");
        return;
    }
    const Event *e = nullptr;
    CHECK(pumpUntil([&] { return (e = find(EventType::Scroll)) != nullptr; }, 2000));
    if (e)
        CHECK(e->dy > 0); // +y = content moves up / "scroll down"
}

void caseKeyAndText() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    g_win->setTextInput({.enabled = true, .caret = {40, 60, 1, 16}});
    pumpFor(100);
    g_events.clear();
    if (!hooks()->injectKey(*g_win, Key::A, true)) {
        skip("backend cannot inject keys");
        return;
    }
    hooks()->injectKey(*g_win, Key::A, false);
    CHECK(pumpUntil([] { return find(EventType::KeyUp) && find(EventType::TextInput); }, 2000));
    const Event *down = find(EventType::KeyDown);
    const Event *text = find(EventType::TextInput);
    CHECK(down && down->key == Key::A);
    CHECK(text && text->text == "a");
}

void caseShortcutChord() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    g_events.clear();
    const Key mod = primaryMod() == ModSuper ? Key::SuperLeft : Key::ControlLeft;
    if (!hooks()->injectKey(*g_win, mod, true)) {
        skip("backend cannot inject keys");
        return;
    }
    hooks()->injectKey(*g_win, Key::C, true);
    hooks()->injectKey(*g_win, Key::C, false);
    hooks()->injectKey(*g_win, mod, false);
    CHECK(pumpUntil(
        [] { return find(EventType::KeyUp, [](auto &e) { return e.key == Key::C; }); }, 2000
    ));
    const Event *c = find(EventType::KeyDown, [](auto &e) { return e.key == Key::C; });
    CHECK(c && (c->mods & primaryMod()));
    CHECK(find(EventType::TextInput) == nullptr);
}

void caseCaptionPressSwallowed() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    hooks()->injectPointerMove(*g_win, {160, 10});
    pumpFor(100);
    g_events.clear();
    hooks()->injectButton(*g_win, Button::Left, true);
    pumpFor(200);
    hooks()->injectButton(*g_win, Button::Left, false);
    pumpFor(200);
    CHECK(find(EventType::PointerDown) == nullptr);
}

void caseResize() {
    g_events.clear();
    g_win->setSize({400, 300});
    const bool ok = pumpUntil(
        [] { return g_win->size().w == 400 && g_win->size().h == 300 && find(EventType::Resized); },
        3000
    );
    if (!ok) {
        skip("window manager did not honour the resize (tiling/kiosk?)");
        return;
    }
    const int before = g_frames;
    CHECK(pumpUntil([&] { return g_frames > before; }, 2000)); // a Frame follows a resize
}

void caseCursors() {
    for (int c = 0; c <= int(Cursor::Hidden); ++c) {
        g_win->setCursor(Cursor(c));
        g_app->pump(0);
    }
    g_win->setCursor(Cursor::Arrow);
}

void caseSecondWindow() {
    auto w2 = g_app->createWindow({.title = "plat second", .size = {200, 100}});
    CHECK(w2 != nullptr);
    CHECK(pumpUntil(
        [&] { return find(EventType::Frame, [&](auto &e) { return e.window == w2.get(); }); }, 5000
    ));
    w2.reset(); // destroying a window mid-session must not disturb the other
    pumpFor(100);
    g_win->requestFrame();
    const int before = g_frames;
    CHECK(pumpUntil([&] { return g_frames > before; }, 2000));
}

// ── Outside the window ──────────────────────────────────────────────────────

Image solidImage(int w, int h, uint32_t argb) {
    return {w, h, std::vector<uint32_t>(size_t(w) * h, argb)};
}

// Timers a DnD case schedules capture its locals; this cancels whatever has
// not fired when the case returns (early SKIP/FAIL paths included). Declare it
// after the locals the timers capture, so it runs first.
struct TimerGuard {
    std::vector<TimerId> ids;
    TimerId              add(int ms, std::function<void()> fn) {
        ids.push_back(g_app->addTimer(ms, false, std::move(fn)));
        return ids.back();
    }
    ~TimerGuard() {
        for (TimerId id : ids)
            g_app->cancelTimer(id);
    }
};

bool contains(const std::vector<std::string> &v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

void caseClipboardMultiType() {
    const std::string text = "multi ✓", html = "<b>multi</b> ✓", blob = std::string("\0\1\2xyz", 6);
    g_app->setClipboard({
        {"text/plain;charset=utf-8", text},
        {"text/html", html},
        {"application/x-plat-test", blob},
    });
    std::vector<std::string> mimes;
    bool                     got = false;
    g_app->requestClipboardMimes([&](auto m) {
        mimes = std::move(m);
        got   = true;
    });
    CHECK(pumpUntil([&] { return got; }, 3000));
    for (auto &m : mimes)
        std::printf("    offered: %s\n", m.c_str());
    CHECK(contains(mimes, "text/plain;charset=utf-8"));
    CHECK(contains(mimes, "text/html"));
    CHECK(contains(mimes, "application/x-plat-test"));
    for (const auto &[mime, want] : std::vector<std::pair<std::string, std::string>>{
             {"text/plain;charset=utf-8", text},
             {"text/html", html},
             {"application/x-plat-test", blob}
         }) {
        std::optional<std::string> v;
        bool                       done = false;
        g_app->requestClipboard(mime, [&](auto r) {
            v    = r;
            done = true;
        });
        CHECK(pumpUntil([&] { return done; }, 3000));
        CHECK(v.has_value());
        if (v && *v != want)
            std::printf("    %s mismatch (%zu bytes)\n", mime.c_str(), v->size());
        CHECK(v && *v == want);
    }
}

void casePrimarySelection() {
#if defined(_WIN32) || defined(__APPLE__)
    skip("no primary selection on this OS");
#else
    g_app->setClipboardText("primary ✓", Selection::Primary);
    std::optional<std::string> v;
    bool                       done = false;
    g_app->requestClipboard(
        "text/plain;charset=utf-8",
        [&](auto r) {
            v    = r;
            done = true;
        },
        Selection::Primary
    );
    CHECK(pumpUntil([&] { return done; }, 3000));
    if (!v && std::string(g_app->backendName()) == "wayland") {
        skip("compositor has no primary-selection protocol");
        return;
    }
    CHECK(v && *v == "primary ✓");
#endif
}

void caseDragWithButtonHeld() {
    // In-app drags (reordering workspaces) are plain pointer events: while the
    // button is held, motion outside the window must keep arriving.
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    if (!hooks()->injectPointerMove(*g_win, {100, 100})) {
        skip("backend cannot inject pointer motion");
        return;
    }
    pumpFor(50);
    g_events.clear();
    hooks()->injectButton(*g_win, Button::Left, true);
    for (int i = 1; i <= 5; ++i) {
        hooks()->injectPointerMove(
            *g_win, {100.0 + i * 10, 100.0 + i * 70}
        ); // leaves at the bottom
        pumpFor(30);
    }
    hooks()->injectButton(*g_win, Button::Left, false);
    CHECK(pumpUntil([] { return find(EventType::PointerUp) != nullptr; }, 2000));
    CHECK(find(EventType::PointerDown) != nullptr);
    if (std::getenv("PLAT_SELFTEST_FULLSCREEN")) {
        // Kiosk compositors make the window the whole output: the pointer
        // cannot leave it, so only check that motion kept coming while held.
        int moves = 0;
        for (auto &e : g_events)
            moves += e.type == EventType::PointerMove;
        CHECK(moves >= 3);
        return;
    }
    const Event *outside =
        find(EventType::PointerMove, [](auto &e) { return e.pos.y > g_win->size().h; });
    CHECK(outside != nullptr);
}

void caseDragAndDropBetweenWindows() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    if (std::getenv("PLAT_SELFTEST_NO_DND")) {
        skip("disabled by the runner (single-window compositor)");
        return;
    }
    // Target window created second, so it stacks on top at its origin; the
    // drag starts at the far corner of the main window, outside it.
    auto target = g_app->createWindow({.title = "plat drop target", .size = {160, 100}});
    CHECK(pumpUntil(
        [&] { return find(EventType::Frame, [&](auto &e) { return e.window == target.get(); }); },
        5000
    ));
    pumpFor(200);
    const Point start{g_win->size().w - 20.0, g_win->size().h - 20.0};
    if (!hooks()->injectPointerMove(*g_win, start)) {
        skip("backend cannot inject pointer motion");
        return;
    }
    pumpFor(100);
    g_events.clear();

    const std::string                  uri     = "file:///tmp/plat%20drop/a.txt";
    bool                               started = false, startedOk = false;
    Window                            *tgt = target.get();
    TimerGuard                         timers;
    // Everything after the press runs from timers, because Win32's DoDragDrop
    // (and macOS's drag session) own the loop until the drop.
    std::function<void(const Event &)> onDown = [&](const Event &e) {
        if (started || e.type != EventType::PointerDown || e.window != g_win.get())
            return;
        started = true;
        for (int i = 1; i <= 6; ++i)
            timers.add(60 * i, [&, i] { hooks()->injectPointerMove(*tgt, {20.0 + i * 10, 50}); });
        timers.add(60 * 8, [&] { hooks()->injectButton(*tgt, Button::Left, false); });
        startedOk = g_app->startDrag(
            *g_win,
            {.items = {{"text/uri-list", uri + "\r\n"}, {"text/plain;charset=utf-8", "dragged ✓"}},
             .image = solidImage(32, 32, 0xff3050d0),
             .hotspot = {16, 16},
             .actions = ActCopy | ActMove}
        );
    };
    g_app->setEventHandler([&](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
        onDown(e);
    });
    hooks()->injectButton(*g_win, Button::Left, true);
    CHECK(pumpUntil([&] { return started; }, 2000));
    if (started && !startedOk) {
        g_app->setEventHandler([](const Event &e) {
            g_events.push_back(e);
            if (e.type == EventType::Frame && e.window)
                paint(*e.window);
        });
        hooks()->injectButton(*g_win, Button::Left, false);
        pumpFor(200);
        skip("backend cannot start a drag");
        return;
    }
    CHECK(pumpUntil([] { return find(EventType::DragFinished) != nullptr; }, 6000));
    g_app->setEventHandler([](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
    });
    const Event *enter = find(EventType::DropEnter, [&](auto &e) { return e.window == tgt; });
    const Event *drop  = find(EventType::Drop, [&](auto &e) { return e.window == tgt; });
    const Event *done  = find(EventType::DragFinished);
    CHECK(enter != nullptr);
    if (enter) {
        bool offersUris = false;
        for (auto &i : enter->items)
            offersUris |= i.mime == "text/uri-list";
        CHECK(offersUris);
        CHECK(enter->allowedActions & ActCopy);
    }
    CHECK(drop != nullptr);
    if (drop) {
        CHECK(drop->uris.size() == 1 && drop->uris[0] == uri);
        CHECK(drop->text == "dragged ✓");
    }
    CHECK(done && done->window == g_win.get() && done->dropAction == DropAction::Copy);
    target.reset();
    pumpFor(100);
}

void caseDropRejected() {
    // A target that answers None gets no Drop and the source sees None.
    if (!hooks() || inputDisabled() || std::getenv("PLAT_SELFTEST_NO_DND")) {
        skip("no input injection / DnD");
        return;
    }
    auto target = g_app->createWindow({.title = "plat reject target", .size = {160, 100}});
    CHECK(pumpUntil(
        [&] { return find(EventType::Frame, [&](auto &e) { return e.window == target.get(); }); },
        5000
    ));
    pumpFor(200);
    Window *tgt = target.get();
    hooks()->injectPointerMove(*g_win, {g_win->size().w - 20.0, g_win->size().h - 20.0});
    pumpFor(100);
    g_events.clear();
    bool       started = false, startedOk = false;
    TimerGuard timers;
    g_app->setEventHandler([&](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
        if ((e.type == EventType::DropEnter || e.type == EventType::DropMove) && e.window == tgt)
            tgt->setDropAction(DropAction::None);
        if (!started && e.type == EventType::PointerDown && e.window == g_win.get()) {
            started = true;
            for (int i = 1; i <= 6; ++i)
                timers.add(60 * i, [&, i] {
                    hooks()->injectPointerMove(*tgt, {20.0 + i * 10, 50});
                });
            timers.add(60 * 8, [&] { hooks()->injectButton(*tgt, Button::Left, false); });
            startedOk = g_app->startDrag(*g_win, {.items = {{"text/plain;charset=utf-8", "nope"}}});
        }
    });
    hooks()->injectButton(*g_win, Button::Left, true);
    const bool finished = pumpUntil([] { return find(EventType::DragFinished) != nullptr; }, 6000);
    g_app->setEventHandler([](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
    });
    if (!startedOk) {
        hooks()->injectButton(*g_win, Button::Left, false);
        pumpFor(200);
        skip("backend cannot start a drag");
        return;
    }
    CHECK(finished);
    CHECK(find(EventType::DropEnter, [&](auto &e) { return e.window == tgt; }) != nullptr);
    CHECK(find(EventType::Drop) == nullptr);
    const Event *done = find(EventType::DragFinished);
    CHECK(done && done->dropAction == DropAction::None);
    target.reset();
    pumpFor(100);
}

void casePhasedScroll() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    hooks()->injectPointerMove(*g_win, {160, 120});
    pumpFor(50);
    g_events.clear();
    if (!hooks()->injectPhasedScroll(*g_win, 0, 0, ScrollPhase::Begin)) {
        skip("backend cannot synthesise phased (touchpad) scrolling");
        return;
    }
    hooks()->injectPhasedScroll(*g_win, -30, 0, ScrollPhase::Update);
    hooks()->injectPhasedScroll(*g_win, -30, 0, ScrollPhase::Update);
    hooks()->injectPhasedScroll(*g_win, 0, 0, ScrollPhase::End);
    CHECK(pumpUntil(
        [] { return find(EventType::Scroll, [](auto &e) { return e.phase == ScrollPhase::End; }); },
        2000
    ));
    std::vector<ScrollPhase> phases;
    double                   sum = 0;
    for (auto &e : g_events)
        if (e.type == EventType::Scroll) {
            phases.push_back(e.phase);
            sum += e.dx;
            CHECK(e.precise);
        }
    CHECK(!phases.empty() && phases.front() == ScrollPhase::Begin);
    CHECK(!phases.empty() && phases.back() == ScrollPhase::End);
    CHECK(sum < -40); // horizontal, content-left: a "forward" swipe candidate
}

void caseSwipeGesture() {
    if (!hooks() || inputDisabled()) {
        skip("no input injection");
        return;
    }
    g_events.clear();
    if (!hooks()->injectGesture(*g_win, Gesture::Swipe, 3, 150, 0)) {
        skip("backend cannot synthesise touchpad gestures");
        return;
    }
    CHECK(
        pumpUntil([] { return find(EventType::GestureEnd) || find(EventType::SwipeGesture); }, 2000)
    );
    if (const Event *s = find(EventType::SwipeGesture)) {
        CHECK(s->dx > 0); // macOS one-shot swipe: fingers right = back
        return;
    }
    const Event *b = find(EventType::GestureBegin);
    CHECK(b && b->gesture == Gesture::Swipe && b->fingers == 3);
    double dx = 0;
    for (auto &e : g_events)
        if (e.type == EventType::GestureUpdate)
            dx += e.dx;
    CHECK(dx > 100);
    const Event *end = find(EventType::GestureEnd);
    CHECK(end && !end->cancelled);
}

void caseAttention() {
    auto other = g_app->createWindow({.title = "plat attention", .size = {120, 80}});
    CHECK(pumpUntil(
        [&] { return find(EventType::Frame, [&](auto &e) { return e.window == other.get(); }); },
        5000
    ));
    other->activate();
    pumpFor(200);
    g_win->requestAttention();
    pumpFor(200);
    bool on = false;
    if (!hooks() || !hooks()->wantsAttention(*g_win, &on)) {
        skip("attention state not readable on this backend (call did not crash)");
        other.reset();
        return;
    }
    if (g_win->isActive()) {
        skip("could not move focus away from the main window");
        other.reset();
        return;
    }
    CHECK(on);
    g_win->activate();
    pumpFor(300);
    if (g_win->isActive() && hooks()->wantsAttention(*g_win, &on))
        CHECK(!on);
    other.reset();
    pumpFor(100);
}

void caseBadge() {
    g_app->setBadgeCount(7);
    pumpFor(100);
    const int seen = hooks() ? hooks()->badgeCount() : -1;
    if (seen < 0) {
        g_app->setBadgeCount(0);
        skip("badge not readable on this backend (call did not crash)");
        return;
    }
    CHECK(seen == 7);
    g_app->setBadgeCount(0);
    pumpFor(100);
    CHECK(hooks()->badgeCount() == 0);
}

void caseTray() {
    auto tray = g_app->createTray();
    if (!tray) {
        skip("no tray host on this desktop");
        return;
    }
    tray->setIcon(
        {solidImage(16, 16, 0xffd02020),
         solidImage(32, 32, 0xffd02020),
         solidImage(64, 64, 0xffd02020)}
    );
    tray->setTooltip("plat selftest ✓");
    tray->setMenu({
        {.id = 1, .label = "Open"},
        {.kind = MenuItem::Kind::Separator},
        {.kind     = MenuItem::Kind::Submenu,
         .label    = "Workspaces",
         .children = {{.id = 10, .label = "Acme"}, {.id = 11, .label = "Globex"}}},
        {.kind = MenuItem::Kind::Checkbox, .id = 2, .label = "Mute", .checked = true},
        {.id = 3, .label = "Disabled", .enabled = false},
        {.id = 4, .label = "Quit"},
    });
    if (!pumpUntil([&] { return tray->isVisible(); }, 3000)) {
        skip("tray created but no host shows it");
        return;
    }
    if (!hooks()) {
        skip("no test hooks");
        return;
    }
    TestHooks::TrayProbe probe;
    if (hooks()->trayProbe(*tray, &probe)) {
        CHECK(!probe.iconSizes.empty());
        CHECK(probe.tooltip == "plat selftest ✓");
        CHECK(contains(probe.menuLabels, "Open"));
        CHECK(contains(probe.menuLabels, "Globex"));
        CHECK(contains(probe.menuLabels, "-"));
        CHECK(!probe.isTemplate);
        // macOS: a template image the menu bar tints (headless records it too).
        tray->setTemplate(true);
        tray->setIcon({solidImage(18, 18, 0xff000000), solidImage(36, 36, 0xff000000)});
        TestHooks::TrayProbe   t;
        const std::string_view be = g_app->backendName();
        if ((be == "cocoa" || be == "headless") && hooks()->trayProbe(*tray, &t))
            CHECK(t.isTemplate);
        tray->setTemplate(false);
    } else {
        std::printf("    (host-side readback unavailable)\n");
    }
    g_events.clear();
    if (hooks()->trayActivate(*tray)) {
        CHECK(pumpUntil([&] { return find(EventType::TrayActivated) != nullptr; }, 2000));
        const Event *a = find(EventType::TrayActivated);
        CHECK(a && a->tray == tray.get() && a->window == nullptr);
    } else {
        std::printf("    (cannot synthesise a tray click)\n");
    }
    g_events.clear();
    if (hooks()->trayMenuSelect(*tray, 11)) {
        CHECK(pumpUntil([&] { return find(EventType::TrayMenuItem) != nullptr; }, 2000));
        const Event *m = find(EventType::TrayMenuItem);
        CHECK(m && m->id == 11 && m->tray == tray.get());
        g_events.clear();
        hooks()->trayMenuSelect(*tray, 3); // disabled: nothing may arrive
        pumpFor(300);
        CHECK(find(EventType::TrayMenuItem) == nullptr);
    } else {
        std::printf("    (cannot synthesise a menu selection)\n");
    }
    tray.reset();
    pumpFor(100);
}

void caseNotification() {
    if (!g_app->notificationsAvailable()) {
        skip("no notification service");
        return;
    }
    Notification n;
    n.title           = "Ada Lovelace";
    n.body            = "Did you see the new engine? ✓";
    n.image           = solidImage(64, 64, 0xff2b8a3e);
    n.actions         = {{"reply", "Reply"}, {"read", "Mark as read"}};
    const uint64_t id = 1;
    const bool     ok = g_app->notify(id, n);
    CHECK(ok);
    if (!ok)
        return;
    pumpFor(500);
    if (const Event *f = find(EventType::NotificationFailed)) {
        skip("OS refused the notification: " + f->text);
        return;
    }
    g_events.clear();
    if (!hooks()) {
        skip("no test hooks");
        return;
    }
    TestHooks::NotificationProbe probe;
    if (hooks()->notificationProbe(id, &probe)) {
        CHECK(probe.title == n.title);
        CHECK(probe.body == n.body);
        std::printf("    server holds image %dx%d, actions:", probe.imageSize.w, probe.imageSize.h);
        for (auto &l : probe.actionLabels)
            std::printf(" '%s'", l.c_str());
        std::printf("\n");
        CHECK(probe.imageSize.w == 64 && probe.imageSize.h == 64);
        CHECK(contains(probe.actionLabels, "Reply"));
    } else {
        std::printf("    (server-side readback unavailable)\n");
    }
    // An action click needs buttons; OSes that render none (Windows balloons)
    // return false and we go on to the body click.
    if (hooks()->notificationInvoke(id, "reply")) {
        CHECK(pumpUntil([&] { return find(EventType::NotificationActivated) != nullptr; }, 3000));
        const Event *a = find(EventType::NotificationActivated);
        CHECK(a && a->id == id && a->action == "reply" && a->window == nullptr);
    } else {
        std::printf("    (no action buttons to click here)\n");
    }

    const uint64_t id2 = id + 1;
    CHECK(g_app->notify(id2, {.title = "Second", .body = "body click"}));
    pumpFor(200);
    g_events.clear();
    const bool clickable = hooks()->notificationInvoke(id2, "");
    if (clickable) {
        CHECK(pumpUntil([&] { return find(EventType::NotificationActivated) != nullptr; }, 3000));
        const Event *b = find(EventType::NotificationActivated);
        CHECK(b && b->id == id2 && b->action.empty());
    }
    // Content was checked above (a failed CHECK still FAILs the case);
    // without a way to click, the case as named is not proven.
    if (!clickable)
        skip("content and image verified; clicks cannot be synthesised here");
}

// ── Round 3: monitors, instances, system events, files ──────────────────────

std::string g_self; // absolute path of this executable, for the second-instance case

bool isHeadless() {
    return std::string(g_app->backendName()) == "headless";
}

void caseMonitors() {
    auto mons = g_app->monitors();
    if (mons.empty()) {
        skip("backend reports no monitors");
        return;
    }
    int primaries = 0;
    for (auto &m : mons) {
        std::printf(
            "    monitor %llu '%s' %d,%d %dx%d work %d,%d %dx%d scale %.2f %d mHz%s\n",
            (unsigned long long)m.id,
            m.name.c_str(),
            m.bounds.x,
            m.bounds.y,
            m.bounds.w,
            m.bounds.h,
            m.workArea.x,
            m.workArea.y,
            m.workArea.w,
            m.workArea.h,
            m.scale,
            m.refreshMilliHz,
            m.primary ? " primary" : ""
        );
        primaries += m.primary;
        CHECK(m.id != 0);
        CHECK(m.bounds.w > 0 && m.bounds.h > 0);
        CHECK(m.workArea.w > 0 && m.workArea.h > 0);
        CHECK(m.workArea.x >= m.bounds.x && m.workArea.y >= m.bounds.y);
        CHECK(m.workArea.x + m.workArea.w <= m.bounds.x + m.bounds.w);
        CHECK(m.workArea.y + m.workArea.h <= m.bounds.y + m.bounds.h);
        CHECK(m.scale >= 0.5 && m.scale <= 4.0);
    }
    CHECK(primaries == 1);
    const uint64_t on = g_win->monitor();
    std::printf("    main window on monitor %llu\n", (unsigned long long)on);
    CHECK(std::any_of(mons.begin(), mons.end(), [&](auto &m) { return m.id == on; }));
}

void caseWindowPosition() {
    auto pos = g_win->position();
    if (!pos) {
        skip("OS does not report window positions (Wayland)");
        return;
    }
    std::printf("    at %.0f,%.0f\n", pos->x, pos->y);
    g_events.clear();
    if (!g_win->setPosition({120, 90})) {
        skip("OS refuses client positioning");
        return;
    }
    const bool ok = pumpUntil(
        [] {
            auto p = g_win->position();
            return p && std::abs(p->x - 120) <= 2 && std::abs(p->y - 90) <= 2;
        },
        3000
    );
    auto now = g_win->position();
    if (now)
        std::printf("    moved to %.0f,%.0f\n", now->x, now->y);
    CHECK(ok);
    CHECK(find(EventType::Moved) != nullptr);
}

// Runs a second copy of this program as `--plat-secondary <key> args…`;
// returns its exit status (-1 = could not start).
int runSecondary(const std::string &key, const std::vector<std::string> &args) {
#if defined(_WIN32)
    auto        quote = [](const std::string &a) { return "\"" + a + "\""; };
    std::string cmd   = quote(g_self) + " --plat-secondary " + quote(key);
    for (auto &a : args)
        cmd += " " + quote(a);
    std::wstring        wcmd(cmd.begin(), cmd.end()); // args are ASCII here
    STARTUPINFOW        si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi
        ))
        return -1;
    // Keep our loop running while the child talks to us.
    DWORD code = STILL_ACTIVE;
    for (int i = 0; i < 1000 && code == STILL_ACTIVE; ++i) {
        g_app->pump(10);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == STILL_ACTIVE ? -1 : int(code);
#else
    std::vector<std::string> all = {g_self, "--plat-secondary", key};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &a : all)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, g_self.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        return -1;
    int status = 0;
    for (int i = 0; i < 1000; ++i) {
        g_app->pump(10);
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
#endif
}

void caseSingleInstance() {
    if (isHeadless()) {
        skip("headless has no cross-process instance channel");
        return;
    }
    const std::string key = "plat-selftest-" + std::to_string(
#if defined(_WIN32)
                                                   GetCurrentProcessId()
#else
                                                   getpid()
#endif
                                               );
    CHECK(g_app->claimSingleInstance(key, {}));
    const bool schemes = std::getenv("PLAT_SELFTEST_ALLOW_REGISTRATION") != nullptr;
    if (schemes)
        CHECK(g_app->registerUrlScheme("plat-selftest"));
    g_events.clear();
    const int rc = runSecondary(key, {"hello world", "plat-selftest://open?x=1"});
    std::printf("    second instance exited with %d\n", rc);
    CHECK(rc == 0); // 0 = it found us and forwarded; 3 = it became primary
    CHECK(pumpUntil([] { return find(EventType::InstanceActivated) != nullptr; }, 3000));
    const Event *a = find(EventType::InstanceActivated);
    if (a) {
        CHECK(a->window == nullptr);
        CHECK((a->strings == std::vector<std::string>{"hello world", "plat-selftest://open?x=1"}));
        CHECK(!a->text.empty()); // its working directory
    }
    if (schemes) {
        CHECK(pumpUntil([] { return find(EventType::OpenUrls) != nullptr; }, 2000));
        const Event *u = find(EventType::OpenUrls);
        CHECK(u && u->strings == std::vector<std::string>{"plat-selftest://open?x=1"});
    } else {
        std::printf(
            "    (scheme registration disabled; set PLAT_SELFTEST_ALLOW_REGISTRATION with a "
            "throwaway HOME)\n"
        );
    }
}

void caseUrlDelivery() {
    if (!hooks()) {
        skip("no test hooks");
        return;
    }
    g_events.clear();
    if (!hooks()->deliverUrl("plat-selftest://direct?y=2")) {
        skip("URLs only arrive through a new process here (covered by the instance case)");
        return;
    }
    CHECK(pumpUntil([] { return find(EventType::OpenUrls) != nullptr; }, 3000));
    const Event *u = find(EventType::OpenUrls);
    CHECK(u && u->strings == std::vector<std::string>{"plat-selftest://direct?y=2"});
}

void caseSystemEvents() {
    if (!hooks()) {
        skip("no test hooks");
        return;
    }
    struct Step {
        EventType   t;
        bool        online;
        const char *name;
    };
    const Step steps[] = {
        {EventType::NetworkChanged, false, "offline"},
        {EventType::NetworkChanged, true, "online"},
        {EventType::Suspending, false, "suspending"},
        {EventType::Resumed, false, "resumed"},
    };
    int delivered = 0;
    for (const auto &st : steps) {
        g_events.clear();
        if (!hooks()->simulateSystemEvent(st.t, st.online)) {
            std::printf("    (%s: cannot simulate here)\n", st.name);
            continue;
        }
        ++delivered;
        const bool got = pumpUntil(
            [&] { return find(st.t, [&](auto &e) { return e.online == st.online && !e.window; }); },
            3000
        );
        if (!got)
            std::printf("    %s: event did not arrive\n", st.name);
        CHECK(got);
        if (st.t == EventType::NetworkChanged && g_app->networkOnline())
            CHECK(*g_app->networkOnline() == st.online);
    }
    if (!delivered)
        skip("backend cannot simulate any system event");
}

// A path from UTF-8 (plat's encoding everywhere). A plain std::string is
// the ANSI code page on Windows, which mangles "✓".
std::filesystem::path u8(const std::string &p) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(p.data()), p.size())
    );
}
void writeFile(const std::string &p, const char *data) {
    std::ofstream(u8(p)) << data;
}

// Asks for one dialog through showFileDialogEx and waits for its answer.
std::optional<FileDialogResult> runDialog(const FileDialogDesc &d, int ms = 10000) {
    std::optional<FileDialogResult> got;
    g_app->showFileDialogEx(d, [&](FileDialogResult r) { got = std::move(r); });
    pumpUntil([&] { return got.has_value(); }, ms);
    return got;
}

void printPaths(const char *what, const std::vector<std::string> &paths) {
    for (auto &p : paths)
        std::printf("    %s -> %s\n", what, p.c_str());
}

// $PLAT_SELFTEST_FILE_DIALOG=unavailable: the runner took the chooser away
// (no portal, a failing portal, no helper tools); the dialog cases then check
// that plat says so instead of pretending the user cancelled.
bool dialogExpectedUnavailable() {
    const char *v = std::getenv("PLAT_SELFTEST_FILE_DIALOG");
    return v && std::string(v) == "unavailable";
}
// =closed: every dialog is shown and then closed with Escape or the window's
// close button (a portal answering code 2 late, as GNOME's and GTK's do).
bool dialogExpectedClosed() {
    const char *v = std::getenv("PLAT_SELFTEST_FILE_DIALOG");
    return v && std::string(v) == "closed";
}

bool dialogHooks() {
    if (!hooks()) {
        skip("no test hooks");
        return false;
    }
    if (dialogExpectedUnavailable()) {
        skip("no file chooser here (see the unavailable case)");
        return false;
    }
    if (dialogExpectedClosed()) {
        skip("every dialog is closed here (see the closed case)");
        return false;
    }
    return true;
}

void caseFileDialogSave() {
    if (!dialogHooks())
        return;
    const std::string dir  = g_app->standardDir(StandardDir::Temp);
    const std::string want = dir + "/plat-save ✓.txt";
    if (!hooks()->fileDialogRespond({want})) {
        skip("backend cannot drive its file dialog");
        return;
    }
    std::optional<std::vector<std::string>> got;
    // The legacy entry point: plain paths.
    g_app->showFileDialog(
        {.mode          = FileDialogDesc::Mode::Save,
         .title         = "Save test file",
         .initialDir    = dir,
         .suggestedName = "plat-save ✓.txt",
         .filters       = {{"Text", {"*.txt"}}},
         .parent        = g_win.get()},
        [&](auto paths) { got = std::move(paths); }
    );
    CHECK(pumpUntil([&] { return got.has_value(); }, 10000));
    if (got) {
        printPaths("save", *got);
        CHECK(got->size() == 1 && (*got)[0] == want);
    }
}

void caseFileDialogOpen() {
    if (!dialogHooks())
        return;
    const std::string dir = g_app->standardDir(StandardDir::Temp);
    const std::string a   = dir + "/plat-open-one ✓.txt";
    writeFile(a, "a");
    if (!hooks()->fileDialogRespond({a})) {
        skip("backend cannot drive its file dialog");
        return;
    }
    // A parent everywhere: macOS can only confirm a panel from code when it
    // is a sheet.
    auto got = runDialog(
        {.mode       = FileDialogDesc::Mode::Open,
         .title      = "Open test file",
         .initialDir = dir,
         .filters    = {{"Text", {"*.txt"}}, {"All files", {"*"}}},
         .parent     = g_win.get()}
    );
    CHECK(got.has_value());
    if (got) {
        printPaths("open", got->paths);
        CHECK(got->status == FileDialogResult::Status::Chosen);
        CHECK(got->paths.size() == 1 && got->paths[0] == a);
    }
    // Cancel: Cancelled, not Unavailable, and no paths.
    CHECK(hooks()->fileDialogRespond({}));
    got = runDialog({.mode = FileDialogDesc::Mode::Open, .initialDir = dir, .parent = g_win.get()});
    CHECK(got && got->status == FileDialogResult::Status::Cancelled && got->paths.empty());
    std::error_code ec;
    std::filesystem::remove(u8(a), ec);
}

void caseFileDialogOpenMultiple() {
    if (!dialogHooks())
        return;
    const std::string dir = g_app->standardDir(StandardDir::Temp);
    const std::string a = dir + "/plat-open-a.txt", b = dir + "/plat-open-b.txt";
    writeFile(a, "a");
    writeFile(b, "b");
    // macOS has no public way to select several files in a panel from code.
    std::vector<std::string> pick{a, b};
    if (!hooks()->fileDialogRespond(pick)) {
        pick = {a};
        if (!hooks()->fileDialogRespond(pick)) {
            skip("backend cannot drive its file dialog");
            return;
        }
        std::printf("    (backend can select one file only when driving the dialog)\n");
    }
    auto got = runDialog(
        {.mode = FileDialogDesc::Mode::OpenMultiple, .initialDir = dir, .parent = g_win.get()}
    );
    CHECK(got && got->status == FileDialogResult::Status::Chosen);
    if (got) {
        auto v = got->paths;
        std::sort(v.begin(), v.end());
        printPaths("open several", v);
        CHECK(v == pick);
    }
    std::error_code ec;
    std::filesystem::remove(u8(a), ec);
    std::filesystem::remove(u8(b), ec);
}

void caseFileDialogPickFolder() {
    if (!dialogHooks())
        return;
    const std::string dir  = g_app->standardDir(StandardDir::Temp);
    const std::string want = dir + "/plat-folder ✓";
    std::error_code   ec;
    std::filesystem::create_directories(u8(want), ec);
    if (!hooks()->fileDialogRespond({want})) {
        skip("backend cannot drive its file dialog");
        return;
    }
    auto got = runDialog(
        {.mode       = FileDialogDesc::Mode::PickFolder,
         .title      = "Choose a folder",
         .initialDir = dir,
         .parent     = g_win.get()}
    );
    CHECK(got && got->status == FileDialogResult::Status::Chosen);
    if (got) {
        printPaths("folder", got->paths);
        CHECK(got->paths.size() == 1 && got->paths[0] == want);
        if (got->paths.size() == 1)
            CHECK(std::filesystem::is_directory(u8(got->paths[0]), ec));
    }
    std::filesystem::remove(u8(want), ec);
}

// No chooser: showFileDialogEx answers Unavailable promptly for every mode
// (the app then shows its own), and the legacy call still answers (empty).
void caseFileDialogUnavailable() {
    // A faked dialog (headless) can be switched off; a real one only by the
    // runner.
    const bool faked =
        !dialogExpectedUnavailable() && hooks() && hooks()->setFileDialogAvailable(false);
    if (!dialogExpectedUnavailable() && !faked) {
        skip("a file chooser is available here");
        return;
    }
    const std::string dir = g_app->standardDir(StandardDir::Temp);
    for (auto mode :
         {FileDialogDesc::Mode::Open,
          FileDialogDesc::Mode::OpenMultiple,
          FileDialogDesc::Mode::Save,
          FileDialogDesc::Mode::PickFolder}) {
        const auto start = Clock::now();
        auto       got   = runDialog(
            {.mode = mode, .initialDir = dir, .suggestedName = "x.txt", .parent = g_win.get()}, 5000
        );
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        std::printf(
            "    mode %d -> %s in %lld ms\n",
            int(mode),
            !got                                                   ? "no answer"
            : got->status == FileDialogResult::Status::Unavailable ? "unavailable"
            : got->status == FileDialogResult::Status::Cancelled   ? "cancelled"
                                                                   : "chosen",
            (long long)ms
        );
        CHECK(got && got->status == FileDialogResult::Status::Unavailable && got->paths.empty());
    }
    // Where the legacy call may still reach a helper tool (the runner put a
    // fake zenity on PATH), have it answer cancel instead of waiting.
    if (!faked && hooks())
        hooks()->fileDialogRespond({});
    std::optional<std::vector<std::string>> legacy;
    g_app->showFileDialog({.mode = FileDialogDesc::Mode::Open, .parent = g_win.get()}, [&](auto p) {
        legacy = std::move(p);
    });
    CHECK(pumpUntil([&] { return legacy.has_value(); }, 5000));
    CHECK(legacy && legacy->empty());
    if (faked)
        hooks()->setFileDialogAvailable(true);
}

// A dialog closed with Escape or its close button is a cancel, not
// Unavailable: the app would put its own chooser up after the native one.
void caseFileDialogClosed() {
    if (!dialogExpectedClosed()) {
        skip("the runner closes no dialogs here");
        return;
    }
    for (auto mode : {FileDialogDesc::Mode::Open, FileDialogDesc::Mode::PickFolder}) {
        auto got = runDialog({.mode = mode, .parent = g_win.get()}, 5000);
        std::printf(
            "    mode %d -> %s\n",
            int(mode),
            !got                                                   ? "no answer"
            : got->status == FileDialogResult::Status::Unavailable ? "unavailable"
            : got->status == FileDialogResult::Status::Cancelled   ? "cancelled"
                                                                   : "chosen"
        );
        CHECK(got && got->status == FileDialogResult::Status::Cancelled && got->paths.empty());
    }
}

void caseStandardDirs() {
    static const char *names[] = {
        "config",
        "data",
        "cache",
        "state",
        "temp",
        "home",
        "desktop",
        "documents",
        "downloads",
        "pictures"
    };
    for (int i = 0; i <= int(StandardDir::Pictures); ++i) {
        const std::string d = g_app->standardDir(StandardDir(i));
        std::printf("    %-9s %s\n", names[i], d.c_str());
        const bool required = i <= int(StandardDir::Home);
        if (d.empty()) {
            CHECK(!required);
            continue;
        }
        CHECK(std::filesystem::path(d).is_absolute());
        CHECK(d.size() == 1 || (d.back() != '/' && d.back() != '\\'));
    }
    CHECK(std::filesystem::is_directory(g_app->standardDir(StandardDir::Temp)));
    CHECK(std::filesystem::is_directory(g_app->standardDir(StandardDir::Home)));
}

void caseSystemSettings() {
    const SystemSettings s = g_app->systemSettings();
    std::printf(
        "    reducedMotion %d highContrast %d textScale %.2f accent %08x caretBlink %d ms\n",
        s.reducedMotion,
        s.highContrast,
        s.textScale,
        s.accentColor,
        s.caretBlinkMs
    );
    CHECK(s.textScale >= 0.5 && s.textScale <= 3.0);
    CHECK(s.caretBlinkMs >= 0 && s.caretBlinkMs <= 5000);
}

} // namespace

// `--plat-secondary <key> args…`: the second instance in caseSingleInstance.
static int secondaryMain(int argc, char **argv) {
    std::string err;
    auto        app = App::create(&err);
    if (!app)
        return 2;
    std::vector<std::string> args(argv + 3, argv + argc);
    return app->claimSingleInstance(argv[2], args) ? 3 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "--plat-secondary")
        return secondaryMain(argc, argv);
    {
        std::error_code ec;
        auto            p = std::filesystem::absolute(argv[0], ec);
#if defined(__linux__)
        p = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec)
            p = std::filesystem::absolute(argv[0], ec);
#endif
        g_self = p.string();
    }
    std::string err;
    g_app = App::create(&err);
    if (!g_app) {
        std::printf("FAIL create app: %s\n", err.c_str());
        return 1;
    }
    std::printf("# backend: %s\n", g_app->backendName());
    g_app->setEventHandler([](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
    });
    g_win = g_app->createWindow({
        .title       = "plat selftest",
        .appId       = "plat-selftest",
        .size        = {320, 240},
        .decorations = Decorations::Custom,
    });
    if (!g_win) {
        std::printf("FAIL create window\n");
        return 1;
    }
    g_win->setHitTest([](Point p) { return p.y < 30 ? HitArea::Caption : HitArea::Client; });

    runCase("window maps and gets a first Frame", caseFirstFrame);
    runCase("presented pixels reach the screen", casePresentReadback);
    runCase("unpresented frames pace to the display rate", caseUnpresentedFramePacing);
    runCase("50 ms timer fires on time", caseTimer);
    runCase("post() from a thread wakes a blocking pump", casePostFromThread);
    runCase("fd watch reports readability", caseWatchFd);
    runCase("clipboard text round-trips (UTF-8)", caseClipboard);
    runCase("pointer motion arrives in logical coords", casePointerMove);
    runCase("click arrives as down/up with click count", caseClick);
    runCase("wheel scroll arrives with +dy = down", caseScroll);
    runCase("key A gives KeyDown(A) + TextInput(\"a\")", caseKeyAndText);
    runCase("primary-mod+C chord: key event, no text", caseShortcutChord);
    runCase("caption press becomes OS move, not a click", caseCaptionPressSwallowed);
    runCase("setSize reaches the window and repaints", caseResize);
    runCase("every cursor shape can be set", caseCursors);
    runCase("second window opens and closes cleanly", caseSecondWindow);

    g_app->setAppInfo({.name = "plat selftest", .id = "org.nisdos.plat-selftest"});
    runCase("clipboard: text + html + custom type together", caseClipboardMultiType);
    runCase("primary selection round-trips (Linux)", casePrimarySelection);
    runCase("button-held drag keeps reporting motion outside", caseDragWithButtonHeld);
    runCase("OS drag and drop between two windows", caseDragAndDropBetweenWindows);
    runCase("drop target answering None gets no Drop", caseDropRejected);
    runCase("touchpad scroll carries Begin/Update/End phases", casePhasedScroll);
    runCase("three-finger swipe arrives as a gesture", caseSwipeGesture);
    runCase("requestAttention flags an inactive window", caseAttention);
    runCase("badge count reaches the Dock/taskbar/launcher", caseBadge);
    runCase("tray: icon, tooltip, menu; click and menu events", caseTray);
    runCase("notification: content, action and body clicks", caseNotification);

    runCase("monitors: sane bounds, work areas, one primary", caseMonitors);
    runCase("window position can be read and set", caseWindowPosition);
    runCase("second instance forwards its args (and URLs)", caseSingleInstance);
    runCase("OS URL delivery reaches OpenUrls", caseUrlDelivery);
    runCase("network / sleep events arrive", caseSystemEvents);
    runCase("file dialog: save with a suggested name", caseFileDialogSave);
    runCase("file dialog: open one file, then cancel", caseFileDialogOpen);
    runCase("file dialog: open several files", caseFileDialogOpenMultiple);
    runCase("file dialog: pick a folder", caseFileDialogPickFolder);
    runCase("file dialog: unavailable is reported, not a cancel", caseFileDialogUnavailable);
    runCase("file dialog: a closed dialog is a cancel", caseFileDialogClosed);
    runCase("standard directories are absolute and exist", caseStandardDirs);
    runCase("system settings are in range", caseSystemSettings);

    g_win.reset();
    g_app.reset();
    return plat_test::summary();
}
