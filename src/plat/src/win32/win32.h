// Win32 backend internals shared by the app half (loop, clipboard, theme,
// test hooks — win32_app.cpp), the window half (wndproc, present, input,
// IME — win32_window.cpp) and the pure helpers (keys, UTF-16 — win32_keys.cpp).
// The test hooks exist only in PLAT_TEST_HOOKS builds.
//
// The desktop-integration half lives in its own files: formats + clipboard
// (win32_data.cpp), OLE drag and drop (win32_dnd.cpp), tray, notifications,
// badge (win32_shell.cpp) and CPU image helpers (win32_image.cpp).
//
// Everything uses the W APIs; strings cross the plat boundary as UTF-8 and
// are converted at the call site. Coordinates inside this backend are
// physical pixels (the process is per-monitor DPI aware v2) and become
// logical only when they reach an Event or a Window getter.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h> // HDROP
#include <unknwn.h>

#include "core/backends.h"
#include "core/loop_core.h"
#include "core/image_util.h"
#include "core/input.h"
#include "core/strings.h"
#include "core/transfer.h"
#ifdef PLAT_TEST_HOOKS
#include "plat/testing.h"
#endif
#include "prim/winstr.h"

#include <atomic>
#include <climits>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct IDropTargetHelper;
struct IPropertyStore;

namespace plat::win32 {

// ── Text, keys, URIs (win32_keys.cpp) ───────────────────────────────────────
// UTF-8 ↔ UTF-16: prim's (src/prim), shared with the app's base library.
inline std::wstring toWide(std::string_view utf8) {
    return prim::wide(utf8);
}
inline std::string toUtf8(std::wstring_view wide) {
    return prim::narrow(wide);
}
// Byte length of the UTF-8 encoding of the first `units` UTF-16 code units —
// IME cursor positions arrive in UTF-16 units, Event offsets are UTF-8 bytes.
int         utf8Length(std::wstring_view wide, size_t units);
// "C:\a b\ü.txt" → "file:///C:/a%20b/%C3%BC.txt"; UNC → "file://server/share/…".
std::string fileUri(std::wstring_view path);

// "file:///C:/a%20b" → "C:\a b", "file://srv/s/x" → "\\srv\s\x"; nullopt
// for anything that is not a local or UNC file (other schemes, /tmp/x).
std::optional<std::wstring> pathFromFileUri(std::string_view uri);
using core::parseUriList; // the URIs of a text/uri-list

Key keyFromVk(UINT vk, bool extended, UINT scan);
#ifdef PLAT_TEST_HOOKS
UINT vkFromKey(Key k, bool *extended); // for SendInput; 0 = no mapping
#endif
uint32_t currentMods(); // from GetKeyState, i.e. as of the current message

// ── Small shared helpers (win32_system.cpp unless noted) ────────────────────
// GetProcAddress as a typed function pointer; null for a missing module or
// symbol. Through void(*)() so -Wcast-function-type accepts the FARPROC cast.
template <class T>
T sym(HMODULE m, const char *name) {
    return m ? reinterpret_cast<T>(reinterpret_cast<void (*)()>(GetProcAddress(m, name))) : nullptr;
}
bool         setRegString(HKEY key, const wchar_t *name, const std::wstring &v); // REG_SZ
std::wstring exePath(); // this executable, long paths included; empty on failure
// GetTempPathW: the temp directory with its trailing '\'; empty on failure.
std::wstring tempPath();
// The app's icon (resource IDI_ICON1), else the stock application icon.
HICON        appIcon();
// The GWLP_USERDATA pointer of a window whose CreateWindowExW passed it as
// lpParam: stored on WM_NCCREATE, then read back for every message.
void        *windowUserData(HWND h, UINT msg, LPARAM lp);
// A hidden top-level tool window of class `cls` (registered on first use):
// broadcasts (TaskbarCreated, WM_SETTINGCHANGE, …) skip message-only ones.
HWND         createHiddenWindow(HINSTANCE inst, const wchar_t *cls, WNDPROC proc, void *param);
// `w` into a fixed buffer of `cap` (NOTIFYICONDATA fields): cut to fit with
// its terminator, never in the middle of a surrogate pair (win32_shell.cpp).
void         copyTruncated(wchar_t *dst, size_t cap, std::wstring_view w);
// PKEY_AppUserModel_ID := id on a property store, committed (win32_shell.cpp).
void         setAppUserModelIdProperty(IPropertyStore *store, const std::wstring &id);
// HGLOBAL ↔ bytes (win32_data.cpp): a movable block holding a copy of `n`
// bytes (at least one byte is allocated), and the whole block's bytes.
HGLOBAL      globalFromBytes(const void *p, size_t n);
std::optional<std::string> bytesFromGlobal(HGLOBAL g);

// A single-interface COM object: IUnknown for interface I, deleted by its
// last Release.
template <class I>
class ComObject : public I {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *out = static_cast<I *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --_refs;
        if (!n)
            delete this;
        return n;
    }

protected:
    virtual ~ComObject() = default;

private:
    std::atomic<ULONG> _refs{1};
};

// ── Optional APIs, resolved at runtime ──────────────────────────────────────
// Linking these directly would make the exe refuse to start on Windows 10
// builds older than 1607/1703 (and some are missing from old Wine); every
// user has a fallback.
struct Api {
    BOOL(WINAPI *setProcessDpiAwarenessContext)(HANDLE)                      = nullptr;
    UINT(WINAPI *getDpiForWindow)(HWND)                                      = nullptr;
    UINT(WINAPI *getDpiForSystem)()                                          = nullptr;
    BOOL(WINAPI *adjustWindowRectExForDpi)(RECT *, DWORD, BOOL, DWORD, UINT) = nullptr;
    HRESULT(WINAPI *setProcessDpiAwareness)(int)                     = nullptr; // shcore, 8.1
    HRESULT(WINAPI *getDpiForMonitor)(HMONITOR, int, UINT *, UINT *) = nullptr; // shcore, 8.1
};
const Api &api();
UINT       dpiForWindow(HWND hwnd);

// ── Formats (win32_data.cpp) ────────────────────────────────────────────────
// One mapping between MIME types and clipboard formats, shared by the
// clipboard and OLE drag and drop so both speak the same dialect.
struct NativeData {
    UINT        cf = 0;
    std::string bytes; // exactly what goes into the HGLOBAL
};
// What setClipboard / a drag source offers for these items: several native
// formats per item where Windows apps expect them (uri-list → CF_HDROP too).
std::vector<NativeData>  encodeForOs(const std::vector<DataItem> &items);
// Offered formats → the de-duplicated MIME list requestClipboardMimes and
// DropEnter report (image/png is added when a DIB can be converted to it).
std::vector<std::string> mimesForFormats(const std::vector<UINT> &cfs);
// Reading `mime` from whatever the source offers, in two steps so the
// source (the clipboard) is held only while bytes are copied out:
// readFromOs fetches the best native format through `get` (one format's
// bytes, nullopt when absent), finishDecode converts them (CRLF, CF_HTML,
// CF_HDROP, DIB → PNG through WIC) with the source released.
struct OsData {
    UINT        cf = 0;
    std::string bytes;
};
std::optional<OsData>
readFromOs(std::string_view mime, const std::function<std::optional<std::string>(UINT)> &get);
std::optional<std::string> finishDecode(std::string_view mime, OsData raw);
// Whether finishDecode is slow for this: a DIB to PNG through WIC (tens to
// hundreds of ms for a large screenshot), done off the loop thread.
bool                       decodeIsSlow(std::string_view mime, const OsData &raw);
using core::isTextMime;
// CF_HTML ("HTML Format"): a header with byte offsets, then the markup.
std::string cfHtmlEncode(std::string_view fragment);
std::string cfHtmlDecode(std::string_view data); // the fragment
// CF_HDROP payload (DROPFILES + NUL-separated paths) ↔ text/uri-list.
std::string hdropToUriList(const void *dropfiles, size_t size);
std::string hdropFromUriList(std::string_view list); // empty: not all local files

// ── Images (win32_image.cpp) ────────────────────────────────────────────────
// The image of `sizes` best suited to a size×size slot: the smallest one at
// least that big, else the largest; then scaled (core::scaleImage) to fit.
Image fitImage(const std::vector<Image> &sizes, int size);
using core::scaleImage;
// HICON from premultiplied pixels (icons want straight alpha); null on failure.
HICON       iconFromImage(const Image &img);
// A top-down 32-bpp premultiplied DIB section, as SHDRAGIMAGE wants it.
HBITMAP     dibFromImage(const Image &img);
// PNG through WIC (part of Windows; no encoder of our own). Empty on failure.
std::string encodePng(const Image &img);
std::string bmpFileToPng(std::string_view bmp);
// The WIC factory is cached for the thread that calls bindWic (the loop
// thread, at App init); releaseWic drops it there, before COM goes.
void        bindWic();
void        releaseWic();
// The taskbar overlay: a red disc with the count ("9+" past nine) drawn by
// a tiny built-in bitmap font, since plat has no text rendering.
Image       badgeImage(int count, int size);
bool        writeFile(const std::wstring &path, std::string_view bytes);
// fitImage for a slot of `size` physical pixels at `scale` physical/logical:
// candidates of equal pixel size are tie-broken by the Image::scale closest
// to it (an icon drawn for this DPI beats one merely the same size).
Image       fitImage(const std::vector<Image> &sizes, int size, double scale);

// ── Monitors: the mixed-DPI coordinate rule (win32_system.cpp) ──────────────
// plat.h: each monitor's origin is its physical origin, sizes within it are
// logical, and a point converts through the monitor that contains it. Pure,
// so win32_tests can check the maths on made-up layouts.
struct MonitorGeom {
    RECT   phys{};      // rcMonitor, physical virtual-desktop pixels
    double scale = 1.0; // its effective DPI / 96
};
Rect     logicalBounds(const MonitorGeom &m);
Rect     logicalRect(const MonitorGeom &m, const RECT &physInside); // e.g. the work area
Point    physicalToLogical(POINT p, const std::vector<MonitorGeom> &mons);
POINT    logicalToPhysical(Point p, const std::vector<MonitorGeom> &mons);
uint64_t monitorId(std::wstring_view deviceName); // stable per device name, never 0
uint64_t monitorIdOf(HMONITOR m);
std::vector<MonitorGeom> monitorGeoms(); // the live layout
std::vector<Monitor>     enumMonitors(bool withRefresh);

// ── Single instance (win32_system.cpp) ──────────────────────────────────────
// Secondary → primary: u32 length of the rest, then "PLAT", u32 version,
// u32 count and count × (u32 length + UTF-8 bytes) — the working directory
// first, then the args. Little-endian. The primary answers "ACK1".
constexpr uint32_t kInstanceProtocol = 1;
constexpr size_t   kInstanceMaxBytes = 1u << 20; // a sane argv; anything bigger is refused
std::string encodeInstanceMessage(std::string_view cwd, const std::vector<std::string> &args);
// Total frame size once the length prefix is in `buf`: 0 = need more bytes,
// SIZE_MAX = over the limit.
size_t      instanceFrameSize(std::string_view buf);
bool        decodeInstanceMessage(
    std::string_view frame, std::string *cwd, std::vector<std::string> *args
);
// \\.\pipe\plat-<sid>-<session>-<key>, with the key made name-safe.
std::wstring instancePipeName(std::wstring_view sid, DWORD session, std::string_view key);
std::wstring urlSchemeCommand(std::wstring_view exe); // "exe" "%1", quoted
bool         validUrlScheme(std::string_view scheme);

// ── Paths and settings (win32_system.cpp) ───────────────────────────────────
// Paths leave plat with '/' separators (what makes
// `standardDir(Temp) + "/name"` equal a path a dialog hands back); Windows
// accepts them, but some shell parsers do not, so incoming paths are turned
// back into '\' before they reach the OS.
std::string  portablePath(std::wstring_view native);
std::wstring nativePath(std::string_view utf8);
uint32_t     accentFromAbgr(DWORD abgr); // DWM AccentColor → 0xAARRGGBB, opaque
double       textScaleFromPercent(DWORD percent);
int          caretBlinkFromOs(UINT ms);

class Win32App;
struct DecodeLink; // win32_data.cpp

// ── Window ──────────────────────────────────────────────────────────────────
class Win32Window final : public Window {
public:
    Win32Window(Win32App *app, const WindowDesc &d);
    ~Win32Window() override;

    void                 setTitle(std::string_view utf8) override;
    Size                 size() const override;
    double               scale() const override { return _dpi / 96.0; }
    void                 setSize(Size logical) override;
    void                 setMinSize(Size logical) override { _minSize = logical; }
    void                 show() override;
    void                 hide() override;
    void                 minimize() override;
    void                 setMaximized(bool on) override;
    void                 setFullscreen(bool on) override;
    bool                 isMaximized() const override { return IsZoomed(_hwnd) != 0; }
    bool                 isFullscreen() const override { return _fullscreen; }
    bool                 isMinimized() const override { return IsIconic(_hwnd) != 0; }
    bool                 supportsAlwaysOnTop() const override { return true; }
    void                 setAlwaysOnTop(bool on) override;
    bool                 isAlwaysOnTop() const override;
    bool                 isActive() const override { return _active; }
    void                 activate() override;
    std::optional<Point> position() const override;
    bool                 setPosition(Point logical) override;
    uint64_t             monitor() const override;
    void                 setCursor(Cursor c) override;
    void   setHitTest(std::function<HitArea(Point)> fn) override { _hitTest = std::move(fn); }
    bool   hasSystemDecorations() const override { return !_custom; }
    void   setTextInput(const TextInputState &s) override;
    void   requestFrame() override;
    Canvas beginPaint() override;
    void   endPaint(const std::vector<Rect> &damage) override;
    void  *nativeHandle() const override { return _hwnd; }
    void   setDropAction(DropAction a) override;
    void   requestAttention() override;

    HWND                hwnd() const { return _hwnd; }
    Win32App           *app() const { return _app; }
    // For the drop target and drag source (win32_dnd.cpp).
    bool                sendEvent(Event e) { return send(std::move(e)); }
    std::weak_ptr<char> aliveToken() const { return _alive; }
    Point               screenToLogical(POINT screen) const;
    // A drag started from a press: the OS drag loop owns the button now, so
    // forget it without a PointerUp (the drag ends with DragFinished).
    void                forgetButtons();
    // Frame pacing, driven by Win32App::service(): true when a requested
    // Frame may be delivered now; otherwise *waitMs (if ≥ 0) says when.
    bool                frameDue(core::Clock::time_point now, int *waitMs) const;
    bool                deliverFrame(); // false when the handler destroyed the window
    // The theme or a system setting changed (from Win32App::themeMaybeChanged).
    bool                themeChanged(bool darkChanged);

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    bool    send(Event e); // emits; false when the handler destroyed this window
    Point   toLogical(POINT phys) const { return {phys.x / scale(), phys.y / scale()}; }
    POINT   screenToClient(LPARAM lp) const;
    void    updateGeometry();
    void    adjustForFrame(RECT *r) const;
    void    ensureDib(int w, int h);
    void    blit(HDC dc, RECT r);
    void    onPaint();
    LRESULT onNcCalcSize(WPARAM wp, LPARAM lp);
    LRESULT onNcHitTest(LPARAM lp);
    void    onPointerMove(POINT clientPhys, bool nonClient);
    void    onPointerLeave();
    bool    onButton(Button b, bool down, POINT clientPhys);
    int     countClick(Button b);
    void    releaseAllButtons();
    LRESULT onKey(UINT msg, WPARAM wp, LPARAM lp);
    bool    isFakeAltGrCtrl(UINT msg) const;
    void    onChar(wchar_t c);
    void    onImeComposition(LPARAM lp);
    void    updateImePosition();
    void    updateSystemCaret(bool focused);
    void    onDropFiles(HDROP drop);
    void    applyBadge();
    void    applyDarkTitleBar(bool repaint);
    void    stateMaybeChanged();
    void    moveClientTo(POINT phys); // client top-left to this physical screen point
    void    positionMaybeChanged();

    Win32App             *_app;
    HWND                  _hwnd = nullptr;
    bool                  _custom;
    bool                  _resizable;
    bool                  _created = false; // no events until the constructor is done
    std::shared_ptr<char> _alive   = std::make_shared<char>(); // expires in the destructor

    UINT            _dpi   = 96;
    int             _physW = 0, _physH = 0; // client size, physical
    Size            _lastSize{};            // last logical size / scale announced by Resized
    double          _lastScale = 0;
    Size            _minSize;
    bool            _active = false, _fullscreen = false, _pendingInitialShow = false;
    int             _lastShowState = -1; // 0 normal, 1 maximized, 2 minimized, for StateChanged
    DWORD           _savedStyle    = 0;
    WINDOWPLACEMENT _savedPlacement{sizeof(WINDOWPLACEMENT)};
    POINT           _lastPos{INT_MIN, INT_MIN}; // client origin (physical) last announced

    // Present: a top-down 32-bit BGRA DIB section is the Canvas, blitted by GDI.
    HDC       _memDC = nullptr;
    HBITMAP   _dib = nullptr, _oldBitmap = nullptr;
    uint32_t *_bits = nullptr;
    int       _dibW = 0, _dibH = 0; // the canvas: the client size last painted
    int       _capW = 0, _capH = 0; // the DIB itself, at least as big
    bool      _everPainted = false, _inSizeMove = false;
    bool      _blittedAll     = false; // endPaint blitted the whole canvas (for onPaint)
    int       _modalRefs      = 0; // OS modal loops this window entered (enterModal calls to undo)
    bool      _frameRequested = true;
    core::Clock::time_point _lastFrame{};

    // Pointer.
    std::function<HitArea(Point)> _hitTest;
    bool               _pointerInside = false, _trackingClient = false, _trackingNonClient = false;
    POINT              _lastMove{INT_MIN, INT_MIN};
    uint32_t           _buttons      = 0;     // bit per Button held (captured)
    bool               _captionPress = false; // left press on HTCAPTION, not yet a drag
    POINT              _captionPressAt{};
    core::ClickCounter _clicks;
    Cursor             _cursor       = Cursor::Arrow;
    HCURSOR            _cursorHandle = nullptr;

    // Keyboard / IME.
    TextInputState _textInput;
    wchar_t        _highSurrogate = 0;
    bool           _preeditActive = false, _hasCaret = false;

    // OLE drop target (null when OLE is unavailable; WM_DROPFILES then).
    struct DropTarget *_dropTarget = nullptr;
};

// ── OLE drag and drop (win32_dnd.cpp) ───────────────────────────────────────
DropTarget *registerDropTarget(Win32Window *w, IDropTargetHelper *helper);
void        revokeDropTarget(DropTarget *t);
void        setDropReply(DropTarget *t, DropAction a);

// ── Shell integration state (win32_shell.cpp) ───────────────────────────────
struct Shell;
struct ShellDeleter {
    void operator()(Shell *s) const;
};

// ── App ─────────────────────────────────────────────────────────────────────
// Posted to the message window to run the next queued file dialog: outside
// runPosted()/runDueTimers(), so the dialog's modal loop never holds up the
// rest of a batch of posted closures or due timers.
constexpr UINT kDialogMsg = WM_APP + 2;

class Win32App final : public BackendApp
#ifdef PLAT_TEST_HOOKS
    ,
                       public TestHooks
#endif
{
public:
    Win32App() = default;
    ~Win32App() override;
    bool init(std::string *error);

    const char             *backendName() const override { return "win32"; }
    std::unique_ptr<Window> createWindow(const WindowDesc &d) override;
    void                    run() override;
    void                    quit() override;
    void                    pump(int timeoutMs) override;
    void                    post(std::function<void()> fn) override { _core.post(std::move(fn)); }
    TimerId                 addTimer(int ms, bool repeat, std::function<void()> fn) override {
        const TimerId id = _core.addTimer(ms, repeat, std::move(fn));
        if (_modalDepth) // added from a window proc inside an OS modal loop
            armModal();
        return id;
    }
    void     cancelTimer(TimerId id) override { _core.cancelTimer(id); }
    // Win32 has no fds: sockets, pipes and events are HANDLEs. The seam for a
    // socket layer here is a future `watchHandle(HANDLE, fn)` — up to 63
    // handles fit in the MsgWaitForMultipleObjectsEx array this loop already
    // blocks in (WSAEventSelect turns a socket into one); beyond that,
    // RegisterWaitForSingleObject on the thread pool + post().
    uint64_t watchFd(int, uint32_t, std::function<void(uint32_t)>) override { return 0; }
    void     unwatchFd(uint64_t) override {}

    void setClipboard(std::vector<DataItem> items, Selection sel) override;
    void requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override;
    void
    requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel) override;
    bool startDrag(Window &source, const DragDesc &drag) override;

    std::unique_ptr<Tray> createTray() override;
    bool                  notificationsAvailable() const override;
    bool                  notify(uint64_t id, const Notification &n) override;
    void                  setBadgeCount(int count) override;

    bool darkMode() const override;
    int  doubleClickMs() const override { return int(GetDoubleClickTime()); }
    bool openUrl(std::string_view url) override;
#ifdef PLAT_TEST_HOOKS
    TestHooks *testHooks() override { return this; }
#endif

    std::vector<Monitor> monitors() const override;
    bool claimSingleInstance(std::string_view key, const std::vector<std::string> &args) override;
    bool registerUrlScheme(std::string_view scheme) override;
    std::optional<bool> networkOnline() const override;
    void                showFileDialog(
        const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
    ) override;
    void
    showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb) override;
    std::string              standardDir(StandardDir d) const override;
    SystemSettings           systemSettings() const override;
    std::vector<std::string> preferredLanguages() const override;

#ifdef PLAT_TEST_HOOKS
    // ── TestHooks ───────────────────────────────────────────────────────────
    bool injectKey(Window &w, Key k, bool down) override;
    bool injectPointerMove(Window &w, Point logical) override;
    bool injectButton(Window &w, Button b, bool down) override;
    bool injectScroll(Window &w, double dx, double dy) override;
    bool readPixel(Window &w, int x, int y, uint32_t *argb) override;
    // Through the notify-icon callback message / WM_COMMAND the shell and
    // the menu use; toasts through the Activated handler's own path.
    bool trayActivate(Tray &t) override;
    bool trayMenuSelect(Tray &t, uint32_t itemId) override;
    bool notificationInvoke(uint64_t id, std::string_view action) override;
    // Neither the tray host, the toast server, the overlay icon nor the
    // flash state can be read back through a public API: the defaults
    // (false / -1) stand, and the selftest skips those checks. A URL only
    // ever arrives through a new process (deliverUrl): false.
    bool fileDialogRespond(std::vector<std::string> paths) override;
    // Sends WM_POWERBROADCAST to the window the OS sends it to. A network
    // change cannot be faked honestly: false.
    bool simulateSystemEvent(EventType type, bool online) override;
#endif

    // ── backend-internal ──
    using BackendApp::emit;
    HINSTANCE instance() const { return _instance; }
    int       frameIntervalMs() const { return _frameIntervalMs; }
    // Lines (or, horizontal, characters) per wheel notch, cached until the
    // next WM_SETTINGCHANGE (settingsChanged()).
    UINT      wheelScrollAmount(bool horizontal);
    void      settingsChanged() { _wheelKnown = 0; }
    void      addWindow(Win32Window *w) { _windows.push_back(w); }
    void      removeWindow(Win32Window *w);
    void      wake() { _core.wake(); }
    // Posted work, due timers, due frames. Runs after every pump and from the
    // hidden window's messages, so it keeps going inside modal loops.
    void      service();
    // OS modal loops (live move/resize, menus) run their own message pump;
    // a SetTimer armed for the next due timer or frame keeps them alive
    // meanwhile (posted work arrives as kWakeMsg anyway).
    void      enterModal();
    void      leaveModal();
    // The modal timer for what is due next, or none: no ticks while idle.
    void      armModal();
#ifdef PLAT_TEST_HOOKS
    // For win32_tests: the modal timer's period now (-1: not armed).
    int modalTimerMs() const { return _modalTimerMs; }
#endif

    bool   oleReady() const { return _oleInit; }
    Shell &shell(); // created on first use
    // AppInfo::startMenuShortcut (win32_shell.cpp).
    void   ensureStartMenuShortcut();
    // Taskbar overlay for one window (on TaskbarButtonCreated and on change).
    void   applyBadge(HWND h);
    UINT   taskbarButtonCreatedMsg() const { return _taskbarButtonCreated; }
#ifdef PLAT_TEST_HOOKS
    // For win32_tests: force the Shell_NotifyIcon balloon path, as on Wine.
    void forceBalloonNotifications(bool on);
    // For win32_tests: how many times each tray was (re-)added to the shell.
    int  trayAddCount(Tray &t) const;
    // For win32_tests: deliver a balloon's notify-icon callback (NIN_BALLOON*)
    // as the shell would; false for an id that has no balloon.
    bool postBalloonCallback(uint64_t id, UINT event);
#endif

    // finishDecode(mime, raw) on a worker thread; `done` gets the result
    // later, on the loop thread, and is never called once the App is gone.
    using DecodeDone = std::function<void(std::optional<std::string>)>;
    void decodeOffThread(std::string mime, OsData raw, DecodeDone done);
    void decodeFinished(uint64_t id, std::optional<std::string> result);
    void detachDecoders(); // workers still running then report to nobody

    // Waitable handles in the loop's MsgWaitForMultipleObjectsEx (at most
    // 60). fn runs on the loop thread whenever h is signalled, inside OS
    // modal loops too, and must reset it: manual-reset events stay signalled.
    uint64_t watchHandle(HANDLE h, std::function<void()> fn);
    void     unwatchHandle(uint64_t id);
    // Re-read the monitors / theme + settings and emit MonitorsChanged /
    // ThemeChanged only when something really changed: one broadcast reaches
    // every top-level window, and each of them forwards it here.
    void     monitorsMaybeChanged();
    void     themeMaybeChanged();
    HWND     systemWindow() const { return _sysHwnd; }
    // The secondary's half of claimSingleInstance: connect, check the pipe
    // belongs to this user, send, wait for the ack. For win32_tests too.
    static bool
    forwardToPrimary(const std::wstring &pipe, const std::string &frame, DWORD timeoutMs);

    static constexpr const wchar_t *kWindowClass = L"plat.window";

private:
    static LRESULT CALLBACK msgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK sysProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT                 onSystemMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void                    flushFrames();
    int                     msUntilFrame() const;
    void                    wait(int timeoutMs);
    void                    pollHandles();
    bool                    initSystem();
    void                    teardownSystem();
    void                    emitPower(EventType t);
    void                    ensureNetwork() const;
    void                    runNextDialog();
    FileDialogResult        runFileDialog(const FileDialogDesc &d, HWND owner);

    core::LoopCore     _core;
    HINSTANCE          _instance = nullptr;
    HWND               _msgHwnd  = nullptr; // HWND_MESSAGE: wake-ups, modal timer, clipboard owner
    HANDLE             _hiresTimer = nullptr;
    std::atomic<bool>  _wakePending{false};
    bool               _quit = false, _comInit = false, _oleInit = false;
    UINT               _taskbarButtonCreated = 0;
    IDropTargetHelper *_dropHelper           = nullptr; // shell drag images over our windows
    std::unique_ptr<Shell, ShellDeleter> _shell;
    int                                  _modalDepth      = 0;
    int                                  _modalTimerMs    = -1; // armed period, -1 = killed
    int                                  _frameIntervalMs = 16;
    std::vector<Win32Window *>           _windows;

    struct HandleWatch {
        uint64_t              id;
        HANDLE                h;
        std::function<void()> fn;
    };
    std::vector<HandleWatch>   _handleWatches;
    uint64_t                   _nextHandleWatch = 1;
    // service()'s per-pass snapshots, kept to save an allocation each pass.
    std::vector<uint64_t>      _scratchIds;
    std::vector<Win32Window *> _scratchWindows;

    // decodeOffThread: the callbacks stay here (destroyed on this thread);
    // workers reach the App through the link, cleared when it goes.
    struct PendingDecode {
        uint64_t   id;
        DecodeDone done;
    };
    std::vector<PendingDecode>  _decodes;
    uint64_t                    _nextDecode = 1;
    std::shared_ptr<DecodeLink> _decodeLink;

    // Round 3 desktop state (win32_system.cpp, win32_dialog.cpp).
    HWND                 _sysHwnd       = nullptr; // hidden top-level: broadcasts, power
    void                *_powerNotify   = nullptr; // HPOWERNOTIFY
    EventType            _lastPower     = EventType::None;
    DWORD                _lastPowerTick = 0;
    std::vector<Monitor> _monitorSnapshot;
    bool                 _monitorsKnown = false;
    bool                 _themeDark     = false;
    SystemSettings       _themeSettings;
    struct Instance;
    std::vector<Instance *>  _instances; // owned; freed in teardownSystem()
    std::vector<std::string> _schemes;   // lower case, registered by this process
    UINT                     _wheelLines = 3, _wheelChars = 3;
    uint8_t                  _wheelKnown = 0; // bit 0: lines, bit 1: chars
    struct Network;
    mutable Network *_network = nullptr; // COM sink; created on first use
    struct DialogRequest {
        FileDialogDesc                        desc;
        HWND                                  owner = nullptr;
        std::function<void(FileDialogResult)> cb;
    };
    std::vector<DialogRequest> _dialogQueue;
    bool                       _dialogActive = false;
#ifdef PLAT_TEST_HOOKS
    std::optional<std::vector<std::string>> _dialogAnswer; // fileDialogRespond, for the next dialog
#endif

    friend class Win32Window;
};

} // namespace plat::win32
