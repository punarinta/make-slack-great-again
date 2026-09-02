// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for the central shortcut registry (src/ui/shortcuts.h):
//   - The table is self-consistent: every id resolves, every binding parses
//   - No two Window-scope actions claim the same sequence (Qt answers a
//     duplicate by declaring the press ambiguous and running NEITHER handler,
//     so a clash silently kills both bindings — the whole point of the registry)
//   - matches() honours exact modifiers, and portable "Ctrl" is the platform's
//     command key
//   - Ctrl/Cmd+K is the quick switcher (issue #52) and nothing else
//   - install() creates a working QShortcut, and refuses a second one on a
//     sequence already taken in that window
//   - keyChips()/nativeKeys() render the primary binding for the help panel
//     and the tooltips
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QShortcut>
#include <QWidget>

#include "ui/shortcuts.h"

using Ui::Shortcut;
using Ui::ShortcutScope;
namespace Shortcuts = Ui::Shortcuts;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-shortcuts");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── Table consistency ─────────────────────────────────────────────────────────

TEST_CASE("Shortcuts: every entry resolves to a parsable binding", "[shortcuts][table]") {
    REQUIRE(!Shortcuts::all().empty());
    for (const auto &def : Shortcuts::all()) {
        CHECK(Shortcuts::def(def.id).id == def.id);
        CHECK_FALSE(Shortcuts::label(def.id).isEmpty());

        const auto seqs = Shortcuts::sequences(def.id);
        REQUIRE_FALSE(seqs.empty());
        for (const auto &seq : seqs) {
            CHECK_FALSE(seq.isEmpty());
            // Single-stroke throughout: matches() only ever compares seq[0].
            CHECK(seq.count() == 1);
        }
        CHECK(Shortcuts::sequence(def.id) == seqs.front());
        CHECK_FALSE(Shortcuts::keyChips(def.id).isEmpty());
    }
}

TEST_CASE("Shortcuts: no two window-scope actions share a sequence", "[shortcuts][table]") {
    // A duplicate here is the ambiguity trap the registry exists to prevent.
    std::vector<std::pair<QKeySequence, Shortcut>> seen;
    for (const auto &def : Shortcuts::all()) {
        if (def.scope != ShortcutScope::Window)
            continue;
        for (const auto &seq : Shortcuts::sequences(def.id)) {
            for (const auto &[taken, by] : seen) {
                if (taken == seq) {
                    FAIL(
                        "sequence " + seq.toString(QKeySequence::PortableText).toStdString() +
                        " claimed by both \"" + Shortcuts::label(by).toStdString() + "\" and \"" +
                        Shortcuts::label(def.id).toStdString() + "\""
                    );
                }
            }
            seen.emplace_back(seq, def.id);
        }
    }
}

TEST_CASE("Shortcuts: composer bindings don't collide with each other", "[shortcuts][table]") {
    // They're matched by hand in one key handler, first match wins — so a
    // duplicate would make one of them unreachable rather than ambiguous.
    std::vector<QKeySequence> seen;
    for (const auto &def : Shortcuts::all()) {
        if (def.scope != ShortcutScope::Composer)
            continue;
        const auto seq = Shortcuts::sequence(def.id);
        for (const auto &taken : seen)
            CHECK(taken != seq);
        seen.push_back(seq);
    }
}

TEST_CASE("Shortcuts: the help panel lists something and labels all of it", "[shortcuts][help]") {
    int inHelp = 0;
    for (const auto &def : Shortcuts::all()) {
        if (!def.inHelp)
            continue;
        ++inHelp;
        CHECK_FALSE(Shortcuts::label(def.id).isEmpty());
        CHECK_FALSE(Shortcuts::keyChips(def.id).isEmpty());
    }
    CHECK(inHelp >= 10); // the hand-typed list this replaced had 10 rows
}

// ── The requested binding ─────────────────────────────────────────────────────

TEST_CASE("Shortcuts: QuickSwitch is Ctrl/Cmd+K", "[shortcuts][quickswitch]") {
    const auto seq = Shortcuts::sequence(Shortcut::QuickSwitch);
    CHECK(seq == QKeySequence(QStringLiteral("Ctrl+K"), QKeySequence::PortableText));
    // Portable Ctrl is Command on macOS and Control elsewhere — Qt's own mapping,
    // which is why the table must never spell this "Meta".
    CHECK(seq[0].keyboardModifiers() == Qt::ControlModifier);
    CHECK(seq[0].key() == Qt::Key_K);
    CHECK(Shortcuts::def(Shortcut::QuickSwitch).scope == ShortcutScope::Window);
    CHECK(Shortcuts::def(Shortcut::QuickSwitch).inHelp);
}

// ── matches() ─────────────────────────────────────────────────────────────────

TEST_CASE("Shortcuts: matches() requires the exact modifier set", "[shortcuts][matches]") {
    const QKeyEvent bold(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier);
    CHECK(Shortcuts::matches(Shortcut::Bold, &bold));

    // Same key, extra modifier → a different binding, not a sloppy match.
    const QKeyEvent boldShift(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK_FALSE(Shortcuts::matches(Shortcut::Bold, &boldShift));

    const QKeyEvent bare(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
    CHECK_FALSE(Shortcuts::matches(Shortcut::Bold, &bare));

    const QKeyEvent strike(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(Shortcuts::matches(Shortcut::Strikethrough, &strike));
    CHECK_FALSE(Shortcuts::matches(Shortcut::Bold, &strike));
}

TEST_CASE("Shortcuts: matches() ignores incidental modifiers", "[shortcuts][matches]") {
    // Keypad/group-switch state says nothing about which binding was struck.
    const QKeyEvent keypad(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier | Qt::KeypadModifier);
    CHECK(Shortcuts::matches(Shortcut::Bold, &keypad));
}

TEST_CASE("Shortcuts: matches() covers every alternate binding", "[shortcuts][matches]") {
    const QKeyEvent altLeft(QEvent::KeyPress, Qt::Key_Left, Qt::AltModifier);
    const QKeyEvent backKey(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
    CHECK(Shortcuts::matches(Shortcut::NavBack, &altLeft));
    CHECK(Shortcuts::matches(Shortcut::NavBack, &backKey));
    CHECK_FALSE(Shortcuts::matches(Shortcut::NavForward, &altLeft));
}

TEST_CASE("Shortcuts: matches() tolerates a null event", "[shortcuts][matches]") {
    CHECK_FALSE(Shortcuts::matches(Shortcut::Bold, nullptr));
}

// ── install() ─────────────────────────────────────────────────────────────────

TEST_CASE("Shortcuts: install() wires a window shortcut", "[shortcuts][install]") {
    QWidget win;
    int     fired = 0;
    auto   *sc    = Shortcuts::install(Shortcut::QuickSwitch, &win, [&fired] { ++fired; });
    REQUIRE(sc != nullptr);
    CHECK(sc->key() == Shortcuts::sequence(Shortcut::QuickSwitch));
    CHECK(sc->parent() == &win);

    emit sc->activated();
    CHECK(fired == 1);
}

TEST_CASE("Shortcuts: install() creates one QShortcut per alternate", "[shortcuts][install]") {
    QWidget win;
    Shortcuts::install(Shortcut::NavBack, &win, [] {});
    CHECK(
        win.findChildren<QShortcut *>().size() ==
        qsizetype(Shortcuts::sequences(Shortcut::NavBack).size())
    );
}

TEST_CASE("Shortcuts: install() refuses a sequence already taken", "[shortcuts][install]") {
    QWidget win;
    int     first = 0, second = 0;
    REQUIRE(Shortcuts::install(Shortcut::QuickSwitch, &win, [&first] { ++first; }) != nullptr);
    // Installing the same action twice is the clash the guard is for: the second
    // call must be a no-op rather than leaving two matching shortcuts behind.
    CHECK(Shortcuts::install(Shortcut::QuickSwitch, &win, [&second] { ++second; }) == nullptr);
    CHECK(win.findChildren<QShortcut *>().size() == 1);

    emit win.findChildren<QShortcut *>().front()->activated();
    CHECK(first == 1);
    CHECK(second == 0);
}

TEST_CASE("Shortcuts: install() ignores non-registry shortcuts", "[shortcuts][install]") {
    // AppDialog and the settings panel own plain QShortcuts on Escape; those are
    // not registry-owned and must not block an install.
    QWidget win;
    auto   *foreign = new QShortcut(Shortcuts::sequence(Shortcut::QuickSwitch), &win);
    CHECK(foreign != nullptr);
    CHECK(Shortcuts::install(Shortcut::QuickSwitch, &win, [] {}) != nullptr);
}

// ── Rendering ─────────────────────────────────────────────────────────────────

TEST_CASE("Shortcuts: keyChips splits the primary binding in order", "[shortcuts][render]") {
    const auto chips = Shortcuts::keyChips(Shortcut::Strikethrough);
    REQUIRE(chips.size() == 3);
#ifdef Q_OS_MAC
    CHECK(chips.at(0) == QString(QChar(0x2318))); // ⌘
    CHECK(chips.at(1) == QString(QChar(0x21E7))); // ⇧
#else
    CHECK(chips.at(0) == "Ctrl");
    CHECK(chips.at(1) == "Shift");
#endif
    CHECK(chips.at(2) == "X");

    // The chips are the declared token order, not Qt's normalisation.
    const auto codeBlock = Shortcuts::keyChips(Shortcut::CodeBlock);
    REQUIRE(codeBlock.size() == 4);
    CHECK(codeBlock.back() == "C");
}

TEST_CASE("Shortcuts: arrow keys render as glyphs", "[shortcuts][render]") {
    const auto chips = Shortcuts::keyChips(Shortcut::EditLastMessage);
    REQUIRE(chips.size() == 1);
    CHECK(chips.front() == QString(QChar(0x2191))); // ↑
}

TEST_CASE("Shortcuts: a key that is itself '+' survives the split", "[shortcuts][render]") {
    // No binding uses it today; the splitter must not eat it if one ever does.
    const auto chips = Shortcuts::keyChips(Shortcut::EmojiPicker);
    REQUIRE(chips.size() == 3);
    CHECK(chips.back() == "\\");
}

TEST_CASE("Shortcuts: nativeKeys renders one tooltip-ready string", "[shortcuts][render]") {
    const QString keys = Shortcuts::nativeKeys(Shortcut::AttachFile);
#ifdef Q_OS_MAC
    CHECK(keys == QString(QChar(0x2318)) + "O");
#else
    CHECK(keys == "Ctrl+O");
#endif
}

TEST_CASE("Shortcuts: a StandardKey entry still renders and installs", "[shortcuts][render]") {
    // CloseFrontmost carries no literal sequence — Qt supplies the platform set
    // (Cmd+W on macOS; Ctrl+W and Ctrl+F4 on Windows).
    CHECK(Shortcuts::def(Shortcut::CloseFrontmost).keys == nullptr);
    CHECK_FALSE(Shortcuts::sequences(Shortcut::CloseFrontmost).empty());
    CHECK_FALSE(Shortcuts::nativeKeys(Shortcut::CloseFrontmost).isEmpty());

    QWidget win;
    CHECK(Shortcuts::install(Shortcut::CloseFrontmost, &win, [] {}) != nullptr);
}
