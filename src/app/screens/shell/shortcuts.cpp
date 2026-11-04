#include "screens/shell/shortcuts.h"

#include "base/i18n.h"
#include "base/str.h"

namespace shell::shortcuts {

namespace {

using K = plat::Key;
constexpr Keys kNone{};

// The table. Ordered as the shortcuts panel lists them
// (inHelp rows, top to bottom); the rest sit next to the group they belong to.
const Def kDefs[] = {
    {Id::OpenSettings, Scope::Window, true, N_("Open settings"), {{K::Comma, Ctrl}, kNone}},
    {Id::QuickSwitch, Scope::Window, true, N_("Jump to a conversation"), {{K::K, Ctrl}, kNone}},
    // Find: Ctrl+F on every platform.
    {Id::SearchMessages, Scope::Window, true, N_("Search messages"), {{K::F, Ctrl}, kNone}},
    // The keys of the next two are replaced by bindings() (the Ctrl+Enter option).
    {Id::SendMessage, Scope::Documented, true, N_("Send message"), {{K::Enter, 0}, kNone}},
    {Id::NewLine, Scope::Documented, true, N_("New line in message"), {{K::Enter, Shift}, kNone}},
    {Id::EditLastMessage, Scope::Documented, true, N_("Edit last message"), {{K::Up, 0}, kNone}},
    {Id::Bold, Scope::Composer, true, N_("Bold"), {{K::B, Ctrl}, kNone}},
    {Id::Italic, Scope::Composer, true, N_("Italic"), {{K::I, Ctrl}, kNone}},
    {Id::Strikethrough, Scope::Composer, true, N_("Strikethrough"), {{K::X, Ctrl | Shift}, kNone}},
    {Id::InlineCode, Scope::Composer, true, N_("Inline code"), {{K::C, Ctrl | Shift}, kNone}},
    {Id::Link, Scope::Composer, true, N_("Insert link"), {{K::U, Ctrl | Shift}, kNone}},
    {Id::AttachFile, Scope::Composer, true, N_("Attach file"), {{K::O, Ctrl}, kNone}},
    {Id::EmojiPicker,
     Scope::Composer,
     true,
     N_("Emoji picker"),
     {{K::Backslash, Ctrl | Shift}, kNone}},
    {Id::CancelOrExitEdit,
     Scope::Documented,
     true,
     N_("Cancel / exit edit"),
     {{K::Escape, 0}, kNone}},
    // Only while the "Sent · Undo" chip is up and the editor is empty;
    // otherwise Ctrl+Z stays the editor's own undo.
    {Id::UndoSend, Scope::Composer, true, N_("Undo send"), {{K::Z, Ctrl}, kNone}},
    // Only where the conversation has a prompt history (a Claude Code session).
    {Id::SearchPromptHistory,
     Scope::Composer,
     false,
     N_("Search earlier prompts"),
     {{K::R, Ctrl}, kNone}},
    // Only where a speech-to-text provider is connected (the mic button
    // shows). Clear of the IME toggles (Ctrl+Space, Super+Space, Shift+Space).
    {Id::VoiceInput, Scope::Composer, false, N_("Voice input"), {{K::Space, Ctrl | Shift}, kNone}},

    // ── Not advertised in the panel ─────────────────────────────────────────
    {Id::Underline, Scope::Composer, false, N_("Underline"), {{K::U, Ctrl}, kNone}},
    {Id::CodeBlock, Scope::Composer, false, N_("Code block"), {{K::C, Ctrl | Alt | Shift}, kNone}},
    {Id::OrderedList, Scope::Composer, false, N_("Ordered list"), {{K::Num7, Ctrl | Shift}, kNone}},
    {Id::BulletList, Scope::Composer, false, N_("Bulleted list"), {{K::Num8, Ctrl | Shift}, kNone}},
    {Id::Quote, Scope::Composer, false, N_("Blockquote"), {{K::Num9, Ctrl | Shift}, kNone}},
    // The dedicated Back/Forward keys alongside the conventional Alt+arrows.
    {Id::NavBack,
     Scope::Window,
     false,
     N_("Previous conversation"),
     {{K::Left, Alt}, {K::Back, 0}}},
    {Id::NavForward,
     Scope::Window,
     false,
     N_("Next conversation"),
     {{K::Right, Alt}, {K::Forward, 0}}},
    // The platform's Close key, resolved per platform.
    {Id::CloseFrontmost,
     Scope::Window,
     false,
     N_("Close dialog or window"),
#if defined(_WIN32)
     {{K::F4, Ctrl}, {K::W, Ctrl}}},
#elif defined(__APPLE__)
     {{K::W, Ctrl}, {K::F4, Ctrl}}},
#else
     {{K::W, Ctrl}, kNone}},
#endif
    // Only what the focused view leaves (an edit in progress, a selection, the
    // search keep their Esc), only from inside the thread or with no focus,
    // and a modal popup still goes first.
    {Id::CloseThread, Scope::Window, false, N_("Close thread"), {{K::Escape, 0}, kNone}},
    // Claude Code workspace only, and only on an idle (gray-dot) session.
    {Id::RemoveIdleSession,
     Scope::Documented,
     false,
     N_("Remove idle session from msga"),
     {{K::Delete, Shift}, kNone}},
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == size_t(Id::Count), "a row per Id");

bool g_ctrlEnterSends = false;

uint32_t platMods(uint8_t m) {
    uint32_t out = 0;
    if (m & Shift)
        out |= plat::ModShift;
    if (m & Ctrl)
        out |= plat::primaryMod();
    if (m & Alt)
        out |= plat::ModAlt;
    return out;
}

// Portable key names, as the table spells them.
std::string portableName(plat::Key k) {
    if (k >= K::A && k <= K::Z)
        return std::string(1, char('A' + (int(k) - int(K::A))));
    if (k >= K::Num0 && k <= K::Num9)
        return std::string(1, char('0' + (int(k) - int(K::Num0))));
    if (k >= K::F1 && k <= K::F24)
        return str::concat({"F", str::number(int64_t(int(k) - int(K::F1) + 1))});
    switch (k) {
    case K::Comma:
        return ",";
    case K::Backslash:
        return "\\";
    case K::Space:
        return "Space";
    case K::Enter:
    case K::KpEnter:
        return "Enter";
    case K::Escape:
        return "Esc";
    case K::Delete:
        return "Del";
    case K::Up:
        return "\xE2\x86\x91"; // ↑
    case K::Down:
        return "\xE2\x86\x93"; // ↓
    case K::Left:
        return "\xE2\x86\x90"; // ←
    case K::Right:
        return "\xE2\x86\x92"; // →
    case K::Back:
        return "Back";
    case K::Forward:
        return "Forward";
    default:
        return plat::keyName(k);
    }
}

std::vector<std::string> tokens(const Keys &k) {
    // In spelling order (Ctrl, Alt, Shift), each token rendered natively.
    std::vector<std::string> out;
#ifdef __APPLE__
    if (k.mods & Ctrl)
        out.emplace_back("\xE2\x8C\x98"); // ⌘
    if (k.mods & Alt)
        out.emplace_back("\xE2\x8C\xA5"); // ⌥
    if (k.mods & Shift)
        out.emplace_back("\xE2\x87\xA7"); // ⇧
#else
    if (k.mods & Ctrl)
        out.emplace_back("Ctrl");
    if (k.mods & Alt)
        out.emplace_back("Alt");
    if (k.mods & Shift)
        out.emplace_back("Shift");
#endif
    out.push_back(portableName(k.key));
    return out;
}

} // namespace

const Def *table() {
    return kDefs;
}

size_t tableSize() {
    return size_t(Id::Count);
}

const Def &def(Id id) {
    // The table is in Id order: straight there (a scan should it ever not be).
    if (size_t(id) < size_t(Id::Count) && kDefs[size_t(id)].id == id)
        return kDefs[size_t(id)];
    for (const Def &d : kDefs)
        if (d.id == id)
            return d;
    return kDefs[0];
}

size_t bindings(Id id, Keys out[2]) {
    if (id == Id::SendMessage) {
        if (g_ctrlEnterSends) {
            out[0] = {K::Enter, Ctrl};
            return 1;
        }
        out[0] = {K::Enter, 0};
        out[1] = {K::Enter, Ctrl};
        return 2;
    }
    if (id == Id::NewLine) {
        if (g_ctrlEnterSends) {
            out[0] = {K::Enter, 0};
            out[1] = {K::Enter, Shift};
            return 2;
        }
        out[0] = {K::Enter, Shift};
        return 1;
    }
    const Def &d = def(id);
    size_t     n = 0;
    for (const Keys &k : d.keys)
        if (k.key != K::Unknown)
            out[n++] = k;
    return n;
}

bool matches(Id id, const ui::Event &e) {
    if (e.type != ui::EventType::KeyDown)
        return false;
    const K        key = e.key == K::KpEnter ? K::Enter : e.key;
    const uint32_t m   = e.mods & ui::kModMask;
    Keys           b[2];
    const size_t   n = bindings(id, b);
    for (size_t i = 0; i < n; ++i)
        if (b[i].key == key && platMods(b[i].mods) == m)
            return true;
    return false;
}

std::vector<std::string> keyChips(Id id) {
    Keys b[2];
    return bindings(id, b) ? tokens(b[0]) : std::vector<std::string>();
}

std::string nativeKeys(Id id) {
    std::string out;
    for (const std::string &t : keyChips(id)) {
#ifndef __APPLE__
        if (!out.empty())
            out += '+';
#endif
        out += t;
    }
    return out;
}

std::string tip(const char *label, Id id) {
    return std::string(label) + " (" + nativeKeys(id) + ")";
}

bool ctrlEnterSends() {
    return g_ctrlEnterSends;
}

void setCtrlEnterSends(bool on) {
    g_ctrlEnterSends = on;
}

void install(ui::Window &w, Id id, std::function<void()> fn) {
    Keys         b[2];
    const size_t n = bindings(id, b);
    for (size_t i = 0; i < n; ++i) {
        uint32_t mods = 0;
        if (b[i].mods & Shift)
            mods |= plat::ModShift;
        if (b[i].mods & Ctrl)
            mods |= ui::Window::kPrimary;
        if (b[i].mods & Alt)
            mods |= plat::ModAlt;
        w.addShortcut(b[i].key, mods, fn);
    }
}

} // namespace shell::shortcuts
