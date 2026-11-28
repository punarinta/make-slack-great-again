// Translation tables. UI text is written in English (sentence case) at the
// call site and looked up here: tr("Yesterday"), trn("%n reply", "%n replies",
// n). English needs no table: a missing entry falls back to the msgid itself.
//
// A language is compiled in as one compressed blob (src/app/i18n/, generated
// from the .po files by tools/i18n.py) that is unpacked only when it becomes
// the active language. The blob is keyed by a 32-bit hash of the msgid, so a
// table carries neither the English text (already in the binary at the call
// sites) nor any pointers (no relocations). Format, little-endian:
//   u32 count
//   u32 hash[count]                 ascending, hash() of the msgid
//   count * formCount strings       NUL-terminated, entry-major; an empty
//                                   string is an untranslated form
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Marks a literal for tools/i18n.py without translating it, for tables that
// are translated where they are shown: tr(kNames[i]).
#define N_(s) s

namespace i18n {

struct Language {
    const char *code; // "ja", … (the primary language subtag)
    // Plural form index for n (0-based, < formCount): the .po Plural-Forms rule.
    int (*plural)(int64_t n);
    int formCount;
    // The table in the format above; called once, when the language is first
    // selected. Empty = could not be produced (the language is not selected).
    std::string (*load)();
};

// FNV-1a 32 over the UTF-8 bytes: the table key (tools/i18n.py matches it).
uint32_t hash(std::string_view s);

// Adds a language to the list setLanguage() picks from (compiled-in ones are
// registered at startup; tests register fakes). The struct must stay alive.
void        registerLanguage(const Language *lang);
// Switches the active language; unknown codes (or a table that fails to
// load) keep the current one and return false. "en" always exists.
bool        setLanguage(std::string_view code);
// The first of the OS's preferred languages ("ja-JP", "en_US.UTF-8", …, most
// preferred first) that msga has, by primary subtag; English when none is.
void        setPreferredLanguage(const std::vector<std::string> &tags);
const char *currentCode();

// The translation of msgid in the active language, or msgid itself.
const char *tr(const char *msgid);
// Plural lookup; every "%n" in the result is replaced by n.
std::string trn(const char *singular, const char *plural, int64_t n);
// Positional substitution: "%1 at %2" with a1, a2, a3 (numbered, so the
// translated strings can reorder their arguments).
std::string
arg(std::string_view fmt, std::string_view a1, std::string_view a2 = {}, std::string_view a3 = {});

} // namespace i18n
