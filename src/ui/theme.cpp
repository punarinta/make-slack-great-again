#include "ui/theme.h"
#include "ui/view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace ui {
namespace {

// Straight ARGB. Tables are indexed by C; keep the order in sync with theme.h
// (the static_asserts below catch a missing row).
constexpr Color kLight[] = {
    0x00000000, // None
    0xffffffff, // WindowBg
    0xffffffff, // Surface
    0xfff8f8f8, // SurfaceHover
    0xff3f0e40, // Rail
    0xff5a2b5c, // Sidebar
    0xfff2ebf2, // SidebarText
    0xffc9b6c9, // SidebarTextMuted
    0x1affffff, // SidebarHover
    0xffece6ed, // SidebarSelected
    0xff1d1c1d, // SidebarSelectedText
    0x64ffffff, // SidebarScrollbar
    0xff1d1c1d, // Text
    0xff616061, // TextMuted
    0xff868686, // TextFaint
    0xff1264a3, // Link
    0xff611f69, // Accent
    0xff4a154b, // AccentHover
    0xffffffff, // AccentText
    0xffe2e2e2, // Border
    0xffbcbcbc, // BorderStrong
    0x0f000000, // Hover
    0x1f000000, // Pressed
    0xffb4d5fe, // Selection
    0xff1d1c1d, // Caret
    0xff1264a3, // FocusRing
    0xffcd2553, // Badge
    0xffffffff, // BadgeText
    0xffffffff, // PopupBg
    0xffdddddd, // PopupBorder
    0x38000000, // Shadow
    0xff1d1c1d, // TooltipBg
    0xffffffff, // TooltipText
    0x40000000, // Scrollbar
    0x70000000, // ScrollbarHover
    0xffffffff, // InputBg
    0xffc8c8c8, // InputBorder
    0xff868686, // InputBorderFocus
    0xff868686, // Placeholder
    0xfff6f6f6, // CodeBg
    0xffc01343, // CodeText
    0x261d9bd1, // MentionBg
    0xff1264a3, // MentionText
    0xfffff5d1, // MentionSelfBg
    0xffe01e5a, // Danger
    0xff2bac76, // Online
    0xffffffff, // FormBg
    0xfff4f4f4, // FormSunken
    0xfff0f0f0, // FormHighlight
    0xffe8e8e8, // FormHighlightStrong
    0xffe8e8e8, // FormDivider
    0xffd1d1d1, // FormDividerStrong
    0xff1d1c1d, // FormText
    0xff616061, // FormTextMuted
    0xff888888, // FormTextFaint
    0xff1264a3, // FormLink
    0xffc0392b, // FormError
    0xff7a5800, // FormWarning
    0xffdddddd, // FieldBorder
    0xff999999, // FieldBorderFocus
    0xffffffff, // FieldWell
    0xffe01e5a, // DangerFill
    0xffc0184f, // DangerFillHover
    0xfffff8ee, // BannerBg
    0xffe8a917, // BannerBorder
    0xff7a5800, // BannerText
    0xfffffde7, // UpdateBannerBg
    0xfff9a825, // UpdateBannerBorder
    0xff1d1c1d, // UpdateBannerText
    0xff3f0e40, // TitleBar (from the palette)
    0xffcfc3cf, // TitleBarControl (from the palette)
    0xffc6920a, // IconStarred
    0xff888888, // ComposerIcon
    0xff505050, // ComposerIconActive
    0xffcccccc, // DropArrow
    0xff1d9bd1, // BadgeActivity
    0xff8b8b8b, // PresenceAway
    0xffe8a33d, // PresencePhantom
    0xfff0dfa0, // BannerAccent
    0xfff8f8f8, // ChipBg
    0xffe0e0e0, // ChipBorder
    0xd2ffffff, // OverlayBg
    0xff1d1c1d, // OverlayText
    0xff350d36, // AccentPressed (palette)
    0xfff4e5f5, // AccentSubtle (palette)
    0xff888888, // FormIcon
    0xff454245, // FormIconStrong
    0xffcfc3cf, // OnDarkDim
    0x0a000000, // RowHover
    0xfffafafa, // FileChipBg
    0xffdddddd, // FileChipBorder
    0xff666666, // FileNameDim
    0xff1164a3, // ReplyLink
    0xffdddddd, // TableBorder
    0x0b000000, // TableHeaderBg
    0x14000000, // TableRowRule
    0xee0c0c0e, // ViewerBackdrop
    0x3cffeb3b, // PinnedBg
    0x1a1d9bd1, // ReminderBg
    0xffffffff, // MenuBg
    0xff1d1c1d, // MenuText
    0xffe01e5a, // MenuDanger
    0x0c000000, // MenuHover
    0x12000000, // MenuSeparator
    0xfff0f0f0, // DividerSubtle
};

constexpr Color kDark[] = {
    0x00000000, // None
    0xff222222, // WindowBg
    0xff222222, // Surface
    0xff2c2c2c, // SurfaceHover
    0xff121016, // Rail
    0xff19171d, // Sidebar
    0xffd1d2d3, // SidebarText
    0xff9a9b9e, // SidebarTextMuted
    0x14ffffff, // SidebarHover
    0xff1164a3, // SidebarSelected
    0xffffffff, // SidebarSelectedText
    0x64ffffff, // SidebarScrollbar
    0xffe6e6e6, // Text
    0xffa8a8a8, // TextMuted
    0xff7b7b7b, // TextFaint
    0xff53b4e5, // Link
    0xff1164a3, // Accent
    0xff0b4c8c, // AccentHover
    0xffffffff, // AccentText
    0xff333333, // Border
    0xff4d4d4d, // BorderStrong
    0x14ffffff, // Hover
    0x24ffffff, // Pressed
    0xff264f78, // Selection
    0xffe6e6e6, // Caret
    0xff1d9bd1, // FocusRing
    0xffcd2553, // Badge
    0xffffffff, // BadgeText
    0xff262626, // PopupBg
    0xff4d4d4d, // PopupBorder
    0x80000000, // Shadow
    0xff0a0a0a, // TooltipBg
    0xffffffff, // TooltipText
    0x40ffffff, // Scrollbar
    0x70ffffff, // ScrollbarHover
    0xff2a2a2a, // InputBg
    0xff3a3a3a, // InputBorder
    0xff6e6e6e, // InputBorderFocus
    0xff7b7b7b, // Placeholder
    0xff2c2d30, // CodeBg
    0xffe8912d, // CodeText
    0x331d9bd1, // MentionBg
    0xff1d9bd1, // MentionText
    0x2afac83c, // MentionSelfBg
    0xffe01e5a, // Danger
    0xff2bac76, // Online
    0xff2a2a2a, // FormBg
    0xff1a1a1a, // FormSunken
    0xff2e2e2e, // FormHighlight
    0xff383838, // FormHighlightStrong
    0xff333333, // FormDivider
    0xff4d4d4d, // FormDividerStrong
    0xffe6e6e6, // FormText
    0xffa8a8a8, // FormTextMuted
    0xff7b7b7b, // FormTextFaint
    0xff53b4e5, // FormLink
    0xffe57373, // FormError
    0xffd9a741, // FormWarning
    0xff3a3a3a, // FieldBorder
    0xff6e6e6e, // FieldBorderFocus
    0xff222222, // FieldWell
    0xffe01e5a, // DangerFill
    0xfff02e6a, // DangerFillHover
    0xff332b18, // BannerBg
    0xffc99a2c, // BannerBorder
    0xffe3c36b, // BannerText
    0xff33301c, // UpdateBannerBg
    0xfff9a825, // UpdateBannerBorder (the light one: dark never set its own)
    0xffe6e6e6, // UpdateBannerText
    0xff1a1d21, // TitleBar (from the palette)
    0xffcfc3cf, // TitleBarControl (from the palette)
    0xffe0b341, // IconStarred
    0xff9c9c9c, // ComposerIcon
    0xffe6e6e6, // ComposerIconActive
    0xff4d4d4d, // DropArrow
    0xff1d9bd1, // BadgeActivity
    0xff8b8b8b, // PresenceAway
    0xffe8a33d, // PresencePhantom
    0xff4a3e1e, // BannerAccent
    0xff282828, // ChipBg
    0xff3e3e3e, // ChipBorder
    0xd2222222, // OverlayBg
    0xffe6e6e6, // OverlayText
    0xff4a4a4a, // AccentPressed (palette)
    0xff333333, // AccentSubtle (palette)
    0xff9c9c9c, // FormIcon
    0xffc9c9c9, // FormIconStrong
    0xffc9c9c9, // OnDarkDim
    0x0cffffff, // RowHover
    0xff202020, // FileChipBg
    0xff3a3a3a, // FileChipBorder
    0xffa6a6a6, // FileNameDim
    0xff1164a3, // ReplyLink
    0xff3e3e3e, // TableBorder
    0x0cffffff, // TableHeaderBg
    0x12ffffff, // TableRowRule
    0xee0c0c0e, // ViewerBackdrop
    0x1cffeb3b, // PinnedBg
    0x181d9bd1, // ReminderBg
    0xff262626, // MenuBg
    0xffe6e6e6, // MenuText
    0xfff06a85, // MenuDanger
    0x0c000000, // MenuHover
    0x12000000, // MenuSeparator
    0xff2a2a2a, // DividerSubtle
};
static_assert(sizeof(kLight) / sizeof(Color) == size_t(C::Count), "kLight out of sync with C");
static_assert(sizeof(kDark) / sizeof(Color) == size_t(C::Count), "kDark out of sync with C");

constexpr float kMetrics[] = {4, 6, 8, 4, 8, 12, 16, 24, 28, 6};
static_assert(sizeof(kMetrics) / sizeof(float) == size_t(M::Count), "kMetrics out of sync");

struct FontRole {
    float        size;
    text::Weight weight;
    bool         mono;
};
constexpr FontRole kFonts[] = {
    {12, text::Weight::Regular, false},     // Small
    {12, text::Weight::Bold, false},        // SmallBold
    {15, text::Weight::Regular, false},     // Body
    {15, text::Weight::Bold, false},        // BodyBold
    {18, text::Weight::Bold, false},        // Title
    {13, text::Weight::Regular, true},      // Mono
    {11, text::Weight::Regular, false},     // Caption
    {13, text::Weight::Regular, false},     // Control
    {13, text::Weight::Semibold, false},    // ControlBold
    {14, text::Weight::Semibold, false},    // Heading
    {15, text::Weight::Semibold, false},    // DialogTitle
    {14, text::Weight::Regular, false},     // Field
    {15, text::Weight::Semibold, false},    // BodySemibold
    {12.3f, text::Weight::Semibold, false}, // Section
    {12.3f, text::Weight::Bold, false},     // SectionBold
    {11.7f, text::Weight::Bold, false},     // CountBadge
    {13.2f, text::Weight::Regular, false},  // YouLabel
    {10.1f, text::Weight::Bold, false},     // TileBold
    {18, text::Weight::Semibold, false},    // HeaderTitle
    {16, text::Weight::Semibold, false},    // UnifiedTitle
    {13, text::Weight::Bold, false},        // TabBold
    {10, text::Weight::Regular, false},     // Tiny
    {11, text::Weight::Semibold, false},    // PlateName
    {12, text::Weight::Semibold, false},    // SmallSemibold
    {28, text::Weight::Bold, false},        // CanvasTitle
};
static_assert(sizeof(kFonts) / sizeof(FontRole) == size_t(Font::Count), "kFonts out of sync");

constexpr uint32_t kSentinelMask = 0xff00ff00u, kSentinel = 0x0000a500u;

} // namespace

// ── Palettes ────────────────────────────────────────────────────────────────
// The chrome presets (kAubergineChrome, …) and their derivation rules,
// evaluated here so a pick recolours the rail, sidebar,
// selection and accent over either content mode.

namespace {

struct Spec {
    Color rail, pill, pillInk, bubble, dim; // dim 0 = derived from the rail
    Color accent, accentHover;
    Color accentDark, accentHoverDark; // 0 = the light accent lifted for dark content
    Color pressed, subtle, pressedDark, subtleDark;
};

constexpr Spec kSpecs[] = {
    {0xff3f0e40,
     0xffe1dbe1,
     0xff350d36,
     0xff4a154b,
     0xffcfc3cf,
     0xff4a154b,
     0xff611f69,
     0,
     0,
     0xff350d36,
     0xfff4e5f5},
    {0xff131313,
     0xff545454,
     0xffdedede,
     0xff333333,
     0xffc9c9c9,
     0xff5a5a5a,
     0xff6a6a6a,
     0xff5a5a5a,
     0xff6a6a6a,
     0xff4a4a4a,
     0xffededed,
     0xff4a4a4a,
     0xff333333},
    {0xff0e2a40,
     0xffdbe0e5,
     0xff0b2335,
     0xff15405e,
     0xffc3ccd4,
     0xff1264a3,
     0xff1b7cc4,
     0,
     0,
     0xff0b4f82,
     0xffe5f0f8},
    {0xff0e3d2e,
     0xffdbe5e0,
     0xff0a3124,
     0xff15543e,
     0xffc3d4cc,
     0xff007a5a,
     0xff148567,
     0,
     0,
     0xff055c42,
     0xffe5f4ee},
};
constexpr Color kDarkInk = 0xff1d1c1d, kWhite = 0xffffffff;

Palette       g_palette[2] = {Palette::Purple, Palette::Charcoal};
CustomPalette g_custom;
PaletteColors g_cache[2];
bool          g_cached[2] = {false, false};

float chan(Color c, int shift) {
    return float((c >> shift) & 0xff) / 255.f;
}

Color pack(float r, float g, float b) {
    auto q = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return 0xff000000u | q(r) << 16 | q(g) << 8 | q(b);
}

float lightness(Color c) {
    const float r = chan(c, 16), g = chan(c, 8), b = chan(c, 0);
    return (std::max({r, g, b}) + std::min({r, g, b})) / 2;
}

// Same hue and saturation (HSL), lightness pinned to l.
Color withLightness(Color c, float l) {
    const float r = chan(c, 16), g = chan(c, 8), b = chan(c, 0);
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    const float l0 = (mx + mn) / 2;
    const float s  = d == 0 ? 0 : d / (1 - std::fabs(2 * l0 - 1));
    float       h  = 0;
    if (d > 0)
        h = mx == r ? std::fmod((g - b) / d + 6, 6.f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    l             = std::clamp(l, 0.f, 1.f);
    const float C = (1 - std::fabs(2 * l - 1)) * s, X = C * (1 - std::fabs(std::fmod(h, 2.f) - 1));
    const float m                   = l - C / 2;
    static const uint8_t kSeg[6][3] = {
        {0, 1, 2}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}, {1, 2, 0}, {0, 2, 1}
    }; // which of C, X, 0 goes to r, g, b
    const float v[3] = {C, X, 0};
    const auto &k    = kSeg[std::min(5, int(h))];
    return pack(v[k[0]] + m, v[k[1]] + m, v[k[2]] + m);
}

Color mix(Color base, Color ink, float a) {
    return pack(
        chan(base, 16) * (1 - a) + chan(ink, 16) * a,
        chan(base, 8) * (1 - a) + chan(ink, 8) * a,
        chan(base, 0) * (1 - a) + chan(ink, 0) * a
    );
}

Color scaled(Color c, float f) {
    return pack(chan(c, 16) * f, chan(c, 8) * f, chan(c, 0) * f);
}

// A brand accent tuned for white content, lifted to stay visible as a filled
// control on dark surfaces.
void liftForDark(Color def, Color *accent, Color *hover, Color *pressed, Color *subtle) {
    const float l = std::max(lightness(def), 0.36f);
    *accent       = withLightness(def, l);
    *hover        = withLightness(def, l + 0.07f);
    *pressed      = withLightness(def, l - 0.06f);
    *subtle       = withLightness(def, 0.21f);
}

Spec customSpec(const CustomPalette &t, bool dark) {
    Spec        s{};
    const float shift = float(std::clamp(t.brightness, 0, 10) - 6) * 0.04f;
    const float pl    = lightness(t.primary);
    s.rail            = !t.sidebarInverted && !dark ? withLightness(t.primary, 0.93f + shift)
                        : shift == 0                ? t.primary
                                                    : withLightness(t.primary, pl + shift);
    s.bubble          = lightness(s.rail) > 0.5f ? scaled(s.rail, 0.9f) : scaled(s.rail, 1.25f);
    const float l     = lightness(t.highlight1);
    s.pill            = t.highlight1;
    s.pillInk         = t.itemSelText ? t.itemSelText : l > 0.5f ? kDarkInk : kWhite;
    s.accent          = t.highlight1;
    s.accentHover     = withLightness(t.highlight1, l + 0.06f);
    s.pressed         = withLightness(t.highlight1, l - 0.06f);
    s.subtle          = withLightness(t.highlight1, 0.95f);
    if (l > 0.7f)
        liftForDark(
            withLightness(t.highlight1, 0.45f),
            &s.accentDark,
            &s.accentHoverDark,
            &s.pressedDark,
            &s.subtleDark
        );
    return s;
}

} // namespace

PaletteColors paletteColors(Palette p, bool dark) {
    const bool    custom = p == Palette::Custom || size_t(p) >= std::size(kSpecs);
    const Spec    s      = custom ? customSpec(g_custom, dark) : kSpecs[size_t(p)];
    const bool    light  = lightness(s.rail) > 0.5f; // a pale rail takes dark ink
    PaletteColors c;
    c.rail    = s.rail;
    c.sidebar = mix(s.rail, kWhite, dark ? 0.03f : 0.12f);
    c.hover   = mix(s.rail, kWhite, dark ? 0.10f : 0.24f);
    c.bubble  = s.bubble;
    c.pill    = s.pill;
    c.pillInk = s.pillInk;
    c.text    = light ? kDarkInk : kWhite;
    c.textDim = s.dim ? s.dim : light ? mix(s.rail, kDarkInk, 0.65f) : mix(s.rail, kWhite, 0.78f);
    c.scrollThumb = light ? 0x64000000 : 0x64ffffff; // ink at alpha 100
    if (!dark) {
        c.accent        = s.accent;
        c.accentHover   = s.accentHover;
        c.accentPressed = s.pressed;
        c.accentSubtle  = s.subtle;
    } else if (s.accentDark) {
        c.accent        = s.accentDark;
        c.accentHover   = s.accentHoverDark;
        c.accentPressed = s.pressedDark;
        c.accentSubtle  = s.subtleDark;
    } else {
        liftForDark(s.accent, &c.accent, &c.accentHover, &c.accentPressed, &c.accentSubtle);
    }
    c.online = custom ? g_custom.highlight2 : kLight[size_t(C::Online)];
    c.badge  = custom ? g_custom.important : kLight[size_t(C::Badge)];
    // A custom theme's pins: dim text derives from
    // the pinned ink, so a pinned off-white keeps its cast.
    if (custom && g_custom.itemHover)
        c.hover = g_custom.itemHover;
    if (custom && g_custom.itemText) {
        c.text    = g_custom.itemText;
        c.textDim = mix(s.rail, g_custom.itemText, 0.72f);
    }
    c.titleBar        = custom && g_custom.titleBarBg ? g_custom.titleBarBg : c.rail;
    c.titleBarControl = custom && g_custom.titleBarText ? g_custom.titleBarText : c.textDim;
    return c;
}

void setPalette(bool dark, Palette p) {
    g_palette[dark] = p < Palette::Count ? p : Palette::Purple;
    g_cached[dark]  = false;
}

Palette palette(bool dark) {
    return g_palette[dark];
}

void setCustomPalette(const CustomPalette &c) {
    g_custom    = c;
    g_cached[0] = g_cached[1] = false;
}

const CustomPalette &customPalette() {
    return g_custom;
}

bool parseHexColor(std::string_view s, Color *out) {
    // Settings files hold opaque RGB: 3 or 6 digits only.
    const size_t n = s.size() - (!s.empty() && s[0] == '#');
    return (n == 3 || n == 6) && gfx::parseHexColor(s, out);
}

// The tokens the sidebar palette paints: each one's PaletteColors member,
// as a byte offset + 1 per token (0: a fixed token from kLight/kDark).
struct PaletteSlots {
    uint8_t at[size_t(C::Count)];
};
constexpr PaletteSlots paletteSlots() {
    struct Pick {
        C      c;
        size_t off;
    };
    constexpr Pick picks[] = {
        {C::Rail, offsetof(PaletteColors, rail)},
        {C::Sidebar, offsetof(PaletteColors, sidebar)},
        {C::SidebarText, offsetof(PaletteColors, text)},
        {C::SidebarTextMuted, offsetof(PaletteColors, textDim)},
        {C::SidebarHover, offsetof(PaletteColors, hover)},
        {C::SidebarSelected, offsetof(PaletteColors, pill)},
        {C::SidebarSelectedText, offsetof(PaletteColors, pillInk)},
        {C::SidebarScrollbar, offsetof(PaletteColors, scrollThumb)},
        {C::Accent, offsetof(PaletteColors, accent)},
        {C::AccentHover, offsetof(PaletteColors, accentHover)},
        {C::AccentPressed, offsetof(PaletteColors, accentPressed)},
        {C::AccentSubtle, offsetof(PaletteColors, accentSubtle)},
        {C::Online, offsetof(PaletteColors, online)},
        {C::Badge, offsetof(PaletteColors, badge)},
        {C::TitleBar, offsetof(PaletteColors, titleBar)},
        {C::TitleBarControl, offsetof(PaletteColors, titleBarControl)},
    };
    PaletteSlots t{};
    for (const Pick &p : picks)
        t.at[size_t(p.c)] = uint8_t(p.off + 1);
    return t;
}
constexpr PaletteSlots kPaletteSlots = paletteSlots();

Color colorIn(C c, bool dark) {
    const size_t i = size_t(c) < size_t(C::Count) ? size_t(c) : 0;
    if (const uint8_t at = kPaletteSlots.at[i]) {
        if (!g_cached[dark]) {
            g_cache[dark]  = paletteColors(g_palette[dark], dark);
            g_cached[dark] = true;
        }
        Color v;
        std::memcpy(&v, reinterpret_cast<const char *>(&g_cache[dark]) + (at - 1), sizeof v);
        return v;
    }
    return (dark ? kDark : kLight)[i];
}

Color color(C c) {
    return colorIn(c, app() && app()->dark());
}

Color systemHighlight() {
    const Color a = app() ? Color(app()->settings().accentColor) : 0;
    return a ? a : 0xff308cc6;
}

float metric(M m) {
    return kMetrics[size_t(m)];
}

text::Style font(Font f, C c) {
    const FontRole &r = kFonts[size_t(f)];
    text::Style     s;
    const double    ts = app() ? app()->settings().textScale : 1.0;
    const double    us = app() ? app()->userTextScale() : 1.0;
    s.size             = float(r.size * (ts > 0 ? ts : 1.0) * us);
    s.weight           = r.weight;
    s.mono             = r.mono;
    s.color            = color(c);
    return s;
}

text::Style pxFont(float px, text::Weight w, Color c) {
    text::Style s = font(Font::Body);
    s.size        = s.size * px / 15.f; // Body is 15 px here
    s.weight      = w;
    s.color       = c;
    return s;
}

Color themed(C c) {
    return kSentinel | uint32_t(c);
}

Color byTheme(uint32_t dark, uint32_t light) {
    return Color(app() && app()->dark() ? dark : light);
}

Color resolve(Color c) {
    return (c & kSentinelMask) == kSentinel ? color(C(c & 0xff)) : c;
}

void resolveSpans(text::AttributedText &t) {
    for (auto &s : t.spans) {
        s.style.color      = resolve(s.style.color);
        s.style.background = resolve(s.style.background);
    }
}

} // namespace ui
