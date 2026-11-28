// What each platform's backend file (backend_{linux,mac,win}.cpp) provides
// to the checker besides createPlatformBackend.
#pragma once

#include "app/spell/spell.h"

namespace spell::detail {

// The languages this machine can check, without loading any of them.
// listsOffThread(): it reads directories, so the callers run it on a worker.
std::vector<Language> availableLanguages();
bool                  listsOffThread();
// For someone with no languages: the package with the dictionary for the
// first preferred UI language, under the distribution's name; "" where the
// OS ships its languages.
std::string           dictionaryPackageHint(const std::vector<std::string> &preferred);

// "en-US.UTF-8" / "en_US" → "en_US" (language, '_', territory), "" when it
// has no language.
std::string localeCode(std::string_view osLanguage);

} // namespace spell::detail
