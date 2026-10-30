// plat — msga's thin platform layer.
//
// Owns exactly what the OS must provide: the event loop, top-level windows,
// input (keyboard/pointer/scroll/IME), clipboard, cursors, DPI, and a way to
// get pixels on screen. Everything above that line (widgets, layout, text
// shaping, painting) is the toolkit's business and never sees Win32, Cocoa,
// Wayland or X11. See docs/platform-layer-plan.md for the design record.
//
// Threading: every function here must be called on the thread that called
// App::create(), except App::post(), which is safe from any thread.
// Lifetime: destroy every Window before its App.
//
// Coordinates: all positions and sizes in events and window APIs are in
// logical pixels (double where sub-pixel input exists). Multiply by
// Window::scale() for physical pixels; Canvas is always physical.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plat {

struct Point {
    double x = 0, y = 0;
};
struct Size {
    int w = 0, h = 0;
};
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    bool operator==(const Rect &) const = default;
};

class Window;
class Tray;

// A MIME-typed blob: clipboard contents, drag payloads, drop contents.
// Standard types every backend maps to its native formats:
//   text/plain;charset=utf-8   text/html   text/uri-list   image/png
// Anything else round-trips under its own name where the OS allows it.
struct DataItem {
    std::string mime;
    std::string data;
};

// A CPU image handed to the OS (tray icons, notification pictures, drag
// images): premultiplied ARGB32 like Canvas, tightly packed (stride = width).
struct Image {
    int                   width = 0, height = 0;
    std::vector<uint32_t> pixels;
    // Physical pixels per logical pixel the image was drawn for (2 = a HiDPI
    // drag image or icon). Backends show it at width/scale logical pixels.
    double                scale = 1.0;
    bool                  empty() const { return width <= 0 || height <= 0; }
};

enum class Selection : uint8_t {
    Clipboard, // the Ctrl/Cmd+C clipboard
    Primary,   // X11/Wayland select-to-copy, middle-click paste; unsupported elsewhere
};

enum class ScrollPhase : uint8_t {
    None,     // no phase information (plain wheels; Windows and X11 touchpads)
    Begin,    // fingers touched down and started scrolling
    Update,   // fingers moving
    End,      // fingers lifted (no delta)
    Momentum, // kinetic coasting after the fingers lifted
};
// Sequence: Begin, Update…, End, then optionally Momentum…, End (macOS only;
// Wayland and X11 leave kinetic scrolling to the app and never send Momentum).
// Continuous sources without a lift signal (trackpoints) may send precise
// events with phase None.

enum class Gesture : uint8_t { None, Swipe };

enum class DropAction : uint8_t { None, Copy, Move, Link };
enum DropActions : uint32_t { ActCopy = 1, ActMove = 2, ActLink = 4 };

// ── Keys ────────────────────────────────────────────────────────────────────
// A Key is the *logical* key, resolved through the active layout, so
// Ctrl+Z means "the key that types z". On a non-Latin layout (Russian,
// Greek, …) letter keys resolve through the first Latin layout the user has
// configured, falling back to the US position — the same rule GTK uses,
// so shortcuts keep working. `scancode` carries the raw physical code.
enum class Key : uint16_t {
    Unknown = 0,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    Escape,
    Enter,
    Tab,
    Backspace,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,
    Left,
    Right,
    Up,
    Down,
    Space,
    Minus,
    Equal,
    BracketLeft,
    BracketRight,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,
    CapsLock,
    ShiftLeft,
    ShiftRight,
    ControlLeft,
    ControlRight,
    AltLeft,
    AltRight,
    SuperLeft,
    SuperRight,
    Menu,
    PrintScreen,
    ScrollLock,
    Pause,
    NumLock,
    Kp0,
    Kp1,
    Kp2,
    Kp3,
    Kp4,
    Kp5,
    Kp6,
    Kp7,
    Kp8,
    Kp9,
    KpDecimal,
    KpDivide,
    KpMultiply,
    KpSubtract,
    KpAdd,
    KpEnter,
    KpEqual,
    // Dedicated navigation keys (XF86Back/XF86Forward, VK_BROWSER_BACK/FORWARD);
    // Macs have none.
    Back,
    Forward,
    Count
};

const char *keyName(Key k);

enum Mod : uint32_t {
    ModShift = 1u << 0,
    ModCtrl  = 1u << 1,
    ModAlt   = 1u << 2, // Option on macOS
    ModSuper = 1u << 3, // Command on macOS, Windows key elsewhere
    ModCaps  = 1u << 4,
    ModNum   = 1u << 5,
};

// The "primary" shortcut modifier: Command on macOS, Ctrl elsewhere.
uint32_t primaryMod();

enum class Button : uint8_t { Left, Right, Middle, Back, Forward };

enum class Cursor : uint8_t {
    Arrow,
    IBeam,
    Hand,
    Wait,
    Progress,
    Crosshair,
    NotAllowed,
    Move,
    Grab,
    Grabbing,
    ResizeH,
    ResizeV,
    ResizeNWSE,
    ResizeNESW,
    Hidden,
};

// What sits under a point of a window drawn with Decorations::Custom. The
// backend asks the window's hit-test callback on every press (and, on Win32,
// on WM_NCHITTEST) and turns Caption/Resize* into the OS's own interactive
// move/resize, so snapping, tiling and Aero Snap keep working.
enum class HitArea : uint8_t {
    Client,
    Caption,
    ResizeTop,
    ResizeBottom,
    ResizeLeft,
    ResizeRight,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight,
    // Title-bar buttons the app draws itself. Presses on them are delivered
    // to the app as ordinary PointerDown/Up (the app acts on them); the
    // backend only uses the classification where the OS wants it — Win32
    // reports HTMAXBUTTON so Windows 11 shows Snap Layouts on hover.
    MinimizeButton,
    MaximizeButton,
    CloseButton,
};

// True for the areas a press turns into an OS move/resize (never delivered).
inline bool isNonClient(HitArea a) {
    return a != HitArea::Client && a != HitArea::MinimizeButton && a != HitArea::MaximizeButton &&
           a != HitArea::CloseButton;
}

// ── Events ──────────────────────────────────────────────────────────────────
enum class EventType : uint8_t {
    None,
    CloseRequested, // user asked to close; nothing happens unless the app destroys the window
    Resized,        // logical size and/or scale changed; a Frame follows
    Frame,          // paint now (after requestFrame(), exposure, resize)
    FocusIn,
    FocusOut,
    StateChanged, // maximised / minimised / fullscreen / active / hasSystemDecorations() changed
    PointerEnter,
    PointerLeave,
    PointerMove,
    PointerDown,
    PointerUp,
    Scroll,
    KeyDown, // also for auto-repeat (repeat == true)
    KeyUp,
    TextInput,   // committed text (typed characters, IME commit, dead-key result);
                 // only guaranteed while setTextInput({.enabled = true}) is in effect
    TextPreedit, // IME composition in progress; empty text = composition ended
    DropEnter,   // a drag entered the window: items lists the offered MIME types (no data
                 // yet); reply with Window::setDropAction() (default Copy)
    DropMove,    // drag moved: pos updated; reply again if acceptance depends on position
    DropLeave,
    Drop,         // dropped: items carry data for the standard types, uris/text are filled
    ThemeChanged, // OS dark/light or accent changed (sent to every window) — re-query
                  // App::darkMode()

    // Multi-finger touchpad swipes (3+ fingers; Wayland/X11). dx/dy are finger
    // motion since the last event in logical px (+x = fingers moved right).
    // Two-finger scrolling is Scroll with phases, not this.
    GestureBegin,
    GestureUpdate,
    GestureEnd, // cancelled = true when the compositor took the gesture over
    // macOS's one-shot three-finger navigation swipe (NSEvent swipeWithEvent):
    // dx = +1 means "back" (content dragged right), -1 forward; dy likewise.
    SwipeGesture,

    DragFinished, // a startDrag() session ended; dropAction = what the target did (None =
                  // cancelled)
    // Drops onto the source window itself are delivered like any other drop.

    // App-level events: window == nullptr.
    TrayActivated, // primary click on the tray icon (tray set)
    TrayMenuItem,  // tray menu item chosen (tray set, id = MenuItem::id)
    // id = notify() id; action = "" for the body, else the action key. A clicked
    // notification is gone without a NotificationClosed. OSes with a history
    // (Plasma, Action Centre) may deliver Activated after Closed.
    NotificationActivated,
    NotificationClosed, // dismissed or expired (id)
    NotificationFailed, // the OS refused it after notify() returned (id, text = reason)

    MonitorsChanged, // monitor added/removed/rearranged/rescaled — re-query App::monitors()
    Moved,           // window position changed (where the OS reports positions)

    // Another launch of the app called claimSingleInstance() with our key and
    // was turned away: strings = its argv (without argv[0]), text = its working
    // directory, activationToken = its XDG_ACTIVATION_TOKEN / startup id.
    InstanceActivated,
    // The OS asked us to open URLs/files: a registered scheme (msga://…),
    // macOS Apple Events, or argv forwarded by a second instance that look like
    // URLs of a registered scheme. strings = the URLs.
    OpenUrls,
    // The user asked the whole app to quit, not one window to close: macOS
    // Cmd+Q, the Dock's and the app menu's Quit, logout. Nothing happens
    // unless the app quits (App::quit()).
    QuitRequested,

    NetworkChanged, // online = the new state; re-check App::networkOnline()
    Suspending,     // the system is about to sleep (best effort, may not arrive)
    Resumed,        // woke from sleep: sockets are probably dead, reconnect
};

struct Event {
    EventType type   = EventType::None;
    Window   *window = nullptr;

    Point       pos; // pointer / drop position, logical
    Button      button = Button::Left;
    int         clicks = 0;      // PointerDown: 1 single, 2 double, … (OS double-click time)
    double      dx = 0, dy = 0;  // Scroll: logical pixels when precise, else notches (+y = down)
    bool        precise = false; // Scroll from a touchpad/high-res wheel; one wheel line = 100/3 px
    Key         key     = Key::Unknown;
    uint32_t    scancode = 0; // Linux: evdev code; Windows: scan code; macOS: virtual keycode
    uint32_t    mods     = 0;
    bool        repeat   = false;
    std::string text; // TextInput / TextPreedit / Drop(text) — UTF-8
    int         preeditCursorBegin = -1, preeditCursorEnd = -1; // byte offsets, -1 = hidden
    std::vector<std::string> uris; // Drop: file:// URIs (local files) or other URIs

    ScrollPhase phase     = ScrollPhase::None; // Scroll
    Gesture     gesture   = Gesture::None;     // Gesture*
    int         fingers   = 0;
    bool        cancelled = false;

    // DropEnter/Move/Drop: the action the source prefers (Copy when allowed) —
    // not what the target answered; DragFinished: what the target did.
    DropAction            dropAction     = DropAction::None;
    uint32_t              allowedActions = 0; // DropEnter/Move/Drop: DropActions the source permits
    std::vector<DataItem> items; // DropEnter/Move: types only; Drop: the standard types, with data

    Tray       *tray = nullptr; // Tray*
    uint64_t    id   = 0;       // TrayMenuItem / Notification*
    std::string action;         // NotificationActivated

    std::vector<std::string> strings;        // InstanceActivated argv / OpenUrls URLs
    bool                     online = false; // NetworkChanged
    // TrayActivated / TrayMenuItem / NotificationActivated / InstanceActivated: a token the
    // OS handed over with the click or launch (Wayland xdg-activation). Pass
    // it to Window::activateWithToken() so the compositor lets us raise the window.
    // X11: a startup-notification id (DESKTOP_STARTUP_ID).
    std::string              activationToken;
};

// ── Pixels ──────────────────────────────────────────────────────────────────
// A CPU framebuffer for one frame, physical pixels, premultiplied ARGB32 in
// native endianness (0xAARRGGBB as a uint32_t) — the format of wl_shm ARGB8888, X11 depth-32
// visuals, BGRA DIBs and kCGImageAlphaPremultipliedFirst|ByteOrder32Little. Keep pixels opaque in
// Decorations::Custom windows: on Win32 the top row sits over the DWM frame and translucent alpha
// shows it through.
struct Canvas {
    uint32_t *pixels = nullptr;
    int       width = 0, height = 0; // physical
    int       stride = 0;            // in pixels, not bytes
    double    scale  = 1.0;
};

enum class Decorations : uint8_t {
    System, // OS title bar and borders (server-side where the compositor offers it)
    Custom, // app draws its own title bar; supply a hit-test callback
};

struct WindowDesc {
    std::string          title = "plat";
    std::string          appId = "plat"; // Wayland app_id / X11 WM_CLASS / Win32 AppUserModelID
    // X11 WM_CLASS res_class (res_name is appId); empty = appId. Toolkits
    // often set it to the application name, so a .desktop StartupWMClass may
    // name either.
    std::string          wmClass;
    // The window's icon in several sizes (square, premultiplied): X11
    // _NET_WM_ICON, which shells show when no .desktop entry matches the
    // window. Wayland shells take the icon from the .desktop entry named by
    // appId; Windows and macOS from the executable / bundle.
    std::vector<Image>   icon;
    Size                 size{800, 600}; // logical
    Size                 minSize{0, 0};
    Decorations          decorations = Decorations::System;
    bool                 resizable   = true;
    bool                 visible     = true;
    // Initial position, logical virtual-desktop coordinates. Ignored where
    // the OS places windows itself (Wayland); nullopt = OS default placement.
    std::optional<Point> position;
};

struct TextInputState {
    bool enabled = false;
    Rect caret; // logical, relative to the window — where the IME candidate box goes
};

class Window {
public:
    virtual ~Window() = default;

    virtual void   setTitle(std::string_view utf8) = 0;
    virtual Size   size() const                    = 0; // logical
    virtual double scale() const                   = 0; // physical / logical, may be fractional
    virtual void   setSize(Size logical)           = 0;
    virtual void   setMinSize(Size logical)        = 0;
    virtual void   show()                          = 0;
    virtual void   hide()                          = 0;
    virtual void   minimize()                      = 0;
    virtual void   setMaximized(bool on)           = 0;
    virtual void   setFullscreen(bool on)          = 0;
    virtual bool   isMaximized() const             = 0;
    virtual bool   isFullscreen() const            = 0;
    // Minimised, as far as the OS tells a client (X11's hidden state,
    // IsIconic, miniaturized); StateChanged follows a change. Always false
    // on Wayland, which doesn't say.
    virtual bool   isMinimized() const { return false; }
    virtual bool   isActive() const = 0;
    virtual void   activate()       = 0; // raise + focus where the OS allows it
    // Same, with a token from an event (Wayland needs one to raise a window
    // on user request; elsewhere it is ignored).
    virtual void   activateWithToken(std::string_view token) { activate(); }

    // Keep the window above other windows ("Pin window on top"): the X11
    // _NET_WM_STATE_ABOVE state, HWND_TOPMOST, NSFloatingWindowLevel.
    // Wayland has no such request for clients: unsupported there, and
    // setAlwaysOnTop does nothing. StateChanged follows a change the window
    // manager makes on its own (X11).
    virtual bool supportsAlwaysOnTop() const { return false; }
    virtual void setAlwaysOnTop(bool on) { (void)on; }
    virtual bool isAlwaysOnTop() const { return false; }

    // Top-left of the window's content in logical virtual-desktop coords;
    // nullopt where the OS does not tell clients (Wayland). setPosition
    // returns false where it is not allowed.
    // setPosition also returns false while maximised/minimised/fullscreen; a
    // move onto a monitor of another scale may shift it by a few pixels —
    // Moved reports where it ended up.
    virtual std::optional<Point> position() const { return std::nullopt; }
    virtual bool                 setPosition(Point logical) { return false; }
    // Id of the monitor the window is (mostly) on — see App::monitors(); 0 if unknown.
    virtual uint64_t             monitor() const { return 0; }

    // Decorations::Custom: the parts the OS still draws (macOS: the traffic
    // lights and the title bar's rim over the content) in their dark or
    // light look, to suit the app's theme rather than the system's. The
    // other backends draw none of it: nothing to do there.
    virtual void   setDarkChrome(bool dark) { (void)dark; }
    // Decorations::Custom: the height of the native title bar the content
    // extends under (macOS: the band the traffic lights sit in, logical), so
    // the app's own header can make room for it; 0 where the app draws the
    // whole title bar.
    virtual double titleBarHeight() const { return 0; }

    virtual void setCursor(Cursor c)                          = 0;
    virtual void setHitTest(std::function<HitArea(Point)> fn) = 0;
    // Only meaningful for Decorations::System on Wayland: false when the
    // compositor refused server-side decorations (GNOME), in which case the
    // app has to draw its own — the backend never draws any.
    virtual bool hasSystemDecorations() const                 = 0;

    virtual void setTextInput(const TextInputState &s) = 0;

    // Painting: ask for a Frame event, then inside the Frame handler call
    // beginPaint(), draw, and endPaint() with the damaged rects (physical;
    // empty = whole canvas). The canvas holds the last presented frame, so
    // repainting only the damaged rects is valid. Outside a Frame handler beginPaint() still works
    // but may block on the compositor.
    virtual void   requestFrame()                            = 0;
    virtual Canvas beginPaint()                              = 0;
    virtual void   endPaint(const std::vector<Rect> &damage) = 0;

    // Answer the current DropEnter/DropMove: None rejects the drop at this
    // position (the OS shows a "no" cursor and no Drop follows). Default: the
    // event's preferred dropAction. The answer sticks until changed.
    virtual void setDropAction(DropAction a) {}

    // Flash the taskbar entry / bounce the Dock icon / set demands-attention,
    // until the window is activated. No-op if it is already active.
    virtual void requestAttention() {}

    // HWND / NSWindow* / wl_surface* / xcb_window_t (cast), for GPU paths
    // and OS APIs the layer does not wrap.
    virtual void *nativeHandle() const = 0;

    void *userData = nullptr;
};

// ── Tray ────────────────────────────────────────────────────────────────────
struct MenuItem {
    enum class Kind : uint8_t { Action, Checkbox, Separator, Submenu };
    Kind                  kind = Kind::Action;
    uint32_t              id   = 0; // echoed in TrayMenuItem; unique within the menu
    std::string           label;    // UTF-8, sentence case, no mnemonics
    bool                  enabled = true;
    bool                  checked = false; // Checkbox
    std::vector<MenuItem> children;        // Submenu
};

// A status-area icon: StatusNotifierItem on Linux (KDE, GNOME with the
// AppIndicator extension, most other desktops), Shell_NotifyIcon on Windows,
// NSStatusItem on macOS. Primary click -> TrayActivated, except on macOS when
// a menu is set: there a click opens the menu, as every status item does.
// Secondary click opens the menu everywhere; the OS draws it.
class Tray {
public:
    virtual ~Tray()                                       = default;
    // Several sizes of the same icon; the backend picks (and scales) the best
    // fit for the host. macOS status icons are 18 pt: supply 18 and 36 px.
    virtual void setIcon(const std::vector<Image> &sizes) = 0;
    // macOS: the next setIcon is a template image — only its alpha counts and
    // the menu bar tints it to suit a light or dark bar. Ignored elsewhere.
    virtual void setTemplate(bool on) { (void)on; }
    virtual void setTooltip(std::string_view utf8)    = 0;
    virtual void setMenu(std::vector<MenuItem> items) = 0;
    // False while no host shows it (Linux: no StatusNotifierWatcher right now;
    // the backend re-registers by itself when one appears).
    virtual bool isVisible() const                    = 0;

    void *userData = nullptr;
};

// ── Notifications ───────────────────────────────────────────────────────────
struct NotificationAction {
    std::string key;   // echoed as Event::action
    std::string label; // button text
};

struct Notification {
    std::string                     title, body;    // UTF-8, plain text
    Image                           image;          // sender avatar etc.; empty = app icon only
    std::vector<NotificationAction> actions;        // buttons, where the OS renders them
    int                             timeoutMs = -1; // -1 = OS default
    bool                            silent    = false;
};

// Who we are to the OS: notification sender, taskbar grouping, launcher
// badges. Set before the first notify()/createTray().
struct AppInfo {
    std::string name = "plat"; // human-readable
    // Reverse-DNS; also the Linux .desktop id, the Windows AUMID, and it must
    // match the macOS bundle id (notifications need a signed .app there).
    std::string id   = "plat";
    Image       icon; // where the OS takes pixels (Windows toast registration)
    // Windows: keep a Start-menu shortcut "<name>.lnk" to this exe carrying
    // the AUMID (created shortly after start when missing). Builds before
    // 1903 get one for toasts either way.
    bool        startMenuShortcut = false;
};

struct DragDesc {
    std::vector<DataItem> items; // what the target can receive
    // Drag image, shown at width/scale logical pixels (Image::scale). Empty =
    // OS default.
    Image                 image;
    Point                 hotspot; // pointer position within the image, logical
    uint32_t              actions = ActCopy;
};

using TimerId = uint64_t;

// ── Monitors ────────────────────────────────────────────────────────────────
// Virtual-desktop coordinates: one logical space on macOS (points), Wayland
// (xdg-output logical layout) and X11 (one scale). Windows with mixed-DPI
// monitors has no single logical space, so there each monitor's
// origin is its physical origin and sizes within it are logical; a point is
// converted using the monitor that contains it.
struct Monitor {
    uint64_t    id = 0;   // stable while the monitor stays connected
    std::string name;     // connector or display name, for diagnostics
    Rect        bounds;   // logical, virtual-desktop coordinates
    Rect        workArea; // bounds minus panels/taskbar/dock, logical (== bounds on Wayland)
    double      scale          = 1.0; // physical / logical
    int         refreshMilliHz = 0;   // 0 = unknown
    bool        primary = false; // exactly one; Wayland has none: the output at 0,0, else the first

    bool operator==(const Monitor &) const = default;
};

// ── File dialogs ────────────────────────────────────────────────────────────
struct FileFilter {
    std::string              name;     // "Images"
    std::vector<std::string> patterns; // {"*.png", "*.jpg"}
};

struct FileDialogDesc {
    enum class Mode : uint8_t { Open, OpenMultiple, Save, PickFolder };
    Mode                    mode = Mode::Open;
    std::string             title;
    std::string             initialDir;    // absolute path; empty = OS default
    std::string             suggestedName; // Save only
    std::vector<FileFilter> filters;
    // Modal to this window where the OS supports it (macOS: a sheet). macOS
    // panels have no filter chooser: all filters are combined into one.
    Window                 *parent = nullptr;
};

// What showFileDialogEx() answers: the paths (absolute, local), and why
// there are none. Unavailable = no native dialog could be shown here (no
// FileChooser portal, a portal whose backend failed, a dialog that could not
// be created); on Linux the caller shows its own chooser instead.
struct FileDialogResult {
    enum class Status : uint8_t { Chosen, Cancelled, Unavailable };
    Status                   status = Status::Cancelled;
    std::vector<std::string> paths; // non-empty exactly when Chosen
};

enum class StandardDir : uint8_t {
    Config,
    Data,
    Cache,
    State,
    Temp,
    Home,
    Desktop,
    Documents,
    Downloads,
    Pictures,
};

// Appearance and accessibility settings a toolkit should honour. Changes
// arrive as ThemeChanged.
struct SystemSettings {
    bool     reducedMotion = false;
    bool     highContrast  = false;
    double   textScale = 1.0; // user's font-size preference on top of DPI scale (always 1 on macOS)
    uint32_t accentColor  = 0;   // 0xAARRGGBB; 0 = the OS has none
    int      caretBlinkMs = 530; // half-period; 0 = don't blink
};

enum FdEvents : uint32_t { FdRead = 1, FdWrite = 2 };

class App {
public:
    // Picks the backend: $PLAT_BACKEND (wayland | x11 | headless — the last
    // only in PLAT_TEST_HOOKS builds) if set,
    // else Wayland when $WAYLAND_DISPLAY is present, else X11 on Linux; the
    // native one on Windows/macOS. Returns null (and fills *error) on failure.
    static std::unique_ptr<App> create(std::string *error = nullptr);
    virtual ~App() = default;

    virtual const char *backendName() const = 0;

    void setEventHandler(std::function<void(const Event &)> fn) { _handler = std::move(fn); }

    virtual std::unique_ptr<Window> createWindow(const WindowDesc &desc) = 0;

    // Runs until quit(). Events, timers, fd callbacks and posted closures are
    // all dispatched from inside run() on the calling thread.
    virtual void run()               = 0;
    virtual void quit()              = 0;
    // One iteration: wait up to timeoutMs (-1 = forever, 0 = don't block),
    // dispatch whatever is ready, return. For tests and embedding.
    virtual void pump(int timeoutMs) = 0;

    // Thread-safe: queue fn to run on the loop thread and wake the loop.
    virtual void post(std::function<void()> fn) = 0;

    virtual TimerId addTimer(int intervalMs, bool repeat, std::function<void()> fn) = 0;
    virtual void    cancelTimer(TimerId id)                                         = 0;

    // POSIX backends only (returns 0 elsewhere): call fn with the ready
    // FdEvents whenever fd becomes readable/writable. The seam the socket
    // layer plugs into.
    virtual uint64_t watchFd(int fd, uint32_t events, std::function<void(uint32_t)> fn) = 0;
    virtual void     unwatchFd(uint64_t id)                                             = 0;

    // Clipboard. Setting is synchronous; reading is asynchronous because on
    // Wayland and X11 the data lives in another process. The callback always
    // runs (with nullopt when unavailable), never re-entrantly.
    // A new selection replaces every type previously offered; offer several
    // representations of the same content at once (text + html + png).
    virtual void
    setClipboard(std::vector<DataItem> items, Selection sel = Selection::Clipboard) = 0;
    virtual void requestClipboard(
        std::string_view                                mime,
        std::function<void(std::optional<std::string>)> cb,
        Selection                                       sel = Selection::Clipboard
    ) = 0;
    // The MIME types currently offered, normalised to the standard names
    // above where a native format maps to one.
    virtual void requestClipboardMimes(
        std::function<void(std::vector<std::string>)> cb, Selection sel = Selection::Clipboard
    ) {
        post([cb = std::move(cb)] { cb({}); });
    }
    void setClipboardText(std::string utf8, Selection sel = Selection::Clipboard) {
        setClipboard({{"text/plain;charset=utf-8", std::move(utf8)}}, sel);
    }

    // Start an OS drag from a window. Call from a PointerDown or PointerMove
    // handler while the button is still held (Wayland needs that press's
    // serial). Win32 runs the whole drag inside this call (DoDragDrop);
    // elsewhere it returns at once. DragFinished always follows a true return.
    // Returns false when the OS sees no physically held button (macOS checks
    // the real mouse) or the backend cannot drag.
    virtual bool startDrag(Window &source, const DragDesc &drag) { return false; }

    void           setAppInfo(AppInfo info) { _info = std::move(info); }
    const AppInfo &appInfo() const { return _info; }

    // Null when this desktop has no status area we can use.
    virtual std::unique_ptr<Tray> createTray() { return nullptr; }

    virtual bool notificationsAvailable() const { return false; }
    // `id` (caller-chosen, > 0) is what NotificationActivated/Closed carry
    // back -- the caller owns generating and persisting it; backends just
    // submit under it. False = couldn't submit.
    virtual bool notify(uint64_t id, const Notification &n) { return false; }

    // Unread count on the Dock icon / taskbar button / launcher entry; 0 clears.
    virtual void setBadgeCount(int count) {}

    // ── Monitors ────────────────────────────────────────────────────────────
    virtual std::vector<Monitor> monitors() const { return {}; }

    // ── Single instance and URLs ────────────────────────────────────────────
    // Call early. True: we are the primary instance and will receive
    // InstanceActivated/OpenUrls from later launches. False: another instance
    // with this key runs; our args (and activation token) were forwarded to
    // it and the caller should exit. key is per user login session (e.g. the
    // app id; Windows RDP and console sessions don't see each other).
    // Calling it again in the primary returns true. With no usable per-user
    // channel (no runtime dir) the app runs as primary rather than refuse.
    virtual bool claimSingleInstance(std::string_view key, const std::vector<std::string> &args) {
        return true;
    }
    // Make `scheme:` URLs launch this executable for the current user (Linux
    // .desktop handler — the app's own <AppInfo.id>.desktop launcher when it
    // declares the scheme, else a hidden one plat writes —, Windows
    // HKCU\Software\Classes, macOS: the bundle must declare it in Info.plist —
    // returns whether it does). Returns success.
    virtual bool registerUrlScheme(std::string_view scheme) { return false; }

    // ── Network and power ───────────────────────────────────────────────────
    // nullopt when the OS gives no reachability information. Online = a
    // connected network with traffic, not "internet reachable" (captive and
    // proxied networks count): Linux portal NetworkMonitor available &&
    // connectivity != local-only, else NetworkManager State >= 60; Windows
    // NLM any connected network; macOS nw_path satisfied. NetworkChanged
    // needs a baseline — call networkOnline() once at startup.
    virtual std::optional<bool> networkOnline() const { return std::nullopt; }

    // ── Files ───────────────────────────────────────────────────────────────
    // Asynchronous: cb receives the chosen paths (absolute, local), or an
    // empty vector when cancelled or when no dialog could be shown. Never
    // re-entrant; Win32/macOS may run a modal loop in which timers keep firing.
    // On Linux without a FileChooser portal this runs zenity/kdialog when
    // installed ($PLAT_FILE_DIALOG_FALLBACK, see linux/file_dialog.h).
    virtual void
    showFileDialog(const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb) {
        post([cb = std::move(cb)] { cb({}); });
    }
    // The same dialog, telling "cancelled" from "no dialog here". For apps
    // with a chooser of their own: on Linux, no portal (or a failing one)
    // answers Unavailable at once instead of trying helper programs, unless
    // $PLAT_FILE_DIALOG_FALLBACK names one. Same threading contract as above.
    // Default: showFileDialog(), empty = Cancelled.
    virtual void
    showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb);
    // Paths leaving plat (standardDir, file dialogs, drops) use '/' on every
    // OS (C:/Users/… on Windows); paths passed in may use either separator.
    virtual std::string standardDir(StandardDir d) const {
        return {};
    } // absolute, no trailing slash

    virtual SystemSettings           systemSettings() const { return {}; }
    // The user's UI languages, most preferred first, as the OS spells them
    // ("ja-JP", "en_US.UTF-8"); empty = unknown. Default (Linux, headless):
    // the $LANGUAGE list, then $LC_ALL, $LC_MESSAGES or $LANG.
    virtual std::vector<std::string> preferredLanguages() const;

    virtual bool darkMode() const              = 0;
    virtual int  doubleClickMs() const         = 0;
    virtual bool openUrl(std::string_view url) = 0;

    // Native input injection for tests (plat/testing.h); null when the
    // backend has no way to synthesise input through the OS, and always null
    // unless plat was built with PLAT_TEST_HOOKS (release builds are not).
    virtual class TestHooks *testHooks() { return nullptr; }

protected:
    void emit(const Event &e) {
        if (_handler)
            _handler(e);
    }

private:
    std::function<void(const Event &)> _handler;
    AppInfo                            _info;
};

} // namespace plat
