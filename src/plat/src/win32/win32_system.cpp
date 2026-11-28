// Win32 backend, system integration: monitors (and the mixed-DPI coordinate
// rule), the single-instance channel, URL-scheme registration, network
// reachability, power events, system settings and standard directories.
//
// Broadcasts (WM_DISPLAYCHANGE, WM_SETTINGCHANGE, WM_POWERBROADCAST) only
// reach top-level windows, never the HWND_MESSAGE window the loop owns, so a
// hidden top-level "system window" receives them.
//
// The single-instance channel is a named pipe per user, session and key,
// guarded by a mutex that decides the primary role (it exists as long as the
// primary process does, even before the pipe is up, and never goes stale:
// the kernel closes it with the process). The primary serves the pipe with
// overlapped I/O whose event sits in the loop's MsgWaitForMultipleObjectsEx,
// so no thread is involved and it keeps working inside OS modal loops.
#include "win32/win32.h"

#include "core/hash.h"
#include "core/wire.h"
#include "prim/str.h"

#include <aclapi.h>
#include <dwmapi.h>
#include <netlistmgr.h>
#include <objbase.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plat::win32 {

namespace {

constexpr int  kInstanceTimeoutMs = 5000; // a client that stalls this long is dropped
constexpr char kAck[]             = "ACK1";

// A COM-free, allocation-light hash: monitor ids and over-long pipe keys.
using core::fnv1a;

UINT monitorDpi(HMONITOR m) {
    UINT x = 0, y = 0;
    if (api().getDpiForMonitor &&
        SUCCEEDED(api().getDpiForMonitor(m, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x)
        return x;
    return api().getDpiForSystem ? api().getDpiForSystem() : dpiForWindow(nullptr);
}

double distance2(const RECT &r, double x, double y) {
    const double dx = x < r.left ? r.left - x : x >= r.right ? x - (r.right - 1) : 0;
    const double dy = y < r.top ? r.top - y : y >= r.bottom ? y - (r.bottom - 1) : 0;
    return dx * dx + dy * dy;
}

// The current user's SID, as a string (pipe and mutex names) and as bytes
// (the owner check on the client side, the DACL on the server side).
struct UserSid {
    std::vector<BYTE> token; // TOKEN_USER
    std::wstring      text;
    PSID              sid() const {
        return token.empty() ? nullptr
                             : reinterpret_cast<const TOKEN_USER *>(token.data())->User.Sid;
    }
};

UserSid currentUser() {
    UserSid u;
    HANDLE  tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return u;
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    u.token.resize(n);
    if (!n || !GetTokenInformation(tok, TokenUser, u.token.data(), n, &n))
        u.token.clear();
    CloseHandle(tok);
    LPWSTR s = nullptr;
    if (u.sid() && ConvertSidToStringSidW(u.sid(), &s)) {
        u.text = s;
        LocalFree(s);
    } else {
        u.token.clear();
    }
    return u;
}

// Owner and sole grantee: this user. Setting the owner explicitly matters
// for elevated processes, whose default owner is BUILTIN\Administrators —
// the secondary checks the owner to rule out a pipe squatted by another user.
PSECURITY_DESCRIPTOR userOnlyDescriptor(const std::wstring &sid) {
    PSECURITY_DESCRIPTOR sd  = nullptr;
    const std::wstring   sdl = L"O:" + sid + L"D:P(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sdl.c_str(), SDDL_REVISION_1, &sd, nullptr
        ))
        return nullptr;
    return sd;
}

std::string currentDir() {
    const DWORD  n = GetCurrentDirectoryW(0, nullptr);
    std::wstring buf(n, L'\0');
    const DWORD  m = n ? GetCurrentDirectoryW(n, buf.data()) : 0;
    buf.resize(m < n ? m : 0);
    return portablePath(buf);
}

// Overlapped I/O on `h` with a deadline: the secondary must never hang on a
// primary that stopped answering.
bool ioWithin(HANDLE h, bool write, void *buf, DWORD len, DWORD timeoutMs, DWORD *done) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev)
        return false;
    OVERLAPPED ov{};
    ov.hEvent = ev;
    BOOL ok   = write ? WriteFile(h, buf, len, nullptr, &ov) : ReadFile(h, buf, len, nullptr, &ov);
    bool res  = false;
    if (ok || GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, timeoutMs) == WAIT_OBJECT_0) {
            res = GetOverlappedResult(h, &ov, done, FALSE) != 0;
        } else {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, done, TRUE); // the buffer is ours again after this
        }
    }
    CloseHandle(ev);
    return res;
}

} // namespace

// ── shared helpers ──────────────────────────────────────────────────────────

bool setRegString(HKEY key, const wchar_t *name, const std::wstring &v) {
    return RegSetValueExW(
               key,
               name,
               0,
               REG_SZ,
               reinterpret_cast<const BYTE *>(v.c_str()),
               DWORD((v.size() + 1) * sizeof(wchar_t))
           ) == ERROR_SUCCESS;
}

std::wstring exePath() {
    std::wstring buf(32768, L'\0'); // long paths
    const DWORD  n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n < buf.size() ? n : 0);
    return buf;
}

std::wstring tempPath() {
    wchar_t     buf[MAX_PATH + 2];
    const DWORD n = GetTempPathW(MAX_PATH + 2, buf);
    return n && n <= MAX_PATH + 1 ? std::wstring(buf, n) : std::wstring();
}

void *windowUserData(HWND h, UINT msg, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(
            h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams)
        );
    return reinterpret_cast<void *>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

HWND createHiddenWindow(HINSTANCE inst, const wchar_t *cls, WNDPROC proc, void *param) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = proc;
    wc.hInstance     = inst;
    wc.lpszClassName = cls;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    // Never shown, and a tool window so nothing about it can reach the
    // taskbar or Alt+Tab.
    return CreateWindowExW(
        WS_EX_TOOLWINDOW, cls, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, param
    );
}

// ── pure helpers ────────────────────────────────────────────────────────────

Rect logicalBounds(const MonitorGeom &m) {
    return logicalRect(m, m.phys);
}

Rect logicalRect(const MonitorGeom &m, const RECT &r) {
    // Edges, not origin + size, are rounded: a work area rounded that way
    // never pokes a pixel past its monitor's rounded bounds.
    const double s  = m.scale > 0 ? m.scale : 1.0;
    const int    l  = int(std::lround((r.left - m.phys.left) / s));
    const int    t  = int(std::lround((r.top - m.phys.top) / s));
    const int    rr = int(std::lround((r.right - m.phys.left) / s));
    const int    b  = int(std::lround((r.bottom - m.phys.top) / s));
    return {m.phys.left + l, m.phys.top + t, rr - l, b - t};
}

Point physicalToLogical(POINT p, const std::vector<MonitorGeom> &mons) {
    const MonitorGeom *best = nullptr;
    double             bd   = 0;
    for (const auto &m : mons) {
        const double d = distance2(m.phys, p.x, p.y);
        if (!best || d < bd) {
            best = &m;
            bd   = d;
        }
    }
    if (!best)
        return {double(p.x), double(p.y)};
    const double s = best->scale > 0 ? best->scale : 1.0;
    return {
        best->phys.left + (p.x - best->phys.left) / s, best->phys.top + (p.y - best->phys.top) / s
    };
}

POINT logicalToPhysical(Point p, const std::vector<MonitorGeom> &mons) {
    const MonitorGeom *best = nullptr;
    double             bd   = 0;
    for (const auto &m : mons) {
        const Rect   l = logicalBounds(m);
        const RECT   r{l.x, l.y, l.x + l.w, l.y + l.h};
        const double d = distance2(r, p.x, p.y);
        if (!best || d < bd) {
            best = &m;
            bd   = d;
        }
    }
    if (!best)
        return {LONG(std::lround(p.x)), LONG(std::lround(p.y))};
    const double s = best->scale > 0 ? best->scale : 1.0;
    return {
        LONG(std::lround(best->phys.left + (p.x - best->phys.left) * s)),
        LONG(std::lround(best->phys.top + (p.y - best->phys.top) * s))
    };
}

uint64_t monitorId(std::wstring_view device) {
    const uint64_t h = fnv1a(device.data(), device.size() * sizeof(wchar_t));
    return h ? h : 1;
}

std::string encodeInstanceMessage(std::string_view cwd, const std::vector<std::string> &args) {
    std::string frame(4, '\0'); // the length of the rest, filled in below
    frame += "PLAT";
    core::putU32(frame, kInstanceProtocol);
    core::putU32(frame, uint32_t(args.size() + 1));
    core::putString(frame, cwd);
    for (const auto &a : args)
        core::putString(frame, a);
    std::string len;
    core::putU32(len, uint32_t(frame.size() - 4));
    frame.replace(0, 4, len);
    return frame;
}

size_t instanceFrameSize(std::string_view buf) {
    if (buf.size() < 4)
        return 0;
    const uint32_t n = core::getU32(buf.data());
    return n > kInstanceMaxBytes ? SIZE_MAX : size_t(n) + 4;
}

bool decodeInstanceMessage(
    std::string_view frame, std::string *cwd, std::vector<std::string> *args
) {
    const size_t total = instanceFrameSize(frame);
    if (total == 0 || total == SIZE_MAX || total != frame.size() || frame.size() < 16 ||
        frame.substr(4, 4) != "PLAT")
        return false;
    // Newer versions may append fields after the strings; the v1 prefix
    // stays readable, so any version ≥ 1 is accepted.
    if (core::getU32(frame.data() + 8) < 1)
        return false;
    const uint32_t count = core::getU32(frame.data() + 12);
    if (count < 1)
        return false;
    std::string_view         rest = frame.substr(16);
    std::vector<std::string> strings;
    for (uint32_t i = 0; i < count; ++i) {
        std::string s;
        if (!core::takeString(rest, &s))
            return false;
        strings.push_back(std::move(s));
    }
    *cwd = std::move(strings[0]);
    args->assign(
        std::make_move_iterator(strings.begin() + 1), std::make_move_iterator(strings.end())
    );
    return true;
}

std::wstring instancePipeName(std::wstring_view sid, DWORD session, std::string_view key) {
    // Keys are app ids ("org.nisdos.msga"), but nothing stops a caller from
    // passing a path; '\' and friends are not allowed in the name.
    const char *const hex = prim::kHexLower;
    std::wstring      safe;
    for (unsigned char c : key) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_') {
            safe += wchar_t(c);
        } else {
            safe += L'~';
            safe += wchar_t(hex[c >> 4]);
            safe += wchar_t(hex[c & 15]);
        }
    }
    if (safe.size() > 160) { // pipe names end at 256 characters
        const uint64_t h = fnv1a(key.data(), key.size());
        safe.resize(120);
        safe += L'-';
        for (int i = 15; i >= 0; --i)
            safe += wchar_t(hex[(h >> (4 * i)) & 15]);
    }
    return L"\\\\.\\pipe\\plat-" + std::wstring(sid) + L"-" + std::to_wstring(session) + L"-" +
           safe;
}

std::wstring urlSchemeCommand(std::wstring_view exe) {
    return L"\"" + std::wstring(exe) + L"\" \"%1\"";
}

bool validUrlScheme(std::string_view s) {
    // RFC 3986 scheme, and two letters at least: "c:" would be a drive.
    return s.size() >= 2 && core::validScheme(s);
}

std::string portablePath(std::wstring_view native) {
    // \\?\C:\x → C:\x and \\?\UNC\srv\s → \\srv\s: the long-path prefix is
    // an API detail no caller wants to see or compare.
    std::wstring p(native);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0)
        p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0)
        p = p.substr(4);
    std::string s = toUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::wstring nativePath(std::string_view utf8) {
    std::wstring w = toWide(utf8);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

uint32_t accentFromAbgr(DWORD abgr) {
    const uint32_t r = abgr & 0xff, g = (abgr >> 8) & 0xff, b = (abgr >> 16) & 0xff;
    return 0xff000000u | (r << 16) | (g << 8) | b; // the accent is opaque whatever alpha says
}

double textScaleFromPercent(DWORD percent) {
    // Settings → Accessibility → Text size writes 100…225.
    return percent >= 100 && percent <= 225 ? percent / 100.0 : 1.0;
}

int caretBlinkFromOs(UINT ms) {
    // INFINITE = "blink rate: none" in the Control Panel.
    if (ms == INFINITE || ms == 0)
        return 0;
    return int(std::min<UINT>(ms, 5000));
}

// ── monitors ────────────────────────────────────────────────────────────────

namespace {
struct EnumCtx {
    std::vector<Monitor>     *mons    = nullptr;
    std::vector<MonitorGeom> *geoms   = nullptr;
    bool                      refresh = false;
};

BOOL CALLBACK enumProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    auto          *ctx = reinterpret_cast<EnumCtx *>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(h, &mi))
        return TRUE;
    const MonitorGeom g{mi.rcMonitor, monitorDpi(h) / 96.0};
    if (ctx->geoms)
        ctx->geoms->push_back(g);
    if (!ctx->mons)
        return TRUE;
    Monitor m;
    m.id       = monitorId(mi.szDevice);
    m.name     = toUtf8(mi.szDevice);
    m.bounds   = logicalBounds(g);
    m.workArea = logicalRect(g, mi.rcWork);
    m.scale    = g.scale;
    m.primary  = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (ctx->refresh) {
        // A driver query (milliseconds), so only when a caller wants it.
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency > 1) // 0 and 1 mean "the hardware default"
            m.refreshMilliHz = int(dm.dmDisplayFrequency) * 1000;
    }
    ctx->mons->push_back(std::move(m));
    return TRUE;
}
} // namespace

std::vector<Monitor> enumMonitors(bool withRefresh) {
    std::vector<Monitor> out;
    EnumCtx              ctx{&out, nullptr, withRefresh};
    EnumDisplayMonitors(nullptr, nullptr, &enumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::vector<MonitorGeom> monitorGeoms() {
    std::vector<MonitorGeom> out;
    EnumCtx                  ctx{nullptr, &out, false};
    EnumDisplayMonitors(nullptr, nullptr, &enumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

uint64_t monitorIdOf(HMONITOR h) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    return h && GetMonitorInfoW(h, &mi) ? monitorId(mi.szDevice) : 0;
}

namespace {
bool sameSettings(const SystemSettings &a, const SystemSettings &b) {
    return a.reducedMotion == b.reducedMotion && a.highContrast == b.highContrast &&
           a.textScale == b.textScale && a.accentColor == b.accentColor &&
           a.caretBlinkMs == b.caretBlinkMs;
}
} // namespace

std::vector<Monitor> Win32App::monitors() const {
    // The layout is cheap to read and always current; only the refresh rate
    // (a driver query per monitor) comes from the snapshot, which is re-read
    // on every change the system window hears of (display, DPI, work area).
    if (!_monitorsKnown)
        return enumMonitors(true);
    std::vector<Monitor> now = enumMonitors(false);
    for (auto &m : now)
        for (const auto &s : _monitorSnapshot)
            if (s.id == m.id)
                m.refreshMilliHz = s.refreshMilliHz;
    return now;
}

void Win32App::monitorsMaybeChanged() {
    auto now = enumMonitors(true);
    if (_monitorsKnown && now == _monitorSnapshot)
        return;
    // Unknown before (the deferred snapshot has not run): report it, since
    // there is nothing to compare with and a spurious re-query is harmless.
    _monitorSnapshot = std::move(now);
    _monitorsKnown   = true;
    emit({.type = EventType::MonitorsChanged});
}

// ── theme and settings ──────────────────────────────────────────────────────

SystemSettings Win32App::systemSettings() const {
    SystemSettings s;
    BOOL           anim = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0))
        s.reducedMotion = !anim; // Settings → Accessibility → Visual effects → Animation effects
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        s.highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Accessibility",
            L"TextScaleFactor",
            RRF_RT_REG_DWORD,
            nullptr,
            &v,
            &size
        ) == ERROR_SUCCESS)
        s.textScale = textScaleFromPercent(v);
    // The accent the user picked (ABGR); DWM's colorisation colour, blended
    // for glass, is only the fallback where that value is missing.
    size = sizeof(v);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\DWM",
            L"AccentColor",
            RRF_RT_REG_DWORD,
            nullptr,
            &v,
            &size
        ) == ERROR_SUCCESS) {
        s.accentColor = accentFromAbgr(v);
    } else {
        DWORD argb   = 0;
        BOOL  opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)) && argb)
            s.accentColor = 0xff000000u | (argb & 0xffffff);
    }
    s.caretBlinkMs = caretBlinkFromOs(GetCaretBlinkTime());
    return s;
}

void Win32App::themeMaybeChanged() {
    const bool           dark = darkMode();
    const SystemSettings s    = systemSettings();
    if (dark == _themeDark && sameSettings(s, _themeSettings))
        return;
    const bool darkChanged = dark != _themeDark;
    _themeDark             = dark;
    _themeSettings         = s;
    for (auto *w : std::vector<Win32Window *>(_windows)) {
        if (std::find(_windows.begin(), _windows.end(), w) == _windows.end())
            continue; // an earlier handler destroyed it
        w->themeChanged(darkChanged);
    }
}

// ── the system window ───────────────────────────────────────────────────────

bool Win32App::initSystem() {
    _themeDark     = darkMode();
    _themeSettings = systemSettings();

    _sysHwnd = createHiddenWindow(_instance, L"plat.system", &Win32App::sysProc, this);
    if (!_sysHwnd)
        return false;

    // On Modern Standby machines suspend/resume only reaches windows that
    // registered for it (8+); elsewhere this just duplicates the broadcast,
    // which emitPower() filters.
    using SrRegister = HPOWERNOTIFY(WINAPI *)(HANDLE, DWORD);
    if (auto reg =
            sym<SrRegister>(GetModuleHandleW(L"user32.dll"), "RegisterSuspendResumeNotification"))
        _powerNotify = reg(_sysHwnd, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Off the start-up path: the monitor baseline (a driver query per
    // monitor), the network listener (loads netprofm, talks to its
    // service) and the Start-menu shortcut (shell COM, a file write). networkOnline() creates the
    // listener at once if asked first.
    addTimer(1000, false, [this] {
        if (!_monitorsKnown) {
            _monitorSnapshot = enumMonitors(true);
            _monitorsKnown   = true;
        }
        ensureNetwork();
        ensureStartMenuShortcut();
    });
    return true;
}

LRESULT CALLBACK Win32App::sysProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto *self = static_cast<Win32App *>(windowUserData(h, msg, lp));
    return self ? self->onSystemMessage(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT Win32App::onSystemMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_POWERBROADCAST:
        // RESUMESUSPEND additionally follows when a user is present; the
        // automatic one comes on every wake, so it alone means Resumed.
        if (wp == PBT_APMSUSPEND)
            emitPower(EventType::Suspending);
        else if (wp == PBT_APMRESUMEAUTOMATIC)
            emitPower(EventType::Resumed);
        return TRUE;
    case WM_DISPLAYCHANGE:
        monitorsMaybeChanged();
        break;
    case WM_DPICHANGED: // a monitor's scale changed; never resize this window
        monitorsMaybeChanged();
        return 0;
    case WM_SETTINGCHANGE:
        settingsChanged();         // the wheel settings, say
        if (wp == SPI_SETWORKAREA) // the taskbar moved, resized or auto-hides now
            monitorsMaybeChanged();
        themeMaybeChanged();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        themeMaybeChanged();
        break;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Win32App::emitPower(EventType t) {
    const DWORD now = GetTickCount();
    if (t == _lastPower && now - _lastPowerTick < 2000)
        return; // the same transition through the broadcast and the registration
    _lastPower     = t;
    _lastPowerTick = now;
    emit({.type = t});
}

#ifdef PLAT_TEST_HOOKS
bool Win32App::simulateSystemEvent(EventType type, bool) {
    if (!_sysHwnd)
        return false;
    switch (type) {
    case EventType::Suspending:
        SendMessageW(_sysHwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
        return true;
    case EventType::Resumed:
        SendMessageW(_sysHwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
        return true;
    default:
        // NetworkChanged: the Network List Manager's callback cannot be
        // triggered from inside the process without a fake of our own, and
        // networkOnline() would still report the real state.
        return false;
    }
}
#endif

// ── network ─────────────────────────────────────────────────────────────────

namespace {
bool onlineFrom(NLM_CONNECTIVITY c) {
    // Any network with traffic, not only NCSI's "internet" verdict: its
    // probe is blocked behind many corporate proxies, and a client that
    // believed it was offline there would never try to connect.
    return (c & (NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK |
                 NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_SUBNET |
                 NLM_CONNECTIVITY_IPV6_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
}
} // namespace

// INetworkListManagerEvents as a plain COM object. In our STA the callback
// arrives through the message queue, i.e. from inside the loop.
struct Win32App::Network final : ComObject<INetworkListManagerEvents> {
    Win32App            *app    = nullptr;
    INetworkListManager *nlm    = nullptr;
    IConnectionPoint    *cp     = nullptr;
    DWORD                cookie = 0;
    std::optional<bool>  last;

    HRESULT STDMETHODCALLTYPE ConnectivityChanged(NLM_CONNECTIVITY c) override {
        const bool on = onlineFrom(c);
        if (app && (!last || *last != on)) {
            last        = on;
            // Posted: the callback runs inside whatever COM call pumped it.
            Win32App *a = app;
            a->post([a, on] { a->emit({.type = EventType::NetworkChanged, .online = on}); });
        }
        return S_OK;
    }

    void shutdown() {
        if (cp) {
            cp->Unadvise(cookie);
            cp->Release();
            cp = nullptr;
        }
        if (nlm) {
            nlm->Release();
            nlm = nullptr;
        }
        app = nullptr;
        Release(); // ours; COM dropped its own in Unadvise
    }
};

void Win32App::ensureNetwork() const {
    if (_network)
        return;
    auto *n  = new Network;
    n->app   = const_cast<Win32App *>(this);
    _network = n;
    // Wine ships netprofm; a stripped system may not, which is nullopt.
    if (FAILED(CoCreateInstance(
            CLSID_NetworkListManager,
            nullptr,
            CLSCTX_ALL,
            IID_INetworkListManager,
            reinterpret_cast<void **>(&n->nlm)
        )))
        n->nlm = nullptr;
    if (!n->nlm)
        return;
    NLM_CONNECTIVITY c{};
    if (SUCCEEDED(n->nlm->GetConnectivity(&c)))
        n->last = onlineFrom(c);
    IConnectionPointContainer *cpc = nullptr;
    if (SUCCEEDED(
            n->nlm->QueryInterface(IID_IConnectionPointContainer, reinterpret_cast<void **>(&cpc))
        )) {
        if (SUCCEEDED(cpc->FindConnectionPoint(IID_INetworkListManagerEvents, &n->cp)) &&
            FAILED(n->cp->Advise(static_cast<INetworkListManagerEvents *>(n), &n->cookie))) {
            n->cp->Release();
            n->cp = nullptr;
        }
        cpc->Release();
    }
}

std::optional<bool> Win32App::networkOnline() const {
    ensureNetwork();
    NLM_CONNECTIVITY c{};
    if (!_network || !_network->nlm || FAILED(_network->nlm->GetConnectivity(&c)))
        return std::nullopt;
    return onlineFrom(c);
}

// ── single instance ─────────────────────────────────────────────────────────

struct Win32App::Instance {
    enum class State { Idle, Connecting, Reading, Draining };

    Win32App    *app = nullptr;
    std::string  key;
    std::wstring name;
    HANDLE       mutex = nullptr;
    HANDLE       pipe  = INVALID_HANDLE_VALUE;
    HANDLE       event = nullptr;
    OVERLAPPED   ov{};
    State        state   = State::Idle;
    bool         pending = false; // an overlapped operation is in flight
    uint64_t     watch   = 0;
    TimerId      timeout = 0;
    std::string  buf;
    char         chunk[4096];

    ~Instance() {
        if (watch)
            app->unwatchHandle(watch);
        if (timeout)
            app->cancelTimer(timeout);
        if (pipe != INVALID_HANDLE_VALUE) {
            cancel();
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
        if (event)
            CloseHandle(event);
        if (mutex)
            CloseHandle(mutex);
    }

    bool start(SECURITY_ATTRIBUTES *sa) {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // FIRST_PIPE_INSTANCE: if someone else already created this name
        // (squatting it), fail instead of joining their pipe. One instance:
        // concurrent secondaries queue in WaitNamedPipe.
        pipe  = CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            4096,
            4096,
            0,
            sa
        );
        if (!event || pipe == INVALID_HANDLE_VALUE)
            return false;
        watch = app->watchHandle(event, [this] { onSignal(); });
        listen();
        return watch != 0;
    }

    // Wait for a pending operation to finish being cancelled, so the next
    // one starts with the event and OVERLAPPED to itself.
    void cancel() {
        if (!pending)
            return;
        CancelIoEx(pipe, &ov);
        DWORD n = 0;
        GetOverlappedResult(pipe, &ov, &n, TRUE);
        pending = false;
    }

    void begin() {
        ResetEvent(event);
        ov        = {};
        ov.hEvent = event;
    }

    void listen() {
        state = State::Connecting;
        begin();
        if (ConnectNamedPipe(pipe, &ov)) {
            connected(); // documented not to happen in overlapped mode, but harmless
            return;
        }
        switch (GetLastError()) {
        case ERROR_IO_PENDING:
            pending = true;
            return;
        case ERROR_PIPE_CONNECTED: // a client came between Create/Disconnect and now
            connected();
            return;
        default: // ERROR_NO_DATA: connected and already gone
            DisconnectNamedPipe(pipe);
            app->post([this] { restartIfIdle(); });
            state = State::Idle;
            return;
        }
    }

    void restartIfIdle() {
        if (state == State::Idle)
            listen();
    }

    void connected() {
        state = State::Reading;
        buf.clear();
        if (timeout)
            app->cancelTimer(timeout);
        timeout = app->addTimer(kInstanceTimeoutMs, false, [this] {
            timeout = 0;
            reset(); // a client that connected and said nothing
        });
        read();
    }

    void read() {
        begin();
        if (!ReadFile(pipe, chunk, sizeof(chunk), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            reset(); // broken pipe: the client went away
            return;
        }
        pending = true; // completes through the event either way
    }

    void reset() {
        if (timeout) {
            app->cancelTimer(timeout);
            timeout = 0;
        }
        cancel();
        DisconnectNamedPipe(pipe);
        listen();
    }

    void onSignal() {
        DWORD      n  = 0;
        const BOOL ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
        if (!ok && GetLastError() == ERROR_IO_INCOMPLETE)
            return; // cannot happen after cancel() waited, but never spin on it
        pending = false;
        ResetEvent(event);
        switch (state) {
        case State::Connecting:
            if (ok)
                connected();
            else
                reset();
            return;
        case State::Reading: {
            if (!ok) {
                reset();
                return;
            }
            buf.append(chunk, n);
            const size_t need = instanceFrameSize(buf);
            if (need == SIZE_MAX) {
                reset();
                return;
            }
            if (need == 0 || buf.size() < need) {
                read();
                return;
            }
            deliver(buf.substr(0, need));
            return;
        }
        case State::Draining:
            // Disconnecting at once would discard the ack before the client
            // read it: wait for its end to close (ERROR_BROKEN_PIPE).
            if (ok)
                read();
            else
                reset();
            return;
        case State::Idle:
            return;
        }
    }

    void deliver(const std::string &frame) {
        std::string              cwd;
        std::vector<std::string> args;
        if (!decodeInstanceMessage(frame, &cwd, &args)) {
            reset();
            return;
        }
        DWORD done = 0;
        ioWithin(pipe, true, const_cast<char *>(kAck), 4, 1000, &done);
        state = State::Draining;
        read();

        std::vector<std::string> urls = core::schemeUrls(args, app->_schemes);
        // Posted, not emitted from here: a handler may well tear down this
        // instance (or the App) and we are inside its I/O callback.
        Win32App                *a    = app;
        a->post([a, args = std::move(args), cwd = std::move(cwd), urls = std::move(urls)] {
            a->emit({.type = EventType::InstanceActivated, .text = cwd, .strings = args});
            if (!urls.empty())
                a->emit({.type = EventType::OpenUrls, .strings = urls});
        });
    }
};

bool Win32App::claimSingleInstance(std::string_view key, const std::vector<std::string> &args) {
    for (auto *i : _instances)
        if (i->key == key)
            return true; // already primary for it
    const UserSid user = currentUser();
    if (user.text.empty())
        return true; // cannot name a per-user channel: run on our own
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring   pipe      = instancePipeName(user.text, session, key);
    // "Local\" is per session, like the session id in the pipe name.
    const std::wstring   mutexName = L"Local\\" + pipe.substr(9); // after \\.\pipe\ //
    PSECURITY_DESCRIPTOR sd        = userOnlyDescriptor(user.text);
    SECURITY_ATTRIBUTES  sa{sizeof(sa), sd, FALSE};
    HANDLE               m   = CreateMutexW(sd ? &sa : nullptr, FALSE, mutexName.c_str());
    const DWORD          err = GetLastError();
    const bool other = (m && err == ERROR_ALREADY_EXISTS) || (!m && err == ERROR_ACCESS_DENIED);
    if (!m && !other) {
        LocalFree(sd);
        return true;
    }
    if (other) {
        if (m)
            CloseHandle(m);
        LocalFree(sd);
        if (!forwardToPrimary(pipe, encodeInstanceMessage(currentDir(), args), kInstanceTimeoutMs))
            OutputDebugStringW(L"plat: the primary instance did not take our arguments\n");
        // Even then: the primary exists (the mutex says so), and a second
        // copy of the app would be worse than a launch that did nothing.
        return false;
    }
    auto *inst  = new Instance;
    inst->app   = this;
    inst->key   = std::string(key);
    inst->name  = pipe;
    inst->mutex = m;
    if (!inst->start(sd ? &sa : nullptr))
        OutputDebugStringW(
            L"plat: single-instance pipe unavailable; later launches cannot reach us\n"
        );
    LocalFree(sd);
    _instances.push_back(inst);
    return true;
}

bool Win32App::forwardToPrimary(
    const std::wstring &pipe, const std::string &frame, DWORD timeoutMs
) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    auto            left     = [&] {
        const ULONGLONG now = GetTickCount64();
        return now >= deadline ? DWORD(0) : DWORD(deadline - now);
    };
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        // SECURITY_IDENTIFICATION: the server may learn who we are, never
        // act as us.
        h = CreateFileW(
            pipe.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            nullptr
        );
        if (h != INVALID_HANDLE_VALUE)
            break;
        const DWORD err = GetLastError();
        if (!left())
            return false;
        if (err == ERROR_PIPE_BUSY)
            WaitNamedPipeW(pipe.c_str(), std::min<DWORD>(left(), 500)); // serving another launch
        else if (err == ERROR_FILE_NOT_FOUND)
            Sleep(20); // the primary holds the mutex but is still creating the pipe
        else
            return false;
    }
    // A pipe of that name not owned by us would be another user's squat.
    bool                 ours  = false;
    PSID                 owner = nullptr;
    PSECURITY_DESCRIPTOR sd    = nullptr;
    const UserSid        me    = currentUser();
    if (GetSecurityInfo(
            h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd
        ) == ERROR_SUCCESS) {
        ours = owner && me.sid() && EqualSid(owner, me.sid());
        LocalFree(sd);
    }
    if (!ours) {
        CloseHandle(h);
        return false;
    }
    // Lets the primary raise its window when it handles InstanceActivated:
    // the foreground lock only yields to a process the foreground one names.
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h, &pid) || !AllowSetForegroundWindow(pid))
        AllowSetForegroundWindow(ASFW_ANY);

    bool  ok   = true;
    DWORD sent = 0;
    for (size_t at = 0; ok && at < frame.size(); at += sent)
        ok = ioWithin(
                 h,
                 true,
                 const_cast<char *>(frame.data() + at),
                 DWORD(frame.size() - at),
                 left(),
                 &sent
             ) &&
             sent > 0;
    char  ack[4];
    DWORD got = 0;
    for (DWORD n = 0; ok && got < 4; got += n)
        ok = ioWithin(h, false, ack + got, 4 - got, left(), &n) && n > 0;
    CloseHandle(h);
    return ok && std::memcmp(ack, kAck, 4) == 0;
}

bool Win32App::registerUrlScheme(std::string_view schemeIn) {
    if (!validUrlScheme(schemeIn))
        return false;
    const std::string scheme = core::asciiLower(schemeIn);
    // Remembered even if the registry write fails (say, policy locks HKCU
    // Classes): an installer may have registered it, and forwarded URLs of
    // it should still come out as OpenUrls.
    core::rememberScheme(_schemes, scheme);
    const std::wstring exe = exePath();
    if (exe.empty())
        return false;
    const std::wstring ws   = toWide(scheme);
    const std::wstring root = L"Software\\Classes\\" + ws;
    HKEY               key = nullptr, cmd = nullptr, icon = nullptr;
    bool               ok = false;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, root.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr
        ) == ERROR_SUCCESS) {
        // "URL Protocol" (empty) is what makes the shell treat it as a scheme.
        ok = setRegString(key, nullptr, L"URL:" + ws) && setRegString(key, L"URL Protocol", L"");
        if (RegCreateKeyExW(
                key, L"shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &cmd, nullptr
            ) == ERROR_SUCCESS) {
            ok = setRegString(cmd, nullptr, urlSchemeCommand(exe)) && ok;
            RegCloseKey(cmd);
        } else {
            ok = false;
        }
        if (RegCreateKeyExW(
                key, L"DefaultIcon", 0, nullptr, 0, KEY_WRITE, nullptr, &icon, nullptr
            ) == ERROR_SUCCESS) {
            setRegString(icon, nullptr, L"\"" + exe + L"\",0");
            RegCloseKey(icon);
        }
        RegCloseKey(key);
    }
    return ok;
}

std::vector<std::string> Win32App::preferredLanguages() const {
    // The display-language list from Settings, a double-NUL-terminated
    // multi-string of names like "ja-JP".
    std::vector<std::string> out;
    ULONG                    num = 0, len = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, nullptr, &len) || !len)
        return out;
    std::wstring buf(len, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, buf.data(), &len))
        return out;
    for (const wchar_t *p = buf.c_str(); *p; p += wcslen(p) + 1)
        out.push_back(toUtf8(p));
    return out;
}

// ── files ───────────────────────────────────────────────────────────────────

std::string Win32App::standardDir(StandardDir d) const {
    if (d == StandardDir::Temp) {
        std::wstring p = tempPath();
        if (p.empty())
            return {};
        while (p.size() > 3 && p.back() == L'\\') // keep "C:\"
            p.pop_back();
        return portablePath(p);
    }
    // Config roams with the profile; data, cache and state are machine-
    // local. Windows has no separate cache/state folders: apps append their
    // own subdirectory (…\msga\cache).
    const KNOWNFOLDERID *id     = nullptr;
    bool                 create = true;
    switch (d) {
    case StandardDir::Config:
        id = &FOLDERID_RoamingAppData;
        break;
    case StandardDir::Data:
    case StandardDir::Cache:
    case StandardDir::State:
        id = &FOLDERID_LocalAppData;
        break;
    case StandardDir::Home:
        id = &FOLDERID_Profile;
        break;
    case StandardDir::Desktop:
        id     = &FOLDERID_Desktop;
        create = false; // the user's folders: report them, never conjure them
        break;
    case StandardDir::Documents:
        id     = &FOLDERID_Documents;
        create = false;
        break;
    case StandardDir::Downloads:
        id     = &FOLDERID_Downloads;
        create = false;
        break;
    case StandardDir::Pictures:
        id     = &FOLDERID_Pictures;
        create = false;
        break;
    case StandardDir::Temp:
        break;
    }
    PWSTR p = nullptr;
    if (!id || FAILED(SHGetKnownFolderPath(*id, create ? DWORD(KF_FLAG_CREATE) : 0, nullptr, &p)))
        return {};
    std::wstring w = p;
    CoTaskMemFree(p);
    while (w.size() > 3 && w.back() == L'\\')
        w.pop_back();
    return portablePath(w);
}

// ── teardown ────────────────────────────────────────────────────────────────

void Win32App::teardownSystem() {
    for (auto *i : _instances)
        delete i;
    _instances.clear();
    if (_network) {
        _network->shutdown();
        _network = nullptr;
    }
    if (_sysHwnd) {
        if (_powerNotify) {
            using SrUnregister = BOOL(WINAPI *)(HPOWERNOTIFY);
            if (auto unreg = sym<SrUnregister>(
                    GetModuleHandleW(L"user32.dll"), "UnregisterSuspendResumeNotification"
                ))
                unreg(HPOWERNOTIFY(_powerNotify));
        }
        SetWindowLongPtrW(_sysHwnd, GWLP_USERDATA, 0);
        DestroyWindow(_sysHwnd);
        _sysHwnd = nullptr;
    }
}

} // namespace plat::win32
