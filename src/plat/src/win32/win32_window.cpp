// Win32 backend, window half: the window procedure — present through a DIB
// section, the Custom-decoration frame (WM_NCCALCSIZE / WM_NCHITTEST),
// pointer, keyboard and IMM32 input, drops, DPI changes.
//
// Events are delivered from inside the window procedure, which is what lets
// them keep flowing while an OS modal loop (live resize/move, menus) owns the
// message pump. The price is re-entrancy: any handler may destroy the
// window, so every emit goes through send(), and code after a failed send()
// must not touch `this`.
#include "win32/win32.h"

#include <dwmapi.h>
#include <imm.h>
#include <propsys.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plat::win32 {

namespace {

constexpr uint32_t bit(Button b) {
    return 1u << unsigned(b);
}

HCURSOR loadCursor(Cursor c) {
    LPCWSTR id = IDC_ARROW;
    switch (c) {
    case Cursor::Arrow:
        id = IDC_ARROW;
        break;
    case Cursor::IBeam:
        id = IDC_IBEAM;
        break;
    case Cursor::Hand:
        id = IDC_HAND;
        break;
    case Cursor::Wait:
        id = IDC_WAIT;
        break;
    case Cursor::Progress:
        id = IDC_APPSTARTING;
        break;
    case Cursor::Crosshair:
        id = IDC_CROSS;
        break;
    case Cursor::NotAllowed:
        id = IDC_NO;
        break;
    case Cursor::Move:
        id = IDC_SIZEALL;
        break;
    // Windows ships no open/closed-hand cursors;
    // the hand and the four-way arrow are the closest system shapes.
    case Cursor::Grab:
        id = IDC_HAND;
        break;
    case Cursor::Grabbing:
        id = IDC_SIZEALL;
        break;
    case Cursor::ResizeH:
        id = IDC_SIZEWE;
        break;
    case Cursor::ResizeV:
        id = IDC_SIZENS;
        break;
    case Cursor::ResizeNWSE:
        id = IDC_SIZENWSE;
        break;
    case Cursor::ResizeNESW:
        id = IDC_SIZENESW;
        break;
    case Cursor::Hidden:
        return nullptr;
    }
    return LoadCursorW(nullptr, id);
}

DWORD styleFor(bool resizable) {
    // Custom windows keep the full overlapped style too: WS_CAPTION and
    // WS_THICKFRAME are what give minimise/maximise animations, Aero Snap,
    // Win+arrow tiling and the DWM shadow; WM_NCCALCSIZE removes the frame.
    DWORD s = WS_OVERLAPPEDWINDOW;
    if (!resizable)
        s &= ~DWORD(WS_THICKFRAME | WS_MAXIMIZEBOX);
    return s;
}

void setAppUserModelId(HWND h, const std::string &id) {
    // Per-window, so a plat window inside a host process groups on the
    // taskbar under its own id.
    IPropertyStore *store = nullptr;
    if (FAILED(
            SHGetPropertyStoreForWindow(h, IID_IPropertyStore, reinterpret_cast<void **>(&store))
        ))
        return;
    setAppUserModelIdProperty(store, toWide(id));
    store->Release();
}

bool g_firstShow = true;

} // namespace

// ── lifecycle ───────────────────────────────────────────────────────────────

Win32Window::Win32Window(Win32App *app, const WindowDesc &d)
    : _app(app), _custom(d.decorations == Decorations::Custom), _resizable(d.resizable),
      _minSize(d.minSize) {
    _cursorHandle = loadCursor(Cursor::Arrow);

    // Size at the system DPI first: the monitor the window lands on (and so
    // its DPI) is only known once it exists; corrected right below.
    _dpi = api().getDpiForSystem ? api().getDpiForSystem() : dpiForWindow(nullptr);
    RECT r{0, 0, LONG(std::lround(d.size.w * scale())), LONG(std::lround(d.size.h * scale()))};
    adjustForFrame(&r);
    // An explicit position is passed to CreateWindow already, so the window
    // is born on the right monitor (and at its DPI); the exact client
    // placement is fixed up below, once the real frame is known.
    std::optional<POINT> at;
    if (d.position)
        at = logicalToPhysical(*d.position, monitorGeoms());
    const std::wstring title = toWide(d.title);
    // Not WS_VISIBLE: showing is deferred (below) so no event reaches the app
    // before createWindow() has returned the Window it refers to.
    _hwnd                    = CreateWindowExW(
        0,
        Win32App::kWindowClass,
        title.c_str(),
        styleFor(_resizable),
        at ? at->x + r.left : CW_USEDEFAULT,
        at ? at->y + r.top : CW_USEDEFAULT,
        r.right - r.left,
        r.bottom - r.top,
        nullptr,
        nullptr,
        app->instance(),
        this
    );
    if (!_hwnd)
        return;
    _memDC = CreateCompatibleDC(nullptr);
    _dpi   = dpiForWindow(_hwnd);

    if (_custom) {
        // With the frame gone, a sliver of DWM frame inside the client is
        // what keeps the drop shadow (and, on Windows 11, the rounded corners
        // and border) on Windows 10/11. Our pixels are opaque, so the sliver
        // is never visible.
        const MARGINS m{0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(_hwnd, &m);
    }
    applyDarkTitleBar(false);
    ImmAssociateContextEx(_hwnd, nullptr, 0); // text input starts disabled: no IME
    // OLE drops carry every type and let the app accept or refuse per
    // position; WM_DROPFILES (files only) is the fallback when this thread
    // could not get OLE. The handler below serves both.
    if (app->oleReady())
        _dropTarget = registerDropTarget(this, app->_dropHelper);
    if (!_dropTarget) {
        DragAcceptFiles(_hwnd, TRUE);
        // Let Explorer's drops through UIPI when we run elevated.
        ChangeWindowMessageFilterEx(_hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(_hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
    }
    // Explorer tells an elevated process about its taskbar button too.
    if (app->taskbarButtonCreatedMsg())
        ChangeWindowMessageFilterEx(_hwnd, app->taskbarButtonCreatedMsg(), MSGFLT_ALLOW, nullptr);
    // The default id would group every plat app together on the taskbar;
    // only an explicit one is worth setting.
    if (!d.appId.empty() && d.appId != WindowDesc{}.appId)
        setAppUserModelId(_hwnd, d.appId);

    // Final size at the real DPI; SWP_FRAMECHANGED makes a Custom window's
    // WM_NCCALCSIZE apply to the frame computed at creation.
    RECT fr{0, 0, LONG(std::lround(d.size.w * scale())), LONG(std::lround(d.size.h * scale()))};
    adjustForFrame(&fr);
    SetWindowPos(
        _hwnd,
        nullptr,
        0,
        0,
        fr.right - fr.left,
        fr.bottom - fr.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );
    if (at)
        moveClientTo(*at);
    updateGeometry();       // records the initial size; _created is still false, so no Resized
    positionMaybeChanged(); // likewise records the position without a Moved
    _lastShowState = 0;
    app->addWindow(this);
    _created = true;

    if (d.visible) {
        _pendingInitialShow       = true;
        std::weak_ptr<char> alive = _alive;
        app->post([this, alive] {
            if (!alive.expired() && _pendingInitialShow)
                show();
        });
    }
}

Win32Window::~Win32Window() {
    _alive.reset(); // anything still holding a weak_ptr to us now sees "gone"
    _app->removeWindow(this);
    while (_modalRefs > 0) {
        --_modalRefs;
        _app->leaveModal();
    }
    revokeDropTarget(_dropTarget); // before the HWND goes: RevokeDragDrop needs it
    _dropTarget = nullptr;
    if (_hwnd) {
        // Detach first: destruction sends WM_ACTIVATE/WM_KILLFOCUS/…, and
        // none of them may reach a half-destroyed object.
        SetWindowLongPtrW(_hwnd, GWLP_USERDATA, 0);
        if (_hasCaret)
            DestroyCaret();
        DestroyWindow(_hwnd);
    }
    if (_memDC) {
        if (_oldBitmap)
            SelectObject(_memDC, _oldBitmap);
        DeleteDC(_memDC);
    }
    if (_dib)
        DeleteObject(_dib);
}

bool Win32Window::send(Event e) {
    if (!_created)
        return true;
    std::weak_ptr<char> alive = _alive;
    e.window                  = this;
    _app->emit(e);
    return !alive.expired();
}

// ── geometry & state ────────────────────────────────────────────────────────

void Win32Window::setTitle(std::string_view utf8) {
    SetWindowTextW(_hwnd, toWide(utf8).c_str());
}

Size Win32Window::size() const {
    const double s = scale();
    return {int(std::lround(_physW / s)), int(std::lround(_physH / s))};
}

void Win32Window::adjustForFrame(RECT *r) const {
    if (_custom)
        return; // client == window
    const DWORD style = _hwnd ? DWORD(GetWindowLongW(_hwnd, GWL_STYLE)) : styleFor(_resizable);
    const DWORD ex    = _hwnd ? DWORD(GetWindowLongW(_hwnd, GWL_EXSTYLE)) : 0;
    if (api().adjustWindowRectExForDpi)
        api().adjustWindowRectExForDpi(r, style, FALSE, ex, _dpi);
    else
        AdjustWindowRectEx(r, style, FALSE, ex);
}

void Win32Window::setSize(Size logical) {
    const Size s{std::max(logical.w, _minSize.w), std::max(logical.h, _minSize.h)};
    RECT       r{0, 0, LONG(std::lround(s.w * scale())), LONG(std::lround(s.h * scale()))};
    adjustForFrame(&r);
    SetWindowPos(
        _hwnd,
        nullptr,
        0,
        0,
        r.right - r.left,
        r.bottom - r.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
    );
}

void Win32Window::show() {
    _pendingInitialShow = false;
    int cmd             = SW_SHOW;
    if (g_firstShow) {
        // Honour "Run: minimised/maximised" from a shortcut, as the first
        // ShowWindow of a process is expected to.
        g_firstShow = false;
        STARTUPINFOW si{sizeof(si)};
        GetStartupInfoW(&si);
        if (si.dwFlags & STARTF_USESHOWWINDOW) {
            switch (si.wShowWindow) {
            case SW_SHOWMINIMIZED:
            case SW_SHOWMINNOACTIVE:
            case SW_MINIMIZE:
            case SW_SHOWMAXIMIZED:
                cmd = si.wShowWindow;
                break;
            default:
                break;
            }
        }
    }
    ShowWindow(_hwnd, cmd);
}

void Win32Window::hide() {
    _pendingInitialShow = false;
    ShowWindow(_hwnd, SW_HIDE);
}

void Win32Window::minimize() {
    _pendingInitialShow = false;
    ShowWindow(_hwnd, SW_MINIMIZE);
}

void Win32Window::setMaximized(bool on) {
    _pendingInitialShow = false;
    ShowWindow(_hwnd, on ? SW_MAXIMIZE : SW_RESTORE);
}

void Win32Window::setAlwaysOnTop(bool on) {
    SetWindowPos(
        _hwnd,
        on ? HWND_TOPMOST : HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
    );
}

bool Win32Window::isAlwaysOnTop() const {
    return (GetWindowLongW(_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

void Win32Window::setFullscreen(bool on) {
    if (on == _fullscreen)
        return;
    // Raymond Chen's recipe: drop the frame styles, cover the monitor, and
    // restore style + placement on the way back.
    if (on) {
        _savedStyle     = DWORD(GetWindowLongW(_hwnd, GWL_STYLE));
        _savedPlacement = {sizeof(WINDOWPLACEMENT)};
        GetWindowPlacement(_hwnd, &_savedPlacement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        _fullscreen = true;
        SetWindowLongW(
            _hwnd, GWL_STYLE, LONG((_savedStyle & ~DWORD(WS_OVERLAPPEDWINDOW)) | WS_POPUP)
        );
        SetWindowPos(
            _hwnd,
            HWND_TOP,
            mi.rcMonitor.left,
            mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );
    } else {
        _fullscreen = false;
        SetWindowLongW(_hwnd, GWL_STYLE, LONG(_savedStyle));
        SetWindowPlacement(_hwnd, &_savedPlacement);
        SetWindowPos(
            _hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );
    }
    send({.type = EventType::StateChanged});
}

void Win32Window::activate() {
    if (IsIconic(_hwnd))
        ShowWindow(_hwnd, SW_RESTORE);
    // May be refused by the foreground lock; the OS then flashes the taskbar
    // button instead, which is the correct behaviour for a background app.
    SetForegroundWindow(_hwnd);
}

std::optional<Point> Win32Window::position() const {
    POINT c{0, 0};
    if (!ClientToScreen(_hwnd, &c))
        return std::nullopt;
    return physicalToLogical(c, monitorGeoms());
}

void Win32Window::moveClientTo(POINT phys) {
    // The client's offset inside the window rect as it is now (0 for a
    // Custom frame; border + caption at this DPI otherwise, including
    // Windows 10's invisible resize borders), rather than recomputed.
    RECT  wr;
    POINT c{0, 0};
    if (!GetWindowRect(_hwnd, &wr) || !ClientToScreen(_hwnd, &c))
        return;
    SetWindowPos(
        _hwnd,
        nullptr,
        phys.x - (c.x - wr.left),
        phys.y - (c.y - wr.top),
        0,
        0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
    );
}

bool Win32Window::setPosition(Point logical) {
    // A maximised, fullscreen or minimised window's position belongs to the
    // OS; moving it would fight the shell (and IsZoomed windows snap back).
    if (IsZoomed(_hwnd) || IsIconic(_hwnd) || _fullscreen)
        return false;
    moveClientTo(logicalToPhysical(logical, monitorGeoms()));
    // Crossing onto a monitor of another DPI resizes the window through
    // WM_DPICHANGED around the OS's suggested rect, which can shift the
    // client by a few pixels; the Moved that follows reports where it is.
    return true;
}

uint64_t Win32Window::monitor() const {
    return monitorIdOf(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST));
}

void Win32Window::positionMaybeChanged() {
    if (IsIconic(_hwnd))
        return; // parked at -32000,-32000: not a position the app should see
    POINT c{0, 0};
    if (!ClientToScreen(_hwnd, &c) || (c.x == _lastPos.x && c.y == _lastPos.y))
        return;
    _lastPos = c;
    send({.type = EventType::Moved});
}

bool Win32Window::themeChanged(bool darkChanged) {
    if (darkChanged)
        applyDarkTitleBar(true);
    return send({.type = EventType::ThemeChanged});
}

void Win32Window::updateGeometry() {
    RECT rc;
    GetClientRect(_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
        return; // minimised: keep the last real size
    _physW         = rc.right;
    _physH         = rc.bottom;
    const Size   s = size();
    const double k = scale();
    if (s.w == _lastSize.w && s.h == _lastSize.h && k == _lastScale)
        return; // a physical-only change still repaints: WM_PAINT sees the DIB is stale
    _lastSize       = s;
    _lastScale      = k;
    _frameRequested = true;
    if (!_created)
        return;
    if (!send({.type = EventType::Resized}))
        return;
    if (_inSizeMove)
        deliverFrame(); // live resize: paint now, inside the OS loop, so content tracks the edge
    else
        _app->wake();
}

void Win32Window::stateMaybeChanged() {
    const int st = IsIconic(_hwnd) ? 2 : IsZoomed(_hwnd) ? 1 : 0;
    if (st == _lastShowState)
        return;
    _lastShowState = st;
    send({.type = EventType::StateChanged});
}

void Win32Window::applyDarkTitleBar(bool repaint) {
    if (_custom)
        return;
    const BOOL dark = _app->darkMode();
    // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 since Windows 10 20H1, 19 before.
    if (FAILED(DwmSetWindowAttribute(_hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(_hwnd, 19, &dark, sizeof(dark));
    if (repaint) // Windows 10 only redraws the caption on the next frame change
        SetWindowPos(
            _hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
        );
}

// ── present ─────────────────────────────────────────────────────────────────

void Win32Window::requestFrame() {
    _frameRequested = true;
    _app->wake(); // may be called from inside an OS modal loop; the wake reaches it
}

bool Win32Window::frameDue(core::Clock::time_point now, int *waitMs) const {
    *waitMs = -1;
    if (!_frameRequested || !_created || !IsWindowVisible(_hwnd) || IsIconic(_hwnd))
        return false;
    // Paced to the display rate: at most one Frame per refresh interval, so
    // an app that requests a frame from every Frame animates at ~60 Hz
    // instead of spinning. The first request after idle is served at once.
    const auto next = _lastFrame + std::chrono::milliseconds(_app->frameIntervalMs());
    if (now >= next)
        return true;
    *waitMs = int(std::chrono::ceil<std::chrono::milliseconds>(next - now).count());
    return false;
}

bool Win32Window::deliverFrame() {
    _frameRequested = false;
    _lastFrame      = core::Clock::now();
    return send({.type = EventType::Frame});
}

void Win32Window::ensureDib(int w, int h) {
    if (_dib && w == _dibW && h == _dibH)
        return;
    // Live resize asks for a new size every few pixels: the DIB grows in
    // steps and is reused while the window fits, so a drag reallocates a
    // handful of times instead of on every step. A much smaller window
    // (a quarter of the area) gives the memory back.
    const bool fits =
        _dib && w <= _capW && h <= _capH && size_t(_capW) * _capH <= size_t(w) * h * 4;
    if (fits) {
        _dibW = w;
        _dibH = h;
        return;
    }
    const int  cw = (w + 127) & ~127, ch = (h + 127) & ~127;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = cw;
    bi.bmiHeader.biHeight      = -ch; // top-down, so row 0 is the top like Canvas
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32; // BGRA in memory == 0xAARRGGBB little-endian
    bi.bmiHeader.biCompression = BI_RGB;
    void   *bits               = nullptr;
    HBITMAP dib                = CreateDIBSection(_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
        return; // keep painting into the old one rather than into nothing
    HGDIOBJ prev = SelectObject(_memDC, dib);
    if (!_oldBitmap)
        _oldBitmap = HBITMAP(prev); // the DC's stock bitmap, restored before DeleteDC
    if (_dib)
        DeleteObject(_dib);
    // No clearing: a new DIB section comes zero-filled from the OS, and a
    // size change is a new canvas the app repaints in full.
    _dib  = dib;
    _bits = static_cast<uint32_t *>(bits);
    _capW = cw;
    _capH = ch;
    _dibW = w;
    _dibH = h;
}

Canvas Win32Window::beginPaint() {
    ensureDib(std::max(1, _physW), std::max(1, _physH));
    if (!_bits)
        return {};
    GdiFlush(); // GDI may still be reading the DIB from a batched BitBlt
    return {_bits, _dibW, _dibH, _capW, scale()};
}

void Win32Window::blit(HDC dc, RECT r) {
    r.left   = std::max<LONG>(r.left, 0);
    r.top    = std::max<LONG>(r.top, 0);
    r.right  = std::min<LONG>(r.right, _dibW);
    r.bottom = std::min<LONG>(r.bottom, _dibH);
    if (r.right > r.left && r.bottom > r.top)
        BitBlt(
            dc, r.left, r.top, r.right - r.left, r.bottom - r.top, _memDC, r.left, r.top, SRCCOPY
        );
}

void Win32Window::endPaint(const std::vector<Rect> &damage) {
    if (!_dib)
        return;
    _everPainted = true;
    // GetDC, not the WM_PAINT DC: the app may have changed pixels outside the
    // current update region, and a BeginPaint DC is clipped to it. Under DWM
    // this writes the redirection surface, composed at the next vblank.
    HDC dc       = GetDC(_hwnd);
    if (damage.empty()) {
        blit(dc, {0, 0, _dibW, _dibH});
        _blittedAll = true;
    }
    for (const Rect &d : damage)
        blit(dc, {d.x, d.y, d.x + d.w, d.y + d.h});
    ReleaseDC(_hwnd, dc);
}

void Win32Window::onPaint() {
    HWND        h = _hwnd;
    PAINTSTRUCT ps;
    HDC         dc = BeginPaint(h, &ps);
    if (!dc)
        return;
    // Exposure alone is served from the DIB; a Frame is only asked for when
    // the content is actually stale (requested, resized, never painted).
    const bool stale = _frameRequested || !_everPainted || _dibW != _physW || _dibH != _physH;
    _blittedAll      = false;
    if (stale && _physW > 0 && _physH > 0 && !deliverFrame()) {
        EndPaint(h, &ps);
        return;
    }
    // The Frame's endPaint already put the whole canvas on screen: no
    // second copy of the same pixels.
    if (_dib && !_blittedAll)
        blit(dc, ps.rcPaint);
    // The app has not painted the new size yet: black, not stale garbage.
    HBRUSH black = HBRUSH(GetStockObject(BLACK_BRUSH));
    if (_physW > _dibW) {
        RECT r{_dibW, 0, _physW, _physH};
        FillRect(dc, &r, black);
    }
    if (_physH > _dibH) {
        RECT r{0, _dibH, std::min(_physW, _dibW), _physH};
        FillRect(dc, &r, black);
    }
    EndPaint(h, &ps);
}

// ── Custom decorations ──────────────────────────────────────────────────────

LRESULT Win32Window::onNcCalcSize(WPARAM wp, LPARAM lp) {
    // Returning 0 without touching the rect makes the whole window client:
    // no caption, no borders; the app draws its title bar and its hit test
    // supplies the resize edges.
    RECT *r =
        wp ? &reinterpret_cast<NCCALCSIZE_PARAMS *>(lp)->rgrc[0] : reinterpret_cast<RECT *>(lp);
    if (IsZoomed(_hwnd) && !_fullscreen) {
        // A maximised window is sized past the monitor by its frame so the
        // borders fall off-screen; without them that overhang would be
        // client and cut off the app's title bar and edges. Clamp to the
        // work area (which also keeps the taskbar visible).
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi)) {
            RECT clamped;
            if (IntersectRect(&clamped, r, &mi.rcWork))
                *r = clamped;
            // An auto-hide taskbar is revealed by the pointer touching its
            // edge — which a window covering that edge would swallow (and
            // the shell would treat it as a fullscreen app). Leave it 1 px.
            APPBARDATA abd{sizeof(abd)};
            if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) {
                for (UINT edge : {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}) {
                    APPBARDATA q{sizeof(q)};
                    q.uEdge = edge;
                    q.rc    = mi.rcMonitor;
                    if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &q))
                        continue;
                    switch (edge) {
                    case ABE_BOTTOM:
                        r->bottom -= 1;
                        break;
                    case ABE_TOP:
                        r->top += 1;
                        break;
                    case ABE_LEFT:
                        r->left += 1;
                        break;
                    case ABE_RIGHT:
                        r->right -= 1;
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

LRESULT Win32Window::onNcHitTest(LPARAM lp) {
    const POINT p = screenToClient(lp);
    RECT        rc;
    GetClientRect(_hwnd, &rc);
    if (!PtInRect(&rc, p))
        return HTNOWHERE;
    if (!_hitTest || _fullscreen)
        return HTCLIENT;
    const bool rs = _resizable;
    switch (_hitTest(toLogical(p))) {
    case HitArea::Client:
        return HTCLIENT;
    case HitArea::Caption:
        return HTCAPTION;
    case HitArea::ResizeTop:
        return rs ? HTTOP : HTBORDER;
    case HitArea::ResizeBottom:
        return rs ? HTBOTTOM : HTBORDER;
    case HitArea::ResizeLeft:
        return rs ? HTLEFT : HTBORDER;
    case HitArea::ResizeRight:
        return rs ? HTRIGHT : HTBORDER;
    case HitArea::ResizeTopLeft:
        return rs ? HTTOPLEFT : HTBORDER;
    case HitArea::ResizeTopRight:
        return rs ? HTTOPRIGHT : HTBORDER;
    case HitArea::ResizeBottomLeft:
        return rs ? HTBOTTOMLEFT : HTBORDER;
    case HitArea::ResizeBottomRight:
        return rs ? HTBOTTOMRIGHT : HTBORDER;
    // Reporting the real button codes is what the shell keys on: HTMAXBUTTON
    // shows the Windows 11 Snap Layouts flyout on hover. The presses are
    // translated back into client events below (WM_NCxBUTTONDOWN).
    case HitArea::MinimizeButton:
        return HTMINBUTTON;
    case HitArea::MaximizeButton:
        return HTMAXBUTTON;
    case HitArea::CloseButton:
        return HTCLOSE;
    }
    return HTCLIENT;
}

// ── pointer ─────────────────────────────────────────────────────────────────

POINT Win32Window::screenToClient(LPARAM lp) const {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(_hwnd, &p);
    return p;
}

void Win32Window::onPointerMove(POINT p, bool nonClient) {
    // Client and non-client leave tracking are separate on Windows; in a
    // Custom window both are "inside" to the app (see onPointerLeave).
    bool &tracking = nonClient ? _trackingNonClient : _trackingClient;
    if (!tracking) {
        TRACKMOUSEEVENT t{sizeof(t), DWORD(TME_LEAVE | (nonClient ? TME_NONCLIENT : 0)), _hwnd, 0};
        tracking = TrackMouseEvent(&t) != 0;
    }
    const uint32_t mods = currentMods();
    if (!_pointerInside) {
        _pointerInside = true;
        _lastMove      = {INT_MIN, INT_MIN};
        Event e{.type = EventType::PointerEnter, .pos = toLogical(p)};
        e.mods = mods;
        if (!send(e))
            return;
    }
    // Windows re-sends WM_MOUSEMOVE for an unchanged position (after
    // SetCursor, z-order changes); those are not motion.
    if (p.x == _lastMove.x && p.y == _lastMove.y)
        return;
    _lastMove = p;
    Event e{.type = EventType::PointerMove, .pos = toLogical(p)};
    e.mods = mods;
    send(e);
}

void Win32Window::onPointerLeave() {
    if (!_pointerInside || _buttons)
        return; // captured: still ours until release, which re-arms tracking
    if (_custom) {
        // Crossing between the client and the app-drawn caption buttons
        // (non-client to Windows) leaves one tracker and enters the other.
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == _hwnd)
            return;
    }
    _pointerInside = false;
    _lastMove      = {INT_MIN, INT_MIN};
    send({.type = EventType::PointerLeave});
}

int Win32Window::countClick(Button b) {
    // Our own counter instead of CS_DBLCLKS: Windows only knows "double",
    // and triple-click (select paragraph) needs the run length.
    const DWORD t   = GetMessageTime();
    const DWORD pos = GetMessagePos();
    const POINT pt{GET_X_LPARAM(pos), GET_Y_LPARAM(pos)};
    return _clicks.press(
        int(b),
        pt.x,
        pt.y,
        t,
        GetDoubleClickTime(),
        GetSystemMetrics(SM_CXDOUBLECLK) / 2,
        GetSystemMetrics(SM_CYDOUBLECLK) / 2
    );
}

bool Win32Window::onButton(Button b, bool down, POINT p) {
    Event e{.pos = toLogical(p), .button = b};
    e.mods = currentMods();
    if (down) {
        e.type   = EventType::PointerDown;
        e.clicks = countClick(b);
        // Capture so the release (and drags) arrive even outside the window.
        if (_buttons == 0)
            SetCapture(_hwnd);
        _buttons |= bit(b);
    } else {
        if (!(_buttons & bit(b)))
            return true; // press began elsewhere (e.g. the click that closed a menu)
        e.type = EventType::PointerUp;
        _buttons &= ~bit(b);
        if (_buttons == 0)
            ReleaseCapture();
    }
    if (!send(e))
        return false;
    if (!down && _buttons == 0) {
        // Released outside after a drag: the hover ended while captured.
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) != _hwnd)
            onPointerLeave();
    }
    return true;
}

void Win32Window::releaseAllButtons() {
    // Capture taken away (Alt+Tab, a modal dialog, another SetCapture): the
    // app would otherwise believe the button is still held.
    for (Button b : {Button::Left, Button::Right, Button::Middle, Button::Back, Button::Forward}) {
        if (!(_buttons & bit(b)))
            continue;
        _buttons &= ~bit(b);
        Event e{.type = EventType::PointerUp, .button = b};
        if (_lastMove.x != INT_MIN)
            e.pos = toLogical(_lastMove);
        if (!send(e))
            return;
    }
}

// ── keyboard ────────────────────────────────────────────────────────────────

bool Win32Window::isFakeAltGrCtrl(UINT msg) const {
    // AltGr arrives as a synthetic left Ctrl followed by right Alt with the
    // same timestamp. Reporting that Ctrl would turn every AltGr character
    // into a Ctrl shortcut; the real Ctrl never shares the Alt's time.
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    MSG        next;
    if (!PeekMessageW(&next, _hwnd, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE))
        return false;
    const bool nextDown = next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN;
    const bool nextUp   = next.message == WM_KEYUP || next.message == WM_SYSKEYUP;
    return (down ? nextDown : nextUp) && next.wParam == VK_MENU &&
           (HIWORD(next.lParam) & KF_EXTENDED) && next.time == DWORD(GetMessageTime());
}

LRESULT Win32Window::onKey(UINT msg, WPARAM wp, LPARAM lp) {
    HWND       h    = _hwnd;
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool sys  = msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
    const UINT vk   = UINT(wp);
    const UINT scan = HIWORD(lp) & 0xff;
    const bool ext  = (HIWORD(lp) & KF_EXTENDED) != 0;
    // VK_PROCESSKEY: the IME took the key; its effect arrives as composition.
    if (vk != VK_PROCESSKEY && !(vk == VK_CONTROL && !ext && isFakeAltGrCtrl(msg))) {
        Event e{.type = down ? EventType::KeyDown : EventType::KeyUp};
        e.key      = keyFromVk(vk, ext, scan);
        e.scancode = scan | (ext ? 0xE000u : 0u); // set-1 code, 0xE0xx for extended keys
        e.mods     = currentMods();
        e.repeat   = down && (HIWORD(lp) & KF_REPEAT);
        if (!send(e))
            return 0;
    }
    // System keys still go to DefWindowProc for Alt+F4 and Alt+Space.
    return sys ? DefWindowProcW(h, msg, wp, lp) : 0;
}

void Win32Window::onChar(wchar_t c) {
    // WM_CHAR is UTF-16: characters outside the BMP (emoji) arrive as two
    // messages, a high then a low surrogate.
    if (IS_HIGH_SURROGATE(c)) {
        _highSurrogate = c;
        return;
    }
    wchar_t buf[2];
    size_t  n = 0;
    if (IS_LOW_SURROGATE(c)) {
        if (!_highSurrogate)
            return;
        buf[n++] = _highSurrogate;
    }
    buf[n++]       = c;
    _highSurrogate = 0;
    if (n == 1 && (c < 0x20 || c == 0x7f))
        return; // Enter, Tab, Backspace, Escape, Ctrl+letter: keys, not text
    if (!_textInput.enabled)
        return;
    const uint32_t m = currentMods();
    // A Ctrl chord types nothing — but Ctrl+Alt is how Windows spells AltGr,
    // and that must type (@ on German, € on most of Europe).
    if (((m & ModCtrl) && !(m & ModAlt)) || (m & ModSuper))
        return;
    Event e{.type = EventType::TextInput};
    e.text = toUtf8(std::wstring_view(buf, n));
    e.mods = m;
    send(e);
}

// ── IME (IMM32) ─────────────────────────────────────────────────────────────

void Win32Window::setTextInput(const TextInputState &s) {
    const bool was = _textInput.enabled;
    _textInput     = s;
    if (was != s.enabled) {
        if (!s.enabled)
            if (HIMC imc = ImmGetContext(_hwnd)) {
                ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(_hwnd, imc);
            }
        // A null context switches the IME off for this window; IACE_DEFAULT
        // gives it back the thread's default context.
        ImmAssociateContextEx(_hwnd, nullptr, s.enabled ? IACE_DEFAULT : 0);
        if (!s.enabled && _preeditActive) {
            _preeditActive = false;
            if (!send({.type = EventType::TextPreedit}))
                return;
        }
    }
    updateSystemCaret(GetFocus() == _hwnd);
    updateImePosition();
}

void Win32Window::updateSystemCaret(bool focused) {
    // A hidden system caret at the text caret: several IMEs (Chinese ones
    // especially), the touch keyboard, Magnifier and screen readers follow
    // it rather than the IMM candidate position. Never shown.
    const bool want = focused && _textInput.enabled;
    if (want && !_hasCaret)
        _hasCaret =
            CreateCaret(
                _hwnd, nullptr, 1, std::max(1, int(std::lround(_textInput.caret.h * scale())))
            ) != 0;
    else if (!want && _hasCaret) {
        DestroyCaret();
        _hasCaret = false;
    }
}

void Win32Window::updateImePosition() {
    if (!_textInput.enabled)
        return;
    const double s = scale();
    const Rect  &c = _textInput.caret;
    const RECT   r{
        LONG(std::lround(c.x * s)),
        LONG(std::lround(c.y * s)),
        LONG(std::lround((c.x + c.w) * s)),
        LONG(std::lround((c.y + std::max(c.h, 1)) * s))
    };
    if (_hasCaret)
        SetCaretPos(r.left, r.top);
    HIMC imc = ImmGetContext(_hwnd);
    if (!imc)
        return;
    // Composition form at the caret (some IMEs anchor the candidate list to
    // it even when its window is hidden), and the candidate list placed
    // below the caret while avoiding it — CFS_EXCLUDE lets the IME flip it
    // above near the bottom of the screen.
    COMPOSITIONFORM cf{};
    cf.dwStyle      = CFS_POINT;
    cf.ptCurrentPos = {r.left, r.top};
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand{};
    cand.dwIndex      = 0;
    cand.dwStyle      = CFS_EXCLUDE;
    cand.ptCurrentPos = {r.left, r.bottom};
    cand.rcArea       = r;
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(_hwnd, imc);
}

void Win32Window::onImeComposition(LPARAM lp) {
    HIMC imc = ImmGetContext(_hwnd);
    if (!imc)
        return;
    auto get = [&](DWORD kind) {
        const LONG   bytes = ImmGetCompositionStringW(imc, kind, nullptr, 0);
        std::wstring s(bytes > 0 ? size_t(bytes) / sizeof(wchar_t) : 0, L'\0');
        if (!s.empty())
            ImmGetCompositionStringW(imc, kind, s.data(), DWORD(bytes));
        return s;
    };
    const bool   hasResult = (lp & GCS_RESULTSTR) != 0;
    const bool   hasComp   = (lp & GCS_COMPSTR) != 0;
    std::wstring result    = hasResult ? get(GCS_RESULTSTR) : std::wstring();
    std::wstring comp      = hasComp ? get(GCS_COMPSTR) : std::wstring();
    LONG         cursor    = -1;
    if (hasComp)
        cursor = (lp & GCS_CURSORPOS) ? ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0)
                                      : LONG(comp.size());
    // Released before emitting: a handler may switch text input off.
    ImmReleaseContext(_hwnd, imc);

    if (!result.empty()) {
        Event e{.type = EventType::TextInput};
        e.text = toUtf8(result);
        if (!send(e))
            return;
    }
    if (hasComp) {
        _preeditActive = !comp.empty();
        Event e{.type = EventType::TextPreedit};
        e.text = toUtf8(comp);
        if (cursor >= 0)
            e.preeditCursorBegin = e.preeditCursorEnd = utf8Length(comp, size_t(cursor));
        send(e);
    } else if (_preeditActive) {
        // Committed with nothing left, or lp == 0: composition cancelled.
        _preeditActive = false;
        send({.type = EventType::TextPreedit});
    }
}

// ── drops ───────────────────────────────────────────────────────────────────

void Win32Window::onDropFiles(HDROP drop) {
    Event e{.type = EventType::Drop};
    POINT pt{};
    DragQueryPoint(drop, &pt); // client coordinates
    e.pos        = toLogical(pt);
    e.mods       = currentMods();
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT   len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        e.uris.push_back(fileUri(path));
    }
    DragFinish(drop);
    send(e);
}

void Win32Window::setDropAction(DropAction a) {
    if (_dropTarget)
        setDropReply(_dropTarget, a);
}

Point Win32Window::screenToLogical(POINT p) const {
    ScreenToClient(_hwnd, &p);
    return toLogical(p);
}

void Win32Window::forgetButtons() {
    _buttons      = 0;
    _captionPress = false;
    if (GetCapture() == _hwnd)
        ReleaseCapture(); // WM_CAPTURECHANGED now finds nothing held
}

// ── attention, badge ────────────────────────────────────────────────────────

void Win32Window::requestAttention() {
    if (_active)
        return;
    // Flash the taskbar button until the window comes to the foreground;
    // FLASHW_TIMERNOFG stops it by itself then, so no bookkeeping here.
    FLASHWINFO fi{sizeof(fi)};
    fi.hwnd    = _hwnd;
    fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    FlashWindowEx(&fi);
}

void Win32Window::applyBadge() {
    _app->applyBadge(_hwnd);
}

// ── cursor ──────────────────────────────────────────────────────────────────

void Win32Window::setCursor(Cursor c) {
    if (c == _cursor)
        return;
    _cursor       = c;
    _cursorHandle = loadCursor(c);
    // WM_SETCURSOR only comes with the next motion; apply now if the pointer
    // is over our client area.
    POINT pt;
    if (GetCursorPos(&pt) && WindowFromPoint(pt) == _hwnd &&
        SendMessageW(_hwnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTCLIENT)
        SetCursor(_cursorHandle);
}

// ── the window procedure ────────────────────────────────────────────────────

LRESULT CALLBACK Win32Window::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // WM_GETMINMAXINFO precedes WM_NCCREATE; it gets the defaults.
    auto *self = static_cast<Win32Window *>(windowUserData(h, msg, lp));
    if (self && msg == WM_NCCREATE)
        self->_hwnd = h;
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT Win32Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
    HWND h = _hwnd; // `this` may be gone by the time DefWindowProc runs
    switch (msg) {
    case WM_CLOSE:
        send({.type = EventType::CloseRequested}); // the app decides; DefWindowProc would destroy
        return 0;
    case WM_ERASEBKGND:
        return 1; // every pixel is ours; erasing would flash
    case WM_PAINT:
        onPaint();
        return 0;

    case WM_SIZE: {
        std::weak_ptr<char> alive = _alive;
        updateGeometry();
        if (!alive.expired())
            stateMaybeChanged();
        return 0;
    }
    case WM_DPICHANGED: {
        // Per-monitor v2: adopt the new DPI and the OS's suggested rect (it
        // keeps the window under the pointer while dragging across monitors).
        _dpi                      = HIWORD(wp);
        const RECT         *r     = reinterpret_cast<const RECT *>(lp);
        std::weak_ptr<char> alive = _alive;
        SetWindowPos(
            h,
            nullptr,
            r->left,
            r->top,
            r->right - r->left,
            r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE
        );
        if (alive.expired())
            return 0;
        updateGeometry(); // the scale changed even if WM_SIZE did not come
        if (alive.expired())
            return 0;
        updateImePosition();
        // A monitor's scale was changed in Settings, or the window crossed
        // onto another monitor; only the first is a MonitorsChanged.
        _app->monitorsMaybeChanged();
        return 0;
    }
    case WM_MOVE:
        positionMaybeChanged();
        return 0;
    case WM_GETMINMAXINFO: {
        auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
        if (_minSize.w > 0 || _minSize.h > 0) {
            RECT r{
                0,
                0,
                LONG(std::lround(_minSize.w * scale())),
                LONG(std::lround(_minSize.h * scale()))
            };
            adjustForFrame(&r);
            if (_minSize.w > 0)
                mmi->ptMinTrackSize.x = r.right - r.left;
            if (_minSize.h > 0)
                mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }

    case WM_NCCALCSIZE:
        if (_custom)
            return onNcCalcSize(wp, lp);
        break;
    case WM_NCHITTEST:
        if (_custom)
            return onNcHitTest(lp);
        break;
    case WM_NCACTIVATE:
        // -1: don't repaint the (nonexistent) caption on activation changes;
        // without it Windows 10 flashes a classic title bar over our content.
        if (_custom)
            return DefWindowProcW(h, msg, wp, -1);
        break;

    case WM_ACTIVATE:
        _active = LOWORD(wp) != WA_INACTIVE;
        if (!send({.type = EventType::StateChanged}))
            return 0;
        break; // DefWindowProc moves keyboard focus to us → WM_SETFOCUS
    case WM_SETFOCUS:
        updateSystemCaret(true);
        updateImePosition();
        send({.type = EventType::FocusIn});
        return 0;
    case WM_KILLFOCUS:
        updateSystemCaret(false);
        _highSurrogate = 0;
        if (_preeditActive) {
            _preeditActive = false;
            if (!send({.type = EventType::TextPreedit}))
                return 0;
        }
        send({.type = EventType::FocusOut});
        return 0;

    // Pointer.
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (_captionPress) {
            // Pressed on the caption and now dragging: hand over to the OS
            // move loop (snapping, Aero Shake, Snap Layouts). DefWindowProc's
            // own WM_NCLBUTTONDOWN handling would block inside DragDetect
            // until the pointer moved or the button came up; doing the drag
            // detection here keeps a press-and-hold from freezing pump().
            POINT s = p;
            ClientToScreen(h, &s);
            if (std::abs(s.x - _captionPressAt.x) > GetSystemMetrics(SM_CXDRAG) ||
                std::abs(s.y - _captionPressAt.y) > GetSystemMetrics(SM_CYDRAG)) {
                _captionPress = false;
                ReleaseCapture();
                SendMessageW(h, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(s.x, s.y));
            }
            return 0;
        }
        onPointerMove(p, false);
        return 0;
    }
    case WM_NCMOUSEMOVE:
        if (_custom)
            onPointerMove(screenToClient(lp), true); // hover over the app's caption buttons
        break;
    case WM_MOUSELEAVE:
        _trackingClient = false;
        onPointerLeave();
        return 0;
    case WM_NCMOUSELEAVE:
        _trackingNonClient = false;
        if (_custom)
            onPointerLeave();
        break;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        const bool   down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        const Button b    = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? Button::Left
                            : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? Button::Right
                                                                             : Button::Middle;
        if (_captionPress && b == Button::Left && !down) {
            _captionPress = false; // a click on the caption that never became a drag
            ReleaseCapture();
            return 0;
        }
        onButton(b, down, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    }
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        onButton(
            GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? Button::Back : Button::Forward,
            msg == WM_XBUTTONDOWN,
            {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}
        );
        return TRUE; // XBUTTON messages want TRUE, or the shell also acts on them

    // Non-client presses in a Custom window: the three button areas are the
    // app's (delivered as ordinary presses, never to DefWindowProc, which
    // would run its own button tracking and act on them); the caption is
    // drag-detected above; resize edges go to the OS sizing loop.
    // Windows always sends NC double-clicks; our counter handles them.
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDOWN:
    case WM_NCMBUTTONDBLCLK:
    case WM_NCLBUTTONUP:
    case WM_NCRBUTTONUP:
    case WM_NCMBUTTONUP: {
        if (!_custom)
            break;
        const bool left =
            msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK || msg == WM_NCLBUTTONUP;
        const bool right =
            msg == WM_NCRBUTTONDOWN || msg == WM_NCRBUTTONDBLCLK || msg == WM_NCRBUTTONUP;
        const bool   up = msg == WM_NCLBUTTONUP || msg == WM_NCRBUTTONUP || msg == WM_NCMBUTTONUP;
        const Button b  = left ? Button::Left : right ? Button::Right : Button::Middle;
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            onButton(b, !up, screenToClient(lp));
            return 0;
        }
        if (wp == HTCAPTION && b == Button::Left && !up) {
            if (countClick(b) == 2 && _resizable) {
                _clicks.reset(); // the toggle consumed the double click
                ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            _captionPress   = true;
            _captionPressAt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(h);
            return 0;
        }
        break; // right-click on the caption → window menu; edges → sizing
    }
    case WM_CAPTURECHANGED:
        if (HWND(lp) != h) {
            _captionPress = false;
            releaseAllButtons();
        }
        return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const int  delta   = GET_WHEEL_DELTA_WPARAM(wp);
        const bool horiz   = msg == WM_MOUSEHWHEEL;
        const bool precise = delta % WHEEL_DELTA != 0;
        double     v       = double(delta) / WHEEL_DELTA; // notches, +away/+right
        if (precise) {
            // High-resolution wheels (and touchpads without WM_POINTER)
            // send fractions of a notch; the contract wants logical pixels
            // then. A notch is the user's lines-per-notch × 100/3 px —
            // Chromium's line height — i.e. 100 px at the default 3.
            const UINT lines = _app->wheelScrollAmount(horiz);
            v *= lines == WHEEL_PAGESCROLL ? double(horiz ? size().w : size().h)
                                           : lines * (100.0 / 3);
        }
        Event e{.type = EventType::Scroll, .pos = toLogical(screenToClient(lp))};
        if (horiz)
            e.dx = v;
        else
            e.dy = -v; // +delta = wheel away from the user = scroll up; plat's +y is down
        e.precise = precise;
        e.mods    = currentMods();
        send(e);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && HWND(wp) == h) {
            SetCursor(_cursorHandle);
            return TRUE;
        }
        break; // resize edges, caption: the OS picks

    // Keyboard.
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        return onKey(msg, wp, lp);
    case WM_CHAR:
        onChar(wchar_t(wp));
        return 0;
    case WM_SYSCHAR:
        // Alt+letter: menu mnemonics, which we have none of; DefWindowProc
        // would beep. Alt+Space still opens the window menu.
        if (wp == L' ')
            break;
        return 0;
    case WM_SYSCOMMAND:
        // Alt or F10 alone enters menu mode for the window menu, which then
        // eats the next key press. Nobody expects that from an app without a
        // menu bar; Alt+Space (lp = ' ') still works.
        if ((wp & 0xfff0) == SC_KEYMENU && lp == 0)
            return 0;
        break;

    // IME: composition is drawn by the app (TextPreedit), the candidate list
    // by the IME at the position setTextInput() gave.
    case WM_IME_SETCONTEXT:
        if (wp)
            lp &= ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_IME_STARTCOMPOSITION:
        updateImePosition();
        return 0; // not DefWindowProc: that opens the IME's own composition window
    case WM_IME_COMPOSITION:
        onImeComposition(lp);
        return 0; // not DefWindowProc: that would also send the result as WM_IME_CHAR
    case WM_IME_ENDCOMPOSITION:
        if (_preeditActive) {
            _preeditActive = false;
            send({.type = EventType::TextPreedit});
        }
        return 0;
    case WM_IME_NOTIFY:
        if (wp == IMN_OPENCANDIDATE || wp == IMN_CHANGECANDIDATE)
            updateImePosition();
        break;

    case WM_DROPFILES:
        onDropFiles(HDROP(wp));
        return 0;

    // OS modal loops: our pump is not running while these last.
    case WM_ENTERSIZEMOVE:
        _inSizeMove = true;
        ++_modalRefs;
        _app->enterModal();
        break;
    case WM_EXITSIZEMOVE:
        _inSizeMove = false;
        if (_modalRefs > 0) {
            --_modalRefs;
            _app->leaveModal();
        }
        break;
    case WM_ENTERMENULOOP:
        ++_modalRefs;
        _app->enterModal();
        break;
    case WM_EXITMENULOOP:
        if (_modalRefs > 0) {
            --_modalRefs;
            _app->leaveModal();
        }
        break;

    // Theme and settings: the same broadcast reaches every top-level window,
    // the system window included, which re-reads them once for all. Only
    // without it (it failed to start) does each window ask; the App compares
    // the new state with the old, so a copy emits nothing twice. A handler
    // may destroy us.
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED: // accent colour
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE: {
        if (_app->systemWindow())
            break;
        if (msg == WM_SETTINGCHANGE)
            _app->settingsChanged();
        std::weak_ptr<char> alive = _alive;
        _app->themeMaybeChanged();
        if (alive.expired())
            return 0;
        break;
    }
    default:
        // Registered messages cannot be case labels.
        if (msg != 0 && msg == _app->taskbarButtonCreatedMsg())
            applyBadge(); // an overlay set before the button existed was lost
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace plat::win32
