// Win32 backend, app half: the event loop (MsgWaitForMultipleObjectsEx over
// the thread's message queue), frame pacing, theme, openUrl and (in
// PLAT_TEST_HOOKS builds) the SendInput-based test hooks.
#include "core/pacing.h"
#include "win32/win32.h"

#include <dwmapi.h>
#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace plat::win32 {

namespace {

constexpr UINT     kWakeMsg     = WM_APP + 1;
constexpr UINT_PTR kModalTimer  = 1;
constexpr int      kModalTickMs = 10; // USER_TIMER_MINIMUM; the OS rounds it to its tick anyway
// Handle watches (the single-instance pipe) have no wake of their own inside
// an OS modal loop: polled this often while one is open.
constexpr int      kModalHandlePollMs = 100;

HINSTANCE thisModule() {
    // The module that contains plat, not the exe: plat may end up in a DLL,
    // and window classes are registered per module.
    HMODULE m = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&thisModule),
        &m
    );
    return m ? m : GetModuleHandleW(nullptr);
}

void setDpiAwareness() {
    // Manifest-free per-monitor v2 (1703+): the OS then scales the non-client
    // area, sends WM_DPICHANGED with a suggested rect, and reports physical
    // pixels everywhere. Failing with ACCESS_DENIED just means a manifest or
    // the host already chose; older systems get the best available level.
    const Api &a = api();
    if (a.setProcessDpiAwarenessContext &&
        a.setProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(-4))) // PER_MONITOR_AWARE_V2
        return;
    if (a.setProcessDpiAwarenessContext && GetLastError() == ERROR_ACCESS_DENIED)
        return;
    if (a.setProcessDpiAwareness && SUCCEEDED(a.setProcessDpiAwareness(2))) // PER_MONITOR
        return;
    SetProcessDPIAware();
}

} // namespace

const Api &api() {
    static const Api a = [] {
        Api     r;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        r.setProcessDpiAwarenessContext =
            sym<decltype(r.setProcessDpiAwarenessContext)>(user32, "SetProcessDpiAwarenessContext");
        r.getDpiForWindow = sym<decltype(r.getDpiForWindow)>(user32, "GetDpiForWindow");
        r.getDpiForSystem = sym<decltype(r.getDpiForSystem)>(user32, "GetDpiForSystem");
        r.adjustWindowRectExForDpi =
            sym<decltype(r.adjustWindowRectExForDpi)>(user32, "AdjustWindowRectExForDpi");
        HMODULE shcore = LoadLibraryW(L"shcore.dll"); // kept loaded: process lifetime
        if (!r.setProcessDpiAwarenessContext)
            r.setProcessDpiAwareness =
                sym<decltype(r.setProcessDpiAwareness)>(shcore, "SetProcessDpiAwareness");
        r.getDpiForMonitor = sym<decltype(r.getDpiForMonitor)>(shcore, "GetDpiForMonitor");
        return r;
    }();
    return a;
}

UINT dpiForWindow(HWND hwnd) {
    if (api().getDpiForWindow)
        if (UINT d = api().getDpiForWindow(hwnd))
            return d;
    HDC  dc = GetDC(nullptr);
    UINT d  = UINT(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

// ── lifecycle ───────────────────────────────────────────────────────────────

bool Win32App::init(std::string *error) {
    setDpiAwareness();
    // OLE (an STA plus the OLE runtime) is what RegisterDragDrop and
    // DoDragDrop need; ShellExecute, WIC and WinRT toasts ride on it too.
    // A host that already made this thread MTA gets RPC_E_CHANGED_MODE: COM
    // still works then, but OLE drag and drop does not (WM_DROPFILES stays).
    _oleInit = SUCCEEDED(OleInitialize(nullptr));
    if (!_oleInit)
        _comInit =
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    if (_oleInit)
        CoCreateInstance(
            CLSID_DragDropHelper,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IDropTargetHelper,
            reinterpret_cast<void **>(&_dropHelper)
        );
    // Broadcast by Explorer once a window's taskbar button exists: overlay
    // icons set before that are lost.
    bindWic(); // WIC's factory is cached for this thread only
    _taskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    _instance             = thisModule();

    // Classes are left registered on purpose: a second App in the same
    // process (tests) reuses them, and the OS unregisters at exit.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style       = CS_HREDRAW | CS_VREDRAW; // a resize invalidates everything → WM_PAINT → Frame
    wc.lpfnWndProc = &Win32Window::wndProc;
    wc.hInstance   = _instance;
    wc.hCursor     = nullptr;   // WM_SETCURSOR decides
    wc.hbrBackground = nullptr; // we paint every pixel; no erase flash
    wc.lpszClassName = kWindowClass;
    // The app's resource script names its icon IDI_ICON1, so windows pick it
    // up without a plat API for icons.
    wc.hIcon         = appIcon();
    wc.hIconSm       = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error)
            *error = "RegisterClassExW failed";
        return false;
    }
    WNDCLASSEXW mc{sizeof(mc)};
    mc.lpfnWndProc   = &Win32App::msgProc;
    mc.hInstance     = _instance;
    mc.lpszClassName = L"plat.msg";
    if (!RegisterClassExW(&mc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error)
            *error = "RegisterClassExW (message window) failed";
        return false;
    }
    _msgHwnd =
        CreateWindowExW(0, L"plat.msg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, _instance, this);
    if (!_msgHwnd) {
        if (error)
            *error = "CreateWindowExW (message window) failed";
        return false;
    }

    // post() may come from any thread; one PostMessage per batch is enough,
    // the flag keeps a flood of posts from filling the 10000-message queue.
    _core.wake = [this] {
        if (!_wakePending.exchange(true))
            PostMessageW(_msgHwnd, kWakeMsg, 0, 0);
    };

    // Precise waits without raising the global timer resolution (Windows
    // 10 1803+); the plain MsgWait timeout rounds to the 15.6 ms tick, which
    // would halve a 60 Hz animation. Older systems / Wine fall back to it.
    _hiresTimer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
    );

    DWM_TIMING_INFO ti{sizeof(ti)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiNumerator &&
        ti.rateRefresh.uiDenominator) {
        const double hz  = double(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator;
        _frameIntervalMs = core::frameIntervalMs(int(std::lround(hz * 1000)));
    }
    initSystem(); // best effort: without it only broadcasts and power/lock events are missing
    return true;
}

Win32App::~Win32App() {
    detachDecoders(); // before shutdown(): a worker's late post must find no App
    _core.shutdown(); // pending closures go while the message window still exists
    teardownSystem(); // pipes, the network sink and the system window, before COM goes
    if (_msgHwnd) {
        SetWindowLongPtrW(_msgHwnd, GWLP_USERDATA, 0);
        DestroyWindow(_msgHwnd);
    }
    if (_hiresTimer)
        CloseHandle(_hiresTimer);
    _shell.reset(); // tray/balloon icons, toast handlers: before COM goes
    releaseWic();
    if (_dropHelper)
        _dropHelper->Release();
    if (_oleInit)
        OleUninitialize();
    if (_comInit)
        CoUninitialize();
}

std::unique_ptr<Window> Win32App::createWindow(const WindowDesc &d) {
    auto w = std::make_unique<Win32Window>(this, d);
    if (!w->hwnd())
        return nullptr;
    return w;
}

void Win32App::removeWindow(Win32Window *w) {
    _windows.erase(std::remove(_windows.begin(), _windows.end(), w), _windows.end());
}

// ── loop ────────────────────────────────────────────────────────────────────

void Win32App::run() {
    _quit = false;
    while (!_quit)
        pump(-1);
}

void Win32App::quit() {
    _quit = true;
    _core.wake();
}

void Win32App::wait(int timeoutMs) {
    // MAXIMUM_WAIT_OBJECTS is 64 including the message queue's slot; one
    // more goes to the high-resolution timer.
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD  n = 0;
    for (const auto &w : _handleWatches)
        if (n < MAXIMUM_WAIT_OBJECTS - 2)
            handles[n++] = w.h;
    DWORD timeout = timeoutMs < 0 ? INFINITE : DWORD(timeoutMs);
    bool  hires   = false;
    if (timeoutMs > 0 && _hiresTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG(timeoutMs) * 10000; // relative, 100 ns units
        if (SetWaitableTimer(_hiresTimer, &due, 0, nullptr, nullptr, FALSE)) {
            handles[n++] = _hiresTimer;
            timeout      = INFINITE;
            hires        = true;
        }
    }
    MsgWaitForMultipleObjectsEx(
        n, n ? handles : nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE
    );
    if (hires)
        CancelWaitableTimer(_hiresTimer);
}

uint64_t Win32App::watchHandle(HANDLE h, std::function<void()> fn) {
    if (!h || _handleWatches.size() >= MAXIMUM_WAIT_OBJECTS - 4)
        return 0;
    const uint64_t id = _nextHandleWatch++;
    _handleWatches.push_back({id, h, std::move(fn)});
    return id;
}

void Win32App::unwatchHandle(uint64_t id) {
    _handleWatches.erase(
        std::remove_if(
            _handleWatches.begin(), _handleWatches.end(), [id](auto &w) { return w.id == id; }
        ),
        _handleWatches.end()
    );
}

void Win32App::pollHandles() {
    // By id: a callback may add or remove watches (its own included). The
    // list is a reused buffer, taken while in use (service() can nest
    // through an OS modal loop a callback opens).
    std::vector<uint64_t> ids;
    ids.swap(_scratchIds);
    ids.clear();
    for (const auto &w : _handleWatches)
        ids.push_back(w.id);
    for (uint64_t id : ids) {
        auto it = std::find_if(_handleWatches.begin(), _handleWatches.end(), [id](auto &w) {
            return w.id == id;
        });
        if (it == _handleWatches.end() || WaitForSingleObject(it->h, 0) != WAIT_OBJECT_0)
            continue;
        auto fn = it->fn; // a copy: the callback may unwatch itself
        fn();
    }
    _scratchIds.swap(ids);
}

void Win32App::pump(int timeoutMs) {
    int timeout = _core.clampTimeout(timeoutMs);
    if (const int f = msUntilFrame(); f >= 0)
        timeout = timeout < 0 ? f : std::min(timeout, f);
    if (_quit || _core.hasPosted())
        timeout = 0;
    // MWMO_INPUTAVAILABLE: return for messages already in the queue too, not
    // only for ones that arrived since the last GetMessage/PeekMessage.
    if (timeout != 0)
        wait(timeout);

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            // Someone called PostQuitMessage (a host, a DLL); honour it.
            _quit = true;
            break;
        }
        TranslateMessage(&msg); // WM_KEYDOWN → WM_CHAR through the active layout / dead keys
        DispatchMessageW(&msg);
    }
    service();
}

void Win32App::service() {
    pollHandles();
    _core.runPosted();
    _core.runDueTimers();
    flushFrames();
    if (_modalDepth)
        armModal();
}

void Win32App::flushFrames() {
    const auto                 now = core::Clock::now();
    // A snapshot in a reused buffer, as in pollHandles().
    std::vector<Win32Window *> windows;
    windows.swap(_scratchWindows);
    windows.assign(_windows.begin(), _windows.end());
    for (auto *w : windows) {
        // A Frame handler may destroy other windows; skip the ones gone.
        if (std::find(_windows.begin(), _windows.end(), w) == _windows.end())
            continue;
        int wait = -1;
        if (w->frameDue(now, &wait))
            w->deliverFrame();
    }
    _scratchWindows.swap(windows);
}

int Win32App::msUntilFrame() const {
    const auto now  = core::Clock::now();
    int        best = -1;
    for (auto *w : _windows) {
        int wait = -1;
        if (w->frameDue(now, &wait))
            return 0;
        if (wait >= 0)
            best = best < 0 ? wait : std::min(best, wait);
    }
    return best;
}

void Win32App::enterModal() {
    if (_modalDepth++ == 0)
        armModal();
}

void Win32App::leaveModal() {
    if (_modalDepth > 0 && --_modalDepth == 0 && _modalTimerMs >= 0) {
        KillTimer(_msgHwnd, kModalTimer);
        _modalTimerMs = -1;
    }
}

void Win32App::armModal() {
    if (!_modalDepth || !_msgHwnd)
        return;
    int due = _core.msUntilNextTimer();
    if (const int f = msUntilFrame(); f >= 0)
        due = due < 0 ? f : std::min(due, f);
    if (!_handleWatches.empty())
        due = due < 0 ? kModalHandlePollMs : std::min(due, kModalHandlePollMs);
    if (due < 0) { // nothing due: posted work still wakes us (kWakeMsg)
        if (_modalTimerMs >= 0) {
            KillTimer(_msgHwnd, kModalTimer);
            _modalTimerMs = -1;
        }
        return;
    }
    // Re-armed after every service() while modal: the period is the wait
    // until the next due thing (SetTimer with the same id replaces it).
    const int ms = std::max(due, kModalTickMs);
    if (ms != _modalTimerMs && SetTimer(_msgHwnd, kModalTimer, UINT(ms), nullptr))
        _modalTimerMs = ms;
}

LRESULT CALLBACK Win32App::msgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto *self = static_cast<Win32App *>(windowUserData(h, msg, lp))) {
        // Both arrive through whatever loop is pumping: ours, or an OS modal
        // loop (move/resize, menu) — which is how posted work, timers and
        // frames keep running while the user drags a window edge.
        if (msg == kWakeMsg) {
            self->_wakePending = false; // before running: a post from here re-arms
            self->service();
            return 0;
        }
        if (msg == WM_TIMER && wp == kModalTimer) {
            self->service();
            return 0;
        }
        if (msg == kDialogMsg) {
            self->runNextDialog();
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

UINT Win32App::wheelScrollAmount(bool horizontal) {
    // Read once per settings change rather than per wheel event.
    const uint8_t bit = horizontal ? 2 : 1;
    UINT         &v   = horizontal ? _wheelChars : _wheelLines;
    if (!(_wheelKnown & bit)) {
        UINT n = 3;
        SystemParametersInfoW(
            horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &n, 0
        );
        v = n;
        _wheelKnown |= bit;
    }
    return v;
}

// ── desktop integration ─────────────────────────────────────────────────────

bool Win32App::darkMode() const {
    // The "app mode" half of Settings → Personalisation → Colours (the other
    // half, SystemUsesLightTheme, is for the taskbar/Start).
    DWORD light = 1, size = sizeof(light);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",
            RRF_RT_REG_DWORD,
            nullptr,
            &light,
            &size
        ) != ERROR_SUCCESS)
        return false;
    return light == 0;
}

bool Win32App::openUrl(std::string_view url) {
    // ShellExecute runs whatever it is handed, and a bare path to an .exe is
    // a program launch, not a URL: insist on a scheme of two or more letters
    // (one letter is a drive, "C:\..."), the way a link in a message has one.
    const size_t colon = url.find(':');
    if (colon == std::string_view::npos || !validUrlScheme(url.substr(0, colon)))
        return false;
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)
    );
    return r > 32;
}

#ifdef PLAT_TEST_HOOKS
// ── test hooks: real input through SendInput ────────────────────────────────

namespace {
bool sendInput(INPUT &in) {
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

void makeForeground(HWND h) {
    // Keyboard input goes to the foreground window's thread; a test process
    // started from the foreground terminal is allowed to take it.
    if (GetForegroundWindow() != h)
        SetForegroundWindow(h);
    if (GetFocus() != h)
        SetFocus(h);
}
} // namespace

bool Win32App::injectKey(Window &win, Key k, bool down) {
    auto &w   = static_cast<Win32Window &>(win);
    bool  ext = false;
    UINT  vk  = vkFromKey(k, &ext);
    if (!vk)
        return false;
    makeForeground(w.hwnd());
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = WORD(vk);
    in.ki.wScan   = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    return sendInput(in);
}

bool Win32App::injectPointerMove(Window &win, Point p) {
    auto        &w = static_cast<Win32Window &>(win);
    const double s = w.scale();
    POINT        pt{LONG(std::lround(p.x * s)), LONG(std::lround(p.y * s))};
    ClientToScreen(w.hwnd(), &pt);
    // Absolute input is normalised to 0..65535 over the virtual desktop and
    // mapped back with floor(n * width / 65536); rounding up lands exactly
    // on the requested pixel.
    const LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const LONG vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT      in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = LONG((LONGLONG(pt.x - vx) * 65536 + vw - 1) / vw);
    in.mi.dy      = LONG((LONGLONG(pt.y - vy) * 65536 + vh - 1) / vh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return sendInput(in);
}

bool Win32App::injectButton(Window &, Button b, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (b) {
    case Button::Left:
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case Button::Right:
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case Button::Middle:
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case Button::Back:
    case Button::Forward:
        in.mi.dwFlags   = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = b == Button::Back ? XBUTTON1 : XBUTTON2;
        break;
    }
    return sendInput(in);
}

bool Win32App::injectScroll(Window &, double dx, double dy) {
    bool ok = true;
    if (dy != 0) {
        INPUT in{};
        in.type         = INPUT_MOUSE;
        in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = DWORD(LONG(std::lround(-dy * WHEEL_DELTA))); // +delta = away = up
        ok &= sendInput(in);
    }
    if (dx != 0) {
        INPUT in{};
        in.type         = INPUT_MOUSE;
        in.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = DWORD(LONG(std::lround(dx * WHEEL_DELTA))); // +delta = right
        ok &= sendInput(in);
    }
    return ok;
}

bool Win32App::readPixel(Window &win, int x, int y, uint32_t *argb) {
    auto &w = static_cast<Win32Window &>(win);
    POINT pt{x, y};
    ClientToScreen(w.hwnd(), &pt);
    DwmFlush(); // let the compositor present what we blitted (no-op without DWM)
    HDC            dc = GetDC(nullptr);
    const COLORREF c  = GetPixel(dc, pt.x, pt.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID)
        return false;
    *argb = 0xff000000u | (uint32_t(GetRValue(c)) << 16) | (uint32_t(GetGValue(c)) << 8) |
            uint32_t(GetBValue(c));
    return true;
}
#endif // PLAT_TEST_HOOKS

} // namespace plat::win32

namespace plat {
std::unique_ptr<App> createWin32App(std::string *error) {
    auto app = std::make_unique<win32::Win32App>();
    if (!app->init(error))
        return nullptr;
    return app;
}
} // namespace plat
