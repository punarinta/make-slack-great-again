// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "shortcuts.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QShortcut>
#include <QWidget>
#include <QtGlobal>

namespace Ui {
namespace {

constexpr auto kNoStd = QKeySequence::UnknownKey;

// Marks a QShortcut as registry-owned, so install() can spot a clash without
// tracking window lifetimes itself (and without tripping over the dialogs'
// own WidgetWithChildren Escape shortcuts).
constexpr char kOwnedProperty[] = "msgaShortcutId";

// Modifiers a binding can carry. Everything else in QKeyEvent::modifiers()
// (KeypadModifier, GroupSwitchModifier) is incidental to which key was struck.
constexpr Qt::KeyboardModifiers kRelevantMods =
    Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;

// ── The table ─────────────────────────────────────────────────────────────────
//
// Ordered as the welcome screen's shortcut panel lists them (inHelp rows, top to
// bottom); the rest sit next to the group they belong to.
//
// NOTE ON MODIFIERS: "Ctrl" is portable text, so Qt resolves it to Command on
// macOS and Control everywhere else — which is what every one of these wants.
// Never write "Meta" meaning "the platform's command key": on macOS that is the
// *physical Control* key.
const std::vector<ShortcutDef> kDefs = {
    {Shortcut::QuickSwitch,
     ShortcutScope::Window,
     "Ctrl+K",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Jump to a conversation"),
     true},
    {Shortcut::SearchMessages,
     ShortcutScope::Window,
     nullptr,
     QKeySequence::Find,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Search messages"),
     true},
    {Shortcut::SendMessage,
     ShortcutScope::Documented,
     "Enter",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Send message"),
     true},
    {Shortcut::NewLine,
     ShortcutScope::Documented,
     "Shift+Enter",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "New line in message"),
     true},
    {Shortcut::EditLastMessage,
     ShortcutScope::Documented,
     "Up",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Edit last message"),
     true},
    {Shortcut::Bold,
     ShortcutScope::Composer,
     "Ctrl+B",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Bold"),
     true},
    {Shortcut::Italic,
     ShortcutScope::Composer,
     "Ctrl+I",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Italic"),
     true},
    {Shortcut::Strikethrough,
     ShortcutScope::Composer,
     "Ctrl+Shift+X",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Strikethrough"),
     true},
    {Shortcut::InlineCode,
     ShortcutScope::Composer,
     "Ctrl+Shift+C",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Inline code"),
     true},
    {Shortcut::Link,
     ShortcutScope::Composer,
     "Ctrl+Shift+U",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Insert link"),
     true},
    {Shortcut::AttachFile,
     ShortcutScope::Composer,
     "Ctrl+O",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Attach file"),
     true},
    {Shortcut::EmojiPicker,
     ShortcutScope::Composer,
     "Ctrl+Shift+\\",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Emoji picker"),
     true},
    {Shortcut::CancelOrExitEdit,
     ShortcutScope::Documented,
     "Esc",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Cancel / exit edit"),
     true},

    // ── Not advertised in the help panel ──────────────────────────────────────
    {Shortcut::Underline,
     ShortcutScope::Composer,
     "Ctrl+U",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Underline"),
     false},
    {Shortcut::CodeBlock,
     ShortcutScope::Composer,
     "Ctrl+Alt+Shift+C",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Code block"),
     false},
    {Shortcut::OrderedList,
     ShortcutScope::Composer,
     "Ctrl+Shift+7",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Ordered list"),
     false},
    {Shortcut::BulletList,
     ShortcutScope::Composer,
     "Ctrl+Shift+8",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Bulleted list"),
     false},
    {Shortcut::Quote,
     ShortcutScope::Composer,
     "Ctrl+Shift+9",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Blockquote"),
     false},
    // Dedicated XF86 Back/Forward keys alongside the conventional Alt+arrows.
    {Shortcut::NavBack,
     ShortcutScope::Window,
     "Alt+Left|Back",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Previous conversation"),
     false},
    {Shortcut::NavForward,
     ShortcutScope::Window,
     "Alt+Right|Forward",
     kNoStd,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Next conversation"),
     false},
    {Shortcut::CloseFrontmost,
     ShortcutScope::Window,
     nullptr,
     QKeySequence::Close,
     QT_TRANSLATE_NOOP("Ui::Shortcuts", "Close dialog or window"),
     false},
};

// '+' is both the separator and a possible key name, so a trailing empty token
// means the key itself was '+'.
QStringList splitKeys(const QString &portable) {
    QStringList out;
    const auto  parts = portable.split('+');
    for (int i = 0; i < parts.size(); ++i) {
        if (parts.at(i).isEmpty()) {
            if (i == parts.size() - 1 && !out.isEmpty())
                out << QStringLiteral("+");
            continue;
        }
        out << parts.at(i);
    }
    return out;
}

// Portable key token → what the user sees on this platform.
QString nativeToken(const QString &token) {
#ifdef Q_OS_MAC
    if (token == QLatin1String("Ctrl"))
        return QString(QChar(0x2318)); // ⌘ — Qt resolves portable Ctrl to Command
    if (token == QLatin1String("Shift"))
        return QString(QChar(0x21E7)); // ⇧
    if (token == QLatin1String("Alt"))
        return QString(QChar(0x2325)); // ⌥
    if (token == QLatin1String("Meta"))
        return QString(QChar(0x2303)); // ⌃ — the physical Control key
#endif
    if (token == QLatin1String("Up"))
        return QString(QChar(0x2191)); // ↑
    if (token == QLatin1String("Down"))
        return QString(QChar(0x2193)); // ↓
    if (token == QLatin1String("Left"))
        return QString(QChar(0x2190)); // ←
    if (token == QLatin1String("Right"))
        return QString(QChar(0x2192)); // →
    return token;
}

// The primary binding in portable text — from the table for a declared sequence,
// from Qt for a StandardKey.
QString primaryPortable(const ShortcutDef &d) {
    if (d.keys)
        return QString::fromLatin1(d.keys).split('|').constFirst();
    const auto bindings = QKeySequence::keyBindings(d.standard);
    if (bindings.isEmpty())
        return {};
    return bindings.constFirst().toString(QKeySequence::PortableText);
}

} // namespace

// ── Accessors ─────────────────────────────────────────────────────────────────

const std::vector<ShortcutDef> &Shortcuts::all() {
    return kDefs;
}

const ShortcutDef &Shortcuts::def(Shortcut id) {
    for (const auto &d : kDefs)
        if (d.id == id)
            return d;
    Q_ASSERT_X(false, "Shortcuts::def", "shortcut missing from kDefs");
    return kDefs.front();
}

QString Shortcuts::label(Shortcut id) {
    // Translated on every call, never cached: the app switches locale live.
    return QCoreApplication::translate("Ui::Shortcuts", def(id).label);
}

std::vector<QKeySequence> Shortcuts::sequences(Shortcut id) {
    const auto               &d = def(id);
    std::vector<QKeySequence> out;
    if (!d.keys) {
        for (const auto &seq : QKeySequence::keyBindings(d.standard))
            out.push_back(seq);
        return out;
    }
    const auto alternates = QString::fromLatin1(d.keys).split('|', Qt::SkipEmptyParts);
    out.reserve(alternates.size());
    for (const auto &alt : alternates)
        out.emplace_back(alt, QKeySequence::PortableText);
    return out;
}

QKeySequence Shortcuts::sequence(Shortcut id) {
    const auto seqs = sequences(id);
    return seqs.empty() ? QKeySequence() : seqs.front();
}

QStringList Shortcuts::keyChips(Shortcut id) {
    QStringList chips;
    for (const auto &token : splitKeys(primaryPortable(def(id))))
        chips << nativeToken(token);
    return chips;
}

QString Shortcuts::nativeKeys(Shortcut id) {
    const QStringList chips = keyChips(id);
#ifdef Q_OS_MAC
    // Native macOS style runs the symbols together: ⌘⇧X.
    return chips.join(QString());
#else
    return chips.join('+');
#endif
}

// ── Matching / installing ─────────────────────────────────────────────────────

bool Shortcuts::matches(Shortcut id, const QKeyEvent *e) {
    if (!e)
        return false;
    const Qt::KeyboardModifiers pressed = e->modifiers() & kRelevantMods;
    for (const auto &seq : sequences(id)) {
        if (seq.count() != 1) // no multi-stroke bindings in the table
            continue;
        const QKeyCombination combo = seq[0];
        if (combo.key() != e->key())
            continue;
        if ((combo.keyboardModifiers() & kRelevantMods) == pressed)
            return true;
    }
    return false;
}

QShortcut *Shortcuts::install(Shortcut id, QWidget *owner, std::function<void()> handler) {
    Q_ASSERT(owner);
    const auto &d = def(id);
    Q_ASSERT_X(
        d.scope == ShortcutScope::Window,
        "Shortcuts::install",
        "only Window-scope bindings can be installed as QShortcuts"
    );

    QWidget *win = owner->window();

    // Another registry shortcut already on this sequence would make every press
    // ambiguous, silently disabling both handlers. Report it and keep the
    // incumbent rather than breaking a working binding.
    const auto clashingLabel = [&](const QKeySequence &seq) -> QString {
        if (!win)
            return {};
        for (const QShortcut *existing : win->findChildren<QShortcut *>()) {
            const QVariant owned = existing->property(kOwnedProperty);
            if (!owned.isValid() || existing->key() != seq)
                continue;
            return label(static_cast<Shortcut>(owned.toInt()));
        }
        return {};
    };

    QShortcut *primary = nullptr;
    for (const auto &seq : sequences(id)) {
        if (seq.isEmpty())
            continue;
        if (const QString clash = clashingLabel(seq); !clash.isEmpty()) {
            qWarning(
                "Shortcuts: %s is already bound to \"%s\" in this window — \"%s\" not installed",
                qPrintable(seq.toString(QKeySequence::PortableText)),
                qPrintable(clash),
                qPrintable(label(id))
            );
            continue;
        }
        auto *sc = new QShortcut(seq, owner);
        sc->setProperty(kOwnedProperty, static_cast<int>(id));
        QObject::connect(sc, &QShortcut::activated, owner, [handler] {
            if (handler)
                handler();
        });
        if (!primary)
            primary = sc;
    }
    return primary;
}

} // namespace Ui
