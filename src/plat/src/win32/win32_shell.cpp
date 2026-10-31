// Win32 backend, shell integration: the tray (Shell_NotifyIcon v4 on a hidden
// window per icon), notifications (WinRT toasts through raw COM, with a
// Shell_NotifyIcon balloon fallback where toasts are unavailable — Wine, a
// toolchain without the WinRT headers, a stripped-down Windows), and the
// taskbar badge (ITaskbarList3 overlay icons).
//
// Toasts for an unpackaged exe need an AppUserModelID Windows knows: we
// register AppInfo.id under HKCU\Software\Classes\AppUserModelId (display name
// + icon), which is what the Windows App SDK and the Community Toolkit do for
// unpackaged apps on 1903+ and needs no Start-menu shortcut; builds before
// that only honour an AUMID carried by a Start-menu shortcut, which we then
// create (an app that asks for it, AppInfo::startMenuShortcut, gets that
// shortcut on every build). Toast events arrive on a WinRT
// worker thread and are posted to the loop; a click on a toast that is only
// left in the Action Centre after the app exited is not delivered (that needs
// a registered COM activator, which a library cannot own).
#include "win32/win32.h"

#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#if __has_include(<windows.ui.notifications.h>) && __has_include(<windows.data.xml.dom.h>) &&       \
    __has_include(<winstring.h>)
#define PLAT_WINRT_TOAST 1
// mingw-w64's windows.foundation.h specialises IReference<boolean> and
// IReference<BYTE>, which are the same type there (boolean is unsigned
// char); skipping the second one is harmless, nothing here uses it.
#ifndef ____FIReference_1_boolean_INTERFACE_DEFINED__
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <winstring.h>
#endif

#include <algorithm>
#include <map>
#include <mutex>

namespace plat::win32 {

namespace {

constexpr UINT kTrayMsg = WM_APP + 7; // notify-icon callbacks

int smallIconSize() {
    // At the system DPI: that is what the notification area renders at.
    using ForDpi     = int(WINAPI *)(int, UINT);
    static auto fn   = sym<ForDpi>(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    const UINT  dpi  = api().getDpiForSystem ? api().getDpiForSystem() : 96;
    const int   size = fn ? fn(SM_CXSMICON, dpi) : GetSystemMetrics(SM_CXSMICON);
    return size > 0 ? size : 16;
}

double systemScale() {
    return (api().getDpiForSystem ? api().getDpiForSystem() : 96) / 96.0;
}

template <size_t N>
void copyTruncated(wchar_t (&dst)[N], std::string_view utf8) {
    win32::copyTruncated(dst, N, toWide(utf8));
}

// Anything that owns a hidden notify-icon window.
struct IconHost {
    virtual ~IconHost()                                            = default;
    virtual LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) = 0;
    HWND            hwnd                                           = nullptr;
};

LRESULT CALLBACK iconProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto *host = static_cast<IconHost *>(windowUserData(h, msg, lp));
    // `h`, not host->hwnd: that is only set once CreateWindowExW returns.
    return host ? host->handle(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

HWND createIconWindow(HINSTANCE inst, IconHost *host) {
    // A hidden top-level window, not HWND_MESSAGE: "TaskbarCreated" is a
    // broadcast, and broadcasts only reach top-level windows.
    HWND h = createHiddenWindow(inst, L"plat.tray", &iconProc, host);
    if (h)
        ChangeWindowMessageFilterEx(
            h, RegisterWindowMessageW(L"TaskbarCreated"), MSGFLT_ALLOW, nullptr
        );
    return h;
}

UINT taskbarCreatedMsg() {
    static const UINT m = RegisterWindowMessageW(L"TaskbarCreated");
    return m;
}

std::string xmlEscape(std::string_view s) {
    return core::escapeMarkup(s, true);
}

// What a toast hands back on activation: "plat:<id>:<percent-encoded key>",
// key empty for the body. The test hook feeds the same string in.
std::string toastArguments(uint64_t id, std::string_view key) {
    return "plat:" + std::to_string(id) + ":" + core::percentEncode(key);
}

bool parseToastArguments(std::string_view args, uint64_t *id, std::string *key) {
    if (args.substr(0, 5) != "plat:")
        return false;
    args.remove_prefix(5);
    const size_t colon = args.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    uint64_t v = 0;
    for (char c : args.substr(0, colon)) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + uint64_t(c - '0');
    }
    *id  = v;
    *key = core::percentDecode(args.substr(colon + 1));
    return true;
}

} // namespace

void copyTruncated(wchar_t *dst, size_t cap, std::wstring_view w) {
    size_t n = std::min(w.size(), cap - 1);
    if (n < w.size() && n > 0 && IS_HIGH_SURROGATE(w[n - 1]))
        --n;
    std::copy(w.begin(), w.begin() + ptrdiff_t(n), dst);
    dst[n] = 0;
}

HICON appIcon() {
    HICON i = LoadIconW(GetModuleHandleW(nullptr), L"IDI_ICON1");
    return i ? i : LoadIconW(nullptr, IDI_APPLICATION);
}

void setAppUserModelIdProperty(IPropertyStore *store, const std::wstring &id) {
    // PKEY_AppUserModel_ID, spelled out to avoid linking propsys for one
    // constant.
    static const PROPERTYKEY kAppId = {
        {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5
    };
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt      = VT_LPWSTR;
    pv.pwszVal = const_cast<wchar_t *>(id.c_str()); // borrowed: SetValue copies
    store->SetValue(kAppId, pv);
    store->Commit();
}

// ── tray ────────────────────────────────────────────────────────────────────

class Win32Tray final : public Tray, public IconHost {
public:
    explicit Win32Tray(Shell *shell);
    ~Win32Tray() override;

    void setIcon(const std::vector<Image> &sizes) override;
    void setTooltip(std::string_view utf8) override;
    void setMenu(std::vector<MenuItem> items) override;
    bool isVisible() const override { return _added; }

    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) override;
    void    detach(); // the App is going away first
#ifdef PLAT_TEST_HOOKS
    bool postActivate();
    bool postCommand(uint32_t itemId);
    int  addCount = 0;
#endif

private:
    struct Command {
        uint32_t id;
        bool     enabled;
    };
    void  add();
    void  modify(UINT flags);
    void  fill(NOTIFYICONDATAW &nid, UINT flags) const;
    void  flatten(const std::vector<MenuItem> &items, bool enabled);
    HMENU build(const std::vector<MenuItem> &items, UINT &next) const;
    void  showMenu(POINT at);
    void  onCommand(UINT cmd);

    Shell                *_shell;
    bool                  _added = false;
    HICON                 _icon  = nullptr; // ours when set from an Image
    std::vector<Image>    _sizes;
    std::wstring          _tip;
    std::vector<MenuItem> _menu;
    std::vector<Command>  _commands; // command id - 1 → item, depth-first like build()
};

// ── shell state ─────────────────────────────────────────────────────────────

#if defined(PLAT_WINRT_TOAST)
namespace wr   = ABI::Windows::UI::Notifications;
namespace xdom = ABI::Windows::Data::Xml::Dom;
using ActivatedHandler =
    ABI::Windows::Foundation::ITypedEventHandler<wr::ToastNotification *, IInspectable *>;
using DismissedHandler = ABI::Windows::Foundation::
    ITypedEventHandler<wr::ToastNotification *, wr::ToastDismissedEventArgs *>;
using FailedHandler = ABI::Windows::Foundation::
    ITypedEventHandler<wr::ToastNotification *, wr::ToastFailedEventArgs *>;

// combase entry points, resolved at runtime so the exe still starts where
// WinRT is missing (and Wine's combase lacks the notification classes).
struct RoApi {
    HRESULT(WINAPI *getActivationFactory)(HSTRING, REFIID, void **) = nullptr;
    HRESULT(WINAPI *activateInstance)(HSTRING, IInspectable **)     = nullptr;
    HRESULT(WINAPI *createString)(PCNZWCH, UINT32, HSTRING *)       = nullptr;
    HRESULT(WINAPI *deleteString)(HSTRING)                          = nullptr;
    PCWSTR(WINAPI *rawBuffer)(HSTRING, UINT32 *)                    = nullptr;
    bool ok() const {
        return getActivationFactory && activateInstance && createString && deleteString &&
               rawBuffer;
    }
};
const RoApi &ro() {
    static const RoApi r = [] {
        RoApi   a;
        HMODULE m              = LoadLibraryW(L"combase.dll"); // process lifetime
        a.getActivationFactory = sym<decltype(a.getActivationFactory)>(m, "RoGetActivationFactory");
        a.activateInstance     = sym<decltype(a.activateInstance)>(m, "RoActivateInstance");
        a.createString         = sym<decltype(a.createString)>(m, "WindowsCreateString");
        a.deleteString         = sym<decltype(a.deleteString)>(m, "WindowsDeleteString");
        a.rawBuffer            = sym<decltype(a.rawBuffer)>(m, "WindowsGetStringRawBuffer");
        return a;
    }();
    return r;
}

struct HStr {
    HSTRING h = nullptr;
    explicit HStr(std::wstring_view s) { ro().createString(s.data(), UINT32(s.size()), &h); }
    ~HStr() {
        if (h)
            ro().deleteString(h);
    }
    HStr(const HStr &)            = delete;
    HStr &operator=(const HStr &) = delete;
};

std::string fromHString(HSTRING h) {
    UINT32       n = 0;
    const PCWSTR p = h ? ro().rawBuffer(h, &n) : nullptr;
    return p ? toUtf8(std::wstring_view(p, n)) : std::string();
}

// Where handler threads find the loop. Nulled when the App goes, so a late
// callback from a WinRT worker thread posts nowhere instead of into freed memory.
struct ToastSink {
    std::mutex mutex;
    Win32App  *app = nullptr;
    void       post(std::function<void()> fn) {
        std::lock_guard lock(mutex);
        if (app)
            app->post(std::move(fn));
    }
};

// The three ITypedEventHandler<ToastNotification*, Args*> a toast takes, as
// one plain COM object: their vtables are the same (IUnknown, then
// Invoke(sender, args) with both pointers), so this implements Activated's
// and answers to the IID it was made for; `fn` reads `args` as that
// handler's own type. Agile: the toast raises its events on a worker thread
// and would otherwise try to marshal the call into our STA (which may not be
// pumping).
class ToastHandler final : public ActivatedHandler, public IAgileObject {
public:
    using Fn = void (*)(const std::weak_ptr<ToastSink> &sink, uint64_t id, IInspectable *args);
    ToastHandler(REFIID iid, Fn fn, std::weak_ptr<ToastSink> sink, uint64_t id)
        : _iid(iid), _fn(fn), _sink(std::move(sink)), _id(id) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == IID_IUnknown || riid == _iid) {
            *out = static_cast<ActivatedHandler *>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject *>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --_refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Invoke(wr::IToastNotification *, IInspectable *args) override {
        _fn(_sink, _id, args);
        return S_OK;
    }
    // This object as handler interface I (DismissedHandler, FailedHandler).
    template <class I>
    I *as() {
        return reinterpret_cast<I *>(static_cast<ActivatedHandler *>(this));
    }

private:
    std::atomic<ULONG>             _refs{1};
    const IID                      _iid;
    const Fn                       _fn;
    const std::weak_ptr<ToastSink> _sink;
    const uint64_t                 _id;
};
#endif

struct Shell final : IconHost {
    explicit Shell(Win32App *a) : app(a) {
        hwnd = createIconWindow(a->instance(), this); // balloons' icons
#if defined(PLAT_WINRT_TOAST)
        sink      = std::make_shared<ToastSink>();
        sink->app = a;
#endif
    }
    ~Shell() override;

    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) override;

    // Balloons: one notify icon per shown balloon, so every callback says
    // which notification it is about (uID in HIWORD(lParam) with v4).
    struct Balloon {
        UINT  uid;
        HICON icon = nullptr;
    };
    uint64_t balloonNotify(const Notification &n, uint64_t id);
    void     balloonRemove(uint64_t id, bool closed);
    uint64_t balloonFor(UINT uid) const {
        for (const auto &[id, b] : balloons)
            if (b.uid == uid)
                return id;
        return 0;
    }

    bool toastsReady();
    // Whether Shell_NotifyIcon accepts an icon at all (a hidden probe). Not
    // FindWindow("Shell_TrayWnd"): Wine's explorer creates that lazily, and
    // its notify icons work before it exists.
    bool balloonsWork() {
        if (balloonsOk || !hwnd)
            return balloonsOk;
        NOTIFYICONDATAW nid{};
        nid.cbSize  = sizeof(nid);
        nid.hWnd    = hwnd;
        nid.uID     = 0xffff; // never a balloon's
        nid.uFlags  = NIF_ICON | NIF_STATE;
        nid.hIcon   = appIcon();
        nid.dwState = nid.dwStateMask = NIS_HIDDEN;
        balloonsOk                    = Shell_NotifyIconW(NIM_ADD, &nid) != 0;
        if (balloonsOk)
            Shell_NotifyIconW(NIM_DELETE, &nid);
        return balloonsOk;
    }
    bool balloonsOk = false;
#if defined(PLAT_WINRT_TOAST)
    struct Toast {
        wr::IToastNotification  *toast = nullptr;
        EventRegistrationToken   activated{}, dismissed{}, failed{};
        std::vector<std::string> actionKeys;
        bool                     inCentre = false; // timed out into the Action Centre
    };
    uint64_t toastNotify(const Notification &n, uint64_t id);
    void     toastForget(uint64_t id);
    void     toastActivated(uint64_t id, const std::string &args);
    bool     registerAumid();

    std::shared_ptr<ToastSink>     sink;
    wr::IToastNotifier            *notifier = nullptr;
    wr::IToastNotificationFactory *factory  = nullptr;
    std::map<uint64_t, Toast>      toasts;
    int                            imageRotation = 0;
#endif
    bool toastsTried = false, toastsOk = false;

    Win32App                   *app;
    std::vector<Win32Tray *>    trays;
    std::map<uint64_t, Balloon> balloons;
    UINT                        nextUid = 1;
#ifdef PLAT_TEST_HOOKS
    bool forceBalloon = false;
#endif

    int            badge        = 0;
    HICON          badgeIcon    = nullptr;
    ITaskbarList3 *taskbar      = nullptr;
    bool           taskbarTried = false;
};

void ShellDeleter::operator()(Shell *s) const {
    delete s;
}

Shell &Win32App::shell() {
    if (!_shell)
        _shell.reset(new Shell(this));
    return *_shell;
}

Shell::~Shell() {
#if defined(PLAT_WINRT_TOAST)
    {
        std::lock_guard lock(sink->mutex);
        sink->app = nullptr;
    }
    while (!toasts.empty())
        toastForget(toasts.begin()->first); // leave them on screen; drop our handlers
    if (notifier)
        notifier->Release();
    if (factory)
        factory->Release();
#endif
    while (!balloons.empty())
        balloonRemove(balloons.begin()->first, false);
    for (auto *t : std::vector<Win32Tray *>(trays))
        t->detach();
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
    if (badgeIcon)
        DestroyIcon(badgeIcon);
    if (taskbar)
        taskbar->Release();
}

// ── Win32Tray ───────────────────────────────────────────────────────────────

Win32Tray::Win32Tray(Shell *shell) : _shell(shell) {
    hwnd = createIconWindow(shell->app->instance(), this);
    shell->trays.push_back(this);
    add();
}

Win32Tray::~Win32Tray() {
    detach();
}

void Win32Tray::detach() {
    if (!_shell)
        return;
    if (_added) {
        NOTIFYICONDATAW nid{};
        fill(nid, 0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        _added = false;
    }
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (_icon)
        DestroyIcon(_icon);
    _icon   = nullptr;
    auto &v = _shell->trays;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    _shell = nullptr;
}

void Win32Tray::fill(NOTIFYICONDATAW &nid, UINT flags) const {
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = 1;
    nid.uFlags           = flags;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon            = _icon ? _icon : appIcon();
    copyTruncated(nid.szTip, std::size(nid.szTip), _tip);
}

void Win32Tray::add() {
    if (!hwnd)
        return;
    // TaskbarCreated also follows a DPI change: re-pick the icon size.
    if (!_sizes.empty())
        if (HICON icon = iconFromImage(fitImage(_sizes, smallIconSize(), systemScale()))) {
            if (_icon)
                DestroyIcon(_icon);
            _icon = icon;
        }
    NOTIFYICONDATAW nid{};
    // NIF_SHOWTIP: with version 4 the standard tooltip is opt-in.
    fill(nid, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    _added = Shell_NotifyIconW(NIM_ADD, &nid) != 0;
    // A TaskbarCreated that did not actually lose our icon (a DPI change, a
    // second broadcast): the add fails because it is still there.
    if (!_added)
        _added = Shell_NotifyIconW(NIM_MODIFY, &nid) != 0;
    if (!_added)
        return;
#ifdef PLAT_TEST_HOOKS
    ++addCount;
#endif
    // Version 4: NIN_SELECT for clicks, WM_CONTEXTMENU with the anchor point
    // for the menu, the icon id in HIWORD(lParam).
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void Win32Tray::modify(UINT flags) {
    if (!_shell)
        return;
    if (!_added) {
        add(); // e.g. the first add raced Explorer's start-up
        return;
    }
    NOTIFYICONDATAW nid{};
    fill(nid, flags | NIF_SHOWTIP);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Win32Tray::setIcon(const std::vector<Image> &sizes) {
    _sizes     = sizes;
    HICON icon = iconFromImage(fitImage(sizes, smallIconSize(), systemScale()));
    if (!icon)
        return;
    HICON old = _icon;
    _icon     = icon;
    modify(NIF_ICON);
    if (old)
        DestroyIcon(old); // after the shell took its copy of the new one
}

void Win32Tray::setTooltip(std::string_view utf8) {
    _tip = toWide(utf8);
    modify(NIF_TIP);
}

void Win32Tray::flatten(const std::vector<MenuItem> &items, bool enabled) {
    for (const auto &m : items) {
        if (m.kind == MenuItem::Kind::Separator)
            continue;
        if (m.kind == MenuItem::Kind::Submenu)
            flatten(m.children, enabled && m.enabled);
        else
            _commands.push_back({m.id, enabled && m.enabled});
    }
}

void Win32Tray::setMenu(std::vector<MenuItem> items) {
    _menu = std::move(items);
    _commands.clear();
    flatten(_menu, true);
}

HMENU Win32Tray::build(const std::vector<MenuItem> &items, UINT &next) const {
    HMENU m = CreatePopupMenu();
    for (const auto &i : items) {
        const std::wstring label = toWide(i.label);
        const UINT         gray  = i.enabled ? 0 : MF_GRAYED;
        switch (i.kind) {
        case MenuItem::Kind::Separator:
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            break;
        case MenuItem::Kind::Submenu: {
            HMENU sub = build(i.children, next);
            AppendMenuW(m, MF_POPUP | MF_STRING | gray, UINT_PTR(sub), label.c_str()); // m owns sub
            break;
        }
        case MenuItem::Kind::Checkbox:
        case MenuItem::Kind::Action:
            AppendMenuW(
                m,
                MF_STRING | gray |
                    (i.kind == MenuItem::Kind::Checkbox && i.checked ? MF_CHECKED : 0),
                next++,
                label.c_str()
            );
            break;
        }
    }
    return m;
}

void Win32Tray::showMenu(POINT at) {
    if (_menu.empty() || !_shell)
        return;
    UINT  next = 1;
    HMENU menu = build(_menu, next);
    // The documented dance: without the foreground the menu does not close
    // when the user clicks elsewhere, and without the WM_NULL afterwards the
    // next click on the icon is swallowed.
    SetForegroundWindow(hwnd);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    Win32App *app = _shell->app;
    app->enterModal(); // the menu's own loop owns the pump meanwhile
    const UINT cmd = UINT(TrackPopupMenuEx(menu, flags, at.x, at.y, hwnd, nullptr));
    app->leaveModal();
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd)
        onCommand(cmd);
}

void Win32Tray::onCommand(UINT cmd) {
    if (!_shell || cmd == 0 || cmd > _commands.size() || !_commands[cmd - 1].enabled)
        return;
    _shell->app->emit({.type = EventType::TrayMenuItem, .tray = this, .id = _commands[cmd - 1].id});
}

#ifdef PLAT_TEST_HOOKS
bool Win32Tray::postActivate() {
    if (!hwnd || !_added)
        return false;
    POINT p;
    GetCursorPos(&p);
    // The v4 layout the shell uses: anchor in wParam, event + icon id in lParam.
    return PostMessageW(hwnd, kTrayMsg, MAKEWPARAM(p.x, p.y), MAKELPARAM(NIN_SELECT, 1)) != 0;
}

bool Win32Tray::postCommand(uint32_t itemId) {
    for (size_t i = 0; i < _commands.size(); ++i)
        if (_commands[i].id == itemId && _commands[i].enabled)
            return hwnd && PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(UINT(i + 1), 0), 0) != 0;
    return false;
}
#endif

LRESULT Win32Tray::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTrayMsg && _shell) {
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            _shell->app->emit({.type = EventType::TrayActivated, .tray = this});
            return 0;
        case WM_CONTEXTMENU:
            showMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            return 0;
        default:
            return 0;
        }
    }
    if (msg == WM_COMMAND && HIWORD(wp) == 0) {
        onCommand(LOWORD(wp));
        return 0;
    }
    if (msg == taskbarCreatedMsg() && _shell) {
        // Explorer restarted (or started after us): the icon is gone.
        _added = false;
        add();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::unique_ptr<Tray> Win32App::createTray() {
    // Always a Tray: whether a notification area exists shows in isVisible()
    // (the NIM_ADD result), and TaskbarCreated re-adds it once one appears.
    return std::make_unique<Win32Tray>(&shell());
}

#ifdef PLAT_TEST_HOOKS
bool Win32App::trayActivate(Tray &t) {
    return static_cast<Win32Tray &>(t).postActivate();
}

bool Win32App::trayMenuSelect(Tray &t, uint32_t itemId) {
    return static_cast<Win32Tray &>(t).postCommand(itemId);
}

int Win32App::trayAddCount(Tray &t) const {
    return static_cast<Win32Tray &>(t).addCount;
}
#endif

// ── balloons ────────────────────────────────────────────────────────────────

uint64_t Shell::balloonNotify(const Notification &n, uint64_t id) {
    if (!hwnd)
        return 0;
    // v4 callbacks carry the icon id in HIWORD(lParam): 16 bits, and 0xffff
    // is the availability probe's.
    Balloon b{nextUid};
    nextUid = nextUid % 0xfffe + 1;
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = b.uid;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon            = appIcon();
    copyTruncated(nid.szTip, app->appInfo().name);
    copyTruncated(nid.szInfoTitle, n.title);
    copyTruncated(
        nid.szInfo, n.body.empty() ? std::string(" ") : n.body
    ); // empty text = no balloon
    nid.dwInfoFlags = NIIF_USER | (n.silent ? NIIF_NOSOUND : 0);
    if (!n.image.empty()) {
        const int big = GetSystemMetrics(SM_CXICON);
        b.icon        = iconFromImage(fitImage({n.image}, big > 0 ? big : 32));
        if (b.icon) {
            nid.hBalloonIcon = b.icon;
            nid.dwInfoFlags |= NIIF_LARGE_ICON;
        }
    }
    nid.uTimeout = n.timeoutMs > 0 ? UINT(n.timeoutMs) : 0; // ignored since Vista; harmless
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        if (b.icon)
            DestroyIcon(b.icon);
        return 0;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    balloons[id] = b;
    return id;
}

void Shell::balloonRemove(uint64_t id, bool closed) {
    auto it = balloons.find(id);
    if (it == balloons.end())
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = it->second.uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (it->second.icon)
        DestroyIcon(it->second.icon);
    balloons.erase(it);
    if (closed)
        app->post([a = app, id] { a->emit({.type = EventType::NotificationClosed, .id = id}); });
}

LRESULT Shell::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTrayMsg) {
        const uint64_t id = balloonFor(HIWORD(lp));
        if (!id)
            return 0;
        switch (LOWORD(lp)) {
        case NIN_BALLOONUSERCLICK: {
            balloonRemove(id, false);
            app->emit({.type = EventType::NotificationActivated, .id = id});
            return 0;
        }
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE:
            balloonRemove(id, true);
            return 0;
        default:
            return 0;
        }
    }
    if (msg == taskbarCreatedMsg()) {
        // Explorer restarted: the balloons went with it.
        while (!balloons.empty())
            balloonRemove(balloons.begin()->first, true);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ── toasts ──────────────────────────────────────────────────────────────────

#if defined(PLAT_WINRT_TOAST)

namespace {

DWORD windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    auto           fn     = sym<RtlGetVersionFn>(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW vi{sizeof(vi)};
    return fn && fn(&vi) == 0 ? vi.dwBuildNumber : 0;
}

// Pre-1903 Windows only accepts toasts from an AUMID carried by a Start-menu
// shortcut. Idempotent: an
// existing one is left alone.
bool writeStartMenuShortcut(const std::wstring &aumid, const std::wstring &name) {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        return false;
    std::wstring lnk = std::wstring(programs) + L"\\" + name + L".lnk";
    CoTaskMemFree(programs);
    if (GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;
    const std::wstring exe = exePath(); // long paths too
    if (exe.empty())
        return false;
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IShellLinkW,
            reinterpret_cast<void **>(&link)
        )))
        return false;
    link->SetPath(exe.c_str());
    bool            ok    = false;
    IPropertyStore *props = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void **>(&props)))) {
        setAppUserModelIdProperty(props, aumid);
        props->Release();
    }
    IPersistFile *file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

std::wstring tempDir() {
    std::wstring p = tempPath();
    return p.empty() ? std::wstring(L".\\") : p;
}

} // namespace

void Win32App::ensureStartMenuShortcut() {
    const AppInfo &info = appInfo();
    if (!info.startMenuShortcut || !oleReady())
        return;
    writeStartMenuShortcut(toWide(info.id), toWide(info.name.empty() ? info.id : info.name));
}

bool Shell::registerAumid() {
    const AppInfo     &info  = app->appInfo();
    const std::wstring aumid = toWide(info.id);
    const std::wstring name  = toWide(info.name.empty() ? info.id : info.name);
    // No SetCurrentProcessExplicitAppUserModelID: the notifier is created
    // for this id explicitly, and taskbar grouping stays WindowDesc::appId's.
    HKEY               key   = nullptr;
    const std::wstring path  = L"Software\\Classes\\AppUserModelId\\" + aumid;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr
        ) != ERROR_SUCCESS)
        return false;
    bool ok = setRegString(key, L"DisplayName", name);
    // The toast header icon: a PNG in %LOCALAPPDATA%\<id>\, rewritten on every
    // start so a changed icon shows up.
    if (!info.icon.empty()) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            const std::wstring dir = std::wstring(local) + L"\\" + aumid;
            CoTaskMemFree(local);
            CreateDirectoryW(dir.c_str(), nullptr);
            const std::wstring file = dir + L"\\notification-icon.png";
            if (writeFile(file, encodePng(info.icon)))
                setRegString(key, L"IconUri", file);
        }
    }
    RegCloseKey(key);
    const DWORD build = windowsBuild();
    if (build && build < 18362)
        ok = writeStartMenuShortcut(aumid, name) && ok;
    return ok;
}

bool Shell::toastsReady() {
#ifdef PLAT_TEST_HOOKS
    if (forceBalloon)
        return false;
#endif
    if (toastsTried)
        return toastsOk;
    toastsTried = true;
    if (!ro().ok())
        return false;
    // Probe the classes before touching the registry: where WinRT has no
    // notifications (Wine) nothing gets written.
    wr::IToastNotificationManagerStatics *mgr = nullptr;
    {
        HStr cls(L"Windows.UI.Notifications.ToastNotificationManager");
        if (FAILED(
                ro().getActivationFactory(
                    cls.h,
                    __uuidof(wr::IToastNotificationManagerStatics),
                    reinterpret_cast<void **>(&mgr)
                )
            ) ||
            !mgr)
            return false;
    }
    {
        HStr cls(L"Windows.UI.Notifications.ToastNotification");
        if (FAILED(
                ro().getActivationFactory(
                    cls.h,
                    __uuidof(wr::IToastNotificationFactory),
                    reinterpret_cast<void **>(&factory)
                )
            ) ||
            !factory) {
            mgr->Release();
            return false;
        }
    }
    registerAumid(); // best effort: a failure shows up as NotificationFailed / nothing shown
    HStr          id(toWide(app->appInfo().id));
    const HRESULT hr = mgr->CreateToastNotifierWithId(id.h, &notifier);
    mgr->Release();
    toastsOk = SUCCEEDED(hr) && notifier;
    return toastsOk;
}

uint64_t Shell::toastNotify(const Notification &n, uint64_t id) {
    std::string xml = "<toast launch=\"" + xmlEscape(toastArguments(id, "")) + "\"";
    if (n.timeoutMs > 10000)
        xml += " duration=\"long\""; // ~25 s instead of ~7 s; the only knob toasts have
    xml += "><visual><binding template=\"ToastGeneric\"><text>" + xmlEscape(n.title) +
           "</text><text>" + xmlEscape(n.body) + "</text>";
    if (!n.image.empty()) {
        // The renderer reads a file; rotate a few so a burst never reads a
        // half-written one.
        const std::wstring path = tempDir() + L"plat-notification-" +
                                  std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(imageRotation++ & 7) + L".png";
        if (writeFile(path, encodePng(n.image)))
            xml += "<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"" +
                   xmlEscape(fileUri(path)) + "\"/>";
    }
    xml += "</binding></visual>";
    if (!n.actions.empty()) {
        xml += "<actions>";
        for (const auto &a : n.actions)
            xml += "<action content=\"" + xmlEscape(a.label) + "\" arguments=\"" +
                   xmlEscape(toastArguments(id, a.key)) + "\" activationType=\"foreground\"/>";
        xml += "</actions>";
    }
    if (n.silent)
        xml += "<audio silent=\"true\"/>";
    xml += "</toast>";

    xdom::IXmlDocument *doc = nullptr;
    {
        IInspectable *insp = nullptr;
        HStr          cls(L"Windows.Data.Xml.Dom.XmlDocument");
        if (FAILED(ro().activateInstance(cls.h, &insp)) || !insp)
            return 0;
        insp->QueryInterface(__uuidof(xdom::IXmlDocument), reinterpret_cast<void **>(&doc));
        xdom::IXmlDocumentIO *io = nullptr;
        insp->QueryInterface(__uuidof(xdom::IXmlDocumentIO), reinterpret_cast<void **>(&io));
        insp->Release();
        HStr    text(toWide(xml));
        HRESULT hr = io ? io->LoadXml(text.h) : E_NOINTERFACE;
        if (io)
            io->Release();
        if (FAILED(hr) || !doc) {
            if (doc)
                doc->Release();
            return 0;
        }
    }
    Toast   t;
    HRESULT hr = factory->CreateToastNotification(doc, &t.toast);
    doc->Release();
    if (FAILED(hr) || !t.toast)
        return 0;
    for (const auto &a : n.actions)
        t.actionKeys.push_back(a.key);

    auto *onActivated = new ToastHandler(
        __uuidof(ActivatedHandler),
        [](const std::weak_ptr<ToastSink> &weak, uint64_t id, IInspectable *args) {
            std::string                   arguments;
            wr::IToastActivatedEventArgs *a = nullptr;
            if (args && SUCCEEDED(args->QueryInterface(
                            __uuidof(wr::IToastActivatedEventArgs), reinterpret_cast<void **>(&a)
                        ))) {
                HSTRING h = nullptr;
                if (SUCCEEDED(a->get_Arguments(&h)) && h) {
                    arguments = fromHString(h);
                    ro().deleteString(h);
                }
                a->Release();
            }
            if (auto s = weak.lock())
                s->post([s, id, arguments] {
                    if (s->app)
                        s->app->shell().toastActivated(id, arguments);
                });
        },
        sink,
        id
    );
    auto *onDismissed = new ToastHandler(
        __uuidof(DismissedHandler),
        [](const std::weak_ptr<ToastSink> &weak, uint64_t id, IInspectable *raw) {
            auto                    *args   = reinterpret_cast<wr::IToastDismissedEventArgs *>(raw);
            wr::ToastDismissalReason reason = wr::ToastDismissalReason_UserCanceled;
            if (args)
                args->get_Reason(&reason);
            const bool timedOut = reason == wr::ToastDismissalReason_TimedOut;
            if (auto s = weak.lock())
                s->post([s, id, timedOut] {
                    if (!s->app)
                        return;
                    Shell &sh = s->app->shell();
                    auto   it = sh.toasts.find(id);
                    if (it == sh.toasts.end() || it->second.inCentre)
                        return; // closed by the app, or already reported
                    if (timedOut) {
                        // The popup expired into the Action Centre: closed as
                        // far as the screen goes, but a click there still
                        // arrives while we run, so keep listening — for the
                        // newest few, not an unbounded history.
                        it->second.inCentre = true;
                        int kept            = 0;
                        for (auto r = sh.toasts.rbegin(); r != sh.toasts.rend(); ++r)
                            kept += r->second.inCentre;
                        for (auto o = sh.toasts.begin(); kept > 32 && o != sh.toasts.end();)
                            if (o->second.inCentre) {
                                --kept;
                                sh.toastForget((o++)->first);
                            } else {
                                ++o;
                            }
                    } else {
                        sh.toastForget(id);
                    }
                    s->app->emit({.type = EventType::NotificationClosed, .id = id});
                });
        },
        sink,
        id
    );
    auto *onFailed = new ToastHandler(
        __uuidof(FailedHandler),
        [](const std::weak_ptr<ToastSink> &weak, uint64_t id, IInspectable *raw) {
            auto   *args = reinterpret_cast<wr::IToastFailedEventArgs *>(raw);
            HRESULT code = E_FAIL;
            if (args)
                args->get_ErrorCode(&code);
            char text[64];
            std::snprintf(text, sizeof(text), "toast failed (0x%08lx)", (unsigned long)code);
            if (auto s = weak.lock())
                s->post([s, id, reason = std::string(text)] {
                    if (!s->app)
                        return;
                    Shell &sh = s->app->shell();
                    if (!sh.toasts.count(id))
                        return;
                    sh.toastForget(id);
                    s->app->emit({.type = EventType::NotificationFailed, .text = reason, .id = id});
                });
        },
        sink,
        id
    );
    t.toast->add_Activated(onActivated, &t.activated);
    t.toast->add_Dismissed(onDismissed->as<DismissedHandler>(), &t.dismissed);
    t.toast->add_Failed(onFailed->as<FailedHandler>(), &t.failed);
    onActivated->Release(); // the toast holds them now
    onDismissed->Release();
    onFailed->Release();

    toasts[id] = t;
    if (FAILED(notifier->Show(t.toast))) {
        toastForget(id);
        return 0;
    }
    return id;
}

void Shell::toastForget(uint64_t id) {
    auto it = toasts.find(id);
    if (it == toasts.end())
        return;
    Toast t = it->second;
    toasts.erase(it);
    t.toast->remove_Activated(t.activated);
    t.toast->remove_Dismissed(t.dismissed);
    t.toast->remove_Failed(t.failed);
    t.toast->Release();
}

void Shell::toastActivated(uint64_t id, const std::string &args) {
    uint64_t    parsed = 0;
    std::string key;
    if (!parseToastArguments(args, &parsed, &key) || parsed != id)
        key.clear(); // not ours (a bare launch string): treat as a body click
    if (!toasts.count(id))
        return;
    toastForget(id); // clicked means gone from the screen
    app->emit({.type = EventType::NotificationActivated, .id = id, .action = key});
}

#else
bool Shell::toastsReady() {
    return false;
}
#endif

// ── App entry points ────────────────────────────────────────────────────────

bool Win32App::notificationsAvailable() const {
    auto *self = const_cast<Win32App *>(this); // lazily probing is not a visible change
    return self->shell().toastsReady() || self->shell().balloonsWork();
}

bool Win32App::notify(uint64_t id, const Notification &n) {
    Shell &s = shell();
#if defined(PLAT_WINRT_TOAST)
    if (s.toastsReady())
        return s.toastNotify(n, id) != 0;
#endif
    return s.balloonNotify(n, id) != 0;
}

#ifdef PLAT_TEST_HOOKS
bool Win32App::notificationInvoke(uint64_t id, std::string_view action) {
    Shell &s = shell();
#if defined(PLAT_WINRT_TOAST)
    if (auto it = s.toasts.find(id); it != s.toasts.end()) {
        const auto &keys = it->second.actionKeys;
        if (!action.empty() && std::find(keys.begin(), keys.end(), action) == keys.end())
            return false;
        // Exactly the string the toast would hand the Activated handler.
        const std::string args = toastArguments(id, action);
        post([this, id, args] { shell().toastActivated(id, args); });
        return true;
    }
#endif
    auto it = s.balloons.find(id);
    if (it == s.balloons.end() || !action.empty())
        return false; // balloons have no buttons: no user could click one
    return PostMessageW(s.hwnd, kTrayMsg, 0, MAKELPARAM(NIN_BALLOONUSERCLICK, it->second.uid)) != 0;
}

bool Win32App::postBalloonCallback(uint64_t id, UINT event) {
    Shell &s  = shell();
    auto   it = s.balloons.find(id);
    return it != s.balloons.end() &&
           PostMessageW(s.hwnd, kTrayMsg, 0, MAKELPARAM(event, it->second.uid)) != 0;
}

void Win32App::forceBalloonNotifications(bool on) {
    shell().forceBalloon = on;
}
#endif

// ── badge ───────────────────────────────────────────────────────────────────

void Win32App::setBadgeCount(int count) {
    Shell &s = shell();
    count    = std::max(0, count);
    if (count == s.badge)
        return;
    s.badge = count;
    if (s.badgeIcon)
        DestroyIcon(s.badgeIcon);
    s.badgeIcon = count > 0 ? iconFromImage(badgeImage(count, smallIconSize())) : nullptr;
    for (auto *w : _windows)
        applyBadge(w->hwnd());
}

void Win32App::applyBadge(HWND h) {
    if (!_shell)
        return; // no badge was ever set: nothing to (re)apply
    Shell &s = *_shell;
    if (!s.taskbarTried) {
        s.taskbarTried = true;
        if (SUCCEEDED(CoCreateInstance(
                CLSID_TaskbarList,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_ITaskbarList3,
                reinterpret_cast<void **>(&s.taskbar)
            )) &&
            FAILED(s.taskbar->HrInit())) {
            s.taskbar->Release();
            s.taskbar = nullptr;
        }
    }
    if (!s.taskbar)
        return;
    // The description is what screen readers announce for the overlay.
    const std::wstring text = s.badge > 0 ? std::to_wstring(s.badge) + L" unread" : std::wstring();
    s.taskbar->SetOverlayIcon(h, s.badgeIcon, text.empty() ? nullptr : text.c_str());
}

} // namespace plat::win32
