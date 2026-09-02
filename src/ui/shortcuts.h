// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

class QKeyEvent;
class QShortcut;
class QWidget;

// Central registry of every keyboard binding in the app.
//
// One table (shortcuts.cpp) owns the sequence, the human-readable action name
// and where the binding lives, so the three consumers can never drift apart:
// the code that reacts to the key, the tooltips that advertise it, and the
// shortcut panel on the welcome screen (which used to be a hand-typed list and
// was already missing half the real bindings).
//
// Adding a binding = one row in kDefs. Never hand-roll a QShortcut or a raw
// modifier comparison in a widget — go through this header.
namespace Ui {

enum class Shortcut {
    // ── Window scope: installed as QShortcuts via Shortcuts::install() ────────
    NavBack,
    NavForward,
    CloseFrontmost,
    SearchMessages,
    QuickSwitch,

    // ── Composer scope: matched in ComposerWidget's key handler ───────────────
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

    // ── Documented only ──────────────────────────────────────────────────────
    SendMessage,
    NewLine,
    EditLastMessage,
    CancelOrExitEdit,
};

enum class ShortcutScope {
    // A binding on the main window, installed through install().
    Window,
    // A binding matched inside the owning widget's own key handler: it acts on
    // that editor's cursor/selection and must not fire while focus is elsewhere,
    // so it can't be a window-scope QShortcut. Declared here anyway so the
    // sequence and the label still have a single source of truth.
    Composer,
    // Implemented by whatever widget owns the plain key (Enter, Up, Escape) and
    // listed here only so the help panel and tooltips read one table.
    Documented,
};

struct ShortcutDef {
    Shortcut                  id;
    ShortcutScope             scope;
    // Portable, '+'-separated sequence ("Ctrl+Shift+X"). Alternate bindings for
    // the same action follow after a '|' ("Alt+Left|Back"); the first is the
    // primary one, the one shown to the user. Null when `standard` is set.
    const char               *keys;
    // Preferred over `keys` when set: Qt's platform-native binding set (Close is
    // Cmd+W on macOS, but Ctrl+W *and* Ctrl+F4 on Windows).
    QKeySequence::StandardKey standard;
    // Untranslated action name; QT_TRANSLATE_NOOP'd in the table so lupdate
    // finds it. Read it through label().
    const char               *label;
    // Listed in the welcome screen's shortcut panel. Off for bindings that are
    // either conventional enough to need no advertising (Alt+arrows, Ctrl+W) or
    // too niche to spend a row on — the panel has to fit an 800x600 window.
    bool                      inHelp;
};

namespace Shortcuts {

const std::vector<ShortcutDef> &all();
const ShortcutDef              &def(Shortcut id);

// Translated action name ("Bold", "Jump to a conversation").
QString label(Shortcut id);

// The primary binding, and every binding including platform alternates.
QKeySequence              sequence(Shortcut id);
std::vector<QKeySequence> sequences(Shortcut id);

// The primary binding split into per-key labels for the help panel's key chips,
// natively rendered: {"⌘", "⇧", "X"} on macOS, {"Ctrl", "Shift", "X"} elsewhere.
QStringList keyChips(Shortcut id);

// The primary binding as one native string for a tooltip: "⌘⇧X" / "Ctrl+Shift+X".
QString nativeKeys(Shortcut id);

// True when `e` is the press for this binding. For widget-local key handlers
// (Composer scope); modifiers must match exactly, as the hand-rolled
// comparisons this replaced did.
bool matches(Shortcut id, const QKeyEvent *e);

// Creates a window-scope QShortcut per binding on `owner` and wires `handler`.
// Returns the QShortcut for the primary binding (nullptr if none was installed).
//
// Refuses — with a qWarning naming both actions — to install a sequence another
// registry shortcut already holds in the same window: Qt answers two matching
// QShortcuts by declaring the press ambiguous and running NEITHER handler, so a
// duplicate silently kills both bindings instead of just losing the new one.
QShortcut *install(Shortcut id, QWidget *owner, std::function<void()> handler);

} // namespace Shortcuts
} // namespace Ui
