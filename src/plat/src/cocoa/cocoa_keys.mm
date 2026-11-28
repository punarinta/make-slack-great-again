// macOS virtual key codes → plat::Key.
//
// kVK_* codes name physical positions on an ANSI keyboard, so a naive table
// would make Cmd+Z on Dvorak fire for the key that types ';'. Printable keys
// are therefore resolved through the active keyboard layout with
// UCKeyTranslate; when that layout types something plat has no Key for
// (Cyrillic, Greek, ö, …) the ASCII-capable layout is tried, and only then
// the US position — plat.h's rule, the usual toolkit one, so shortcuts
// keep working on non-Latin layouts.
#import <Carbon/Carbon.h>

#include "cocoa/cocoa_internal.h"

namespace plat::cocoa {
namespace {

struct VkKey {
    uint16_t vk;
    Key      key;
};

// Printable keys: the US position, used only as the last fallback.
constexpr VkKey kLayoutKeys[] = {
    {kVK_ANSI_A, Key::A},
    {kVK_ANSI_B, Key::B},
    {kVK_ANSI_C, Key::C},
    {kVK_ANSI_D, Key::D},
    {kVK_ANSI_E, Key::E},
    {kVK_ANSI_F, Key::F},
    {kVK_ANSI_G, Key::G},
    {kVK_ANSI_H, Key::H},
    {kVK_ANSI_I, Key::I},
    {kVK_ANSI_J, Key::J},
    {kVK_ANSI_K, Key::K},
    {kVK_ANSI_L, Key::L},
    {kVK_ANSI_M, Key::M},
    {kVK_ANSI_N, Key::N},
    {kVK_ANSI_O, Key::O},
    {kVK_ANSI_P, Key::P},
    {kVK_ANSI_Q, Key::Q},
    {kVK_ANSI_R, Key::R},
    {kVK_ANSI_S, Key::S},
    {kVK_ANSI_T, Key::T},
    {kVK_ANSI_U, Key::U},
    {kVK_ANSI_V, Key::V},
    {kVK_ANSI_W, Key::W},
    {kVK_ANSI_X, Key::X},
    {kVK_ANSI_Y, Key::Y},
    {kVK_ANSI_Z, Key::Z},
    {kVK_ANSI_0, Key::Num0},
    {kVK_ANSI_1, Key::Num1},
    {kVK_ANSI_2, Key::Num2},
    {kVK_ANSI_3, Key::Num3},
    {kVK_ANSI_4, Key::Num4},
    {kVK_ANSI_5, Key::Num5},
    {kVK_ANSI_6, Key::Num6},
    {kVK_ANSI_7, Key::Num7},
    {kVK_ANSI_8, Key::Num8},
    {kVK_ANSI_9, Key::Num9},
    {kVK_ANSI_Minus, Key::Minus},
    {kVK_ANSI_Equal, Key::Equal},
    {kVK_ANSI_LeftBracket, Key::BracketLeft},
    {kVK_ANSI_RightBracket, Key::BracketRight},
    {kVK_ANSI_Backslash, Key::Backslash},
    {kVK_ANSI_Semicolon, Key::Semicolon},
    {kVK_ANSI_Quote, Key::Apostrophe},
    {kVK_ANSI_Grave, Key::Grave},
    {kVK_ANSI_Comma, Key::Comma},
    {kVK_ANSI_Period, Key::Period},
    {kVK_ANSI_Slash, Key::Slash},
    // No US position: resolvable through the layout only.
    {kVK_ISO_Section, Key::Unknown},
    {kVK_JIS_Yen, Key::Unknown},
    {kVK_JIS_Underscore, Key::Unknown},
};

// Keys whose meaning does not depend on the layout.
constexpr VkKey kFixedKeys[] = {
    {kVK_Return, Key::Enter},
    {kVK_Tab, Key::Tab},
    {kVK_Space, Key::Space},
    {kVK_Delete, Key::Backspace},
    {kVK_ForwardDelete, Key::Delete},
    {kVK_Help, Key::Insert}, // Help sits where Insert is on PC keyboards
    {kVK_Escape, Key::Escape},
    {kVK_Command, Key::SuperLeft},
    {kVK_RightCommand, Key::SuperRight},
    {kVK_Shift, Key::ShiftLeft},
    {kVK_RightShift, Key::ShiftRight},
    {kVK_CapsLock, Key::CapsLock},
    {kVK_Option, Key::AltLeft},
    {kVK_RightOption, Key::AltRight},
    {kVK_Control, Key::ControlLeft},
    {kVK_RightControl, Key::ControlRight},
    {0x6E, Key::Menu}, // kVK_ContextualMenu (not in every SDK's Events.h)
    {kVK_Home, Key::Home},
    {kVK_End, Key::End},
    {kVK_PageUp, Key::PageUp},
    {kVK_PageDown, Key::PageDown},
    {kVK_LeftArrow, Key::Left},
    {kVK_RightArrow, Key::Right},
    {kVK_UpArrow, Key::Up},
    {kVK_DownArrow, Key::Down},
    {kVK_F1, Key::F1},
    {kVK_F2, Key::F2},
    {kVK_F3, Key::F3},
    {kVK_F4, Key::F4},
    {kVK_F5, Key::F5},
    {kVK_F6, Key::F6},
    {kVK_F7, Key::F7},
    {kVK_F8, Key::F8},
    {kVK_F9, Key::F9},
    {kVK_F10, Key::F10},
    {kVK_F11, Key::F11},
    {kVK_F12, Key::F12},
    {kVK_F13, Key::F13},
    {kVK_F14, Key::F14},
    {kVK_F15, Key::F15},
    {kVK_F16, Key::F16},
    {kVK_F17, Key::F17},
    {kVK_F18, Key::F18},
    {kVK_F19, Key::F19},
    {kVK_F20, Key::F20},
    {kVK_ANSI_Keypad0, Key::Kp0},
    {kVK_ANSI_Keypad1, Key::Kp1},
    {kVK_ANSI_Keypad2, Key::Kp2},
    {kVK_ANSI_Keypad3, Key::Kp3},
    {kVK_ANSI_Keypad4, Key::Kp4},
    {kVK_ANSI_Keypad5, Key::Kp5},
    {kVK_ANSI_Keypad6, Key::Kp6},
    {kVK_ANSI_Keypad7, Key::Kp7},
    {kVK_ANSI_Keypad8, Key::Kp8},
    {kVK_ANSI_Keypad9, Key::Kp9},
    {kVK_ANSI_KeypadDecimal, Key::KpDecimal},
    {kVK_ANSI_KeypadDivide, Key::KpDivide},
    {kVK_ANSI_KeypadMultiply, Key::KpMultiply},
    {kVK_ANSI_KeypadMinus, Key::KpSubtract},
    {kVK_ANSI_KeypadPlus, Key::KpAdd},
    {kVK_ANSI_KeypadEnter, Key::KpEnter},
    {kVK_ANSI_KeypadEquals, Key::KpEqual},
    {kVK_ANSI_KeypadClear, Key::NumLock}, // Clear occupies the NumLock position
};

Key keyFromChar(UniChar c) {
    return core::keyFromAscii(c);
}

#ifdef PLAT_TEST_HOOKS
// Carbon's modifier state byte for UCKeyTranslate ((EventModifiers >> 8) & 0xff).
UInt32 carbonModState(NSEventModifierFlags f) {
    UInt32 m = 0;
    if (f & NSEventModifierFlagShift)
        m |= shiftKey;
    if (f & NSEventModifierFlagOption)
        m |= optionKey;
    if (f & NSEventModifierFlagControl)
        m |= controlKey;
    if (f & NSEventModifierFlagCommand)
        m |= cmdKey;
    if (f & NSEventModifierFlagCapsLock)
        m |= alphaLock;
    return (m >> 8) & 0xff;
}
#endif

// UTF-16 of what vk types on `source` (dead keys yield their accent).
std::u16string translate(TISInputSourceRef source, uint16_t vk, UInt32 modState) {
    if (!source)
        return {};
    auto data = (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData);
    if (!data) // pure input methods (Kotoeri, Pinyin) carry no layout data
        return {};
    auto           layout = (const UCKeyboardLayout *)CFDataGetBytePtr(data);
    UInt32         dead   = 0;
    UniChar        buf[8] = {};
    UniCharCount   len    = 0;
    const OSStatus err    = UCKeyTranslate(
        layout,
        vk,
        kUCKeyActionDown,
        modState,
        LMGetKbdType(),
        kUCKeyTranslateNoDeadKeysMask,
        &dead,
        std::size(buf),
        &len,
        buf
    );
    if (err != noErr)
        return {};
    return std::u16string(reinterpret_cast<const char16_t *>(buf), len);
}

struct LayoutTable {
    bool valid = false;
    Key  keys[128]; // by vk
};
LayoutTable g_table;

void buildTable() {
    for (auto &k : g_table.keys)
        k = Key::Unknown;
    for (const auto &f : kFixedKeys)
        g_table.keys[f.vk] = f.key;

    TISInputSourceRef          current = TISCopyCurrentKeyboardLayoutInputSource();
    TISInputSourceRef          ascii   = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    bool                       used[size_t(Key::Count)] = {};
    std::vector<const VkKey *> unresolved;
    for (const auto &lk : kLayoutKeys) {
        Key k = Key::Unknown;
        for (TISInputSourceRef src : {current, ascii}) {
            const auto s = translate(src, lk.vk, 0);
            if (s.size() == 1 && (k = keyFromChar(s[0])) != Key::Unknown)
                break;
        }
        if (k == Key::Unknown) {
            unresolved.push_back(&lk);
            continue;
        }
        g_table.keys[lk.vk] = k;
        used[size_t(k)]     = true;
    }
    // US position last, but never for a Key another key of this layout
    // already types (German: '-' lives on the US '/' key, so the US '-' key,
    // which types 'ß', must not also claim Minus).
    for (const VkKey *lk : unresolved) {
        if (lk->key != Key::Unknown && !used[size_t(lk->key)]) {
            g_table.keys[lk->vk]  = lk->key;
            used[size_t(lk->key)] = true;
        }
    }
    if (current)
        CFRelease(current);
    if (ascii)
        CFRelease(ascii);
    g_table.valid = true;
}

const LayoutTable &table() {
    if (!g_table.valid)
        buildTable();
    return g_table;
}

} // namespace

Key keyFromKeyCode(uint16_t vk) {
    return vk < 128 ? table().keys[vk] : Key::Unknown;
}

#ifdef PLAT_TEST_HOOKS
int keyCodeForKey(Key k) {
    if (k == Key::Unknown)
        return -1;
    const auto &t = table();
    for (int vk = 0; vk < 128; ++vk)
        if (t.keys[vk] == k)
            return vk;
    return -1;
}
#endif

uint32_t modsFromFlags(NSEventModifierFlags f) {
    uint32_t m = 0;
    if (f & NSEventModifierFlagShift)
        m |= ModShift;
    if (f & NSEventModifierFlagControl)
        m |= ModCtrl;
    if (f & NSEventModifierFlagOption)
        m |= ModAlt;
    if (f & NSEventModifierFlagCommand)
        m |= ModSuper;
    if (f & NSEventModifierFlagCapsLock)
        m |= ModCaps;
    // Macs have no NumLock state; NSEventModifierFlagNumericPad marks keypad
    // and arrow keys, which is something else.
    return m;
}

#ifdef PLAT_TEST_HOOKS
NSString *charactersForKeyCode(uint16_t vk, NSEventModifierFlags flags) {
    // The window server fills NSEvent.characters for the special keys with
    // the Unicode private-use function-key range (NSUpArrowFunctionKey …) and
    // control characters; mirror the few a test can press.
    switch (vk) {
    case kVK_Return:
        return @"\r";
    case kVK_Tab:
        return @"\t";
    case kVK_Delete:
        return @"\x7f";
    case kVK_Escape:
        return @"\x1b";
    default:
        break;
    }
    TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
    auto              s   = translate(src, vk, carbonModState(flags));
    if (src)
        CFRelease(src);
    return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(s.data())
                                   length:s.size()];
}
#endif

void invalidateKeyboardLayout() {
    g_table.valid = false;
}

} // namespace plat::cocoa
