// XkbKeyboard against real keymaps compiled from xkeyboard-config names —
// the same data a compositor/X server hands us.
#include "freedesktop/xkb_keyboard.h"
#include "test_util.h"

#include "freedesktop/evdev_codes.h"

using namespace plat;
using namespace plat::linux_input;
using plat_test::runCase;
using plat_test::skip;

namespace {

bool load(XkbKeyboard &kb, const char *layout, const char *variant = "", const char *options = "") {
    xkb_rule_names names{"evdev", "pc105", layout, variant, options};
    xkb_keymap *km = xkb_keymap_new_from_names(kb.context(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km)
        return false;
    kb.setKeymap(km);
    return true;
}

constexpr uint32_t kc(uint32_t evdev) {
    return evdev + 8;
}

std::string type(XkbKeyboard &kb, std::initializer_list<uint32_t> evdevs) {
    std::string out;
    for (uint32_t e : evdevs) {
        out += kb.key(kc(e), true, true).text;
        kb.key(kc(e), false, true);
    }
    return out;
}

void testUsLetters() {
    XkbKeyboard kb;
    if (!load(kb, "us")) {
        skip("no xkeyboard-config data");
        return;
    }
    CHECK(type(kb, {KEY_H, KEY_I}) == "hi");
    kb.key(kc(KEY_LEFTSHIFT), true, true);
    auto r = kb.key(kc(KEY_A), true, true);
    CHECK(r.key == Key::A);
    CHECK(r.text == "A");
    CHECK(r.mods & ModShift);
    kb.key(kc(KEY_A), false, true);
    kb.key(kc(KEY_LEFTSHIFT), false, true);
    CHECK(!(kb.mods() & ModShift));
}

void testCtrlChordHasNoText() {
    XkbKeyboard kb;
    if (!load(kb, "us")) {
        skip("no xkeyboard-config data");
        return;
    }
    kb.key(kc(KEY_LEFTCTRL), true, true);
    auto r = kb.key(kc(KEY_C), true, true);
    CHECK(r.key == Key::C);
    CHECK(r.mods & ModCtrl);
    CHECK(r.text.empty());
}

void testControlKeysAreNotText() {
    XkbKeyboard kb;
    if (!load(kb, "us")) {
        skip("no xkeyboard-config data");
        return;
    }
    for (uint32_t e : {KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_ESC, KEY_DELETE})
        CHECK(kb.key(kc(e), true, true).text.empty());
    CHECK(kb.keyFor(kc(KEY_ENTER)) == Key::Enter);
    CHECK(kb.keyFor(kc(KEY_F5)) == Key::F5);
}

void testDvorakUsesLayoutForLetters() {
    XkbKeyboard kb;
    if (!load(kb, "us", "dvorak")) {
        skip("no dvorak layout");
        return;
    }
    // Dvorak's "j" sits on the US "c" key: the logical key is J.
    CHECK(kb.keyFor(kc(KEY_C)) == Key::J);
    CHECK(type(kb, {KEY_C}) == "j");
    // …but F-keys and arrows stay physical.
    CHECK(kb.keyFor(kc(KEY_LEFT)) == Key::Left);
}

void testCyrillicFallsBackToLatinLayout() {
    XkbKeyboard kb;
    if (!load(kb, "ru,us")) {
        skip("no ru layout");
        return;
    }
    // Russian types "с" on the C key, but Ctrl+C must still be Key::C.
    CHECK(type(kb, {KEY_C}) == "с");
    CHECK(kb.keyFor(kc(KEY_C)) == Key::C);
}

void testCyrillicAloneFallsBackToUsPosition() {
    XkbKeyboard kb;
    if (!load(kb, "ru")) {
        skip("no ru layout");
        return;
    }
    CHECK(kb.keyFor(kc(KEY_Q)) == Key::Q);
}

void testDeadKeyCompose() {
    XkbKeyboard kb;
    if (!load(kb, "us", "intl")) {
        skip("no us(intl) layout");
        return;
    }
    // us(intl): apostrophe is dead_acute; ' then e → é.
    auto dead = kb.key(kc(KEY_APOSTROPHE), true, true);
    kb.key(kc(KEY_APOSTROPHE), false, true);
    if (!dead.composing) {
        skip("compose table unavailable for this locale");
        return;
    }
    CHECK(dead.text.empty());
    auto e = kb.key(kc(KEY_E), true, true);
    CHECK(e.text == "é");
}

// A keymap whose codes aren't evdev's (one made up to collide with evdev
// letters, then xkeyboard-config's xfree86 one, OpenBSD's): what isn't text
// is known by its symbol.
void testNonEvdevNavigation() {
    XkbKeyboard kb;
    const char *map = R"(xkb_keymap {
        xkb_keycodes { minimum = 8; maximum = 255; <UP> = 38; <LEFT> = 39; <KP1> = 40; };
        xkb_types { type "ONE_LEVEL" { modifiers = none; map[None] = Level1; }; };
        xkb_compatibility {};
        xkb_symbols {
            key <UP> { [ Up ] }; key <LEFT> { [ Left ] }; key <KP1> { [ KP_End ] };
        };
    };)";
    auto       *km  = xkb_keymap_new_from_string(
        kb.context(), map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    CHECK(km != nullptr);
    if (!km)
        return;
    kb.setKeymap(km);
    CHECK(kb.keyFor(38) == Key::Up); // evdev's A
    CHECK(kb.keyFor(39) == Key::Left);
    CHECK(kb.keyFor(40) == Key::Kp1);
}

void testXfree86Keymap() {
    XkbKeyboard    kb;
    xkb_rule_names names{"base", "pc105", "us", "", ""};
    xkb_keymap *km = xkb_keymap_new_from_names(kb.context(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        skip("no xkeyboard-config data");
        return;
    }
    const auto code = [km](const char *name) { return xkb_keymap_key_by_name(km, name); };
    if (code("UP") == kc(KEY_UP)) {
        xkb_keymap_unref(km);
        skip("the base rules give evdev codes here");
        return;
    }
    kb.setKeymap(km);
    const struct {
        const char *name;
        Key         key;
    } keys[] = {
        {"UP", Key::Up},
        {"DOWN", Key::Down},
        {"LEFT", Key::Left},
        {"RGHT", Key::Right},
        {"PGUP", Key::PageUp},
        {"PGDN", Key::PageDown},
        {"HOME", Key::Home},
        {"END", Key::End},
        {"INS", Key::Insert},
        {"DELE", Key::Delete},
        {"KPEN", Key::KpEnter},
        {"KPDV", Key::KpDivide},
        {"RCTL", Key::ControlRight},
        {"FK05", Key::F5},
        {"FK11", Key::F11},
        {"ESC", Key::Escape},
        {"RTRN", Key::Enter},
        {"KP7", Key::Kp7},
        {"AC01", Key::A},
        {"AE01", Key::Num1},
        {"AB03", Key::C},
    };
    for (const auto &k : keys) {
        const xkb_keycode_t c = code(k.name);
        CHECK(c != XKB_KEYCODE_INVALID);
        CHECK(kb.keyFor(c) == k.key);
    }
    // Text still comes from the keymap.
    const uint32_t a = code("AC01");
    CHECK(kb.key(a, true, true).text == "a");
    kb.key(a, false, true);
}

void testEvdevRoundTrip() {
    for (int k = int(Key::A); k < int(Key::Count); ++k) {
        const uint32_t code = evdevFromKey(Key(k));
        if (code)
            CHECK(keyFromEvdev(code) == Key(k));
    }
    CHECK(evdevFromKey(Key::A) == KEY_A);
}

} // namespace

int main() {
    runCase("us: letters, shift, mods", testUsLetters);
    runCase("ctrl chord: key C, no text", testCtrlChordHasNoText);
    runCase("Enter/Tab/Backspace are keys, not text", testControlKeysAreNotText);
    runCase("dvorak: letter keys follow the layout", testDvorakUsesLayoutForLetters);
    runCase("ru,us: Ctrl+C resolves through the Latin group", testCyrillicFallsBackToLatinLayout);
    runCase("ru only: falls back to US position", testCyrillicAloneFallsBackToUsPosition);
    runCase("dead key + e composes é", testDeadKeyCompose);
    runCase("evdev <-> Key tables are consistent", testEvdevRoundTrip);
    runCase("non-evdev keycodes: keys by their symbols", testNonEvdevNavigation);
    runCase("xfree86 keymap: navigation, keypad, modifiers", testXfree86Keymap);
    return plat_test::summary();
}
