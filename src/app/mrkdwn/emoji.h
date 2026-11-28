// Slack shortcode → Unicode, from a compiled-in table generated from
// scripts/emoji_table.json (see gen_emoji_table.py). The table is front-coded
// and searched in place: no startup cost, no heap.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emoji {

// The Unicode for `name` (no colons), or empty when it is not a standard
// shortcode (custom workspace emoji are the Store's business). Understands
// Slack's modifier suffix: "+1::skin-tone-3", "wave::skin-tone-5".
std::string toUnicode(std::string_view name);
bool        isKnown(std::string_view name);

// Applies a Fitzpatrick modifier (2-6) to an emoji sequence: inserted after
// the first code point, replacing a VS16 there, as Unicode's modifier
// sequences are built ("☝️" + tone → "☝🏽", ZWJ sequences keep the rest).
std::string applySkinTone(std::string_view unicode, int tone);

// Replaces every known :shortcode: in plain text ("Hello :wave:" →
// "Hello 👋"); unknown ones stay as typed. For notification bodies and
// window titles, which have no rich text.
std::string expandShortcodes(std::string_view text);

// Up to `max` shortcodes starting with `prefix`, in sorted order (the
// composer's ":" completion).
void complete(std::string_view prefix, size_t max, std::vector<std::string> &out);
// Every entry in sorted order (the picker); stop early by returning false.
void forEach(const std::function<bool(std::string_view name, const std::string &unicode)> &fn);
// The same, names only (nothing decoded). Names are lowercase ASCII:
// already case-folded.
void forEachName(const std::function<bool(std::string_view name)> &fn);
int  count();

// The picker's categories (iamcal/emoji-data order: "Smileys &
// People", "Animals & Nature", … "Flags"), and whether an emoji has
// per-person skin-tone variants.
int         categoryCount();
const char *categoryId(int cat);    // "people", "nature", …
const char *categoryLabel(int cat); // translated
// (shortcode, Unicode) pairs of a category in display order, appended.
void        categoryEntries(int cat, std::vector<std::pair<std::string, std::string>> &out);
bool        supportsSkinTone(std::string_view name);

} // namespace emoji
