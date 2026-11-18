// xkbcommon keyboard translation shared by the Wayland and X11 backends:
// keycode → plat::Key (with the Latin-layout fallback for shortcuts), the
// modifier mask, and committed text with dead-key/Compose handling.
// Keycodes are xkb keycodes: evdev + 8 on Wayland and on most X servers; an
// X server with xfree86 keycodes (OpenBSD's) is told apart by its keymap.
#pragma once

#include "plat/plat.h"

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

namespace plat::linux_input {

class XkbKeyboard {
public:
    XkbKeyboard();
    ~XkbKeyboard();
    XkbKeyboard(const XkbKeyboard &)            = delete;
    XkbKeyboard &operator=(const XkbKeyboard &) = delete;

    xkb_context *context() const { return _ctx; }

    // Takes ownership of keymap and builds a fresh state for it. For X11 pass
    // an already-created state from xkb_x11_state_new_from_device (then
    // updateMask keeps it in sync); for Wayland pass null.
    void setKeymap(xkb_keymap *keymap, xkb_state *state = nullptr);
    bool hasKeymap() const { return _state != nullptr; }

    void updateMask(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    // X11 XkbStateNotify carries base/latched/locked groups separately.
    void updateMask(
        uint32_t depressed,
        uint32_t latched,
        uint32_t locked,
        uint32_t baseGroup,
        uint32_t latchedGroup,
        uint32_t lockedGroup
    );

    struct Result {
        Key         key  = Key::Unknown;
        uint32_t    mods = 0;          // plat::Mod bits, as they were *before* this key
        std::string text;              // committed UTF-8; empty for chords/modifiers/dead keys
        bool        composing = false; // a dead key/Compose sequence is in progress
    };
    // Translate a press/release. Only updates the xkb state itself when the
    // backend has no server-side state (Wayland sends modifiers separately;
    // pass updateState=false there).
    Result key(uint32_t keycode, bool down, bool updateState);

    uint32_t mods() const;
    bool     repeats(uint32_t keycode) const;

    // Test/debug helpers: the logical key for a keycode ignoring modifiers.
    Key keyFor(uint32_t keycode) const;

private:
    void loadCompose();

    xkb_context       *_ctx          = nullptr;
    xkb_keymap        *_keymap       = nullptr;
    xkb_state         *_state        = nullptr;
    xkb_compose_table *_compose      = nullptr;
    xkb_compose_state *_composeState = nullptr;
    bool               _composeTried = false;
    bool               _evdevCodes   = true; // the keymap numbers keys as evdev + 8
    xkb_mod_index_t    _shift = XKB_MOD_INVALID, _ctrl = XKB_MOD_INVALID, _alt = XKB_MOD_INVALID,
                       _super = XKB_MOD_INVALID, _caps = XKB_MOD_INVALID, _num = XKB_MOD_INVALID;
};

// keysym → Key, no layout fallback (Unknown for non-Latin letters).
Key      keyFromKeysym(xkb_keysym_t sym);
// Evdev (keycode - 8) → Key by US position; the last-resort fallback and the
// inverse used by test injection (virtual keyboards / XTEST take evdev codes).
Key      keyFromEvdev(uint32_t evdev);
uint32_t evdevFromKey(Key k);

} // namespace plat::linux_input
