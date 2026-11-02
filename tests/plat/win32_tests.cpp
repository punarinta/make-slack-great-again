// Win32-specific end-to-end checks the portable selftest cannot express:
// the Custom-frame machinery (NC button presses, caption double-click and
// drag through the OS move loop, the maximised inset), click runs, UTF-16
// surrogates, AltGr, DPI changes, frame pacing, CRLF clipboard, file drops,
// the native format mapping (CF_HTML offsets, CF_HDROP, DIB → PNG), icon and
// badge bitmaps, the tray's TaskbarCreated re-add, the balloon fallback and
// DoDragDrop's modal loop.
// Runs the real backend, so it needs a desktop: scripts/plat-selftest-wine.sh.
#include "win32/win32.h"

#include "plat/plat.h"
#include "plat/testing.h"
#include "test_util.h"

#include <propsys.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstring>

using namespace plat;
using namespace plat::win32;
using plat_test::runCase;
using plat_test::skip;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kCaptionH = 30, kBtnW = 40;

std::unique_ptr<App>    g_app;
std::unique_ptr<Window> g_win;
std::vector<Event>      g_events;
int                     g_frames = 0;

bool pumpUntil(const std::function<bool()> &pred, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && Clock::now() < end)
        g_app->pump(10);
    return pred();
}
void pumpFor(int ms) {
    pumpUntil([] { return false; }, ms);
}

int count(EventType t, const std::function<bool(const Event &)> &extra = nullptr) {
    int n = 0;
    for (auto &e : g_events)
        n += e.type == t && (!extra || extra(e));
    return n;
}
const Event *find(EventType t, const std::function<bool(const Event &)> &extra = nullptr) {
    for (auto &e : g_events)
        if (e.type == t && (!extra || extra(e)))
            return &e;
    return nullptr;
}

TestHooks *hooks() {
    return g_app->testHooks();
}
HWND hwnd() {
    return static_cast<HWND>(g_win->nativeHandle());
}

void paint(Window &w) {
    Canvas c = w.beginPaint();
    for (int y = 0; y < c.height; ++y)
        std::fill(
            c.pixels + size_t(y) * c.stride, c.pixels + size_t(y) * c.stride + c.width, 0xff808080u
        );
    w.endPaint({});
    ++g_frames;
}

void click(Button b = Button::Left) {
    hooks()->injectButton(*g_win, b, true);
    hooks()->injectButton(*g_win, b, false);
}

void restore() {
    if (g_win->isMaximized()) {
        g_win->setMaximized(false);
        pumpUntil([] { return !g_win->isMaximized(); }, 2000);
    }
    pumpFor(100);
}

// ── pure helpers ────────────────────────────────────────────────────────────

void caseHelpers() {
    CHECK(fileUri(L"C:\\a b\\\u00fc.txt") == "file:///C:/a%20b/%C3%BC.txt");
    CHECK(fileUri(L"\\\\srv\\share\\x#1.txt") == "file://srv/share/x%231.txt");
    CHECK(fileUri(L"\\\\?\\C:\\long\\p") == "file:///C:/long/p");
    CHECK(fileUri(L"\\\\?\\UNC\\srv\\s\\f") == "file://srv/s/f");
    const std::wstring w = L"a\u00e9\u65e5\U0001F600b"; // 1 + 2 + 3 + 4 + 1 bytes
    CHECK(utf8Length(w, 0) == 0);
    CHECK(utf8Length(w, 3) == 6);
    CHECK(utf8Length(w, 5) == 10); // the surrogate pair counts once
    CHECK(utf8Length(w, 99) == 11);
    CHECK(keyFromVk(VK_RETURN, true, 0x1c) == Key::KpEnter);
    CHECK(keyFromVk(VK_RETURN, false, 0x1c) == Key::Enter);
    CHECK(keyFromVk(VK_SHIFT, false, 0x36) == Key::ShiftRight);
    CHECK(keyFromVk(VK_SHIFT, false, 0x2a) == Key::ShiftLeft);
    CHECK(keyFromVk(VK_CONTROL, true, 0x1d) == Key::ControlRight);
    CHECK(keyFromVk(VK_MENU, true, 0x38) == Key::AltRight);
    CHECK(keyFromVk('Q', false, 0x10) == Key::Q);
    CHECK(keyFromVk(VK_F13, false, 0x64) == Key::F13);
    CHECK(keyFromVk(VK_OEM_MINUS, false, 0x0c) == Key::Minus); // US layout under Wine
    bool ext = false;
    CHECK(vkFromKey(Key::Left, &ext) == VK_LEFT && ext);
    CHECK(vkFromKey(Key::KpEnter, &ext) == VK_RETURN && ext);
    CHECK(vkFromKey(Key::A, &ext) == 'A' && !ext);
}

// ── pointer ─────────────────────────────────────────────────────────────────

void caseTripleClick() {
    hooks()->injectPointerMove(*g_win, {100, 150});
    pumpFor(50);
    g_events.clear();
    click();
    click();
    click();
    pumpUntil([] { return count(EventType::PointerUp) >= 3; }, 2000);
    std::vector<int> clicks;
    for (auto &e : g_events)
        if (e.type == EventType::PointerDown)
            clicks.push_back(e.clicks);
    CHECK((clicks == std::vector<int>{1, 2, 3}));
    g_events.clear();
    click(Button::Right); // another button starts a new run
    pumpUntil([] { return find(EventType::PointerUp) != nullptr; }, 2000);
    const Event *d = find(EventType::PointerDown);
    CHECK(d && d->button == Button::Right && d->clicks == 1);
    pumpFor(600); // let the double-click time run out before the next case
}

void caseBackButton() {
    g_events.clear();
    click(Button::Back);
    click(Button::Forward);
    pumpUntil([] { return count(EventType::PointerUp) >= 2; }, 2000);
    CHECK(find(EventType::PointerDown, [](auto &e) { return e.button == Button::Back; }));
    CHECK(find(EventType::PointerUp, [](auto &e) { return e.button == Button::Forward; }));
}

void caseCloseButtonPressDelivered() {
    // The hit test says CloseButton (HTCLOSE to Windows); the press must still
    // reach the app as an ordinary PointerDown/Up and must not close anything.
    const int w = g_win->size().w;
    hooks()->injectPointerMove(*g_win, {double(w - kBtnW / 2), 10});
    pumpFor(100);
    CHECK(SendMessageW(hwnd(), WM_NCHITTEST, 0, [] {
              POINT p;
              GetCursorPos(&p);
              return MAKELPARAM(p.x, p.y);
          }()) == HTCLOSE);
    g_events.clear();
    click();
    CHECK(pumpUntil([] { return find(EventType::PointerUp) != nullptr; }, 2000));
    const Event *d = find(EventType::PointerDown);
    CHECK(d && d->pos.y < kCaptionH && d->pos.x > w - kBtnW);
    CHECK(!find(EventType::CloseRequested));
    CHECK(IsWindow(hwnd()));
    // Hovering the button is PointerMove to the app, not a leave.
    CHECK(!find(EventType::PointerLeave));
    pumpFor(600);
}

void caseCaptionDoubleClickMaximises() {
    restore();
    hooks()->injectPointerMove(*g_win, {120, 10});
    pumpFor(100);
    g_events.clear();
    click();
    click();
    const bool maxed = pumpUntil([] { return g_win->isMaximized(); }, 2000);
    CHECK(maxed);
    CHECK(!find(EventType::PointerDown));
    CHECK(find(EventType::StateChanged));
    if (maxed) {
        // WM_NCCALCSIZE clamps the maximised client to the work area: nothing
        // of the app's frame falls off-screen.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
        RECT  rc;
        POINT origin{0, 0};
        GetClientRect(hwnd(), &rc);
        ClientToScreen(hwnd(), &origin);
        std::printf(
            "    client %ld,%ld %ldx%ld; work area %ld,%ld %ldx%ld\n",
            origin.x,
            origin.y,
            rc.right,
            rc.bottom,
            mi.rcWork.left,
            mi.rcWork.top,
            mi.rcWork.right - mi.rcWork.left,
            mi.rcWork.bottom - mi.rcWork.top
        );
        CHECK(origin.x >= mi.rcWork.left && origin.y >= mi.rcWork.top);
        CHECK(origin.x + rc.right <= mi.rcWork.right && origin.y + rc.bottom <= mi.rcWork.bottom);
        CHECK(rc.right >= (mi.rcWork.right - mi.rcWork.left) - 2);
    }
    pumpFor(600);
}

void caseCaptionDoubleClickRestores() {
    if (!g_win->isMaximized()) {
        g_win->setMaximized(true);
        pumpUntil([] { return g_win->isMaximized(); }, 2000);
    }
    pumpFor(100);
    hooks()->injectPointerMove(*g_win, {120, 10});
    pumpFor(50);
    POINT p;
    GetCursorPos(&p);
    if (WindowFromPoint(p) != hwnd()) {
        // Wine's X11 driver sees a maximised WS_CAPTION window whose client
        // is inset from its window rect and assumes a WM-drawn title bar
        // there, cutting that band out of the window's visible region; the
        // pointer then lands on the desktop. Real Windows routes it to us.
        skip("Wine gives the top band of a maximised custom-frame window to the desktop");
        restore();
        return;
    }
    g_events.clear();
    click();
    click();
    CHECK(pumpUntil([] { return !g_win->isMaximized(); }, 2000));
    CHECK(!find(EventType::PointerDown));
    pumpFor(600);
}

void caseCaptionDragMovesThroughOsLoop() {
    restore();
    RECT before;
    GetWindowRect(hwnd(), &before);
    hooks()->injectPointerMove(*g_win, {120, 10});
    pumpFor(100);
    g_events.clear();
    // Once the drag starts, the OS move loop owns the message pump until the
    // release, so pump() does not return in between: the rest of the gesture
    // is driven from a plat timer — which also proves timers keep firing
    // inside the modal loop.
    int   ticks = 0;
    POINT start;
    GetCursorPos(&start);
    TimerId id = 0;
    id         = g_app->addTimer(20, true, [&] {
        ++ticks;
        if (ticks <= 5) {
            INPUT in{};
            in.type       = INPUT_MOUSE;
            in.mi.dx      = 12;
            in.mi.dy      = 8;
            in.mi.dwFlags = MOUSEEVENTF_MOVE; // relative
            SendInput(1, &in, sizeof(in));
        } else if (ticks == 6) {
            hooks()->injectButton(*g_win, Button::Left, false);
        } else if (ticks >= 8) {
            g_app->cancelTimer(id);
        }
    });
    hooks()->injectButton(*g_win, Button::Left, true);
    pumpUntil([&] { return ticks >= 8; }, 4000);
    pumpFor(100);
    RECT after;
    GetWindowRect(hwnd(), &after);
    std::printf(
        "    window moved by %ld,%ld; %d timer ticks\n",
        after.left - before.left,
        after.top - before.top,
        ticks
    );
    CHECK(ticks >= 8);
    CHECK(after.left - before.left >= 20 && after.top - before.top >= 10);
    CHECK(!find(EventType::PointerDown));
    CHECK(!find(EventType::PointerUp));
    pumpFor(600);
}

void caseLiveResizePaintsInsideOsLoop() {
    // Drag the right edge through the OS sizing loop; Resized and a painted
    // Frame must arrive while the loop runs (before the release), which is
    // what keeps the content glued to the edge on real Windows.
    restore();
    const Size before = g_win->size();
    hooks()->injectPointerMove(*g_win, {double(before.w - 2), 150});
    pumpFor(100);
    if (SendMessageW(hwnd(), WM_NCHITTEST, 0, [] {
            POINT p;
            GetCursorPos(&p);
            return MAKELPARAM(p.x, p.y);
        }()) != HTRIGHT) {
        CHECK(!"edge hit test is not HTRIGHT");
        return;
    }
    g_events.clear();
    int     ticks = 0, framesBeforeRelease = -1, resizedBeforeRelease = -1;
    int     startFrames = g_frames;
    TimerId id          = 0;
    id                  = g_app->addTimer(20, true, [&] {
        ++ticks;
        if (ticks <= 5) {
            INPUT in{};
            in.type       = INPUT_MOUSE;
            in.mi.dx      = 15;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof(in));
        } else if (ticks == 7) {
            framesBeforeRelease  = g_frames - startFrames;
            resizedBeforeRelease = count(EventType::Resized);
            hooks()->injectButton(*g_win, Button::Left, false);
        } else if (ticks >= 9) {
            g_app->cancelTimer(id);
        }
    });
    hooks()->injectButton(*g_win, Button::Left, true);
    pumpUntil([&] { return ticks >= 9; }, 4000);
    pumpFor(100);
    std::printf(
        "    width %d -> %d; %d Resized, %d frames before release\n",
        before.w,
        g_win->size().w,
        resizedBeforeRelease,
        framesBeforeRelease
    );
    CHECK(g_win->size().w >= before.w + 30);
    CHECK(resizedBeforeRelease >= 1);
    CHECK(framesBeforeRelease >= 1);
    CHECK(!find(EventType::PointerDown));
    g_win->setSize({320, 240});
    pumpFor(600);
}

void casePointerLeave() {
    hooks()->injectPointerMove(*g_win, {100, 100});
    pumpFor(50);
    g_events.clear();
    RECT r;
    GetWindowRect(hwnd(), &r);
    SetCursorPos(r.right + 40, r.bottom + 40); // plain motion outside the window
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE; // a zero relative move makes the OS re-check
    SendInput(1, &in, sizeof(in));
    CHECK(pumpUntil([] { return find(EventType::PointerLeave) != nullptr; }, 2000));
    g_events.clear();
    hooks()->injectPointerMove(*g_win, {100, 100});
    CHECK(pumpUntil([] { return find(EventType::PointerEnter) != nullptr; }, 2000));
}

void casePreciseWheel() {
    hooks()->injectPointerMove(*g_win, {100, 100});
    pumpFor(50);
    g_events.clear();
    INPUT in{};
    in.type         = INPUT_MOUSE;
    in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = DWORD(-40); // a third of a notch, towards the user
    SendInput(1, &in, sizeof(in));
    hooks()->injectScroll(*g_win, 2, 0);
    CHECK(pumpUntil([] { return count(EventType::Scroll) >= 2; }, 2000));
    const Event *v = find(EventType::Scroll, [](auto &e) { return e.dy != 0; });
    const Event *h = find(EventType::Scroll, [](auto &e) { return e.dx != 0; });
    CHECK(v && v->precise && v->dy > 5 && v->dy < 100); // pixels, downwards
    CHECK(h && !h->precise && h->dx == 2);              // notches, rightwards
}

// ── keyboard ────────────────────────────────────────────────────────────────

void caseSurrogatePair() {
    g_win->setTextInput({.enabled = true});
    g_events.clear();
    PostMessageW(hwnd(), WM_CHAR, 0xD83D, 1);
    PostMessageW(hwnd(), WM_CHAR, 0xDE00, 1);
    PostMessageW(hwnd(), WM_CHAR, 0x08, 1); // Backspace as a char: not text
    CHECK(pumpUntil([] { return find(EventType::TextInput) != nullptr; }, 1000));
    pumpFor(50);
    CHECK(count(EventType::TextInput) == 1);
    const Event *t = find(EventType::TextInput);
    CHECK(t && t->text == "\xF0\x9F\x98\x80");
}

void caseTextDisabledTypesNothing() {
    g_win->setTextInput({.enabled = false});
    g_events.clear();
    hooks()->injectKey(*g_win, Key::B, true);
    hooks()->injectKey(*g_win, Key::B, false);
    CHECK(pumpUntil([] { return find(EventType::KeyUp) != nullptr; }, 2000));
    CHECK(find(EventType::KeyDown, [](auto &e) { return e.key == Key::B; }));
    CHECK(!find(EventType::TextInput));
    g_win->setTextInput({.enabled = true});
}

void caseAltGrHasNoFakeCtrl() {
    // AltGr = synthetic LCtrl + RAlt sharing one timestamp. Give SendInput
    // that exact pair; the Ctrl half must not reach the app.
    const DWORD t = GetTickCount();
    INPUT       in[4]{};
    for (auto &i : in)
        i.type = INPUT_KEYBOARD;
    in[0].ki = {VK_LCONTROL, 0x1d, 0, t, 0};
    in[1].ki = {VK_RMENU, 0x38, KEYEVENTF_EXTENDEDKEY, t, 0};
    in[2].ki = {VK_RMENU, 0x38, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, t + 50, 0};
    in[3].ki = {VK_LCONTROL, 0x1d, KEYEVENTF_KEYUP, t + 50, 0};
    SetForegroundWindow(hwnd());
    g_events.clear();
    SendInput(4, in, sizeof(INPUT));
    pumpUntil([] { return count(EventType::KeyUp) >= 1; }, 2000);
    pumpFor(100);
    const Event *alt = find(EventType::KeyDown, [](auto &e) { return e.key == Key::AltRight; });
    if (!alt) {
        skip("no AltRight KeyDown arrived");
        return;
    }
    if (find(EventType::KeyDown, [](auto &e) { return e.key == Key::ControlLeft; })) {
        // Real Windows keeps ki.time, so a Ctrl here is our bug; Wine may
        // restamp injected input, which makes the pairing untestable there.
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
            skip("Wine does not keep SendInput timestamps (AltGr pairing untestable)");
            return;
        }
        CHECK(!"fake AltGr Ctrl was reported");
    }
}

// ── window ──────────────────────────────────────────────────────────────────

void caseDpiChange() {
    restore();
    const Size before = g_win->size();
    RECT       r;
    GetWindowRect(hwnd(), &r);
    // What Windows sends when the window is dragged onto a 150 % monitor.
    RECT suggested{
        r.left,
        r.top,
        r.left + LONG((r.right - r.left) * 1.5),
        r.top + LONG((r.bottom - r.top) * 1.5)
    };
    g_events.clear();
    const int frames = g_frames;
    SendMessageW(hwnd(), WM_DPICHANGED, MAKEWPARAM(144, 144), LPARAM(&suggested));
    CHECK(g_win->scale() == 1.5);
    CHECK(find(EventType::Resized));
    const Size after = g_win->size();
    std::printf("    logical %dx%d -> %dx%d at scale 1.5\n", before.w, before.h, after.w, after.h);
    CHECK(std::abs(after.w - before.w) <= 1 && std::abs(after.h - before.h) <= 1);
    CHECK(pumpUntil([&] { return g_frames > frames; }, 2000));
    Canvas c = g_win->beginPaint();
    CHECK(c.scale == 1.5 && std::abs(c.width - int(std::lround(after.w * 1.5))) <= 1);
    g_win->endPaint({});
    // And back.
    GetWindowRect(hwnd(), &r);
    RECT back{
        r.left,
        r.top,
        r.left + LONG(std::lround((r.right - r.left) / 1.5)),
        r.top + LONG(std::lround((r.bottom - r.top) / 1.5))
    };
    SendMessageW(hwnd(), WM_DPICHANGED, MAKEWPARAM(96, 96), LPARAM(&back));
    CHECK(g_win->scale() == 1.0);
    pumpFor(100);
}

void caseFramePacing() {
    // An app that asks for a frame from every Frame gets the display rate,
    // not a busy loop.
    int                   frames = 0;
    std::function<void()> none;
    const auto            start = Clock::now();
    auto                  prev  = std::move(g_events);
    g_win->requestFrame();
    while (Clock::now() - start < std::chrono::milliseconds(500)) {
        const int before = g_frames;
        g_app->pump(50);
        if (g_frames > before) {
            frames += g_frames - before;
            g_win->requestFrame();
        }
    }
    std::printf("    %d frames in 500 ms\n", frames);
    CHECK(frames >= 15 && frames <= 40);
}

void caseFullscreen() {
    restore();
    g_win->setFullscreen(true);
    pumpFor(200);
    CHECK(g_win->isFullscreen());
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
    RECT rc;
    GetClientRect(hwnd(), &rc);
    CHECK(rc.right == mi.rcMonitor.right - mi.rcMonitor.left);
    CHECK(rc.bottom == mi.rcMonitor.bottom - mi.rcMonitor.top);
    g_win->setFullscreen(false);
    pumpFor(200);
    CHECK(!g_win->isFullscreen());
    GetClientRect(hwnd(), &rc);
    CHECK(rc.right < mi.rcMonitor.right - mi.rcMonitor.left);
}

void caseResizeLiveFramesAndMinSize() {
    restore();
    g_win->setMinSize({200, 150});
    g_win->setSize({100, 100});
    pumpFor(200);
    CHECK(g_win->size().w >= 200 && g_win->size().h >= 150);
    g_win->setSize({320, 240});
    pumpFor(200);
    CHECK(g_win->size().w == 320);
}

// ── clipboard, drops ────────────────────────────────────────────────────────

void caseClipboardCrlfAndCustomMime() {
    g_app->setClipboardText("one\ntwo\r\nthree");
    std::wstring raw;
    if (OpenClipboard(nullptr)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            raw = static_cast<const wchar_t *>(GlobalLock(h));
            GlobalUnlock(h);
        }
        CloseClipboard();
    }
    CHECK(raw == L"one\r\ntwo\r\nthree");
    std::optional<std::string> got;
    bool                       done = false;
    g_app->requestClipboard("text/plain", [&](auto v) {
        got  = v;
        done = true;
    });
    CHECK(!done);
    CHECK(pumpUntil([&] { return done; }, 1000));
    CHECK(got && *got == "one\ntwo\nthree");

    const std::string blob("a\0b\xff", 4);
    g_app->setClipboard({{"application/x-plat-test", blob}});
    done = false;
    g_app->requestClipboard("application/x-plat-test", [&](auto v) {
        got  = v;
        done = true;
    });
    CHECK(pumpUntil([&] { return done; }, 1000));
    CHECK(got && got->substr(0, 4) == blob);
    done = false;
    g_app->requestClipboard("text/plain", [&](auto v) { // replaced by the blob
        got  = v;
        done = true;
    });
    CHECK(pumpUntil([&] { return done; }, 1000));
    CHECK(!got);
}

void caseDropFiles() {
    // Build the HDROP Explorer would: DROPFILES + double-NUL-terminated
    // wide paths, owned by the receiver (DragFinish frees it).
    const std::wstring files =
        std::wstring(L"C:\\dir x\\\u00e5.txt") + L'\0' + L"D:\\b" + L'\0' + L'\0';
    HGLOBAL g  = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + files.size() * 2);
    auto   *df = static_cast<DROPFILES *>(GlobalLock(g));
    df->pFiles = sizeof(DROPFILES);
    df->pt     = {50, 60};
    df->fNC    = FALSE;
    df->fWide  = TRUE;
    std::memcpy(reinterpret_cast<char *>(df) + sizeof(DROPFILES), files.data(), files.size() * 2);
    GlobalUnlock(g);
    g_events.clear();
    PostMessageW(hwnd(), WM_DROPFILES, WPARAM(g), 0);
    CHECK(pumpUntil([] { return find(EventType::Drop) != nullptr; }, 1000));
    const Event *d = find(EventType::Drop);
    CHECK(d && d->uris.size() == 2);
    if (d && d->uris.size() == 2) {
        CHECK(d->uris[0] == "file:///C:/dir%20x/%C3%A5.txt");
        CHECK(d->uris[1] == "file:///D:/b");
        CHECK(d->pos.x == 50 && d->pos.y == 60);
    }
}

// ── formats ─────────────────────────────────────────────────────────────────

Win32App &app() {
    return static_cast<Win32App &>(*g_app);
}

std::optional<std::string> readClipboard(std::string_view mime) {
    std::optional<std::string> got;
    bool                       done = false;
    g_app->requestClipboard(mime, [&](auto v) {
        got  = v;
        done = true;
    });
    pumpUntil([&] { return done; }, 1000);
    return got;
}

std::vector<std::string> clipboardMimes() {
    std::vector<std::string> m;
    bool                     done = false;
    g_app->requestClipboardMimes([&](auto v) {
        m    = std::move(v);
        done = true;
    });
    pumpUntil([&] { return done; }, 1000);
    return m;
}

bool has(const std::vector<std::string> &v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

long cfHtmlField(const std::string &d, const char *key) {
    const size_t at = d.find(key);
    return at == std::string::npos ? -2
                                   : std::strtol(d.c_str() + at + std::strlen(key), nullptr, 10);
}

void caseCfHtmlHeader() {
    const std::string frag = "<b>bold</b> \xE2\x9C\x93 &amp; more"; // UTF-8: offsets are bytes
    const std::string d    = cfHtmlEncode(frag);
    const long        sh = cfHtmlField(d, "StartHTML:"), eh = cfHtmlField(d, "EndHTML:");
    const long        sf = cfHtmlField(d, "StartFragment:"), ef = cfHtmlField(d, "EndFragment:");
    std::printf(
        "    StartHTML %ld EndHTML %ld StartFragment %ld EndFragment %ld size %zu\n",
        sh,
        eh,
        sf,
        ef,
        d.size()
    );
    CHECK(sh > 0 && d.compare(size_t(sh), 6, "<html>") == 0);
    CHECK(eh == long(d.size()) - 1 && d.back() == '\0'); // the terminator is outside
    CHECK(d.substr(size_t(sf), size_t(ef - sf)) == frag);
    CHECK(d.compare(size_t(sf) - 20, 20, "<!--StartFragment-->") == 0);
    CHECK(cfHtmlDecode(d) == frag);
    // Foreign producers: offsets of -1 (markers only), and a header with no
    // fragment at all (the whole html is taken).
    const std::string markers = "Version:1.0\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:-1\r\n"
                                "EndFragment:-1\r\n<html><body><!--StartFragment--><i>x</i>"
                                "<!--EndFragment--></body></html>";
    CHECK(cfHtmlDecode(markers) == "<i>x</i>");
    const std::string bare =
        "Version:0.9\r\nStartHTML:0000000055\r\nEndHTML:0000000071\r\n<p>whole doc</p>";
    CHECK(cfHtmlDecode(bare) == "<p>whole doc</p>");

    // And what the clipboard really holds after setClipboard(text/html).
    g_app->setClipboard({{"text/html", frag}, {"text/plain;charset=utf-8", "bold"}});
    std::string raw;
    if (OpenClipboard(nullptr)) {
        if (HANDLE h = GetClipboardData(RegisterClipboardFormatW(L"HTML Format"))) {
            raw.assign(static_cast<const char *>(GlobalLock(h)), GlobalSize(h));
            GlobalUnlock(h);
        }
        CloseClipboard();
    }
    CHECK(raw.substr(0, d.size()) == d);
    CHECK(readClipboard("text/html") == frag);
}

void caseHdropUriList() {
    CHECK(
        pathFromFileUri("file:///C:/dir%20x/%C3%A5.txt") == std::wstring(L"C:\\dir x\\\u00e5.txt")
    );
    CHECK(pathFromFileUri("file://srv/share/b") == std::wstring(L"\\\\srv\\share\\b"));
    CHECK(pathFromFileUri("file:///tmp/x") == std::nullopt); // POSIX path: not a Windows file
    CHECK(pathFromFileUri("https://example.com/a") == std::nullopt);
    const std::string list = "file:///C:/dir%20x/%C3%A5.txt\r\nfile://srv/share/b\r\n";
    const std::string drop = hdropFromUriList(list);
    CHECK(!drop.empty());
    CHECK(hdropToUriList(drop.data(), drop.size()) == list);
    CHECK(hdropFromUriList("file:///C:/a\r\nhttps://example.com/\r\n").empty()); // not all files

    // A uri-list of local files also goes out as CF_HDROP, which Explorer pastes.
    g_app->setClipboard({{"text/uri-list", list}});
    std::vector<std::wstring> paths;
    if (OpenClipboard(nullptr)) {
        if (HANDLE h = GetClipboardData(CF_HDROP)) {
            const UINT n = DragQueryFileW(HDROP(h), 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH];
                DragQueryFileW(HDROP(h), i, buf, MAX_PATH);
                paths.emplace_back(buf);
            }
        }
        CloseClipboard();
    }
    CHECK(
        paths.size() == 2 && paths[0] == L"C:\\dir x\\\u00e5.txt" &&
        paths[1] == L"\\\\srv\\share\\b"
    );

    // A CF_HDROP from another app (only that format) reads as text/uri-list.
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, drop.size());
        std::memcpy(GlobalLock(g), drop.data(), drop.size());
        GlobalUnlock(g);
        SetClipboardData(CF_HDROP, g);
        CloseClipboard();
    }
    CHECK(has(clipboardMimes(), "text/uri-list"));
    CHECK(readClipboard("text/uri-list") == list);
}

void caseDibClipboard() {
    // What PrtScn leaves: a bare CF_DIB. Offered as image/bmp and, through
    // WIC, as image/png.
    const int        w = 3, h = 2;
    BITMAPINFOHEADER bh{sizeof(bh), w, h, 1, 32, BI_RGB};
    std::string      dib(reinterpret_cast<const char *>(&bh), sizeof(bh));
    for (int i = 0; i < w * h; ++i)
        dib.append("\x10\x20\xf0\x00", 4); // BGRX: red-ish
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, dib.size());
        std::memcpy(GlobalLock(g), dib.data(), dib.size());
        GlobalUnlock(g);
        SetClipboardData(CF_DIB, g);
        CloseClipboard();
    }
    const auto mimes = clipboardMimes();
    for (auto &m : mimes)
        std::printf("    offered: %s\n", m.c_str());
    CHECK(has(mimes, "image/bmp"));
    CHECK(has(mimes, "image/png"));
    const auto bmp = readClipboard("image/bmp");
    CHECK(bmp && bmp->size() >= 14 + dib.size() && bmp->compare(0, 2, "BM") == 0);
    if (bmp) {
        BITMAPFILEHEADER fh;
        std::memcpy(&fh, bmp->data(), sizeof(fh));
        CHECK(fh.bfOffBits == sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    }
    const auto png = readClipboard("image/png");
    CHECK(png && png->compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    CHECK(!readClipboard("text/plain"));
}

void caseImageHelpers() {
    const Image red{16, 16, std::vector<uint32_t>(256, 0xffd02020)};
    const Image big{64, 64, std::vector<uint32_t>(64 * 64, 0x80400000)}; // premultiplied half alpha
    Image       fit = fitImage({red, big}, 20); // smallest that is big enough
    CHECK(fit.width == 20 && fit.height == 20 && fit.pixels[210] == 0x80400000);
    fit = fitImage({red}, 24); // only a smaller one: scaled up
    CHECK(fit.width == 24 && (fit.pixels[300] & 0xffffff) == 0xd02020);
    HICON icon = iconFromImage(fit);
    CHECK(icon != nullptr);
    if (icon) {
        ICONINFO ii{};
        BITMAP   bm{};
        CHECK(GetIconInfo(icon, &ii));
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        CHECK(bm.bmWidth == 24 && bm.bmHeight == 24);
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        DestroyIcon(icon);
    }
    const std::string png = encodePng(big);
    CHECK(png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    // Badge: transparent corners, red disc, white glyph pixels; "9+" past nine.
    for (int n : {7, 12}) {
        const Image b = badgeImage(n, 16);
        CHECK(b.width == 16 && b.pixels[0] == 0);
        int white = 0, red = 0;
        for (uint32_t p : b.pixels) {
            white += p == 0xffffffff;
            red += p == 0xffd93025;
        }
        CHECK(white >= 8 && red >= 60);
    }
    CHECK(badgeImage(12, 16).pixels != badgeImage(9, 16).pixels);
    g_app->setBadgeCount(3); // overlay on the taskbar button; not readable back
    g_app->setBadgeCount(0);
}

// ── shell ───────────────────────────────────────────────────────────────────

void caseTaskbarCreatedReAdds() {
    auto tray = g_app->createTray();
    CHECK(tray != nullptr);
    if (!tray)
        return;
    tray->setIcon({Image{16, 16, std::vector<uint32_t>(256, 0xff2040d0)}});
    if (!tray->isVisible()) {
        skip("no notification area accepted the icon");
        return;
    }
    CHECK(app().trayAddCount(*tray) == 1);
    // What Explorer broadcasts after it (re)starts.
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    CHECK(pumpUntil([&] { return app().trayAddCount(*tray) == 2; }, 2000));
    CHECK(tray->isVisible());
}

void caseBalloonFallback() {
    // Wine has no WinRT toasts, so there the fallback is the default; force
    // it for real Windows too.
    app().forceBalloonNotifications(true);
    if (!g_app->notificationsAvailable()) {
        skip("Shell_NotifyIcon refused the probe icon");
        app().forceBalloonNotifications(false);
        return;
    }
    Image          avatar{48, 48, std::vector<uint32_t>(48 * 48, 0xff2b8a3e)};
    const uint64_t a = 1, b = 2;
    CHECK(g_app->notify(
        a, {.title = "Balloon", .body = "clicked", .image = avatar, .actions = {{"reply", "Reply"}}}
    ));
    CHECK(g_app->notify(b, {.title = "Balloon", .body = "times out"}));
    pumpFor(100);
    g_events.clear();
    CHECK(!hooks()->notificationInvoke(a, "reply")); // balloons have no buttons
    CHECK(hooks()->notificationInvoke(a, ""));       // NIN_BALLOONUSERCLICK
    CHECK(app().postBalloonCallback(b, NIN_BALLOONTIMEOUT));
    CHECK(pumpUntil([&] { return count(EventType::NotificationClosed) >= 1; }, 2000));
    const Event *act = find(EventType::NotificationActivated);
    CHECK(act && act->id == a && act->action.empty() && act->window == nullptr);
    CHECK(find(EventType::NotificationClosed, [&](auto &e) { return e.id == b; }));
    CHECK(!find(EventType::NotificationClosed, [&](auto &e) {
        return e.id == a;
    })); // clicked ≠ dismissed
    // Gone after the click: a second click on it has nothing to hit.
    CHECK(!hooks()->notificationInvoke(a, ""));
    app().forceBalloonNotifications(false);
}

// ── drag source ─────────────────────────────────────────────────────────────

void caseDoDragDropKeepsTimersAlive() {
    // DoDragDrop owns the loop until the drop; plat timers must keep firing
    // inside it (they inject the Escape that cancels the drag here), and an
    // Escape ends it with DragFinished(None).
    hooks()->injectPointerMove(*g_win, {100, 150});
    pumpFor(100);
    g_events.clear();
    bool    inDrag = false, started = false, ok = false;
    int     ticksInside = 0;
    TimerId id          = g_app->addTimer(20, true, [&] {
        if (!inDrag)
            return;
        if (++ticksInside == 5) {
            hooks()->injectKey(*g_win, Key::Escape, true);
            hooks()->injectKey(*g_win, Key::Escape, false);
        }
    });
    g_app->setEventHandler([&](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
        if (!started && e.type == EventType::PointerDown) {
            started = true;
            inDrag  = true;
            ok = g_app->startDrag(*g_win, {.items = {{"text/plain;charset=utf-8", "escape me"}}});
            inDrag = false;
        }
    });
    hooks()->injectButton(*g_win, Button::Left, true);
    pumpUntil([&] { return started && find(EventType::DragFinished); }, 4000);
    hooks()->injectButton(*g_win, Button::Left, false);
    pumpFor(100);
    g_app->cancelTimer(id);
    g_app->setEventHandler([](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
    });
    std::printf("    %d timer ticks inside DoDragDrop\n", ticksInside);
    CHECK(started && ok);
    CHECK(ticksInside >= 5);
    const Event *done = find(EventType::DragFinished);
    CHECK(done && done->dropAction == DropAction::None && done->window == g_win.get());
    CHECK(!find(EventType::Drop));
    pumpFor(600);
}

// An OS modal loop (a menu, a file dialog, a window move) runs its own pump:
// the modal timer is armed for the next due timer or frame only, killed when
// nothing is due (no 10 ms ticks while a dialog sits open), and posted work
// still arrives through the wake message. The modal loop is simulated with a
// bare GetMessage/DispatchMessage loop, as the OS ones are.
void caseModalTimerArmsOnlyForDueWork() {
    auto &w32 = static_cast<Win32App &>(*g_app);
    pumpFor(300); // pending frames and timers settle
    const auto modalPump = [](int ms, const std::function<bool()> &until) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end && !(until && until())) {
            MSG m;
            if (MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
                WAIT_TIMEOUT)
                continue;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    };
    w32.enterModal();
    std::printf("    idle modal timer: %d ms\n", w32.modalTimerMs());
    // Nothing due (no plat timer, no frame; a handle watch polls at 100 ms).
    CHECK(w32.modalTimerMs() == -1 || w32.modalTimerMs() >= 100);
    // A plat timer: armed for it (not 10 ms), and it fires inside the loop.
    bool    fired = false;
    TimerId t     = g_app->addTimer(300, false, [&] { fired = true; });
    std::printf("    with a 300 ms timer: %d ms\n", w32.modalTimerMs());
    CHECK(w32.modalTimerMs() >= 100 && w32.modalTimerMs() <= 300);
    modalPump(2000, [&] { return fired; });
    CHECK(fired);
    // Posted work reaches the modal loop through the wake message.
    bool posted = false;
    g_app->post([&] { posted = true; });
    modalPump(1000, [&] { return posted; });
    CHECK(posted);
    // A frame request: served inside the loop.
    const int frames = g_frames;
    g_win->requestFrame();
    modalPump(1000, [&] { return g_frames > frames; });
    CHECK(g_frames > frames);
    // Idle again: no ticks.
    modalPump(100, nullptr);
    std::printf("    idle again: %d ms\n", w32.modalTimerMs());
    CHECK(w32.modalTimerMs() == -1 || w32.modalTimerMs() >= 100);
    w32.leaveModal();
    CHECK(w32.modalTimerMs() == -1);
    g_app->cancelTimer(t);
}

void caseDesktopQueries() {
    std::printf("    darkMode %d, doubleClickMs %d\n", g_app->darkMode(), g_app->doubleClickMs());
    CHECK(g_app->doubleClickMs() > 0);
    CHECK(!g_app->openUrl("C:\\Windows\\System32\\calc.exe")); // a path, not a URL
    CHECK(!g_app->openUrl("notaurl"));
}

// ── round 3: monitors, instances, settings, system events ───────────────────

bool approxAt(Point p, double x, double y) {
    return std::abs(p.x - x) < 1e-9 && std::abs(p.y - y) < 1e-9;
}

void caseMixedDpiMath() {
    // A 1.5x monitor left of a 1x primary, a 2x one to its right — the
    // layout where Windows has no single logical space.
    const MonitorGeom              left{{-2880, 0, 0, 1620}, 1.5};
    const MonitorGeom              prim{{0, 0, 1920, 1080}, 1.0};
    const MonitorGeom              right{{1920, 0, 1920 + 3840, 2160}, 2.0};
    const std::vector<MonitorGeom> mons{left, prim, right};

    // Origins stay physical, sizes become logical.
    const Rect rb = logicalBounds(right), lb = logicalBounds(left);
    CHECK(rb.x == 1920 && rb.y == 0 && rb.w == 1920 && rb.h == 1080);
    CHECK(lb.x == -2880 && lb.w == 1920 && lb.h == 1080);

    // A point converts through the monitor that contains it.
    CHECK(approxAt(physicalToLogical({2120, 100}, mons), 2020, 50));
    CHECK(approxAt(physicalToLogical({100, 100}, mons), 100, 100));
    CHECK(approxAt(physicalToLogical({-60, 30}, mons), -1000, 20));
    const POINT r = logicalToPhysical({2020, 50}, mons);
    CHECK(r.x == 2120 && r.y == 100);
    const POINT l = logicalToPhysical({-1000, 20}, mons);
    CHECK(l.x == -60 && l.y == 30);
    // Round trips on every monitor.
    for (const auto &m : mons) {
        const Rect  b = logicalBounds(m);
        const Point p{b.x + b.w / 3.0, b.y + b.h / 4.0};
        const POINT q    = logicalToPhysical(p, mons);
        const Point back = physicalToLogical(q, mons);
        CHECK(std::abs(back.x - p.x) <= 1 && std::abs(back.y - p.y) <= 1);
    }
    // Off every monitor: the nearest one decides (a window half off-screen).
    CHECK(approxAt(physicalToLogical({1920 + 200, 3000}, mons), 1920 + 100, 1500));
    CHECK(approxAt(physicalToLogical({5, 5}, {}), 5, 5)); // no monitors at all

    // Work areas: edges rounded, so they never poke past the bounds.
    const MonitorGeom odd{{0, 0, 1001, 751}, 1.5};
    const Rect        ob = logicalBounds(odd);
    const Rect        ow = logicalRect(odd, RECT{1, 0, 1001, 700});
    CHECK(ow.x >= ob.x && ow.x + ow.w <= ob.x + ob.w && ow.y + ow.h <= ob.y + ob.h);

    CHECK(monitorId(L"\\\\.\\DISPLAY1") == monitorId(L"\\\\.\\DISPLAY1"));
    CHECK(monitorId(L"\\\\.\\DISPLAY1") != monitorId(L"\\\\.\\DISPLAY2"));
    CHECK(monitorId(L"") != 0);
}

void caseLiveMonitorsAndPosition() {
    const auto mons = g_app->monitors();
    CHECK(!mons.empty());
    const auto geoms = monitorGeoms();
    CHECK(geoms.size() == mons.size());
    // The window's own position goes through the same rule.
    auto w = g_app->createWindow({
        .title       = "plat position",
        .size        = {200, 120},
        .decorations = Decorations::System,
        .visible     = false,
        .position    = Point{150, 110},
    });
    CHECK(w != nullptr);
    if (!w)
        return;
    auto p = w->position();
    CHECK(p && std::abs(p->x - 150) <= 1 && std::abs(p->y - 110) <= 1);
    if (p)
        std::printf("    system-framed window created with its client at %.0f,%.0f\n", p->x, p->y);
    // The client, not the frame, lands on the requested point.
    POINT c{0, 0};
    ClientToScreen(static_cast<HWND>(w->nativeHandle()), &c);
    RECT wr;
    GetWindowRect(static_cast<HWND>(w->nativeHandle()), &wr);
    CHECK(c.y > wr.top); // a caption above the client
    g_events.clear();
    CHECK(w->setPosition({60, 70}));
    p = w->position();
    CHECK(p && std::abs(p->x - 60) <= 1 && std::abs(p->y - 70) <= 1);
    CHECK(find(EventType::Moved, [&](auto &e) { return e.window == w.get(); }));
    const int moves = count(EventType::Moved);
    CHECK(w->setPosition({60, 70})); // no change: no Moved
    CHECK(count(EventType::Moved) == moves);
    CHECK(w->monitor() == mons.front().id || std::any_of(mons.begin(), mons.end(), [&](auto &m) {
              return m.id == w->monitor();
          }));
    w->setMaximized(true);
    pumpFor(100);
    if (w->isMaximized())
        CHECK(!w->setPosition({10, 10})); // the shell's to place while maximised
}

void caseMonitorAndThemeDedup() {
    // WM_DISPLAYCHANGE / WM_SETTINGCHANGE reach every top-level window; the
    // App only reports actual changes.
    pumpFor(1100); // the deferred monitor baseline
    HWND sys = static_cast<Win32App *>(g_app.get())->systemWindow();
    CHECK(sys != nullptr);
    if (!sys)
        return;
    g_events.clear();
    SendMessageW(sys, WM_DISPLAYCHANGE, 32, MAKELPARAM(1280, 800));
    SendMessageW(sys, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
    SendMessageW(hwnd(), WM_SETTINGCHANGE, 0, LPARAM(L"ImmersiveColorSet"));
    SendMessageW(sys, WM_SETTINGCHANGE, 0, LPARAM(L"ImmersiveColorSet"));
    pumpFor(50);
    CHECK(count(EventType::MonitorsChanged) == 0);
    CHECK(count(EventType::ThemeChanged) == 0);

    if (!std::getenv("PLAT_SELFTEST_ALLOW_REGISTRATION")) {
        std::printf(
            "    (settings changes need PLAT_SELFTEST_ALLOW_REGISTRATION: they write HKCU)\n"
        );
        return;
    }
    // A real change, the way Settings makes it: the value, then the broadcast.
    HKEY key = nullptr;
    CHECK(
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Accessibility",
            0,
            nullptr,
            0,
            KEY_ALL_ACCESS,
            nullptr,
            &key,
            nullptr
        ) == ERROR_SUCCESS
    );
    HKEY dwm = nullptr;
    CHECK(
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\DWM",
            0,
            nullptr,
            0,
            KEY_ALL_ACCESS,
            nullptr,
            &dwm,
            nullptr
        ) == ERROR_SUCCESS
    );
    if (!key || !dwm)
        return;
    DWORD pct = 150, accent = 0xffd77800; // ABGR: Windows' default blue
    RegSetValueExW(key, L"TextScaleFactor", 0, REG_DWORD, reinterpret_cast<BYTE *>(&pct), 4);
    RegSetValueExW(dwm, L"AccentColor", 0, REG_DWORD, reinterpret_cast<BYTE *>(&accent), 4);
    SendMessageW(hwnd(), WM_SETTINGCHANGE, 0, LPARAM(L"WindowMetrics"));
    SendMessageW(sys, WM_SETTINGCHANGE, 0, LPARAM(L"WindowMetrics")); // the same broadcast again
    const SystemSettings s = g_app->systemSettings();
    std::printf("    textScale %.2f accent %08x\n", s.textScale, s.accentColor);
    CHECK(s.textScale == 1.5);
    CHECK(s.accentColor == 0xff0078d7);
    CHECK(count(EventType::ThemeChanged, [](auto &e) { return e.window == g_win.get(); }) == 1);
    RegDeleteValueW(key, L"TextScaleFactor");
    RegDeleteValueW(dwm, L"AccentColor");
    RegCloseKey(key);
    RegCloseKey(dwm);
    g_events.clear();
    SendMessageW(sys, WM_SETTINGCHANGE, 0, 0);
    CHECK(count(EventType::ThemeChanged) == 1);
    CHECK(g_app->systemSettings().textScale == 1.0);
}

void caseSettingsParsing() {
    CHECK(accentFromAbgr(0xffd77800) == 0xff0078d7);
    CHECK(accentFromAbgr(0x00112233) == 0xff332211); // alpha is not trusted
    CHECK(textScaleFromPercent(100) == 1.0 && textScaleFromPercent(225) == 2.25);
    CHECK(textScaleFromPercent(0) == 1.0 && textScaleFromPercent(400) == 1.0);
    CHECK(caretBlinkFromOs(INFINITE) == 0 && caretBlinkFromOs(530) == 530);
    CHECK(portablePath(L"C:\\a b\\\u00fc.txt") == "C:/a b/\xc3\xbc.txt");
    CHECK(portablePath(L"\\\\?\\C:\\long\\x") == "C:/long/x");
    CHECK(portablePath(L"\\\\?\\UNC\\srv\\share\\x") == "//srv/share/x");
    CHECK(nativePath("C:/a/b") == L"C:\\a\\b");
    // Tray: pixel size decides, Image::scale breaks a tie.
    Image one{32, 32, std::vector<uint32_t>(32 * 32, 0xff0000ff), 1.0};
    Image two{32, 32, std::vector<uint32_t>(32 * 32, 0xffff0000), 2.0};
    CHECK(fitImage({one, two}, 32, 2.0).pixels[0] == 0xffff0000);
    CHECK(fitImage({two, one}, 32, 1.0).pixels[0] == 0xff0000ff);
    Image small{16, 16, std::vector<uint32_t>(16 * 16, 0xff00ff00), 1.0};
    CHECK(fitImage({small, two}, 16, 2.0).pixels[0] == 0xff00ff00); // exact size beats scale
    // Power: the broadcast and the suspend/resume registration both deliver.
    g_events.clear();
    CHECK(hooks()->simulateSystemEvent(EventType::Suspending, false));
    CHECK(hooks()->simulateSystemEvent(EventType::Suspending, false));
    CHECK(hooks()->simulateSystemEvent(EventType::Resumed, false));
    CHECK(count(EventType::Suspending) == 1 && count(EventType::Resumed) == 1);
    CHECK(!hooks()->simulateSystemEvent(EventType::NetworkChanged, false));
    const auto online = g_app->networkOnline();
    std::printf("    networkOnline %s\n", online ? (*online ? "true" : "false") : "unknown");
}

void caseInstanceFraming() {
    const std::vector<std::string> args{"hello world", "", "msga://open?x=1", "\xe2\x9c\x93"};
    const std::string              f = encodeInstanceMessage("C:/work dir", args);
    CHECK(instanceFrameSize(f.substr(0, 3)) == 0);
    CHECK(instanceFrameSize(f) == f.size());
    std::string              cwd;
    std::vector<std::string> got;
    CHECK(decodeInstanceMessage(f, &cwd, &got));
    CHECK(cwd == "C:/work dir" && got == args);
    CHECK(
        decodeInstanceMessage(encodeInstanceMessage("", {}), &cwd, &got) && cwd.empty() &&
        got.empty()
    );
    // Truncated, padded, wrong magic, lying lengths, too big: all refused.
    CHECK(!decodeInstanceMessage(f.substr(0, f.size() - 1), &cwd, &got));
    CHECK(!decodeInstanceMessage(f + "x", &cwd, &got));
    std::string bad = f;
    bad[4]          = 'X';
    CHECK(!decodeInstanceMessage(bad, &cwd, &got));
    bad     = f;
    bad[16] = char(0xff); // the cwd length now runs past the end
    CHECK(!decodeInstanceMessage(bad, &cwd, &got));
    const std::string huge("\xff\xff\xff\x7f", 4);
    CHECK(instanceFrameSize(huge) == SIZE_MAX);
    // A newer secondary: a higher version still reads.
    std::string v2 = f;
    v2[8]          = 2;
    CHECK(decodeInstanceMessage(v2, &cwd, &got) && got == args);

    // Names: per user, session and key; nothing unsafe; bounded length.
    const std::wstring n = instancePipeName(L"S-1-5-21-1", 3, "org.nisdos.msga");
    CHECK(n == L"\\\\.\\pipe\\plat-S-1-5-21-1-3-org.nisdos.msga");
    const std::wstring slash = instancePipeName(L"S", 1, "a\\b/c d");
    CHECK(slash.substr(9).find_first_of(L"\\/ ") == std::wstring::npos);
    CHECK(slash != instancePipeName(L"S", 1, "a_b_c_d"));
    const std::wstring longName = instancePipeName(
        L"S-1-5-21-1111111111-2222222222-3333333333-1001", 1, std::string(500, 'k')
    );
    CHECK(longName.size() < 256);
    CHECK(
        longName != instancePipeName(
                        L"S-1-5-21-1111111111-2222222222-3333333333-1001", 1, std::string(499, 'k')
                    )
    );
    CHECK(validUrlScheme("msga") && validUrlScheme("x-plat+1.2"));
    CHECK(
        !validUrlScheme("c") && !validUrlScheme("1abc") && !validUrlScheme("a b") &&
        !validUrlScheme("")
    );
    CHECK(urlSchemeCommand(L"C:\\x y\\a.exe") == L"\"C:\\x y\\a.exe\" \"%1\"");
    // The bytes themselves never change: a running primary of an older build
    // must read what a newer secondary sends.
    const std::string golden = std::string("\x17\0\0\0", 4) + "PLAT" +
                               std::string("\x01\0\0\0\x02\0\0\0\x02\0\0\0", 12) +
                               "C:" + std::string("\x01\0\0\0", 4) + "a";
    CHECK(encodeInstanceMessage("C:", {"a"}) == golden);
}

void caseTruncatedFields() {
    // Notify-icon fields (tooltip, balloon texts) are cut to fit, never in
    // the middle of a surrogate pair.
    wchar_t buf[4];
    copyTruncated(buf, 4, L"abc");
    CHECK(std::wstring(buf) == L"abc");
    copyTruncated(buf, 4, L"abcd");
    CHECK(std::wstring(buf) == L"abc");
    copyTruncated(buf, 4, L"ab\U0001F600"); // the pair would straddle the cut
    CHECK(std::wstring(buf) == L"ab");
    copyTruncated(buf, 4, L"a\U0001F600x"); // the pair fits exactly
    CHECK(std::wstring(buf) == L"a\U0001F600");
    copyTruncated(buf, 4, L"");
    CHECK(buf[0] == 0);
}

std::wstring testPipeName(const std::string &key) {
    HANDLE tok = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    GetTokenInformation(tok, TokenUser, buf.data(), n, &n);
    CloseHandle(tok);
    LPWSTR s = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &s);
    std::wstring sid = s ? s : L"";
    LocalFree(s);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return instancePipeName(sid, session, key);
}

// The primary's pipe, driven from a thread in this process while the loop
// pumps: framing, the ack, and that bad or silent clients cannot jam it.
void caseInstancePipe() {
    const std::string key = "plat-w32-" + std::to_string(GetCurrentProcessId());
    CHECK(g_app->claimSingleInstance(key, {}));
    CHECK(g_app->claimSingleInstance(key, {})); // still primary
    const std::wstring pipe    = testPipeName(key);
    auto               forward = [&](const std::string &frame, DWORD timeout) {
        std::atomic<int> result{-1};
        std::thread t([&] { result = Win32App::forwardToPrimary(pipe, frame, timeout) ? 1 : 0; });
        pumpUntil([&] { return result >= 0; }, int(timeout) + 2000);
        t.join();
        return result == 1;
    };
    g_events.clear();
    CHECK(forward(encodeInstanceMessage("C:/cwd", {"one", "two"}), 3000));
    CHECK(pumpUntil([] { return find(EventType::InstanceActivated) != nullptr; }, 1000));
    const Event *a = find(EventType::InstanceActivated);
    CHECK(a && a->text == "C:/cwd" && (a->strings == std::vector<std::string>{"one", "two"}));

    // Garbage: dropped without an ack, and the next client is served.
    CHECK(!forward(std::string("\x10\x00\x00\x00", 4) + "NOPE000000000000", 2000));
    CHECK(!forward(std::string("\xff\xff\xff\x7f", 4), 2000));
    g_events.clear();
    CHECK(forward(encodeInstanceMessage("C:/", {"after garbage"}), 3000));
    CHECK(pumpUntil([] { return find(EventType::InstanceActivated) != nullptr; }, 1000));

    // Silent: connects and never writes; dropped after the server's timeout.
    std::atomic<bool> done{false};
    std::thread       silent([&] {
        HANDLE h = CreateFileW(
            pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr
        );
        char  c;
        DWORD n = 0;
        if (h != INVALID_HANDLE_VALUE) {
            ReadFile(h, &c, 1, &n, nullptr); // returns when the server hangs up
            CloseHandle(h);
        }
        done = true;
    });
    pumpFor(200);
    g_events.clear();
    const auto t0 = Clock::now();
    CHECK(forward(encodeInstanceMessage("C:/", {"after silence"}), 9000));
    std::printf(
        "    served after a silent client in %lld ms\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()
    );
    pumpUntil([&] { return done.load(); }, 2000);
    silent.join();
    CHECK(done);
    CHECK(pumpUntil([] { return find(EventType::InstanceActivated) != nullptr; }, 1000));
}

void caseUrlSchemeRegistry() {
    if (!std::getenv("PLAT_SELFTEST_ALLOW_REGISTRATION")) {
        skip(
            "writes HKCU\\Software\\Classes; set PLAT_SELFTEST_ALLOW_REGISTRATION in a throwaway "
            "prefix"
        );
        return;
    }
    CHECK(!g_app->registerUrlScheme("c"));
    CHECK(g_app->registerUrlScheme("Plat-W32test"));
    auto read = [](const wchar_t *sub, const wchar_t *name) -> std::optional<std::wstring> {
        wchar_t buf[1024];
        DWORD   size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, sub, name, RRF_RT_REG_SZ, nullptr, buf, &size) !=
            ERROR_SUCCESS)
            return std::nullopt;
        return std::wstring(buf);
    };
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto def  = read(L"Software\\Classes\\plat-w32test", nullptr);
    const auto prot = read(L"Software\\Classes\\plat-w32test", L"URL Protocol");
    const auto cmd  = read(L"Software\\Classes\\plat-w32test\\shell\\open\\command", nullptr);
    CHECK(def && *def == L"URL:plat-w32test");
    CHECK(prot && prot->empty());
    CHECK(cmd && *cmd == L"\"" + std::wstring(exe) + L"\" \"%1\"");
    // Forwarded args of that scheme (any case) now also come out as OpenUrls.
    const std::string key = "plat-w32-url-" + std::to_string(GetCurrentProcessId());
    CHECK(g_app->claimSingleInstance(key, {}));
    std::atomic<int> result{-1};
    const auto frame = encodeInstanceMessage("C:/", {"--flag", "PLAT-W32TEST://x", "other://y"});
    const auto pipe  = testPipeName(key);
    g_events.clear();
    std::thread t([&] { result = Win32App::forwardToPrimary(pipe, frame, 3000) ? 1 : 0; });
    pumpUntil([&] { return result >= 0 && find(EventType::OpenUrls); }, 5000);
    t.join();
    const Event *u = find(EventType::OpenUrls);
    CHECK(u && u->strings == std::vector<std::string>{"PLAT-W32TEST://x"});
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\plat-w32test");
}

// AppInfo::startMenuShortcut: "<name>.lnk" in the user's Start-menu
// Programs, pointing at this exe and carrying the AUMID; left alone once there.
void caseStartMenuShortcut() {
    if (!std::getenv("PLAT_SELFTEST_ALLOW_REGISTRATION")) {
        skip("writes the Start menu; set PLAT_SELFTEST_ALLOW_REGISTRATION in a throwaway prefix");
        return;
    }
    const AppInfo saved    = g_app->appInfo();
    auto         &w32      = static_cast<Win32App &>(*g_app);
    PWSTR         programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        CHECK(!"no Programs folder");
        return;
    }
    const std::wstring lnk = std::wstring(programs) + L"\\Plat W32 Shortcut Test.lnk";
    CoTaskMemFree(programs);
    DeleteFileW(lnk.c_str());
    g_app->setAppInfo({"Plat W32 Shortcut Test", "plat.w32.shortcut-test", {}, false});
    w32.ensureStartMenuShortcut();
    CHECK(GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES); // not asked for
    g_app->setAppInfo({"Plat W32 Shortcut Test", "plat.w32.shortcut-test", {}, true});
    w32.ensureStartMenuShortcut();
    CHECK(GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES);
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IShellLinkW,
            reinterpret_cast<void **>(&link)
        ))) {
        CHECK(!"no ShellLink");
        g_app->setAppInfo(saved);
        return;
    }
    IPersistFile *file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
        CHECK(SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)));
        file->Release();
    }
    wchar_t target[MAX_PATH] = {}, exe[MAX_PATH] = {};
    link->GetPath(target, MAX_PATH, nullptr, 0);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    CHECK(_wcsicmp(target, exe) == 0);
    IPropertyStore *props = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void **>(&props)))) {
        const PROPERTYKEY aumidKey = {
            {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5
        };
        PROPVARIANT pv;
        PropVariantInit(&pv);
        const HRESULT hr = props->GetValue(aumidKey, &pv);
        if (hr == E_NOTIMPL) { // Wine's shell link stores no properties
            std::printf("    the shell link's property store is a stub here (Wine)\n");
        } else {
            CHECK(SUCCEEDED(hr));
            CHECK(
                pv.vt == VT_LPWSTR && pv.pwszVal &&
                std::wstring(pv.pwszVal) == L"plat.w32.shortcut-test"
            );
        }
        PropVariantClear(&pv);
        props->Release();
    }
    link->Release();
    // Already there: kept as it is (a second start writes nothing).
    FILETIME before{}, after{};
    HANDLE   h =
        CreateFileW(lnk.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    GetFileTime(h, nullptr, nullptr, &before);
    CloseHandle(h);
    Sleep(20);
    w32.ensureStartMenuShortcut();
    h = CreateFileW(lnk.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    GetFileTime(h, nullptr, nullptr, &after);
    CloseHandle(h);
    CHECK(CompareFileTime(&before, &after) == 0);
    DeleteFileW(lnk.c_str());
    g_app->setAppInfo(saved);
}

} // namespace

int main() {
    std::string err;
    g_app = App::create(&err);
    if (!g_app) {
        std::printf("FAIL create app: %s\n", err.c_str());
        return 1;
    }
    g_app->setEventHandler([](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::Frame && e.window)
            paint(*e.window);
    });
    g_win = g_app->createWindow({
        .title       = "plat win32 tests",
        .size        = {320, 240},
        .decorations = Decorations::Custom,
    });
    if (!g_win) {
        std::printf("FAIL create window\n");
        return 1;
    }
    g_win->setHitTest([](Point p) {
        const int w = g_win->size().w;
        if (!g_win->isMaximized() && p.x >= w - 5 && p.y >= kCaptionH)
            return HitArea::ResizeRight;
        if (p.y >= kCaptionH)
            return HitArea::Client;
        if (p.x >= w - kBtnW)
            return HitArea::CloseButton;
        if (p.x >= w - 2 * kBtnW)
            return HitArea::MaximizeButton;
        return HitArea::Caption;
    });
    pumpUntil([] { return g_frames > 0; }, 5000);

    runCase("helpers: file URIs, UTF-8 offsets, VK mapping", caseHelpers);
    runCase("click runs count 1, 2, 3; another button restarts", caseTripleClick);
    runCase("X buttons arrive as Back/Forward", caseBackButton);
    runCase(
        "press on CloseButton area is delivered, closes nothing", caseCloseButtonPressDelivered
    );
    runCase("caption double-click maximises into the work area", caseCaptionDoubleClickMaximises);
    runCase("caption double-click on a maximised window restores", caseCaptionDoubleClickRestores);
    runCase(
        "caption drag moves via the OS loop; timers keep firing", caseCaptionDragMovesThroughOsLoop
    );
    runCase("live edge resize paints inside the OS loop", caseLiveResizePaintsInsideOsLoop);
    runCase("pointer leaves and re-enters", casePointerLeave);
    runCase("fractional wheel is precise pixels; tilt is notches", casePreciseWheel);
    runCase("UTF-16 surrogate pair becomes one TextInput", caseSurrogatePair);
    runCase("text input disabled: keys but no text", caseTextDisabledTypesNothing);
    runCase("AltGr does not report its synthetic Ctrl", caseAltGrHasNoFakeCtrl);
    runCase("WM_DPICHANGED rescales, keeps logical size", caseDpiChange);
    runCase("frames pace to the display rate", caseFramePacing);
    runCase("fullscreen covers the monitor and restores", caseFullscreen);
    runCase("min size clamps setSize", caseResizeLiveFramesAndMinSize);
    runCase("clipboard CRLF conversion; custom MIME replaces text", caseClipboardCrlfAndCustomMime);
    runCase("WM_DROPFILES becomes Drop with file URIs", caseDropFiles);
    runCase("CF_HTML header offsets; foreign HTML Format", caseCfHtmlHeader);
    runCase("CF_HDROP <-> text/uri-list, both directions", caseHdropUriList);
    runCase("bare CF_DIB reads as image/bmp and image/png", caseDibClipboard);
    runCase("icon fit/scale, HICON, PNG via WIC, badge bitmap", caseImageHelpers);
    runCase("tray is re-added on TaskbarCreated", caseTaskbarCreatedReAdds);
    runCase("balloon fallback: click, timeout, close", caseBalloonFallback);
    runCase("DoDragDrop keeps timers alive; Escape cancels", caseDoDragDropKeepsTimersAlive);
    runCase("modal timer: armed for due work only", caseModalTimerArmsOnlyForDueWork);
    runCase("desktop queries; openUrl rejects paths", caseDesktopQueries);
    runCase("mixed-DPI rule: physical origins, logical sizes", caseMixedDpiMath);
    runCase("WindowDesc::position / setPosition place the client", caseLiveMonitorsAndPosition);
    runCase("broadcasts only report real monitor/theme changes", caseMonitorAndThemeDedup);
    runCase("settings parsing, tray icon scale, power dedup", caseSettingsParsing);
    runCase("instance message framing and pipe names", caseInstanceFraming);
    runCase("notify-icon texts keep surrogate pairs whole", caseTruncatedFields);
    runCase("instance pipe: ack, garbage and silent clients", caseInstancePipe);
    runCase("URL scheme lands in HKCU; forwarded URLs become OpenUrls", caseUrlSchemeRegistry);
    runCase("Start-menu shortcut with the AUMID, when asked for", caseStartMenuShortcut);

    g_win.reset();
    g_app.reset();
    return plat_test::summary();
}
