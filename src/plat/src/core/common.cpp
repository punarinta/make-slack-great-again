#include "core/backends.h"
#include "core/pacing.h"

#include <cstdlib>
#include <cstring>

namespace plat {

namespace {
constexpr const char *kKeyNames[] = {
    "Unknown",   "A",           "B",           "C",
    "D",         "E",           "F",           "G",
    "H",         "I",           "J",           "K",
    "L",         "M",           "N",           "O",
    "P",         "Q",           "R",           "S",
    "T",         "U",           "V",           "W",
    "X",         "Y",           "Z",           "0",
    "1",         "2",           "3",           "4",
    "5",         "6",           "7",           "8",
    "9",         "F1",          "F2",          "F3",
    "F4",        "F5",          "F6",          "F7",
    "F8",        "F9",          "F10",         "F11",
    "F12",       "F13",         "F14",         "F15",
    "F16",       "F17",         "F18",         "F19",
    "F20",       "F21",         "F22",         "F23",
    "F24",       "Escape",      "Enter",       "Tab",
    "Backspace", "Delete",      "Insert",      "Home",
    "End",       "PageUp",      "PageDown",    "Left",
    "Right",     "Up",          "Down",        "Space",
    "Minus",     "Equal",       "BracketLeft", "BracketRight",
    "Backslash", "Semicolon",   "Apostrophe",  "Grave",
    "Comma",     "Period",      "Slash",       "CapsLock",
    "ShiftLeft", "ShiftRight",  "ControlLeft", "ControlRight",
    "AltLeft",   "AltRight",    "SuperLeft",   "SuperRight",
    "Menu",      "PrintScreen", "ScrollLock",  "Pause",
    "NumLock",   "Kp0",         "Kp1",         "Kp2",
    "Kp3",       "Kp4",         "Kp5",         "Kp6",
    "Kp7",       "Kp8",         "Kp9",         "KpDecimal",
    "KpDivide",  "KpMultiply",  "KpSubtract",  "KpAdd",
    "KpEnter",   "KpEqual",     "Back",        "Forward",
};
static_assert(std::size(kKeyNames) == size_t(Key::Count), "kKeyNames out of sync with Key");
} // namespace

const char *keyName(Key k) {
    const auto i = size_t(k);
    return i < std::size(kKeyNames) ? kKeyNames[i] : "Unknown";
}

uint32_t primaryMod() {
#if defined(__APPLE__)
    return ModSuper;
#else
    return ModCtrl;
#endif
}

void App::showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb) {
    // Backends whose dialog is always there (panels, IFileDialog) cannot
    // tell a cancel from anything else: empty = Cancelled.
    showFileDialog(d, [cb = std::move(cb)](std::vector<std::string> paths) {
        FileDialogResult r;
        r.status =
            paths.empty() ? FileDialogResult::Status::Cancelled : FileDialogResult::Status::Chosen;
        r.paths = std::move(paths);
        cb(std::move(r));
    });
}

std::vector<std::string> App::preferredLanguages() const {
    // gettext's order, simplified: a "C"/"POSIX" locale means untranslated
    // and disables $LANGUAGE.
    const char *locale = nullptr;
    for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char *v = std::getenv(var); v && *v) {
            locale = v;
            break;
        }
    std::vector<std::string> out;
    if (!locale || std::strcmp(locale, "C") == 0 || std::strcmp(locale, "POSIX") == 0)
        return out;
    if (const char *list = std::getenv("LANGUAGE")) {
        for (const char *p = list; *p;) {
            const char  *e = std::strchr(p, ':');
            const size_t n = e ? size_t(e - p) : std::strlen(p);
            if (n)
                out.emplace_back(p, n);
            p += n + (e ? 1 : 0);
        }
    }
    out.emplace_back(locale);
    return out;
}

std::unique_ptr<App> App::create(std::string *error) {
    const char           *forced = std::getenv("PLAT_BACKEND");
    [[maybe_unused]] auto is     = [&](const char *name) {
        return forced && std::strcmp(forced, name) == 0;
    };

#if defined(PLAT_PORT)
    // A port build has one backend; $PLAT_BACKEND (the tests' "headless")
    // has nothing to choose between.
    (void)is;
    return createPortApp(error);
#else
#ifdef PLAT_TEST_HOOKS
    if (is("headless"))
        return createHeadlessApp(error);
#endif
#if defined(_WIN32)
    return createWin32App(error);
#elif defined(__APPLE__)
    return createCocoaApp(error);
#else
    std::string why;
#if defined(PLAT_HAS_WAYLAND)
    if (is("wayland") || (!forced && std::getenv("WAYLAND_DISPLAY"))) {
        if (auto app = createWaylandApp(&why))
            return app;
        if (forced) {
            if (error)
                *error = why;
            return nullptr;
        }
    }
#endif
#if defined(PLAT_HAS_X11)
    if (!forced || is("x11")) {
        std::string x11why;
        if (auto app = createX11App(&x11why))
            return app;
        why += (why.empty() ? "" : "; ") + x11why;
    }
#endif
    if (error)
        *error = why.empty() ? "no usable backend" : why;
    return nullptr;
#endif
#endif
}

} // namespace plat

namespace plat::core {

int frameIntervalMs(int refreshMilliHz) {
    const int ms = refreshMilliHz > 0 ? 1000000 / refreshMilliHz : 16;
    return ms < 2 ? 2 : ms > 50 ? 50 : ms;
}

} // namespace plat::core
