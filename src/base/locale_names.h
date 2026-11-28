// The OS's regional format, for base/time.cpp's "system" date language: the
// locale tag and its date names. Internal to base.
#pragma once

#include <string>

namespace base::detail {

struct OsDateNames {
    std::string month[12], monthShort[12];
    std::string weekday[7], weekdayShort[7]; // Sunday first
    std::string am, pm;
    std::string shortDate;     // a formatCivil pattern ("dd.MM.yy"); "" = unknown
    int         firstDay = -1; // the week's first day, 0 = Sunday; -1 = unknown
};

// The names for `tag` ("sv-SE", osLocale()'s). False when there are none
// (Linux: a language outside date_locales_generated.inc, or English); the
// short date may still be filled. Windows and macOS ask the OS for its own
// (current-user) format; Linux looks `tag` up in compiled-in CLDR data.
bool osDateNames(const std::string &tag, OsDateNames &out);

} // namespace base::detail
