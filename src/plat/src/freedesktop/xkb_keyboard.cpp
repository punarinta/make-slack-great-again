#include "freedesktop/xkb_keyboard.h"

#include "core/input.h"
#include "freedesktop/evdev_codes.h"

#include <cstdlib>

namespace plat::linux_input {

namespace {

struct EvdevKey {
    uint16_t code;
    Key      key;
};
// US-position table: the fallback for layouts with no Latin group, and the
// inverse map test injection uses.
constexpr EvdevKey kEvdev[] = {
    {KEY_A, Key::A},
    {KEY_B, Key::B},
    {KEY_C, Key::C},
    {KEY_D, Key::D},
    {KEY_E, Key::E},
    {KEY_F, Key::F},
    {KEY_G, Key::G},
    {KEY_H, Key::H},
    {KEY_I, Key::I},
    {KEY_J, Key::J},
    {KEY_K, Key::K},
    {KEY_L, Key::L},
    {KEY_M, Key::M},
    {KEY_N, Key::N},
    {KEY_O, Key::O},
    {KEY_P, Key::P},
    {KEY_Q, Key::Q},
    {KEY_R, Key::R},
    {KEY_S, Key::S},
    {KEY_T, Key::T},
    {KEY_U, Key::U},
    {KEY_V, Key::V},
    {KEY_W, Key::W},
    {KEY_X, Key::X},
    {KEY_Y, Key::Y},
    {KEY_Z, Key::Z},
    {KEY_0, Key::Num0},
    {KEY_1, Key::Num1},
    {KEY_2, Key::Num2},
    {KEY_3, Key::Num3},
    {KEY_4, Key::Num4},
    {KEY_5, Key::Num5},
    {KEY_6, Key::Num6},
    {KEY_7, Key::Num7},
    {KEY_8, Key::Num8},
    {KEY_9, Key::Num9},
    {KEY_F1, Key::F1},
    {KEY_F2, Key::F2},
    {KEY_F3, Key::F3},
    {KEY_F4, Key::F4},
    {KEY_F5, Key::F5},
    {KEY_F6, Key::F6},
    {KEY_F7, Key::F7},
    {KEY_F8, Key::F8},
    {KEY_F9, Key::F9},
    {KEY_F10, Key::F10},
    {KEY_F11, Key::F11},
    {KEY_F12, Key::F12},
    {KEY_F13, Key::F13},
    {KEY_F14, Key::F14},
    {KEY_F15, Key::F15},
    {KEY_F16, Key::F16},
    {KEY_F17, Key::F17},
    {KEY_F18, Key::F18},
    {KEY_F19, Key::F19},
    {KEY_F20, Key::F20},
    {KEY_F21, Key::F21},
    {KEY_F22, Key::F22},
    {KEY_F23, Key::F23},
    {KEY_F24, Key::F24},
    {KEY_ESC, Key::Escape},
    {KEY_ENTER, Key::Enter},
    {KEY_TAB, Key::Tab},
    {KEY_BACKSPACE, Key::Backspace},
    {KEY_DELETE, Key::Delete},
    {KEY_INSERT, Key::Insert},
    {KEY_HOME, Key::Home},
    {KEY_END, Key::End},
    {KEY_PAGEUP, Key::PageUp},
    {KEY_PAGEDOWN, Key::PageDown},
    {KEY_LEFT, Key::Left},
    {KEY_RIGHT, Key::Right},
    {KEY_UP, Key::Up},
    {KEY_DOWN, Key::Down},
    {KEY_SPACE, Key::Space},
    {KEY_MINUS, Key::Minus},
    {KEY_EQUAL, Key::Equal},
    {KEY_LEFTBRACE, Key::BracketLeft},
    {KEY_RIGHTBRACE, Key::BracketRight},
    {KEY_BACKSLASH, Key::Backslash},
    {KEY_SEMICOLON, Key::Semicolon},
    {KEY_APOSTROPHE, Key::Apostrophe},
    {KEY_GRAVE, Key::Grave},
    {KEY_COMMA, Key::Comma},
    {KEY_DOT, Key::Period},
    {KEY_SLASH, Key::Slash},
    {KEY_CAPSLOCK, Key::CapsLock},
    {KEY_LEFTSHIFT, Key::ShiftLeft},
    {KEY_RIGHTSHIFT, Key::ShiftRight},
    {KEY_LEFTCTRL, Key::ControlLeft},
    {KEY_RIGHTCTRL, Key::ControlRight},
    {KEY_LEFTALT, Key::AltLeft},
    {KEY_RIGHTALT, Key::AltRight},
    {KEY_LEFTMETA, Key::SuperLeft},
    {KEY_RIGHTMETA, Key::SuperRight},
    {KEY_COMPOSE, Key::Menu},
    {KEY_SYSRQ, Key::PrintScreen},
    {KEY_SCROLLLOCK, Key::ScrollLock},
    {KEY_PAUSE, Key::Pause},
    {KEY_NUMLOCK, Key::NumLock},
    {KEY_KP0, Key::Kp0},
    {KEY_KP1, Key::Kp1},
    {KEY_KP2, Key::Kp2},
    {KEY_KP3, Key::Kp3},
    {KEY_KP4, Key::Kp4},
    {KEY_KP5, Key::Kp5},
    {KEY_KP6, Key::Kp6},
    {KEY_KP7, Key::Kp7},
    {KEY_KP8, Key::Kp8},
    {KEY_KP9, Key::Kp9},
    {KEY_KPDOT, Key::KpDecimal},
    {KEY_KPSLASH, Key::KpDivide},
    {KEY_KPASTERISK, Key::KpMultiply},
    {KEY_KPMINUS, Key::KpSubtract},
    {KEY_KPPLUS, Key::KpAdd},
    {KEY_KPENTER, Key::KpEnter},
    {KEY_KPEQUAL, Key::KpEqual},
    {KEY_BACK, Key::Back},
    {KEY_FORWARD, Key::Forward},
};

bool isKeypad(Key k) {
    return k >= Key::Kp0 && k <= Key::KpEqual;
}
bool isPositional(Key k) {
    // Keys whose identity is "what the layout types" — only these consult the
    // keysym; everything else (F-keys, arrows, modifiers, keypad) is physical.
    return (k >= Key::A && k <= Key::Z) || (k >= Key::Num0 && k <= Key::Num9) ||
           (k >= Key::Minus && k <= Key::Slash);
}

// Keys other than the positional ones by what they are called in the
// keymap: on an X server whose keycodes aren't evdev's (OpenBSD's wscons,
// FreeBSD's legacy kbd driver: xfree86 codes) the code says nothing about
// arrows, navigation, the keypad or the right-hand modifiers.
Key nonPositionalFromKeysym(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F24)
        return Key(int(Key::F1) + int(sym - XKB_KEY_F1));
    if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9)
        return Key(int(Key::Kp0) + int(sym - XKB_KEY_KP_0));
    static constexpr struct {
        xkb_keysym_t sym;
        Key          key;
    } kKeys[] = {
        {XKB_KEY_Escape, Key::Escape},
        {XKB_KEY_Return, Key::Enter},
        {XKB_KEY_Tab, Key::Tab},
        {XKB_KEY_ISO_Left_Tab, Key::Tab},
        {XKB_KEY_BackSpace, Key::Backspace},
        {XKB_KEY_Delete, Key::Delete},
        {XKB_KEY_Insert, Key::Insert},
        {XKB_KEY_Home, Key::Home},
        {XKB_KEY_End, Key::End},
        {XKB_KEY_Prior, Key::PageUp},
        {XKB_KEY_Next, Key::PageDown},
        {XKB_KEY_Left, Key::Left},
        {XKB_KEY_Right, Key::Right},
        {XKB_KEY_Up, Key::Up},
        {XKB_KEY_Down, Key::Down},
        {XKB_KEY_Caps_Lock, Key::CapsLock},
        {XKB_KEY_Shift_L, Key::ShiftLeft},
        {XKB_KEY_Shift_R, Key::ShiftRight},
        {XKB_KEY_Control_L, Key::ControlLeft},
        {XKB_KEY_Control_R, Key::ControlRight},
        {XKB_KEY_Alt_L, Key::AltLeft},
        {XKB_KEY_Meta_L, Key::AltLeft},
        {XKB_KEY_Alt_R, Key::AltRight},
        {XKB_KEY_Meta_R, Key::AltRight},
        {XKB_KEY_ISO_Level3_Shift, Key::AltRight},
        {XKB_KEY_Super_L, Key::SuperLeft},
        {XKB_KEY_Super_R, Key::SuperRight},
        {XKB_KEY_Menu, Key::Menu},
        {XKB_KEY_Multi_key, Key::Menu},
        {XKB_KEY_Print, Key::PrintScreen},
        {XKB_KEY_Sys_Req, Key::PrintScreen},
        {XKB_KEY_Scroll_Lock, Key::ScrollLock},
        {XKB_KEY_Pause, Key::Pause},
        {XKB_KEY_Num_Lock, Key::NumLock},
        // The keypad's first level, NumLock off: still the digit's key.
        {XKB_KEY_KP_Insert, Key::Kp0},
        {XKB_KEY_KP_End, Key::Kp1},
        {XKB_KEY_KP_Down, Key::Kp2},
        {XKB_KEY_KP_Next, Key::Kp3},
        {XKB_KEY_KP_Left, Key::Kp4},
        {XKB_KEY_KP_Begin, Key::Kp5},
        {XKB_KEY_KP_Right, Key::Kp6},
        {XKB_KEY_KP_Home, Key::Kp7},
        {XKB_KEY_KP_Up, Key::Kp8},
        {XKB_KEY_KP_Prior, Key::Kp9},
        {XKB_KEY_KP_Delete, Key::KpDecimal},
        {XKB_KEY_KP_Decimal, Key::KpDecimal},
        {XKB_KEY_KP_Divide, Key::KpDivide},
        {XKB_KEY_KP_Multiply, Key::KpMultiply},
        {XKB_KEY_KP_Subtract, Key::KpSubtract},
        {XKB_KEY_KP_Add, Key::KpAdd},
        {XKB_KEY_KP_Enter, Key::KpEnter},
        {XKB_KEY_KP_Equal, Key::KpEqual},
    };
    for (const auto &k : kKeys)
        if (k.sym == sym)
            return k.key;
    return Key::Unknown;
}

// The highest keycode that means the same key in evdev and xfree86 keymaps
// (Escape through F12, the main block and the keypad's digits); above it
// they part ways.
constexpr uint32_t kLastSharedKeycode = 96;

const char *localeForCompose() {
    for (const char *v : {"LC_ALL", "LC_CTYPE", "LANG"})
        if (const char *s = std::getenv(v); s && *s)
            return s;
    return "C";
}

} // namespace

Key keyFromEvdev(uint32_t evdev) {
    for (const auto &e : kEvdev)
        if (e.code == evdev)
            return e.key;
    return Key::Unknown;
}

uint32_t evdevFromKey(Key k) {
    for (const auto &e : kEvdev)
        if (e.key == k)
            return e.code;
    return 0;
}

Key keyFromKeysym(xkb_keysym_t sym) {
    // Latin-1 keysyms are their code points.
    return sym == XKB_KEY_space ? Key::Space : sym < 0x80 ? core::keyFromAscii(sym) : Key::Unknown;
}

XkbKeyboard::XkbKeyboard() {
    _ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

void XkbKeyboard::loadCompose() {
    // Deferred to the first key press: compiling the locale table costs ~3 ms
    // and ~140 KB, and which keysyms start a sequence is only known from the
    // table itself (locales and ~/.XCompose start them with arbitrary keys).
    _composeTried = true;
    if (!_ctx)
        return;
    _compose =
        xkb_compose_table_new_from_locale(_ctx, localeForCompose(), XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!_compose)
        _compose = xkb_compose_table_new_from_locale(_ctx, "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (_compose)
        _composeState = xkb_compose_state_new(_compose, XKB_COMPOSE_STATE_NO_FLAGS);
}

XkbKeyboard::~XkbKeyboard() {
    xkb_compose_state_unref(_composeState);
    xkb_compose_table_unref(_compose);
    xkb_state_unref(_state);
    xkb_keymap_unref(_keymap);
    xkb_context_unref(_ctx);
}

void XkbKeyboard::setKeymap(xkb_keymap *keymap, xkb_state *state) {
    xkb_state_unref(_state);
    xkb_keymap_unref(_keymap);
    _keymap = keymap;
    _state  = keymap ? (state ? state : xkb_state_new(keymap)) : nullptr;
    if (!_keymap)
        return;
    _shift                 = xkb_keymap_mod_get_index(_keymap, XKB_MOD_NAME_SHIFT);
    _ctrl                  = xkb_keymap_mod_get_index(_keymap, XKB_MOD_NAME_CTRL);
    _alt                   = xkb_keymap_mod_get_index(_keymap, XKB_MOD_NAME_ALT);
    _super                 = xkb_keymap_mod_get_index(_keymap, XKB_MOD_NAME_LOGO);
    _caps                  = xkb_keymap_mod_get_index(_keymap, XKB_MOD_NAME_CAPS);
    _num                   = xkb_keymap_mod_get_index(_keymap, XKB_MOD_NAME_NUM);
    // evdev's Up is 103 + 8; a keymap that names Up but puts it elsewhere
    // numbers its keys some other way (xfree86's is 98).
    const xkb_keycode_t up = xkb_keymap_key_by_name(_keymap, "UP");
    _evdevCodes            = up == XKB_KEYCODE_INVALID || up == KEY_UP + 8;
    if (_composeState)
        xkb_compose_state_reset(_composeState);
}

void XkbKeyboard::updateMask(
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group
) {
    if (_state)
        xkb_state_update_mask(_state, depressed, latched, locked, 0, 0, group);
}

void XkbKeyboard::updateMask(
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t baseGroup,
    uint32_t latchedGroup,
    uint32_t lockedGroup
) {
    if (_state)
        xkb_state_update_mask(
            _state, depressed, latched, locked, baseGroup, latchedGroup, lockedGroup
        );
}

uint32_t XkbKeyboard::mods() const {
    if (!_state)
        return 0;
    auto on = [this](xkb_mod_index_t i) {
        return i != XKB_MOD_INVALID &&
               xkb_state_mod_index_is_active(_state, i, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    uint32_t m = 0;
    if (on(_shift))
        m |= ModShift;
    if (on(_ctrl))
        m |= ModCtrl;
    if (on(_alt))
        m |= ModAlt;
    if (on(_super))
        m |= ModSuper;
    if (on(_caps))
        m |= ModCaps;
    if (on(_num))
        m |= ModNum;
    return m;
}

bool XkbKeyboard::repeats(uint32_t keycode) const {
    return _keymap && xkb_keymap_key_repeats(_keymap, keycode);
}

Key XkbKeyboard::keyFor(uint32_t keycode) const {
    Key                      physical = keyFromEvdev(keycode - 8);
    const xkb_layout_index_t active   = _state ? xkb_state_key_get_layout(_state, keycode) : 0;
    if (_keymap && !_evdevCodes) {
        // Not evdev's numbers: what isn't typed text is known by its
        // level-0 symbol, and the code means its US key only where the two
        // numberings agree.
        const xkb_keysym_t *syms = nullptr;
        const int count = xkb_keymap_key_get_syms_by_level(_keymap, keycode, active, 0, &syms);
        for (int s = 0; s < count; ++s)
            if (const Key k = nonPositionalFromKeysym(syms[s]); k != Key::Unknown)
                return k;
        if (keycode > kLastSharedKeycode)
            physical = Key::Unknown;
    }
    if (!_keymap || (physical != Key::Unknown && !isPositional(physical)) || isKeypad(physical))
        return physical;
    // Level-0 symbol of the active layout first, then every other layout the
    // user has, so Ctrl+C on a Russian+US setup still means C.
    const xkb_layout_index_t n = xkb_keymap_num_layouts_for_key(_keymap, keycode);
    for (xkb_layout_index_t i = 0; i <= n; ++i) {
        const xkb_layout_index_t layout = i == 0 ? active : i - 1;
        if (i > 0 && layout == active)
            continue;
        const xkb_keysym_t *syms = nullptr;
        const int count = xkb_keymap_key_get_syms_by_level(_keymap, keycode, layout, 0, &syms);
        for (int s = 0; s < count; ++s)
            if (Key k = keyFromKeysym(syms[s]); k != Key::Unknown)
                return k;
    }
    return physical;
}

XkbKeyboard::Result XkbKeyboard::key(uint32_t keycode, bool down, bool updateState) {
    Result r;
    r.key  = keyFor(keycode);
    r.mods = mods();
    if (!_state)
        return r;

    if (down) {
        const xkb_keysym_t sym      = xkb_state_key_get_one_sym(_state, keycode);
        bool               composed = false;
        if (!_composeTried && sym != XKB_KEY_NoSymbol)
            loadCompose();
        if (_composeState && sym != XKB_KEY_NoSymbol &&
            xkb_compose_state_feed(_composeState, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
            switch (xkb_compose_state_get_status(_composeState)) {
            case XKB_COMPOSE_COMPOSING:
                r.composing = true;
                break;
            case XKB_COMPOSE_COMPOSED: {
                char      buf[64];
                const int n = xkb_compose_state_get_utf8(_composeState, buf, sizeof buf);
                if (n > 0)
                    r.text.assign(buf, size_t(n));
                xkb_compose_state_reset(_composeState);
                composed = true;
                break;
            }
            case XKB_COMPOSE_CANCELLED:
                xkb_compose_state_reset(_composeState);
                composed = true; // the cancelling key types nothing
                break;
            case XKB_COMPOSE_NOTHING:
                break;
            }
        }
        if (!r.composing && !composed && !(r.mods & (ModCtrl | ModSuper))) {
            char      buf[64];
            const int n = xkb_state_key_get_utf8(_state, keycode, buf, sizeof buf);
            if (n > 0) {
                const auto c = (unsigned char)buf[0];
                if (!(n == 1 && (c < 0x20 || c == 0x7f))) // Enter/Tab/Backspace are keys, not text
                    r.text.assign(buf, size_t(n));
            }
        }
    }
    if (updateState)
        xkb_state_update_key(_state, keycode, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    return r;
}

} // namespace plat::linux_input
