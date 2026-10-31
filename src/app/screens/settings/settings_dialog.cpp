#include "screens/settings/settings_dialog.h"

#include "screens/settings/settings_parts.h"

#include "app/screens/common/custom_theme.h"

#include "app/media/sounds.h"
#include "app/spell/spell.h"
#ifdef MSGA_SELF_UPDATE
#include "app/update/updater.h"
#endif
#include "base/i18n.h"
#include "base/json.h"
#include "base/str.h"
#include "gfx/icons_generated.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui;
using i18n::tr;

namespace settings {

namespace {

constexpr float kPanelW = 720, kPanelH = 540;
constexpr float kListW = 175;
constexpr float kHeadH = 48;

const char *const kPageNames[] = {
    N_("Appearance"),
    N_("Notifications"),
    N_("AI assistance"),
    N_("Storage"),
    N_("System"),
    N_("About")
};
static_assert(std::size(kPageNames) == size_t(SettingsDialog::Page::Count));

const char *const kPaletteNames[] = {
    N_("Purple"), N_("Charcoal"), N_("Blue"), N_("Green"), N_("Custom")
};
static_assert(std::size(kPaletteNames) == size_t(Palette::Count));

// The Appearance / Notifications values a Save copies from the draft.
bool shell::Settings::*const kAppearanceBools[] = {
    &shell::Settings::use24h,
    &shell::Settings::threadsInline,
    &shell::Settings::linkPreviews,
    &shell::Settings::ctrlEnterSends,
    &shell::Settings::spellCheck,
    &shell::Settings::markdownFormatting,
    &shell::Settings::showAgentsApps,
    &shell::Settings::unreadsOnly,
    &shell::Settings::animateEmoji,
    &shell::Settings::animateMedia,
};
int shell::Settings::*const kAppearanceInts[] = {
    &shell::Settings::fontSize, &shell::Settings::relevantDays, &shell::Settings::names
};
bool shell::Settings::*const kNotifyBools[] = {
    &shell::Settings::notifications,
    &shell::Settings::notifyHuddles,
    &shell::Settings::boldMentionsOnly,
    &shell::Settings::notifySound,
};

// Language names stay in their own language, readable whatever the UI is.
const char *const kLanguageCodes[] = {"system", "en", "ja"};
const char *const kLanguageNames[] = {
    N_("System default"), "English", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
};

// The language msga started in: the "applied the next time" note compares to it.
std::string &startupLanguage() {
    static std::string s = "\x01"; // unset
    return s;
}

View *findIn(View *v, std::string_view name) {
    if (!v->visible())
        return nullptr;
    if (v->accessibleName() == name)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (View *f = findIn(v->child(i), name))
            return f;
    return nullptr;
}

// The grey header strip, rounded where it meets the card's top corners.
class DialogHeader final : public View {
public:
    void paint(gfx::Painter &p) override {
        const float r = metric(M::RadiusL) - 1;
        const Color c = color(C::FormHighlight);
        p.fillRoundRect(bounds(), r, c);
        p.fillRect({0, r, width(), height() - r}, c);
        p.fillRect({0, height() - 1, width(), 1}, color(C::FormDivider));
    }
};

// The page's scroll area. While the page scrolls, its content stops 8 px
// short of the right edge, room for the scroll bar.
class PageScroll final : public ScrollView {
public:
    void layout() override {
        ScrollView::layout();
        const bool bar = canScroll();
        if (bar != _bar) {
            _bar                     = bar;
            content()->style().pad.r = 24 + (bar ? 8.f : 0.f);
        }
    }

private:
    bool _bar = false;
};

// One theme preset as a miniature app: rail, conversation list with the
// selection pill and a mention badge, message lines and an accent button —
// in the palette's own colours for its row's mode.
class ThemeCard final : public Clickable {
public:
    static constexpr float kW = 90, kMockH = 64;

    ThemeCard(Palette p, bool dark) : _p(p), _dark(dark) {
        setRole(Role::Button);
        setLook({C::None, C::None, C::None, C::None, 0});
        style().size(kW, kMockH + labelH()).noShrink();
    }
    Palette     palette() const { return _p; }
    bool        dark() const { return _dark; }
    std::string accessibleName() const override {
        return i18n::arg(_dark ? tr("Dark %1") : tr("Light %1"), tr(kPaletteNames[size_t(_p)]));
    }
    void styleChanged() override {
        _l.reset();
        style().height(kMockH + labelH());
        update();
    }
    void paint(gfx::Painter &p) override {
        const PaletteColors pc = paletteColors(_p, _dark);
        const RectF         m{1.5f, 1.5f, width() - 3, kMockH - 3};
        p.save();
        p.clipRoundRect(m, 8);
        const float railW = 12, navW = 30;
        p.fillRect({m.x, m.y, railW, m.h}, pc.rail);
        p.fillRect({m.x + railW, m.y, navW, m.h}, pc.sidebar);
        p.fillRect({m.x + railW + navW, m.y, m.w - railW - navW, m.h}, colorIn(C::Surface, _dark));
        p.fillRoundRect({m.x + 2.5f, m.y + 6, 7, 7}, 2.5f, pc.bubble);
        const float nx = m.x + railW + 3, nw = navW - 6;
        p.fillRoundRect({nx, m.y + 7, nw, 6}, 2.5f, pc.pill);
        p.fillRoundRect({nx, m.y + 17, nw * 0.8f, 3.5f}, 1.75f, pc.textDim);
        p.fillRoundRect({nx, m.y + 25, nw * 0.9f, 3.5f}, 1.75f, pc.textDim);
        p.fillRoundRect({nx, m.y + 33, nw * 0.7f, 3.5f}, 1.75f, pc.textDim);
        p.fillCircle({nx + 2, m.y + 43}, 2, pc.online);
        p.fillCircle({nx + nw - 2.5f, m.y + 35}, 2.5f, pc.badge);
        const float mx = m.x + railW + navW + 5, mw = m.x + m.w - mx - 4;
        p.fillRoundRect({mx, m.y + 8, mw * 0.55f, 4.5f}, 2, colorIn(C::Text, _dark));
        p.fillRoundRect({mx, m.y + 17, mw, 3.5f}, 1.75f, colorIn(C::TextMuted, _dark));
        p.fillRoundRect({mx, m.y + 24, mw * 0.8f, 3.5f}, 1.75f, colorIn(C::TextMuted, _dark));
        p.fillRoundRect({mx, m.y + 35, mw * 0.5f, 8}, 2.5f, pc.accent);
        p.restore();
        const bool sel = checked();
        if (sel)
            p.strokeRoundRect(
                {m.x - 0.5f, m.y - 0.5f, m.w + 1, m.h + 1}, 8.5f, 2, color(C::Accent)
            );
        else
            p.strokeRoundRect(m, 8, 1, color(hovered() ? C::FormDividerStrong : C::FormDivider));
        if (!_l || _lSel != sel) {
            _lSel = sel;
            _l    = text::layoutPlain(
                tr(kPaletteNames[size_t(_p)]),
                font(sel ? Font::SmallBold : Font::Small, sel ? C::Accent : C::FormTextMuted),
                windowScale()
            );
        }
        _l->paint(
            p,
            snapPx(
                {std::floor((width() - _l->width()) / 2),
                 kMockH + std::floor((height() - kMockH - _l->height()) / 2)}
            )
        );
    }

private:
    static float                  labelH() { return std::ceil(font(Font::Small).size * 1.4f) + 12; }
    std::unique_ptr<text::Layout> _l;
    Palette                       _p;
    bool                          _dark, _lSel = false;
};

} // namespace

std::vector<std::string> strs(std::initializer_list<const char *> l) {
    return std::vector<std::string>(l.begin(), l.end());
}

// ── Frame ───────────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(screens::Context &ctx, shell::Settings &s, Hooks hooks, Page first)
    : Dialog(kPanelW, kPanelH), _ctx(ctx), _s(s), _draft(s), _hooks(std::move(hooks)),
      _p(std::make_unique<Parts>()) {
    if (startupLanguage() == "\x01")
        startupLanguage() = s.language;
    View *panel = this->panel();
    panel->style().column();

    auto *head = panel->add<DialogHeader>();
    head->style().row().height(kHeadH).padding(16, 0, 11, 1).items(Align::Center).noShrink();
    head->add<Label>(tr("Settings"), Font::DialogTitle, C::FormText)->style().flex(1);
    auto *close = head->add<IconButton>(gfx::Icon::X, tr("Close"));
    close->style().size(28, 28);
    close->setIconSize(14);
    close->setTextColor(C::FormTextFaint);
    close->setLook({C::None, C::FormHighlight, C::FormHighlight, C::None, 14});
    close->onClick = [this] { this->close(); };

    auto *bodyRow = panel->add<View>();
    bodyRow->style().row().flex(1);
    std::vector<std::string> pages;
    for (const char *n : kPageNames)
        pages.push_back(tr(n));
    _sections = bodyRow->add<SectionList>(std::move(pages));
    _sections->style().width(kListW).padding(0, 8, 0, 8).noShrink();
    _sections->onChange = [this](int i) { showPage(Page(i)); };
    _scroll             = bodyRow->add<PageScroll>();
    _scroll->style().flex(1);
    _content = _scroll->content();
    _content->style().padding(24, 16, 24, 16).spacing(16);

    showPage(first);
}

SettingsDialog::~SettingsDialog() {
    if (_glossaryTimer) {
        _ctx.app.cancelTimer(_glossaryTimer);
        saveGlossary();
    }
    _ctx.app.cancelTimer(_ramTimer);
#ifdef MSGA_SELF_UPDATE
    if (_hooks.updater)
        _hooks.updater->unlisten(_updListener);
#endif
}

void SettingsDialog::showPage(Page p) {
    if (p >= Page::Count || p == _page)
        return;
    if (_glossaryTimer) { // leaving the AI page with an edit pending
        _ctx.app.cancelTimer(_glossaryTimer);
        _glossaryTimer = 0;
        saveGlossary();
    }
    _ctx.app.cancelTimer(_ramTimer);
    _ramTimer = 0;
    _page     = p;
    _sections->setSelected(int(p));
    _content->clearChildren();
    *_p                                             = Parts{};
    static void (SettingsDialog::*const kBuild[])() = {
        &SettingsDialog::buildAppearance,
        &SettingsDialog::buildNotifications,
        &SettingsDialog::buildAi,
        &SettingsDialog::buildStorage,
        &SettingsDialog::buildSystem,
        &SettingsDialog::buildAbout,
    };
    (this->*kBuild[size_t(p)])();
    _scroll->scrollTo(0);
}

View *SettingsDialog::find(std::string_view name) const {
    return findIn(_content, name);
}

void SettingsDialog::changed() {
    if (_hooks.changed)
        _hooks.changed();
}

// ── Building blocks ─────────────────────────────────────────────────────────

Label *SettingsDialog::heading(View *parent, const char *text) {
    return parent->add<Label>(text, Font::Heading, C::FormText);
}

Label *SettingsDialog::caption(View *parent, std::string_view text, C c) {
    return parent->add<Label>(std::string(text), Font::Small, c);
}

Label *SettingsDialog::body(View *parent, std::string_view text, C c) {
    return parent->add<Label>(std::string(text), Font::Control, c);
}

text::Style linkStyle() {
    text::Style st = font(Font::Control, C::FormLink);
    st.color       = themed(C::FormLink);
    st.underline   = true;
    st.linkId      = 1;
    return st;
}

Label *SettingsDialog::link(View *parent, std::string text, std::string url) {
    text::AttributedText t;
    t.append(text, linkStyle());
    auto *l = parent->add<Label>(std::move(text), Font::Control, C::FormLink);
    l->setRichText(std::move(t));
    l->style().alignSelf(Align::Start);
    l->onLink = [this, url = std::move(url)](uint32_t) {
        if (_ctx.openUrl)
            _ctx.openUrl(url);
    };
    return l;
}

View *SettingsDialog::group(View *parent, float gap) {
    auto *g = parent->add<View>();
    g->style().spacing(gap);
    return g;
}

View *SettingsDialog::row(View *parent, float gap) {
    auto *r = parent->add<View>();
    r->style().row().spacing(gap).items(Align::Center);
    return r;
}

Button *SettingsDialog::button(View *parent, const char *label, Button::Kind k) {
    auto *b = parent->add<Button>(label, k, Button::Form::Small);
    b->style().alignSelf(Align::Start);
    return b;
}

CheckBox *SettingsDialog::check(View *parent, const char *label, bool *field, bool draft) {
    auto *c = parent->add<CheckBox>(label, *field);
    c->style().alignSelf(Align::Start);
    c->onChange = [this, field, draft](bool on) {
        *field = on;
        if (!draft)
            changed();
    };
    return c;
}

RadioGroup *
SettingsDialog::radios(View *parent, std::initializer_list<const char *> options, int *field) {
    auto *g     = parent->add<RadioGroup>(strs(options), *field);
    g->onChange = [field](int i) { *field = i; };
    return g;
}

RadioGroup *
SettingsDialog::boolRadios(View *parent, std::initializer_list<const char *> options, bool *field) {
    auto *g     = parent->add<RadioGroup>(strs(options), *field ? 1 : 0);
    g->onChange = [field](int i) { *field = i == 1; };
    return g;
}

TextField *SettingsDialog::fieldRow(View *parent, const char *label, std::string placeholder) {
    auto *col = group(parent, 4);
    body(col, label);
    return col->add<TextField>(std::move(placeholder));
}

void SettingsDialog::saveButton(void (SettingsDialog::*save)()) {
    _content->add<View>()->style().flex(1); // pinned to the bottom of short pages
    auto *r = row(_content);
    r->style().justifyContent(Justify::End);
    auto *b         = r->add<Button>(tr("Save"), Button::Kind::Primary, Button::Form::Normal);
    b->style().minW = 80;
    b->onClick      = [this, save] { (this->*save)(); };
}

// ── Appearance ──────────────────────────────────────────────────────────────

void SettingsDialog::buildAppearance() {
    shell::Settings &d = _draft;

    heading(_content, tr("Color mode"));
    {
        auto     *g    = group(_content);
        const int m    = _ctx.app.themeMode() == ThemeMode::Light  ? 0
                         : _ctx.app.themeMode() == ThemeMode::Dark ? 1
                                                                   : 2;
        auto     *mode = g->add<RadioGroup>(strs({tr("Light"), tr("Dark"), tr("System")}), m);
        mode->onChange = [this](int i) {
            // Applies at once: the dialog restyling doubles as the preview.
            const ThemeMode t = i == 0   ? ThemeMode::Light
                                : i == 1 ? ThemeMode::Dark
                                         : ThemeMode::System;
            _ctx.app.setThemeMode(t);
            _s.theme = _draft.theme = t;
            styleChanged();
            changed();
        };
        _p->modeHint = caption(g, {});
        styleChanged();
    }

    heading(_content, tr("Color theme"));
    {
        auto *rows    = group(_content, 4);
        _p->themeRows = rows;
        buildThemeRow(false);
        rows->add<View>()->style().height(13);
        buildThemeRow(true);
    }
    buildCustomEditor();

    heading(_content, tr("Font size"));
    radios(_content, {tr("Small"), tr("Medium (default)"), tr("Large")}, &d.fontSize);

    heading(_content, tr("Language"));
    {
        auto *g = group(_content);
        auto *r = row(g);
        body(r, tr("App language"));
        int sel = 0;
        for (size_t i = 0; i < std::size(kLanguageCodes); ++i)
            if (d.language == kLanguageCodes[i])
                sel = int(i);
        std::vector<std::string> names(std::begin(kLanguageNames), std::end(kLanguageNames));
        names[0]   = tr(kLanguageNames[0]); // the endonyms stay as they are
        auto *lang = r->add<Dropdown>(std::move(names), sel);
        lang->style().width(180);
        auto *note = g->add<Label>(
            tr("The new language will be applied the next time MSGA starts.\n"
               "Time and date formats update immediately."),
            Font::Small,
            C::BannerText
        );
        note->setBackground(C::BannerBg, 4);
        note->setBorder(C::BannerBorder);
        note->style().padding(8, 6);
        note->setVisible(d.language != startupLanguage());
        lang->onChange = [this, note](int i) {
            _draft.language = kLanguageCodes[i];
            note->setVisible(_draft.language != startupLanguage());
        };
    }

    heading(_content, tr("Date/Time"));
    boolRadios(_content, {tr("12-hour clock (2:34 PM)"), tr("24-hour clock (14:34)")}, &d.use24h);

    heading(_content, tr("Threads"));
    boolRadios(
        _content,
        {tr("Standalone (open replies in a side panel)"),
         tr("Inline (expand replies under the message)")},
        &d.threadsInline
    );

    heading(_content, tr("Names"));
    radios(_content, {tr("As set in Slack"), tr("Full names"), tr("Display names")}, &d.names);
    caption(_content, tr("Where a person has no display name, their full name is shown."));

    heading(_content, tr("Link previews"));
    check(_content, tr("Show link previews"), &d.linkPreviews, true);
    caption(
        _content,
        tr("Show web, app, and shared-message link previews and load their images automatically.\n"
           "When off, links stay clickable. This setting only affects your client.")
    );

    heading(_content, tr("Composer"));
#ifdef __APPLE__
    const char *sendKey = "\xE2\x8C\x98"
                          "Enter"; // native: ⌘Enter
#else
    const char *sendKey = "Ctrl+Enter";
#endif
    check(_content, i18n::arg(tr("Send with %1"), sendKey).c_str(), &d.ctrlEnterSends, true);
    {
        // Spell checking: off by default. Its languages are listed only while
        // it is on — listing them is the first thing that asks the OS about
        // spelling.
        auto *spell = check(_content, tr("Check spelling"), &d.spellCheck, true);
        auto *sec   = group(_content);
        sec->style().padding(24, 0, 0, 0); // indented under its checkbox
        caption(sec, tr("A word is underlined when none of the checked languages knows it."));
        _p->spellGrid = row(sec, 16);
        _p->spellGrid->style().items(Align::Start);
        _p->spellHint = caption(sec, {});
        _p->spellHint->setVisible(false);
        sec->setVisible(d.spellCheck);
        if (d.spellCheck)
            refreshSpellLanguages();
        spell->onChange = [this, sec](bool on) {
            _draft.spellCheck = on;
            if (on)
                refreshSpellLanguages();
            sec->setVisible(on);
        };
    }

    heading(_content, tr("Formatting"));
    boolRadios(
        _content,
        {tr("Slack (*bold*, _italic_)"), tr("Markdown (**bold**, *italic*)")},
        &d.markdownFormatting
    );
    caption(
        _content,
        tr("How the message box reads what you type.\n"
           "Messages are sent in Slack's format either way.")
    );

    heading(_content, tr("Conversations"));
    {
        auto *g       = group(_content);
        auto *r       = row(g);
        _p->daysLabel = body(r, tr("Show conversations active in the last"));
        auto *days    = r->add<SpinBox>(d.relevantDays, 1, 365, tr(" days"));
        days->style().width(90);
        days->onChange = [this](int v) { _draft.relevantDays = v; };
        _p->days       = days;
        _p->daysDesc   = caption(
            g,
            tr("Conversations with no activity in this period are hidden\n"
               "under an \"N more...\" row at the bottom of each section.")
        );
        check(g, tr("Show the Agents & apps section"), &d.showAgentsApps, true);
        auto *unread = check(g, tr("Show only unread conversations"), &d.unreadsOnly, true);
        caption(
            g,
            tr("The conversation you are reading stays listed until you move on.\n"
               "Starred conversations are always shown.")
        );
        // The activity window has nothing to decide while only unread chats
        // are listed: grey it out so the two don't read as competing.
        auto sync = [this](bool unreadsOnly) {
            _p->days->setEnabled(!unreadsOnly);
            _p->daysLabel->setColor(unreadsOnly ? C::FormTextFaint : C::FormText);
            _p->daysDesc->setColor(unreadsOnly ? C::FormTextFaint : C::FormTextMuted);
        };
        sync(d.unreadsOnly);
        unread->onChange = [this, sync](bool on) {
            _draft.unreadsOnly = on;
            sync(on);
        };
    }

    heading(_content, tr("Visual effects"));
    {
        auto *g = group(_content);
        check(g, tr("Animate emoji"), &d.animateEmoji, true);
        check(g, tr("Animate GIFs and images in messages"), &d.animateMedia, true);
        caption(
            g, tr("Turning an effect off shows a still image instead and saves memory and CPU.")
        );
    }

    heading(_content, tr("Tray icon"));
    {
        auto *g      = group(_content);
        auto *custom = g->add<CheckBox>(
            tr("Use custom tray icon"), _s.customTrayIcon && !_s.trayIconPath.empty()
        );
        custom->style().alignSelf(Align::Start);
        _p->trayCheck  = custom;
        _p->trayChange = button(g, tr("Change icon\xE2\x80\xA6"), Button::Kind::Secondary);
        _p->trayChange->setVisible(custom->checked());
        _p->trayChange->onClick = [this] { pickTrayIcon(); };
        custom->onChange        = [this](bool on) {
            _p->trayChange->setVisible(on);
            if (on && _s.trayIconPath.empty()) {
                pickTrayIcon(); // turns it on once a picture is chosen
                return;
            }
            _s.customTrayIcon = _draft.customTrayIcon = on;
            changed();
        };
        // Nothing to draw it in with System → Use tray off.
        custom->setEnabled(_s.useTray);
        _p->trayChange->setEnabled(_s.useTray);
    }

    saveButton(&SettingsDialog::saveAppearance);
}

void SettingsDialog::buildThemeRow(bool dark) {
    View *rows = _p->themeRows;
    body(rows, dark ? tr("Dark theme") : tr("Light theme"));
    auto *r = rows->add<View>();
    r->style().row().spacing(8);
    for (size_t i = 0; i < size_t(Palette::Count); ++i) {
        auto *card = r->add<ThemeCard>(Palette(i), dark);
        card->setChecked(int(i) == (dark ? _s.paletteDark : _s.paletteLight));
        _p->cards.push_back(card);
        card->onClick = [this, card, dark, i] {
            // Applies and persists at once: cheap and
            // trivially reversible. A pick for the other mode changes
            // nothing on screen until the mode flips.
            (dark ? _s.paletteDark : _s.paletteLight)         = int(i);
            (dark ? _draft.paletteDark : _draft.paletteLight) = int(i);
            for (Clickable *c : _p->cards)
                if (static_cast<ThemeCard *>(c)->dark() == dark)
                    c->setChecked(c == card);
            _s.applyPalettes();
            _ctx.app.restyle();
            refreshCustomSection();
            changed();
        };
    }
}

void SettingsDialog::refreshCustomSection() {
    if (_p->customSection)
        _p->customSection->setVisible(
            _s.paletteLight == int(Palette::Custom) || _s.paletteDark == int(Palette::Custom)
        );
}

void SettingsDialog::styleChanged() {
    if (_p && _p->modeHint) {
        // Only for System: which way it resolved right now.
        _p->modeHint->setVisible(_ctx.app.themeMode() == ThemeMode::System);
        _p->modeHint->setText(
            _ctx.app.dark() ? tr("Follows the system setting (currently dark)")
                            : tr("Follows the system setting (currently light)")
        );
    }
    Dialog::styleChanged();
}

// The shell's tray icon dialog saves the picture
// and the monochrome choice; then (or when it is cancelled) the switch shows
// what the tray now shows.
void SettingsDialog::pickTrayIcon() {
    std::weak_ptr<char> alive = _p->alive;
    const auto          sync  = [this, alive] {
        if (alive.expired() || !_p->trayCheck)
            return;
        _draft.customTrayIcon = _s.customTrayIcon;
        _draft.trayIconPath   = _s.trayIconPath;
        const bool on         = _s.customTrayIcon && !_s.trayIconPath.empty();
        _p->trayCheck->setChecked(on);
        _p->trayChange->setVisible(on);
    };
    if (_hooks.pickTrayIcon)
        _hooks.pickTrayIcon(sync);
    else
        sync();
}

// The language grid: two columns of
// checkboxes, or the hint naming the dictionary package to install. Ticks
// made since the dialog opened stay; the first time, the saved choice shows
// (or what the checker picks when there is none).
void SettingsDialog::refreshSpellLanguages() {
    std::weak_ptr<char> alive = _p->alive;
    spell::listLanguages(_ctx.app.platform(), [this, alive](spell::Available a) {
        if (alive.expired())
            return;
        if (!_p->spellFilled && _draft.spellLanguages.empty())
            _draft.spellLanguages = spell::Checker::defaultLanguages(
                a.languages, _ctx.app.platform().preferredLanguages()
            );
        _p->spellFilled = true;
        _p->spellGrid->clearChildren();
        View *cols[2] = {group(_p->spellGrid), group(_p->spellGrid)};
        for (size_t i = 0; i < a.languages.size(); ++i) {
            const std::string &code   = a.languages[i].code;
            const auto        &picked = _draft.spellLanguages;
            auto              *box    = cols[i % 2]->add<CheckBox>(
                a.languages[i].name, std::find(picked.begin(), picked.end(), code) != picked.end()
            );
            box->style().alignSelf(Align::Start);
            box->onChange = [this, code](bool on) {
                auto &l = _draft.spellLanguages;
                l.erase(std::remove(l.begin(), l.end(), code), l.end());
                if (on)
                    l.push_back(code);
            };
        }
        _p->spellGrid->setVisible(!a.languages.empty());
        if (a.languages.empty())
            _p->spellHint->setText(
                a.packageHint.empty()
                    ? std::string(tr("No spelling languages are available on this system."))
                    : i18n::arg(
                          tr("No spelling dictionaries were found. Install the one for your "
                             "language (for example the %1 package) and open Settings again."),
                          a.packageHint
                      )
            );
        _p->spellHint->setVisible(a.languages.empty());
    });
}

void SettingsDialog::saveAppearance() {
    for (bool shell::Settings::*f : kAppearanceBools)
        _s.*f = _draft.*f;
    for (int shell::Settings::*f : kAppearanceInts)
        _s.*f = _draft.*f;
    _s.language       = _draft.language;
    _s.spellLanguages = _draft.spellLanguages;
    _ctx.app.setUserTextScale(_s.fontScale());
    changed();
    close();
}

// ── Custom theme ────────────────────────────────────────────────────────────

namespace {

double luminance(Color c) {
    auto ch = [](uint32_t v) {
        const double s = double(v & 0xff) / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch(c >> 16) + 0.7152 * ch(c >> 8) + 0.0722 * ch(c);
}

double contrastRatio(Color a, Color b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

bool lowContrast(bool dark) {
    const PaletteColors pc = paletteColors(Palette::Custom, dark);
    return contrastRatio(pc.text, pc.sidebar) < 3.0;
}

} // namespace

// A custom theme from text: the ia_theme JSON ({"primary":{"hex":…} or
// {"palette":"aubergine"},…, plus the app's own "gradient" and "pins"), or the
// legacy share string (8 or 10 hex colours, comma or space separated, in
// Slack's slot order column_bg, menu_bg, active_item, active_item_text,
// hover_item, text_color, active_presence, badge[, top_nav_bg,
// top_nav_text]). Either describes a whole theme: what it doesn't name is
// the default's.
bool parseSlackTheme(std::string_view text, CustomPalette *out) {
    text = str::trim(text);
    CustomPalette t;
    if (!text.empty() && text[0] == '{') {
        json::Document d;
        if (!d.parse(std::string(text), nullptr) || !d.root().isObject())
            return false;
        if (!screens::readCustomTheme(d.root(), &t))
            return false; // some other JSON
        *out = t;
        return true;
    }
    std::vector<Color> v;
    size_t             i     = 0;
    const auto         blank = [](char c) {
        return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (i < text.size()) {
        while (i < text.size() && blank(text[i]))
            ++i;
        size_t j = i;
        while (j < text.size() && !blank(text[j]))
            ++j;
        if (j > i) {
            Color c;
            if (!parseHexColor(text.substr(i, j - i), &c))
                return false;
            v.push_back(c);
        }
        i = j;
    }
    if (v.size() != 8 && v.size() != 10)
        return false;
    t.primary     = v[0];
    t.highlight1  = v[2];
    t.itemSelText = v[3];
    t.itemHover   = v[4];
    t.itemText    = v[5];
    t.highlight2  = v[6];
    t.important   = v[7];
    if (v.size() == 10) {
        t.titleBarBg   = v[8];
        t.titleBarText = v[9];
    }
    *out = t;
    return true;
}

void SettingsDialog::buildCustomEditor() {
    auto *sec         = group(_content, 16);
    _p->customSection = sec;
    heading(sec, tr("Custom theme"));
    auto *box = group(sec);

    auto edited = [this] {
        // A whole theme may have come in: the controls follow it.
        if (_p->inverted)
            _p->inverted->setChecked(_s.custom.sidebarInverted);
        if (_p->gradient)
            _p->gradient->setChecked(_s.custom.gradient);
        _draft.custom = _s.custom;
        _s.applyPalettes();
        _ctx.app.restyle();
        _p->customStatus->setVisible(false);
        _p->contrast->setVisible(lowContrast(_ctx.app.dark()));
        changed();
    };
    auto status = [this](std::string_view text, bool error) {
        _p->customStatus->setText(std::string(text));
        _p->customStatus->setColor(error ? C::FormError : C::FormTextMuted);
        _p->customStatus->setVisible(true);
    };

    auto *imp       = row(box);
    _p->importField = imp->add<TextField>(tr("Paste a Slack theme (colour list or JSON)"));
    _p->importField->style().flex(1);
    auto *importBtn = imp->add<Button>(tr("Import"), Button::Kind::Secondary, Button::Form::Small);
    auto  doImport  = [this, edited, status] {
        CustomPalette t = _s.custom;
        if (!parseSlackTheme(_p->importField->text(), &t)) {
            status(
                tr("Not a Slack theme. Paste 8 or 10 colours separated by commas, or theme JSON."),
                true
            );
            return;
        }
        _p->importField->setText({});
        _s.custom = t;
        edited();
        status(tr("Theme imported"), false);
    };
    importBtn->onClick        = doImport;
    _p->importField->onReturn = doImport;

    auto *inverted = box->add<CheckBox>(tr("Darker sidebar"), _s.custom.sidebarInverted);
    inverted->style().alignSelf(Align::Start);
    inverted->onChange = [this, edited](bool on) {
        _s.custom.sidebarInverted = on;
        edited();
    };
    auto *gradient = box->add<CheckBox>(tr("Window gradient"), _s.custom.gradient);
    gradient->style().alignSelf(Align::Start);
    gradient->onChange = [this, edited](bool on) {
        _s.custom.gradient = on;
        edited();
    };
    _p->inverted = inverted;
    _p->gradient = gradient;
    _p->contrast =
        caption(box, tr("Low contrast: sidebar text may be hard to read"), C::FormWarning);
    _p->contrast->setVisible(lowContrast(_ctx.app.dark()));
    auto *btns = row(box);
    auto *copy = btns->add<Button>(tr("Copy theme"), Button::Kind::Secondary, Button::Form::Small);
    // Offered only when a signed-in Slack workspace can provide its theme:
    // the redesign theme, else
    // the legacy colours.
    auto *mine =
        btns->add<Button>(tr("Use my Slack theme"), Button::Kind::Secondary, Button::Form::Small);
    mine->setVisible(bool(_hooks.fetchSlackTheme));
    mine->onClick = [this, edited, status] {
        status(tr("Reading your Slack theme\xE2\x80\xA6"), false);
        std::weak_ptr<char> alive = _p->alive;
        _hooks.fetchSlackTheme(
            [this, alive, edited, status](std::string ia, std::string legacy, std::string err) {
                if (alive.expired())
                    return;
                if (!err.empty()) {
                    status(i18n::arg(tr("Could not read your Slack theme (%1)"), err), true);
                    return;
                }
                CustomPalette t = _s.custom;
                if (!parseSlackTheme(ia, &t) && !parseSlackTheme(legacy, &t)) {
                    status(tr("Your Slack account has no custom theme"), true);
                    return;
                }
                _s.custom = t;
                edited();
                status(tr("Slack theme applied"), false);
            }
        );
    };
    copy->onClick = [this, status] {
        // Slack's 10-value share string, from the variant on screen.
        const PaletteColors c   = paletteColors(Palette::Custom, _ctx.app.dark());
        const Color         v[] = {
            c.rail,
            c.hover,
            c.pill,
            c.pillInk,
            c.hover,
            c.text,
            c.online,
            c.badge,
            c.titleBar,
            c.titleBarControl
        };
        std::string s;
        for (Color x : v)
            s += (s.empty() ? "" : ",") + hexColor(x);
        _ctx.app.platform().setClipboardText(std::move(s));
        status(tr("Theme copied: paste it into Slack's Import theme field"), false);
    };
    _p->customStatus = caption(box, {});
    _p->customStatus->setVisible(false);
    refreshCustomSection();
}

// ── Notifications ───────────────────────────────────────────────────────────

namespace {

// The sound dropdown's options: bundled sounds, a divider, the OS's sounds.
// The stored id is selected; one that isn't offered (once the OS's list is
// in: `final`) falls back to the first option, which saving then keeps.
void fillSounds(
    Dropdown                  *pick,
    std::vector<std::string>  &ids,
    std::string               &soundId,
    std::vector<sounds::Entry> system,
    bool                       final
) {
    std::vector<sounds::Entry> all = sounds::bundled();
    const int                  sep = system.empty() ? -1 : int(all.size()) - 1;
    for (auto &e : system)
        all.push_back(std::move(e));
    std::vector<std::string> labels;
    ids.clear();
    int selected = -1;
    for (auto &e : all) {
        if (e.id == soundId && selected < 0)
            selected = int(labels.size());
        ids.push_back(std::move(e.id));
        labels.push_back(std::move(e.label));
    }
    if (selected < 0 && final)
        soundId = ids[0];
    pick->setSeparatorAfter(sep);
    pick->setOptions(std::move(labels), std::max(selected, 0));
}

} // namespace

void SettingsDialog::buildNotifications() {
    shell::Settings &d = _draft;
    auto *master    = check(_content, tr("Enable desktop notifications"), &d.notifications, true);
    _p->notifyLevel = radios(
        _content, {tr("All new messages"), tr("Direct messages and mentions only")}, &d.notifyLevel
    );
    _p->notifyHuddles =
        check(_content, tr("Notify me when a huddle starts"), &d.notifyHuddles, true);
    // Shapes the conversation list, not alerts: independent of the master switch.
    check(
        _content,
        tr("Highlight mentions-only channels for any new message"),
        &d.boldMentionsOnly,
        true
    );
    _p->notifySound = check(_content, tr("Play a sound for notifications"), &d.notifySound, true);

    // The sound chooser: the bundled chime, then the OS's own sounds (listed
    // off the UI thread, afresh each time the page opens), and a preview.
    _p->soundRow = row(_content);
    body(_p->soundRow, tr("Sound:"));
    _p->sound = _p->soundRow->add<Dropdown>(std::vector<std::string>{}, 0);
    _p->sound->style().flex(1);
    _p->sound->style().minW = 220;
    _p->sound->onChange     = [this](int i) {
        if (size_t(i) < _p->soundIds.size())
            _draft.soundId = _p->soundIds[size_t(i)];
    };
    fillSounds(_p->sound, _p->soundIds, _draft.soundId, {}, false);
    sounds::systemSounds(
        _ctx.app.platform(),
        [this, alive = std::weak_ptr<char>(_p->alive)](std::vector<sounds::Entry> sys) {
            if (!alive.expired())
                fillSounds(_p->sound, _p->soundIds, _draft.soundId, std::move(sys), true);
        }
    );
    _p->soundRow->add<Button>(tr("Test"), Button::Kind::Secondary, Button::Form::Small)->onClick =
        [this] {
            const size_t i = size_t(_p->sound->selected());
            if (i < _p->soundIds.size())
                sounds::play(_ctx.app.platform(), _p->soundIds[i]);
        };

    heading(_content, tr("Sample notifications"));
    auto *sr = row(_content);
    _p->sample =
        sr->add<Dropdown>(strs({tr("New DM"), tr("New channel message"), tr("New huddle")}), 0);
    _p->sample->style().flex(1);
    _p->sample->style().minW = 220;
    _p->sampleTest   = sr->add<Button>(tr("Test"), Button::Kind::Secondary, Button::Form::Small);
    _p->sampleResult = caption(_content, {});
    _p->sampleResult->setVisible(false);
    _p->sampleTest->onClick = [this] {
        // A representative, self-contained notification (no real conversation).
        const int          k    = _p->sample->selected();
        const char        *user = tr("Sample User");
        plat::Notification n;
        n.title  = k == 0 ? user : "#general";
        n.body   = k == 0   ? std::string(tr("Hey \xE2\x80\x94 do you have a minute?"))
                   : k == 1 ? i18n::arg(tr("%1: Heads up, the deploy is going out at 3pm"), user)
                            : i18n::arg(tr("%1 started a huddle"), user);
        n.silent = sounds::kSilentNotifications; // the sample: no chime of ours either
        if (k == 2)                              // the sample huddle has the real one's "Join"
            n.actions.push_back({"join", tr("Join")});
        _p->sampleResult->setText({});
        _p->sampleResult->setVisible(false);
        if (!_hooks.testNotification) {
            // Never resolved (no Shell to click through to): any id will do.
            static uint64_t id = 0;
            _ctx.app.platform().notify(++id, n);
            return;
        }
        std::weak_ptr<char> alive = _p->alive;
        _hooks.testNotification(std::move(n), [this, alive](const std::string &text) {
            if (alive.expired())
                return; // another page is up, or the dialog is gone
            _p->sampleResult->setText(text);
            _p->sampleResult->setVisible(!text.empty());
        });
    };

    master->onChange = [this](bool on) {
        _draft.notifications = on;
        syncNotifyEnabled();
    };
    _p->notifySound->onChange = [this](bool on) {
        _draft.notifySound = on;
        syncNotifyEnabled();
    };
    syncNotifyEnabled();
    saveButton(&SettingsDialog::saveNotifications);
}

void SettingsDialog::syncNotifyEnabled() {
    const bool on = _draft.notifications;
    _p->notifyLevel->setEnabled(on);
    _p->notifyHuddles->setEnabled(on);
    _p->notifySound->setEnabled(on);
    _p->soundRow->setEnabled(on && _draft.notifySound);
    _p->sample->setEnabled(on);
    _p->sampleTest->setEnabled(on);
}

void SettingsDialog::saveNotifications() {
    for (bool shell::Settings::*f : kNotifyBools)
        _s.*f = _draft.*f;
    _s.notifyLevel = _draft.notifyLevel;
    _s.soundId     = _draft.soundId;
    changed();
    close();
}

} // namespace settings
