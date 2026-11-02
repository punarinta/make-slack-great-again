// Design tokens: one flat table of colours per variant (light/dark) plus
// shared metrics and font roles. Views store token ids, never resolved
// colours, so switching the theme is a repaint (and a text re-layout), not a
// tree rebuild. Resolved through the current App's theme.
#pragma once

#include "gfx/gfx.h"
#include "text/text.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ui {

using gfx::Color;

// Colour tokens. Names say where they are used; the tables in theme.cpp are
// the only place with hex values.
enum class C : uint8_t {
    None = 0, // "no colour" (no background, no border)
    WindowBg,
    Surface,      // message pane, popups' content
    SurfaceHover, // hovered message row
    Rail,         // workspace rail
    Sidebar,
    SidebarText,
    SidebarTextMuted,
    SidebarHover,
    SidebarSelected,
    SidebarSelectedText,
    SidebarScrollbar, // chats-list thumb (nav.scrollThumb)
    Text,
    TextMuted,
    TextFaint,
    Link,
    Accent,
    AccentHover,
    AccentText, // text on Accent
    Border,
    BorderStrong,
    Hover,   // generic hover wash (buttons on Surface)
    Pressed, // generic pressed wash
    Selection,
    Caret,
    FocusRing,
    Badge,
    BadgeText,
    PopupBg,
    PopupBorder,
    Shadow,
    TooltipBg,
    TooltipText,
    Scrollbar,
    ScrollbarHover,
    InputBg,
    InputBorder,
    InputBorderFocus,
    Placeholder,
    CodeBg,
    CodeText,
    MentionBg,
    MentionText,
    MentionSelfBg, // a mention of me, @here/@channel, my usergroup (yellow)
    Danger,
    Online,
    // Form controls and dialogs (Settings): the form palette, so every
    // dialog reads the same (surface.raised/sunken/highlight, the divider
    // pair, text.primary/secondary/tertiary, …).
    FormBg,              // dialog card, inputs, secondary buttons
    FormSunken,          // section list, secondary button hover
    FormHighlight,       // dialog header, hovered section / icon button
    FormHighlightStrong, // selected section, pressed secondary button
    FormDivider,         // header rule, section list edge, framed boxes
    FormDividerStrong,   // dialog border, checkbox/radio/spin box border
    FormText,
    FormTextMuted, // hints, captions
    FormTextFaint, // "Last checked", hover border of indicators
    FormLink,
    FormError,   // red warning text
    FormWarning, // amber advisory text
    FieldBorder, // text fields, dropdowns, the glossary
    FieldBorderFocus,
    FieldWell, // inside a checkbox / radio / spin box
    DangerFill,
    DangerFillHover,
    BannerBg, // "applied the next time msga starts"
    BannerBorder,
    BannerText,
    UpdateBannerBg, // updateBanner.*: the update bar
    UpdateBannerBorder,
    UpdateBannerText,
    TitleBar, // titleBar.bg / controlDefault: the palette's, a custom theme's pins
    TitleBarControl,
    // The icon/composer/badge tokens the shell paints with
    // (icon.starred, composer.toolbarIcon(Active), composer.dropArrow,
    // badge.activity, presence.away / phantom, editBanner.accent).
    IconStarred,
    ComposerIcon,
    ComposerIconActive,
    DropArrow,
    BadgeActivity,
    PresenceAway,
    PresencePhantom,
    BannerAccent,
    ChipBg, // composer.attachmentChip*: pending attachment cards
    ChipBorder,
    OverlayBg, // composer.attachmentOverlay*: the name plates on them
    OverlayText,
    // The rest of the palette that dialogs, toolbars, menus, cards and
    // message rows paint with.
    AccentPressed,  // accent.pressed (follows the palette, like Accent)
    AccentSubtle,   // accent.subtleBg (follows the palette)
    FormIcon,       // icon.def
    FormIconStrong, // icon.strong
    OnDarkDim,      // text.onDarkDim (on TooltipBg)
    RowHover,       // message.hover (toolbar button wash)
    FileChipBg,     // message.fileChipBg
    FileChipBorder,
    FileNameDim, // message.fileNameDim
    ReplyLink,   // message.replyLink
    TableBorder, // message.tableBorder
    TableHeaderBg,
    TableRowRule,
    ViewerBackdrop, // surface.viewerBackdrop
    PinnedBg,       // message.pinnedBg
    ReminderBg,     // message.reminderBg
    MenuBg,         // contextMenu.bg
    MenuText,
    MenuDanger,
    MenuHover,
    MenuSeparator,
    DividerSubtle, // divider.subtle: the rule under macOS's unified header
    Count
};

// Metrics (logical px), shared by both variants.
enum class M : uint8_t {
    RadiusS,
    RadiusM,
    RadiusL,
    SpaceXS,
    SpaceS,
    SpaceM,
    SpaceL,
    SpaceXL,
    ControlH,   // buttons, sidebar rows
    ScrollbarW, // thumb width (widens on hover)
    Count
};

// Font roles → text::Style (size scaled by the OS text-size preference).
// Control (13), ControlBold (13 semibold), Heading (14 semibold), DialogTitle
// (15 semibold) and Field (14) are the dialogs' form sizes.
enum class Font : uint8_t {
    Small,
    SmallBold,
    Body,
    BodyBold,
    Title,
    Mono,
    Caption,
    Control,
    ControlBold,
    Heading,
    DialogTitle,
    Field,
    // The sidebar/badge faces: demiBold, the section
    // label (0.82, DemiBold / Bold), countBadge (0.78 bold), youLabel (0.88).
    BodySemibold,
    Section,
    SectionBold,
    CountBadge,
    YouLabel,
    TileBold,      // the group-DM tile's count (0.38 of its 20 px)
    HeaderTitle,   // the conversation header's name (fonts.xxl, 600)
    UnifiedTitle,  // …and the macOS unified header's, the workspace's there (fonts.xl, 600)
    TabBold,       // the active Messages / canvas tab (fonts.md, bold)
    Tiny,          // fonts.xs: "Also send to channel"
    PlateName,     // fonts.sm semibold: attachment chip names
    SmallSemibold, // fonts.caption 600: "Editing message"
    CanvasTitle,   // the canvas page's title line (28 px bold)
    Count
};

Color       color(C c);
// A token in a given variant regardless of the current one (theme previews).
Color       colorIn(C c, bool dark);
// The system's text-selection highlight: the OS accent colour, a default
// blue where the OS reports none. Selected text is drawn white on it.
Color       systemHighlight();
float       metric(M m);
text::Style font(Font f, C color = C::Text);

// A style for a size given in pixels (13, 14, …):
// Body scaled by px / 15, so it follows the text-size preference too.
text::Style pxFont(float px, text::Weight w, Color c);

// Sentinel colours for text::AttributedText handed to Label/TextEdit:
// themed(C::Link) is resolved when the layout is built, so rich text follows
// theme switches too. resolve() maps a sentinel to the live colour and
// passes ordinary colours through (a sentinel has alpha 0 and a marker byte).
Color themed(C c);
// A raw colour per theme, for values the token table doesn't
// carry (message.* tints); resolved now, not a sentinel.
Color byTheme(uint32_t dark, uint32_t light);
Color resolve(Color c);
// Resolves colour and background sentinels in every span.
void  resolveSpans(text::AttributedText &t);

// ── Palettes ────────────────────────────────────────────────────────────────
// The chrome colour sets of the theme presets (Settings → Color
// theme): rail, sidebar, selection pill and accent over light or dark content.
// Each content mode keeps its own pick; the content tokens stay as tabled.
enum class Palette : uint8_t { Purple, Charcoal, Blue, Green, Custom, Count };

// A user-defined palette in Slack's shape: four colours and the switches.
struct CustomPalette {
    Color primary         = 0xff3f0e40; // the rail
    Color highlight1      = 0xff3f0e40; // selection pill and accent
    Color highlight2      = 0xff2bac76; // presence dot
    Color important       = 0xffcd2553; // mention badge
    int   brightness      = 6;          // 0–10, 6 = the rail as designed
    bool  sidebarInverted = true;       // a dark rail over light content
    bool  gradient        = true;       // kept for Slack round trips; drawn flat
    // Pins a legacy Slack theme string names outright; 0 = derive: menu_bg / hover_item,
    // active_item_text, text_color, top_nav_bg, top_nav_text.
    Color itemHover = 0, itemSelText = 0, itemText = 0, titleBarBg = 0, titleBarText = 0;
};

// Everything a preview card paints for one palette in one mode.
struct PaletteColors {
    Color rail, sidebar, bubble, hover, pill, pillInk, text, textDim, scrollThumb;
    Color accent, accentHover, accentPressed, accentSubtle, online, badge;
    Color titleBar, titleBarControl; // the rail and the dim text unless pinned
};

// The picks; defaults are Purple (light) and Charcoal (dark). Call
// App::restyle() afterwards to repaint.
void                 setPalette(bool dark, Palette p);
Palette              palette(bool dark);
void                 setCustomPalette(const CustomPalette &c);
const CustomPalette &customPalette();
PaletteColors        paletteColors(Palette p, bool dark);
// "#3f0e40" ↔ Color (for settings files); false/0 on garbage. hexColor is
// gfx's.
bool                 parseHexColor(std::string_view s, Color *out);

using gfx::hexColor;

} // namespace ui
