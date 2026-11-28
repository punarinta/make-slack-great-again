// The compiled-in translations: languages_generated.cpp, made from the .po
// files next to it by tools/i18n.py. English is built into base/i18n.
#pragma once

#include <cstddef>
#include <string>

namespace app_i18n {

// Makes every compiled-in language available to i18n::setLanguage().
void registerLanguages();

// Unpacks a table: raw deflate of exactly `size` bytes; empty on failure.
std::string inflate(const unsigned char *data, size_t len, size_t size);

} // namespace app_i18n
