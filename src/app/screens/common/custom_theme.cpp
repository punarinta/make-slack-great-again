#include "app/screens/common/custom_theme.h"

#include "base/str.h"

#include <algorithm>
#include <cstdint>

namespace screens {

namespace {

// Slack's theme swatches by name: what an ia_theme slot names when it has no
// hex of its own.
bool swatchColor(std::string_view name, ui::Color *out) {
    static const struct {
        const char *name;
        uint32_t    rgb;
    } kSwatches[] = {
        {"aubergine", 0x3F0E40},
        {"graphite", 0x1F1F1F},
        {"ocean", 0x0E2A40},
        {"forest", 0x0E3D2E},
        {"nocturne", 0x1A1D21},
        {"ochin", 0x303E4D},
        {"blueberry", 0x3B4CCA},
        {"lagoon", 0x1264A3},
        {"jade", 0x2BAC76},
        {"banana", 0xECB22E},
        {"clementine", 0xE8912D},
        {"cherry", 0xCD2553},
        {"hoth", 0xF5F0EB},
    };
    const std::string n = str::asciiLower(str::trim(name));
    for (const auto &w : kSwatches)
        if (n == w.name) {
            *out = ui::Color(0xff000000u | w.rgb);
            return true;
        }
    return false;
}

using Field = ui::Color ui::CustomPalette::*;
const struct {
    const char *key;
    Field       field;
} kSlots[] =
    {
        {"primary", &ui::CustomPalette::primary},
        {"highlight1", &ui::CustomPalette::highlight1},
        {"highlight2", &ui::CustomPalette::highlight2},
        {"important", &ui::CustomPalette::important},
},
  kPins[] = {
      // The pinned colours, under their "pins" names; absent = derived.
      {"itemHover", &ui::CustomPalette::itemHover},
      {"itemSelText", &ui::CustomPalette::itemSelText},
      {"itemText", &ui::CustomPalette::itemText},
      {"titleBarBg", &ui::CustomPalette::titleBarBg},
      {"titleBarText", &ui::CustomPalette::titleBarText},
};

} // namespace

bool parseThemeColor(const json::Value &v, ui::Color *out) {
    if (v.isObject())
        return ui::parseHexColor(v["hex"].str(), out) || swatchColor(v["palette"].str(), out);
    return v.isString() && (ui::parseHexColor(v.str(), out) || swatchColor(v.str(), out));
}

bool readCustomTheme(const json::Value &obj, ui::CustomPalette *t) {
    bool any = false;
    for (const auto &k : kSlots) {
        any |= obj.has(k.key);
        parseThemeColor(obj[k.key], &(t->*k.field)); // else the one there
    }
    t->brightness      = int(std::clamp<int64_t>(obj["brightness"].integer(6), 0, 10));
    t->sidebarInverted = obj["sidebarInverted"].boolean(true);
    t->gradient        = obj["gradient"].boolean(true);
    for (const auto &k : kPins)
        ui::parseHexColor(obj["pins"][k.key].str(), &(t->*k.field));
    return any;
}

void writeCustomTheme(json::Writer &w, const ui::CustomPalette &t) {
    w.beginObject();
    for (const auto &k : kSlots)
        w.key(k.key).value(ui::hexColor(t.*k.field));
    w.key("brightness").value(t.brightness);
    w.key("sidebarInverted").value(t.sidebarInverted);
    w.key("gradient").value(t.gradient);
    w.key("pins").beginObject();
    for (const auto &k : kPins)
        if (t.*k.field)
            w.key(k.key).value(ui::hexColor(t.*k.field));
    w.endObject().endObject();
}

} // namespace screens
