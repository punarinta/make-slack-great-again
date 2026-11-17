#include "screens/shell/status_dialog.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/common/file_dialogs.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#endif

using namespace ui;
using gfx::Icon;
using i18n::tr;

namespace shell {

namespace {

// "Clear after", in menu order.
enum ClearAfter { DontClear, Min30, Hour1, Hour4, Today, ThisWeek };
const char *const kDurations[] = {
    N_("Don't clear"), N_("30 minutes"), N_("1 hour"), N_("4 hours"), N_("Today"), N_("This week")
};
struct Preset {
    const char *emoji, *text;
    int         clearAfter;
};
constexpr Preset kPresets[] = {
    {"calendar", N_("In a meeting"), Hour1},
    {"bus", N_("Commuting"), Min30},
    {"face_with_thermometer", N_("Out sick"), Today},
    {"palm_tree", N_("Vacationing"), DontClear},
    {"house_with_garden", N_("Working remotely"), Today},
};
constexpr float kCardW = 560;

} // namespace

StatusDialog::StatusDialog(screens::Context &ctx) : Dialog(tr("Set a status"), kCardW), _ctx(ctx) {
    View *card = content(); // the titled card

    // The input: emoji button + one-line field in a bordered box.
    _inputBox = card->add<View>();
    _inputBox->style().row().items(Align::Center).padding(8, 4).spacing(8).height(40);
    _inputBox->setBackground(C::FormBg, 6);
    _inputBox->setBorder(C::FieldBorder);
    _emojiBtn = _inputBox->add<Clickable>();
    _emojiBtn->style().size(30, 30).stack().items(Align::Center).noShrink();
    _emojiBtn->setLook({C::None, C::FormSunken, C::FormSunken, C::None, 6});
    _smiley     = _emojiBtn->add<IconView>(Icon::Smile, 20, C::FormTextFaint);
    _emojiGlyph = _emojiBtn->add<Label>("", Font::Title);
    _emojiGlyph->setVisible(false);
    _emojiBtn->onClick = [this] {
#ifdef MSGA_HAVE_MESSAGES
        if (Window *w = window())
            screens::EmojiPicker::show(
                *w, _emojiBtn->windowRect(), _ctx, [this](const std::string &n) {
                    setEmoji(n);
                    _text->focus();
                }
            );
#endif
    };
    _text = _inputBox->add<TextEdit>();
    _text->style().flex(1);
    _text->setMaxLines(1);
    _text->setFont(Font::Field);
    _text->setPlaceholder(tr("What's your status?"));
    _text->onSubmit = [this] { return save(), true; };
    _text->onChange = [this] {
        if (_text->text().size() > 100) // Slack caps status_text at 100
            _text->setText(_text->text().substr(0, 100));
    };

    auto *clear = card->add<View>();
    clear->style().row().items(Align::Center).spacing(8).margins(0, 12, 0, 8);
    clear->add<Label>(tr("Clear after"), Font::Field, C::FormText);
    _clearAfter = clear->add<Dropdown>([] {
        std::vector<std::string> v;
        for (const char *d : kDurations)
            v.push_back(tr(d));
        return v;
    }());
    _clearAfter->style().flex(1).height(kFormNormalH);

    const std::string &ws  = ctx.store().workspaceName;
    auto              *hdr = card->add<Label>(
        ws.empty() ? std::string(tr("Suggestions")) : i18n::arg(tr("For %1"), ws),
        Font::SmallBold,
        C::FormTextMuted
    );
    hdr->style().margins(0, 0, 0, 2); // sp.xs under it
    auto *list = card->add<View>();
    list->style().spacing(12);
    for (int i = 0; i < int(std::size(kPresets)); ++i) {
        const Preset &p   = kPresets[i];
        auto         *row = list->add<Clickable>();
        row->setLook({C::None, C::FormSunken, C::FormSunken, C::None, 6});
        row->style().row().items(Align::Center).padding(8, 4).spacing(8);
        auto *g = row->add<Label>(ctx.store().emojiFor(p.emoji).unicode, Font::Title);
        g->style().width(26);
        g->setLineHeight(1.15f); // a 20 px glyph, rows 29 px apart from 12
        // "<b>In a meeting</b>  —  1 hour": the name bold, the duration dim.
        text::AttributedText t;
        text::Style          bold = font(Font::BodySemibold), dim = font(Font::Body);
        bold.weight = text::Weight::Bold;
        bold.color  = themed(C::FormText);
        dim.color   = themed(C::FormTextMuted);
        t.append(tr(p.text), bold);
        t.append(str::concat({"  \xE2\x80\x94  ", tr(kDurations[p.clearAfter])}), dim);
        auto *l = row->add<Label>("", Font::Body, C::FormText);
        l->setRichText(std::move(t));
        l->setHitTransparent(true);
        row->onClick = [this, i] { applyPreset(i); };
    }

    const auto &me       = ctx.store().user(ctx.store().me);
    Button     *clearBtn = nullptr;
    if (!me.statusText.empty() || !me.statusEmoji.empty()) {
        clearBtn          = makeButton(tr("Clear status"), Button::Kind::Ghost);
        clearBtn->onClick = [this] { clearStatus(); };
    }
    auto *save    = makeButton(tr("Save"), Button::Kind::Primary);
    save->onClick = [this] { this->save(); };
    addButtonRow(save, makeButton(tr("Cancel"), Button::Kind::Secondary), clearBtn)
        ->style()
        .margins(0, 12, 0, 0);

    // Prefilled with what is set, so reopening shows it.
    _text->setText(me.statusText);
    setEmoji(me.statusEmoji);
}

StatusDialog *StatusDialog::show(Window &w, screens::Context &ctx) {
    auto *d = new StatusDialog(ctx);
    w.showPopup(std::unique_ptr<Popup>(d));
    d->_text->focus();
    return d;
}

void StatusDialog::setEmoji(std::string shortcode) {
    _emoji = std::move(shortcode);
    const std::string glyph =
        _emoji.empty() ? std::string() : _ctx.store().emojiFor(_emoji).unicode;
    _smiley->setVisible(glyph.empty());
    _emojiGlyph->setVisible(!glyph.empty());
    _emojiGlyph->setText(glyph);
}

void StatusDialog::applyPreset(int i) {
    const Preset &p = kPresets[i];
    setEmoji(p.emoji);
    _text->setText(tr(p.text));
    _text->setSelection(uint32_t(_text->text().size()), uint32_t(_text->text().size()));
    _clearAfter->setSelected(p.clearAfter);
    _text->focus();
}

int64_t StatusDialog::expiry(int64_t now) const {
    const base::CivilTime t = base::localTime(now);
    switch (_clearAfter->selected()) {
    case Min30:
        return now + 30 * 60;
    case Hour1:
        return now + 60 * 60;
    case Hour4:
        return now + 4 * 60 * 60;
    case Today: // the next local midnight
        return base::fromLocal(t.year, t.month, t.day + 1);
    case ThisWeek: { // next Monday at local midnight
        const int toMonday = (8 - (t.weekday == 0 ? 7 : t.weekday)) % 7;
        return base::fromLocal(t.year, t.month, t.day + (toMonday == 0 ? 7 : toMonday));
    }
    default:
        return 0;
    }
}

void StatusDialog::apply(std::string emoji, std::string text, int64_t expiry) {
    _ctx.backend().setStatus(
        std::move(emoji),
        std::move(text),
        expiry,
        [onError = onError](bool ok, const std::string &err) {
            if (!ok && onError)
                onError(
                    i18n::arg(tr("Could not set status: %1"), err) +
                    (err == "missing_scope"
                         ? tr(" \xE2\x80\x94 sign in to this workspace again to grant the new "
                              "permission")
                         : std::string())
                );
        }
    );
}

void StatusDialog::save() {
    std::string text(str::trim(_text->text()));
    // No text and no emoji: the status is cleared.
    if (text.empty() && _emoji.empty())
        apply({}, {}, 0);
    else
        apply(_emoji, std::move(text), expiry(base::nowSecs()));
    accept();
}

void StatusDialog::clearStatus() {
    apply({}, {}, 0);
    accept();
}

// ── Profile ─────────────────────────────────────────────────────────────────

namespace {

// The 120 px rounded avatar (radius 20) with the camera on a dark wash on
// hover, "Change photo".
class ProfileAvatar final : public Clickable {
public:
    ProfileAvatar() {
        style().size(120, 120).alignSelf(Align::Center).noShrink();
        setLook({C::None, C::None, C::None, C::None, 20});
        setTooltip(tr("Change photo"));
        setHoverRepaint(true);
        image = add<Image>();
        image->style().size(120, 120);
        image->setRadius(20);
        image->setPlaceholder(C::PresenceAway);
        image->setHitTransparent(true);
    }
    void paintOver(gfx::Painter &p) override {
        if (!hovered())
            return;
        p.fillRoundRect(bounds(), 20, 0x6e000000U);
        const float d = 36;
        gfx::drawIcon(p, Icon::Camera, {(width() - d) / 2, (height() - d) / 2, d, d}, 0xffffffffU);
    }
    Image *image;
};

} // namespace

ProfileDialog::ProfileDialog(screens::Context &ctx, Avatars &avatars)
    : Dialog(tr("Profile"), kCardW), _ctx(ctx), _avatars(avatars) {
    View *card = content();

    auto *avatar = card->add<ProfileAvatar>();
    avatar->style().margins(0, 0, 0, 16);
    _avatar         = avatar->image;
    avatar->onClick = [this] { pickPhoto(); };

    Label *lastLabel = nullptr;
    auto   field     = [&](const char *label, const char *placeholder) {
        lastLabel    = card->add<Label>(label, Font::BodyBold, C::FormText);
        auto *f      = card->add<TextField>(placeholder);
        f->style().h = kFormNormalH;
        f->style().margins(0, 0, 0, 8);
        return f;
    };
    _name             = field(tr("Name"), tr("Your display name"));
    _email            = field(tr("Email"), tr("name@example.com"));
    Label *emailLabel = lastLabel;
    _phone            = field(tr("Phone"), tr("Optional"));
    // A profile that is only a name and a picture (Claude Code) has no contacts.
    if (!_ctx.backend().capabilities().profileContact)
        for (View *v : {(View *)emailLabel, (View *)_email, (View *)lastLabel, (View *)_phone})
            v->setVisible(false);
    // StyledLineEdit's max-length counter: the characters left of 80.
    auto *left = _name->add<Label>("80", Font::Caption, C::FormTextFaint);
    left->style().noShrink();
    _name->edit().onChange = [this, left] {
        size_t n = utf8::countCodePoints(_name->text());
        if (n > 80) { // Slack's cap: keep the first 80
            _name->setText(_name->text().substr(0, utf8::prefixBytes(_name->text(), 80)));
            n = 80;
        }
        left->setText(str::number(int64_t(80) - int64_t(n)));
    };
    _status = card->add<Label>("", Font::Caption, C::FormTextMuted);
    _status->setVisible(false);
    _save          = makeButton(tr("Save changes"), Button::Kind::Primary);
    _save->onClick = [this] { save(); };
    addButtonRow(_save, makeButton(tr("Cancel"), Button::Kind::Secondary))
        ->style()
        .margins(0, 8, 0, 0);

    // What is known now, refined by the profile load.
    const auto &me = ctx.store().user(ctx.store().me);
    _name->setText(me.label());
    _avatar->setBitmap(avatars.get(me.avatar, 240));
    std::weak_ptr<int> alive = _alive;
    ctx.backend().loadMyProfile([this, alive](model::Backend::MyProfile p) {
        if (alive.expired())
            return;
        _loaded   = p;
        _loadedOk = true;
        _name->setText(p.displayName.empty() ? p.realName : p.displayName);
        _email->setText(p.email);
        _phone->setText(p.phone);
        _avatar->setBitmap(_avatars.get(p.avatar, 240));
    });
}

ProfileDialog *ProfileDialog::show(Window &w, screens::Context &ctx, Avatars &avatars) {
    auto *d = new ProfileDialog(ctx, avatars);
    w.showPopup(std::unique_ptr<Popup>(d));
    d->_name->edit().focus();
    return d;
}

void ProfileDialog::status(std::string text, bool error) {
    _status->setText(std::move(text));
    _status->setColor(error ? C::FormError : C::FormTextMuted);
    _status->setVisible(!_status->text().empty());
    invalidateLayout();
}

void ProfileDialog::pickPhoto() {
    std::weak_ptr<int>   alive = _alive;
    plat::FileDialogDesc d;
    d.mode    = plat::FileDialogDesc::Mode::Open;
    d.title   = tr("Choose a profile photo");
    d.filters = {{tr("Images"), {"*.png", "*.jpg", "*.jpeg", "*.gif"}}};
    screens::fileDialog(_ctx, std::move(d), [this, alive](std::vector<std::string> paths) {
        if (alive.expired() || paths.empty())
            return;
        status(tr("Uploading photo\xE2\x80\xA6"), false);
        _ctx.backend().setPhoto(paths[0], [this, alive](bool ok, const std::string &err) {
            if (alive.expired())
                return;
            if (!ok)
                return status(i18n::arg(tr("Could not upload photo: %1"), err), true);
            status(tr("Photo updated."), false);
            _avatar->setBitmap(_avatars.get(_ctx.store().user(_ctx.store().me).avatar, 240));
        });
    });
}

void ProfileDialog::save() {
    if (!_loadedOk)
        return;
    const std::string name(str::trim(_name->text())), email(str::trim(_email->text())),
        phone(str::trim(_phone->text()));
    if (name == _loaded.displayName && email == _loaded.email && phone == _loaded.phone) {
        accept();
        return;
    }
    _save->setEnabled(false);
    status(tr("Saving\xE2\x80\xA6"), false);
    std::weak_ptr<int> alive = _alive;
    _ctx.backend().updateProfile(
        name, email, phone, [this, alive](bool ok, const std::string &err) {
            if (alive.expired())
                return;
            if (!ok) {
                _save->setEnabled(true);
                return status(i18n::arg(tr("Could not save: %1"), err), true);
            }
            accept();
        }
    );
}

} // namespace shell
