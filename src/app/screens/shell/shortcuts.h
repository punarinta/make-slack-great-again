// Every keyboard binding of the app in one table (actions, keys, labels and
// order). Three consumers read
// it, so they can never drift apart: the code that reacts to the key, the
// tooltips that advertise it ("Bold (Ctrl+B)"), and the "Keyboard shortcuts"
// panel shown while no conversation is open.
//
// Scopes:
//  - Window: installed on the window (install()), fire unless the focused
//    view consumed the key first.
//  - Composer: matched by the composer's own key handler (matches()): they
//    act on that editor and must not fire while focus is elsewhere.
//  - Documented: implemented by whatever owns the plain key (Enter, Up,
//    Escape, Shift+Del), listed so the help panel and tooltips read one table.
//
// "Ctrl" in the table means the platform's command key: Cmd on macOS,
// Control elsewhere (a portable Ctrl).
#pragma once

#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace shell::shortcuts {

enum class Id : uint8_t {
    // ── Window scope ────────────────────────────────────────────────────────
    NavBack,
    NavForward,
    CloseFrontmost,
    CloseThread, // Esc, after the focused view passes on it
    SearchMessages,
    QuickSwitch,
    OpenSettings,
    // ── Composer scope ──────────────────────────────────────────────────────
    Bold,
    Italic,
    Underline,
    Strikethrough,
    InlineCode,
    CodeBlock,
    Link,
    AttachFile,
    EmojiPicker,
    OrderedList,
    BulletList,
    Quote,
    UndoSend,
    SearchPromptHistory,
    VoiceInput,
    // ── Documented ──────────────────────────────────────────────────────────
    // SendMessage/NewLine swap with "send with Ctrl+Enter" (setCtrlEnterSends):
    // Enter sends by default, only Ctrl+Enter when it is on, and the other
    // Enter inserts a newline. Ctrl+Enter sends in both modes.
    SendMessage,
    NewLine,
    EditLastMessage,
    CancelOrExitEdit,
    // Filtered window-wide ahead of the focused view (the composer keeps focus
    // and would take Shift+Del as a delete).
    RemoveIdleSession,
    Count
};

enum class Scope : uint8_t { Window, Composer, Documented };

// Portable modifiers. Ctrl = Cmd on macOS.
enum Mod : uint8_t { Shift = 1, Ctrl = 2, Alt = 4 };

struct Keys {
    plat::Key key  = plat::Key::Unknown;
    uint8_t   mods = 0;
};

struct Def {
    Id          id;
    Scope       scope;
    bool        inHelp;  // a row in the shortcuts panel
    const char *label;   // "Jump to a conversation"; untranslated, tr() it
    Keys        keys[2]; // primary, then an alternate (Unknown = none)
};

// The table, in the panel's order (inHelp rows top to bottom).
const Def *table();
size_t     tableSize();
const Def &def(Id id);

// Every effective binding of `id` (the send/newline swap and the per-OS
// close keys applied); the first is the primary one. Returns how many.
size_t bindings(Id id, Keys out[2]);

// True when a KeyDown is a press of this binding: modifiers exactly as bound
// (Caps/Num Lock ignored); the keypad Enter counts as Enter.
bool matches(Id id, const ui::Event &e);

// The primary binding for display. keyChips: one label per key for the
// panel's chips ({"⌘", "⇧", "X"} on macOS, {"Ctrl", "Shift", "X"} elsewhere);
// nativeKeys: one string for a tooltip ("⌘⇧X" / "Ctrl+Shift+X").
std::vector<std::string> keyChips(Id id);
std::string              nativeKeys(Id id);
// "Bold (Ctrl+B)", the tooltip form; label is already translated.
std::string              tip(const char *label, Id id);

// The composer's send key (Settings "send with Ctrl+Enter"). The shell sets
// it from the saved settings; the table reads it on every match.
bool ctrlEnterSends();
void setCtrlEnterSends(bool on);

// A Window-scope binding: one window shortcut per key.
void install(ui::Window &w, Id id, std::function<void()> fn);

} // namespace shell::shortcuts
