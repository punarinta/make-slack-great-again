// Pure helpers of the Win32 backend: UTF-16 offsets, virtual-key ↔ Key,
// modifier state, file URIs.
#include "win32/win32.h"

#include <cctype>

namespace plat::win32 {

int utf8Length(std::wstring_view w, size_t units) {
    int bytes = 0;
    for (size_t i = 0; i < units && i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (c < 0x80)
            bytes += 1;
        else if (c < 0x800)
            bytes += 2;
        else if (IS_HIGH_SURROGATE(c) && i + 1 < w.size() && IS_LOW_SURROGATE(w[i + 1])) {
            bytes += 4; // the pair is one code point
            ++i;
        } else
            bytes += 3; // BMP, or a lone surrogate (WideCharToMultiByte makes it U+FFFD)
    }
    return bytes;
}

std::string fileUri(std::wstring_view path) {
    // portablePath drops the long-path prefixes (an API detail, not part of
    // the file's name) and turns '\' into '/'.
    const std::string p   = portablePath(path);
    // UNC: the server becomes the URI authority; "C:/x" → "file:///C:/x".
    const bool        unc = p.rfind("//", 0) == 0;
    return (unc ? "file://" : "file:///") +
           core::percentEncode(std::string_view(p).substr(unc ? 2 : 0), "/:");
}

std::optional<std::wstring> pathFromFileUri(std::string_view uri) {
    if (uri.size() < 8 || uri.substr(0, 7) != "file://")
        return std::nullopt;
    std::string_view rest = uri.substr(7);
    std::string      raw;
    if (rest.substr(0, 1) == "/") {
        // file:///C:/x — only a drive path is a Windows file; /tmp/x is not.
        rest = rest.substr(1);
        if (rest.size() < 2 || !std::isalpha((unsigned char)rest[0]) ||
            (rest[1] != ':' && rest[1] != '|'))
            return std::nullopt;
    } else if (rest.substr(0, 10) == "localhost/") {
        return pathFromFileUri(std::string("file:///") + std::string(rest.substr(10)));
    } else {
        raw = "\\\\"; // UNC: the authority is the server
    }
    // Separators first (a decoded %2F stays a '/'), up to any query/fragment.
    std::string path(rest.substr(0, rest.find_first_of("?#")));
    for (char &c : path)
        if (c == '/')
            c = '\\';
    raw += core::percentDecode(path);
    if (raw.size() >= 2 && raw[1] == '|')
        raw[1] = ':';
    if (raw.find('\0') != std::string::npos)
        return std::nullopt;
    return toWide(raw);
}

namespace {

// Punctuation keys resolve through the active layout: VK_OEM_* codes are
// assigned per layout, so ask what the key types unshifted and name it by
// that; fall back to the US position when it types something that has no Key
// (a letter such as ö, a dead key's base).
Key keyFromChar(wchar_t c) {
    return core::keyFromPunctuation(uint32_t(c));
}

Key oemKeyUsPosition(UINT vk) {
    switch (vk) {
    case VK_OEM_1:
        return Key::Semicolon;
    case VK_OEM_PLUS:
        return Key::Equal;
    case VK_OEM_COMMA:
        return Key::Comma;
    case VK_OEM_MINUS:
        return Key::Minus;
    case VK_OEM_PERIOD:
        return Key::Period;
    case VK_OEM_2:
        return Key::Slash;
    case VK_OEM_3:
        return Key::Grave;
    case VK_OEM_4:
        return Key::BracketLeft;
    case VK_OEM_5:
        return Key::Backslash;
    case VK_OEM_6:
        return Key::BracketRight;
    case VK_OEM_7:
        return Key::Apostrophe;
    case VK_OEM_102:
        return Key::Backslash; // ISO <> key; no Key of its own
    default:
        return Key::Unknown;
    }
}

} // namespace

Key keyFromVk(UINT vk, bool ext, UINT scan) {
    // Letter VKs follow the layout (AZERTY's A key is VK_A), and non-Latin
    // layouts (Russian, Greek) keep US-position Latin VKs — exactly the
    // "first Latin layout, else US position" rule plat.h asks for.
    if (vk >= 'A' && vk <= 'Z')
        return Key(int(Key::A) + int(vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return Key(int(Key::Num0) + int(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F24)
        return Key(int(Key::F1) + int(vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return Key(int(Key::Kp0) + int(vk - VK_NUMPAD0));
    switch (vk) {
    case VK_ESCAPE:
        return Key::Escape;
    case VK_RETURN:
        return ext ? Key::KpEnter : Key::Enter;
    case VK_TAB:
        return Key::Tab;
    case VK_BACK:
        return Key::Backspace;
    // With NumLock off the keypad sends the navigation VKs without the
    // extended bit; they act as navigation keys, so they are named so
    // (scancode keeps the physical key).
    case VK_DELETE:
        return Key::Delete;
    case VK_INSERT:
        return Key::Insert;
    case VK_HOME:
        return Key::Home;
    case VK_END:
        return Key::End;
    case VK_PRIOR:
        return Key::PageUp;
    case VK_NEXT:
        return Key::PageDown;
    case VK_LEFT:
        return Key::Left;
    case VK_RIGHT:
        return Key::Right;
    case VK_UP:
        return Key::Up;
    case VK_DOWN:
        return Key::Down;
    case VK_CLEAR:
        return Key::Kp5; // keypad 5 with NumLock off
    case VK_SPACE:
        return Key::Space;
    case VK_CAPITAL:
        return Key::CapsLock;
    // Messages carry the generic VK_SHIFT/CONTROL/MENU; side comes from the
    // scancode (right shift is 0x36) or the extended bit.
    case VK_SHIFT:
        return scan == 0x36 ? Key::ShiftRight : Key::ShiftLeft;
    case VK_LSHIFT:
        return Key::ShiftLeft;
    case VK_RSHIFT:
        return Key::ShiftRight;
    case VK_CONTROL:
        return ext ? Key::ControlRight : Key::ControlLeft;
    case VK_LCONTROL:
        return Key::ControlLeft;
    case VK_RCONTROL:
        return Key::ControlRight;
    case VK_MENU:
        return ext ? Key::AltRight : Key::AltLeft;
    case VK_LMENU:
        return Key::AltLeft;
    case VK_RMENU:
        return Key::AltRight;
    case VK_LWIN:
        return Key::SuperLeft;
    case VK_RWIN:
        return Key::SuperRight;
    case VK_APPS:
        return Key::Menu;
    case VK_SNAPSHOT:
        return Key::PrintScreen;
    case VK_SCROLL:
        return Key::ScrollLock;
    case VK_PAUSE:
        return Key::Pause;
    case VK_NUMLOCK:
        return Key::NumLock;
    case VK_DECIMAL:
        return Key::KpDecimal;
    case VK_DIVIDE:
        return Key::KpDivide;
    case VK_MULTIPLY:
        return Key::KpMultiply;
    case VK_SUBTRACT:
        return Key::KpSubtract;
    case VK_ADD:
        return Key::KpAdd;
    case VK_OEM_NEC_EQUAL:
        return Key::KpEqual;
    case VK_BROWSER_BACK:
        return Key::Back;
    case VK_BROWSER_FORWARD:
        return Key::Forward;
    default:
        break;
    }
    if (const Key us = oemKeyUsPosition(vk); us != Key::Unknown) {
        const UINT ch     = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fff; // high bit = dead key
        const Key  byChar = keyFromChar(wchar_t(ch));
        return byChar != Key::Unknown ? byChar : us;
    }
    return Key::Unknown;
}

#ifdef PLAT_TEST_HOOKS
UINT vkFromKey(Key k, bool *ext) {
    *ext        = false;
    const int i = int(k);
    if (k >= Key::A && k <= Key::Z)
        return UINT('A' + (i - int(Key::A)));
    if (k >= Key::Num0 && k <= Key::Num9)
        return UINT('0' + (i - int(Key::Num0)));
    if (k >= Key::F1 && k <= Key::F24)
        return UINT(VK_F1 + (i - int(Key::F1)));
    if (k >= Key::Kp0 && k <= Key::Kp9)
        return UINT(VK_NUMPAD0 + (i - int(Key::Kp0)));
    auto extended = [&](UINT vk) {
        *ext = true;
        return vk;
    };
    auto oem = [](wchar_t c, UINT usVk) {
        const SHORT r = VkKeyScanW(c); // layout-aware; -1 when the layout lacks it
        return (r != -1 && (r & 0xff00) == 0) ? UINT(r & 0xff) : usVk;
    };
    switch (k) {
    case Key::Escape:
        return VK_ESCAPE;
    case Key::Enter:
        return VK_RETURN;
    case Key::KpEnter:
        return extended(VK_RETURN);
    case Key::Tab:
        return VK_TAB;
    case Key::Backspace:
        return VK_BACK;
    case Key::Delete:
        return extended(VK_DELETE);
    case Key::Insert:
        return extended(VK_INSERT);
    case Key::Home:
        return extended(VK_HOME);
    case Key::End:
        return extended(VK_END);
    case Key::PageUp:
        return extended(VK_PRIOR);
    case Key::PageDown:
        return extended(VK_NEXT);
    case Key::Left:
        return extended(VK_LEFT);
    case Key::Right:
        return extended(VK_RIGHT);
    case Key::Up:
        return extended(VK_UP);
    case Key::Down:
        return extended(VK_DOWN);
    case Key::Space:
        return VK_SPACE;
    case Key::Minus:
        return oem(L'-', VK_OEM_MINUS);
    case Key::Equal:
        return oem(L'=', VK_OEM_PLUS);
    case Key::BracketLeft:
        return oem(L'[', VK_OEM_4);
    case Key::BracketRight:
        return oem(L']', VK_OEM_6);
    case Key::Backslash:
        return oem(L'\\', VK_OEM_5);
    case Key::Semicolon:
        return oem(L';', VK_OEM_1);
    case Key::Apostrophe:
        return oem(L'\'', VK_OEM_7);
    case Key::Grave:
        return oem(L'`', VK_OEM_3);
    case Key::Comma:
        return oem(L',', VK_OEM_COMMA);
    case Key::Period:
        return oem(L'.', VK_OEM_PERIOD);
    case Key::Slash:
        return oem(L'/', VK_OEM_2);
    case Key::CapsLock:
        return VK_CAPITAL;
    case Key::ShiftLeft:
        return VK_LSHIFT;
    case Key::ShiftRight:
        return VK_RSHIFT;
    case Key::ControlLeft:
        return VK_LCONTROL;
    case Key::ControlRight:
        return extended(VK_RCONTROL);
    case Key::AltLeft:
        return VK_LMENU;
    case Key::AltRight:
        return extended(VK_RMENU);
    case Key::SuperLeft:
        return extended(VK_LWIN);
    case Key::SuperRight:
        return extended(VK_RWIN);
    case Key::Menu:
        return extended(VK_APPS);
    case Key::PrintScreen:
        return extended(VK_SNAPSHOT);
    case Key::ScrollLock:
        return VK_SCROLL;
    case Key::Pause:
        return VK_PAUSE;
    case Key::NumLock:
        return extended(VK_NUMLOCK);
    case Key::KpDecimal:
        return VK_DECIMAL;
    case Key::KpDivide:
        return extended(VK_DIVIDE);
    case Key::KpMultiply:
        return VK_MULTIPLY;
    case Key::KpSubtract:
        return VK_SUBTRACT;
    case Key::KpAdd:
        return VK_ADD;
    case Key::KpEqual:
        return VK_OEM_NEC_EQUAL;
    case Key::Back:
        return extended(VK_BROWSER_BACK);
    case Key::Forward:
        return extended(VK_BROWSER_FORWARD);
    default:
        return 0;
    }
}
#endif // PLAT_TEST_HOOKS

uint32_t currentMods() {
    // GetKeyState (not GetAsyncKeyState) is synchronised with the message
    // queue, so it describes the moment the message being handled was sent.
    auto     down = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
    uint32_t m    = 0;
    if (down(VK_SHIFT))
        m |= ModShift;
    if (down(VK_CONTROL))
        m |= ModCtrl;
    if (down(VK_MENU))
        m |= ModAlt;
    if (down(VK_LWIN) || down(VK_RWIN))
        m |= ModSuper;
    if (GetKeyState(VK_CAPITAL) & 1)
        m |= ModCaps;
    if (GetKeyState(VK_NUMLOCK) & 1)
        m |= ModNum;
    return m;
}

} // namespace plat::win32
