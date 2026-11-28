// A custom theme in JSON: what the settings file keeps ("customTheme") and
// what Settings → Appearance imports (Slack's ia_theme, plus the app's own
// "gradient" and "pins"). The one reader of both.
#pragma once

#include "base/json.h"
#include "ui/ui.h"

namespace screens {

// One theme colour: {"hex":…} (it wins when valid) or {"palette":"aubergine"};
// a bare string is a hex or a palette name. False, *out untouched, for
// neither.
bool parseThemeColor(const json::Value &v, ui::Color *out);

// A theme object over *t: the colour slots primary, highlight1, highlight2
// and important (one absent or unreadable keeps *t's), brightness,
// sidebarInverted, gradient and the hex "pins". Whether any colour slot is
// named at all (else it is likely some other JSON).
bool readCustomTheme(const json::Value &obj, ui::CustomPalette *t);
// *t as the object readCustomTheme reads back: the colour slots as hex, the
// flags, and the pins that are set.
void writeCustomTheme(json::Writer &w, const ui::CustomPalette &t);

} // namespace screens
