// Test-only hooks: synthesise input through the real OS path (XTEST,
// SendInput, CGEventPost, wlroots virtual-keyboard/pointer) so the selftest
// exercises the same code a user's keystroke does. Not for product code.
#pragma once

#include "plat/plat.h"

namespace plat {

class TestHooks {
public:
    virtual ~TestHooks()                                            = default;
    // Each returns false when the backend cannot do it (the test then SKIPs).
    virtual bool injectKey(Window &w, Key k, bool down)             = 0;
    virtual bool injectPointerMove(Window &w, Point logical)        = 0;
    virtual bool injectButton(Window &w, Button b, bool down)       = 0;
    virtual bool injectScroll(Window &w, double dx, double dy)      = 0;
    // Reads back one on-screen pixel (0xAARRGGBB) at a physical position in
    // the window, as the OS composited it — proves present() really reached
    // the screen.
    virtual bool readPixel(Window &w, int x, int y, uint32_t *argb) = 0;
    // False when readPixel can only return what we handed the compositor
    // (macOS without Screen Recording permission) — then a readback proves
    // the buffer, not the screen, and the selftest SKIPs instead of passing.
    virtual bool readsComposited() const { return true; }

    // ── Outside the window ──────────────────────────────────────────────────
    // Each goes through the OS's own path where one exists (a D-Bus call to
    // our StatusNotifierItem, the notify-icon callback message, the status
    // item's button) so the selftest proves the wiring, not just our code.
    virtual bool trayActivate(Tray &t) { return false; }
    virtual bool trayMenuSelect(Tray &t, uint32_t itemId) { return false; }
    // What the tray host actually received, read back from the OS side.
    struct TrayProbe {
        std::vector<Size>        iconSizes;
        std::string              tooltip;
        std::vector<std::string> menuLabels; // flattened, depth-first, separators as "-"
        bool isTemplate = false;             // macOS (Tray::setTemplate); headless records it
    };
    virtual bool trayProbe(Tray &t, TrayProbe *out) { return false; }

    // Click a shown notification (action "" = body) as the user would; false
    // when there is no such action or the OS rendered no buttons.
    virtual bool notificationInvoke(uint64_t id, std::string_view action) { return false; }
    // What the notification server received for id, read back from its side.
    struct NotificationProbe {
        std::string              title, body;
        Size                     imageSize;
        std::vector<std::string> actionLabels;
        bool                     silent = false; // headless only: Notification::silent
    };
    virtual bool     notificationProbe(uint64_t id, NotificationProbe *out) { return false; }
    // The highest id notify() has been called with so far (0 = none yet).
    // Ids are caller-chosen now (see plat::App::notify), not a small backend
    // counter a test could just scan from 1 -- this is how a test finds
    // "the one just posted" instead.
    virtual uint64_t maxNotificationId() const { return 0; }
    // How many are currently known (posted, not yet closed) -- same reason.
    virtual size_t   notificationCount() const { return 0; }

    // Touchpad input the OS can synthesise (gestures, phased scrolling).
    virtual bool injectGesture(Window &w, Gesture g, int fingers, double dx, double dy) {
        return false;
    }
    virtual bool injectPhasedScroll(Window &w, double dx, double dy, ScrollPhase phase) {
        return false;
    }

    // Launcher/taskbar/Dock badge as the OS shows it; -1 if unreadable.
    virtual int  badgeCount() { return -1; }
    // Whether the window is currently flagged as wanting attention.
    virtual bool wantsAttention(Window &w, bool *out) { return false; }

    // ── Round 3 ─────────────────────────────────────────────────────────────
    // Make the next showFileDialog() answer with `paths` (empty = cancel), via
    // the real dialog where possible (portal fake, keystrokes into the dialog,
    // the panel's own OK). False if the backend cannot drive its dialog.
    virtual bool fileDialogRespond(std::vector<std::string> paths) { return false; }
    // Backends that fake their dialog (headless): make showFileDialogEx()
    // answer Unavailable while `on` is false, and report the last request.
    virtual bool setFileDialogAvailable(bool on) { return false; }
    virtual bool lastFileDialog(FileDialogDesc *out) { return false; }
    // Deliver an OS-level signal through its real path where one exists (fake
    // NetworkManager/logind on a private bus, WM_POWERBROADCAST messages,
    // NSWorkspace notifications). type is NetworkChanged (with online),
    // Suspending or Resumed.
    virtual bool simulateSystemEvent(EventType type, bool online) { return false; }
    // Deliver `url` the way the OS does for a registered scheme (macOS: a
    // real kAEGetURL Apple Event to ourselves). False where a URL can only
    // arrive through a new process (use a second instance instead).
    virtual bool deliverUrl(std::string_view url) { return false; }
    // The last URL App::openUrl() was asked to open (headless records them).
    virtual bool lastOpenedUrl(std::string *out) { return false; }
};

} // namespace plat
