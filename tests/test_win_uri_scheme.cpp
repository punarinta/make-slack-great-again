// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Guards a Windows sign-in regression that can't be reproduced on the Linux
// test host: registerUriScheme() in desktop_notifier_win.cpp registers the
// msga:// URL scheme so the OAuth redirect (msga://oauth/callback) can reach the
// app. The mingw-w64 release cross build lacks the WinRT toast headers, so
// MSGA_WINRT_TOAST is undefined there — and if scheme registration is gated
// behind that guard, the whole DesktopNotifier ctor compiles to nothing, the
// scheme is never registered, and Windows sign-in hangs "loading forever".
//
// The function is in an anonymous namespace in a Windows-only translation unit
// that writes to the registry, so it can't be linked or run here. Instead we
// parse the source and assert, structurally, that registration is NOT inside a
// `#if defined(MSGA_WINRT_TOAST)` region — while confirming the toast-only
// machinery IS inside it, which proves the parser actually detects guarding.
#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <vector>

namespace {

QStringList sourceLines() {
    const QString path = QStringLiteral(MSGA_SOURCE_DIR) + "/src/util/desktop_notifier_win.cpp";
    QFile         f(path);
    REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    return QString::fromUtf8(f.readAll()).split('\n');
}

// True if `needle` first appears on a line that is lexically inside an open
// `#if[def] ...MSGA_WINRT_TOAST...` region. Tracks a stack of conditional
// blocks so unrelated #if/#endif pairs (and nesting) don't confuse the result.
// REQUIREs the needle to be present at all.
bool firstOccurrenceIsWinrtGuarded(const QStringList &lines, const QString &needle) {
    std::vector<bool> guardStack; // one entry per open #if; true = it's the WinRT guard
    bool              found = false, guarded = false;

    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith("#if")) {
            const bool isWinrt = t.contains("MSGA_WINRT_TOAST");
            guardStack.push_back(isWinrt);
        } else if (t.startsWith("#endif")) {
            if (!guardStack.empty())
                guardStack.pop_back();
        } else if (line.contains(needle)) {
            found   = true;
            guarded = std::any_of(guardStack.begin(), guardStack.end(), [](bool b) { return b; });
            break;
        }
    }
    REQUIRE(found);
    return guarded;
}

} // namespace

TEST_CASE("registerUriScheme is defined outside the MSGA_WINRT_TOAST guard", "[win][auth]") {
    const QStringList lines = sourceLines();
    CHECK_FALSE(firstOccurrenceIsWinrtGuarded(lines, "void registerUriScheme()"));
}

TEST_CASE("DesktopNotifier ctor calls registerUriScheme() unconditionally", "[win][auth]") {
    const QStringList lines = sourceLines();
    // The definition is `void registerUriScheme()`; the call is `registerUriScheme();`.
    // Locate the call specifically (semicolon, no `void`).
    CHECK_FALSE(firstOccurrenceIsWinrtGuarded(lines, "registerUriScheme();"));
}

TEST_CASE("toast-only machinery stays behind the guard (parser sanity)", "[win][auth]") {
    const QStringList lines = sourceLines();
    // ensureStartMenuShortcut() is genuinely WinRT/toast-only. If this is NOT
    // detected as guarded, the parser is broken and the checks above are
    // meaningless — so this asserts the negative space.
    CHECK(firstOccurrenceIsWinrtGuarded(lines, "ensureStartMenuShortcut()"));
}
