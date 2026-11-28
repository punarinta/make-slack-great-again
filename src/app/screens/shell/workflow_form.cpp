#include "screens/shell/workflow_form.h"

#include "app/mrkdwn/mrkdwn.h"
#include "app/screens/common/user_search.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/shell/shell_text.h"
#include "ui/controls.h"
#include "ui/datetime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

using namespace ui;
using i18n::tr;
using Kind = model::FormField::Kind;

namespace shell {

// ── Checks ──────────────────────────────────────────────────────────────────

namespace {

bool isNumber(std::string_view s, bool decimal) {
    s = str::trim(s);
    if (s.empty())
        return false;
    if (s.front() == '-' || s.front() == '+')
        s.remove_prefix(1);
    bool digits = false, dot = false;
    for (char c : s) {
        if (c >= '0' && c <= '9')
            digits = true;
        else if (c == '.' && decimal && !dot)
            dot = true;
        else
            return false;
    }
    return digits;
}

bool answers(Kind k) {
    return k != Kind::Note && k != Kind::Unsupported;
}

} // namespace

std::vector<std::string>
formProblems(const model::Form &form, const std::vector<model::FormAnswer> &answers) {
    static const model::FormAnswer kNone;
    std::vector<std::string>       out(form.fields.size());
    for (size_t i = 0; i < form.fields.size(); ++i) {
        const model::FormField  &f = form.fields[i];
        const model::FormAnswer &a = i < answers.size() ? answers[i] : kNone;
        const bool               text =
            f.kind == Kind::Text || f.kind == Kind::Paragraph || f.kind == Kind::Number;
        const bool empty = text ? str::trim(a.text).empty() : a.empty();
        if (!::shell::answers(f.kind))
            continue;
        if (empty) {
            if (!f.optional)
                out[i] = tr("Please fill in this field.");
            continue;
        }
        if (f.kind == Kind::Number && !isNumber(a.text, f.decimal))
            out[i] = f.decimal ? std::string(tr("Please enter a number."))
                               : std::string(tr("Please enter a whole number."));
        else if (
            text && f.minLength > 0 &&
            int(utf8::countCodePoints(str::trim(a.text))) < int(f.minLength)
        )
            out[i] =
                i18n::arg(tr("Please enter at least %1 characters."), str::number(f.minLength));
    }
    return out;
}

bool formNeedsSlack(const model::Form &form) {
    for (const model::FormField &f : form.fields)
        if (f.kind == Kind::Unsupported && !f.optional)
            return true;
    return false;
}

namespace {

// ── People and channels ─────────────────────────────────────────────────────

struct Candidate {
    std::string id, label;
};

class PickField;

// The drop-down under the search: the matches, the current one highlighted.
class Matches final : public Popup {
public:
    Matches(PickField &owner, std::vector<Candidate> items)
        : _owner(owner), _items(std::move(items)) {
        setModal(false);
        setCard(false);
    }
    SizeF measureContent(float, float) override {
        const float n = float(std::min<size_t>(_items.size(), 6));
        return {_w, rowH() * n + 4};
    }
    void  setWidth(float w) { _w = w; }
    float rowH() const { return std::ceil(pxFont(15, text::Weight::Regular, 0).size * 1.4f) + 12; }
    void  paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 4, color(C::FormBg));
        p.save();
        p.clipRoundRect(b, 4);
        for (size_t i = 0; i < _items.size(); ++i) {
            const float y = 2 + rowH() * float(i) - _scroll;
            if (y + rowH() < 0 || y > b.h)
                continue;
            const bool cur = int(i) == _current;
            if (cur)
                p.fillRect({1, y, b.w - 2, rowH()}, color(C::AccentSubtle));
            else if (int(i) == _hover)
                p.fillRect({1, y, b.w - 2, rowH()}, color(C::FormHighlight));
            const auto l = oneLineLayout(
                _items[i].label,
                pxFont(15, text::Weight::Regular, color(cur ? C::ReplyLink : C::FormText)),
                b.w - 24,
                windowScale()
            );
            l->paint(p, snapPx({12, y + std::floor((rowH() - l->height()) / 2)}));
        }
        p.restore();
        p.strokeRoundRect(b, 4, 1, color(C::FormDividerStrong));
    }
    bool    onEvent(Event &e) override;
    uint8_t cursorAt(PointF p) const override {
        return rowAt(p.y) >= 0 ? uint8_t(plat::Cursor::Hand) : View::cursorAt(p);
    }
    const std::vector<Candidate> &items() const { return _items; }
    int                           current() const { return _current; }
    void                          step(int d) {
        if (_items.empty())
            return;
        _current        = std::clamp(_current + d, 0, int(_items.size()) - 1);
        // Keep it in sight.
        const float top = rowH() * float(_current), bottom = top + rowH() + 4;
        if (top < _scroll)
            _scroll = top;
        else if (bottom > _scroll + height())
            _scroll = bottom - height();
        update();
    }

private:
    int rowAt(float y) const {
        const int i = int((y - 2 + _scroll) / rowH());
        return i >= 0 && i < int(_items.size()) ? i : -1;
    }
    PickField             &_owner;
    std::vector<Candidate> _items;
    float                  _w = 300, _scroll = 0;
    int                    _hover = -1, _current = 0;
};

// Chips of the people (or conversations) picked, then a search that offers
// more under it. Single: picking replaces the chip.
class PickField final : public View {
public:
    PickField(screens::Context &ctx, const model::FormField &f) : _ctx(ctx), _field(f) {
        style().row().padding(8, 4, 8, 4).spacing(6).items(Align::Center).noShrink();
        style().height(kFormNormalH);
        _chips = add<View>();
        _chips->style().row().spacing(4).items(Align::Center);
        _edit = add<TextEdit>();
        _edit->style().flex(1);
        _edit->setMaxLines(1);
        _edit->setPlaceholder(
            !f.placeholder.empty() ? f.placeholder
            : f.kind == Kind::People
                ? std::string(f.multi ? tr("Choose people") : tr("Choose someone"))
                : std::string(f.multi ? tr("Choose channels") : tr("Choose a channel"))
        );
        _edit->onChange = [this] { refresh(); };
        _edit->onSubmit = [this] {
            if (_list && !_list->items().empty())
                pick(_list->items()[size_t(std::max(0, _list->current()))]);
            return true;
        };
        _edit->onKey = [this](const Event &e) {
            if (!_list)
                return false;
            if (e.key == plat::Key::Down || e.key == plat::Key::Up) {
                _list->step(e.key == plat::Key::Down ? 1 : -1);
                return true;
            }
            if (e.key == plat::Key::Escape) {
                closeList();
                return true;
            }
            return false;
        };
        for (const std::string &id : f.initial.ids)
            addId(id);
        setCursor(plat::Cursor::IBeam);
    }
    ~PickField() override { closeList(); }

    const std::vector<std::string> &ids() const { return _ids; }
    std::function<void()>           onChange;
    TextEdit                       &edit() { return *_edit; }

    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 4, color(C::FormBg));
        p.strokeRoundRect(b, 4, 1, color(_edit->focused() ? C::FieldBorderFocus : C::FieldBorder));
        View::paint(p);
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerDown) {
            _edit->focus();
            return true;
        }
        if (e.type == EventType::FocusOut)
            _ctx.app.platform().post([this, alive = std::weak_ptr<int>(_alive)] {
                if (!alive.expired() && !_edit->focused())
                    closeList();
            });
        return false;
    }
    void pick(const Candidate &c) {
        closeList();
        _edit->clear();
        if (!_field.multi)
            _ids.clear();
        addId(c.id);
        if (onChange)
            onChange();
    }
    void closeList() {
        if (Matches *l = _list) {
            _list = nullptr;
            l->close();
        }
    }

private:
    std::string labelOf(const std::string &id) const {
        const model::Store &st = _ctx.store;
        if (const model::UserRef u = st.findUser(id);
            u != model::kNoUser && _field.kind == Kind::People)
            return std::string(st.user(u).label());
        if (const model::ConvRef c = st.findConversation(id); c != model::kNoConv)
            return st.conversation(c).isDirect() ? st.displayName(c) : "#" + st.displayName(c);
        if (const model::UserRef u = st.findUser(id); u != model::kNoUser)
            return std::string(st.user(u).label());
        return id;
    }
    void addId(const std::string &id) {
        if (id.empty() || std::find(_ids.begin(), _ids.end(), id) != _ids.end())
            return;
        _ids.push_back(id);
        rebuildChips();
    }
    void rebuildChips() {
        _chips->clearChildren();
        for (const std::string &id : _ids) {
            auto *chip = _chips->add<Button>(
                labelOf(id) + "  \xC3\x97", Button::Kind::Secondary, Button::Form::Small
            );
            chip->setTooltip(tr("Remove"));
            chip->onClick = [this, id] {
                std::erase(_ids, id);
                rebuildChips();
                if (onChange)
                    onChange();
            };
        }
        _edit->setVisible(_field.multi || _ids.empty());
        update();
    }
    void refresh() {
        closeList();
        const std::string q(str::trim(_edit->text()));
        if (q.empty() || !window())
            return;
        const std::string      fq = utf8::foldCase(q);
        const model::Store    &st = _ctx.store;
        std::vector<Candidate> prefix, inside;
        auto                   offer = [&](std::string id, std::string label, std::string key) {
            if (std::find(_ids.begin(), _ids.end(), id) != _ids.end())
                return;
            if (str::startsWith(utf8::foldCase(label), fq))
                prefix.push_back({std::move(id), std::move(label)});
            else if (utf8::containsFoldedNeedle(key, fq))
                inside.push_back({std::move(id), std::move(label)});
        };
        if (_field.kind == Kind::People) {
            for (model::UserRef u = 0; u < st.userCount(); ++u) {
                const model::User &x = st.user(u);
                if (x.deleted || x.placeholder || x.stranger || x.id.empty() || x.bot)
                    continue;
                offer(x.id, std::string(x.label()), screens::userSearchKey(x));
            }
        } else {
            // Channels; a conversations picker offers the DMs too.
            const bool dms = _field.type.find("conversations") != std::string::npos;
            for (model::ConvRef c = 0; c < st.conversationCount(); ++c) {
                const model::Conversation &x = st.conversation(c);
                if (x.id.empty() || (x.isDirect() && !dms) || (x.isDirect() && !x.member))
                    continue;
                const std::string name = st.displayName(c);
                offer(x.id, x.isDirect() ? name : "#" + name, utf8::foldCase(name));
            }
        }
        std::vector<Candidate> out = std::move(prefix);
        for (Candidate &c : inside)
            out.push_back(std::move(c));
        if (out.size() > 50)
            out.resize(50);
        if (out.empty())
            return;
        auto l = std::make_unique<Matches>(*this, std::move(out));
        l->setWidth(width());
        l->setAnchor(windowRect(), Popup::Place::Below);
        _list = l.get();
        window()->showPopup(std::move(l));
    }

    screens::Context        &_ctx;
    const model::FormField   _field;
    View                    *_chips = nullptr;
    TextEdit                *_edit  = nullptr;
    Matches                 *_list  = nullptr;
    std::vector<std::string> _ids;
    std::shared_ptr<int>     _alive = std::make_shared<int>(0);
};

bool Matches::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerMove: {
        const int i = rowAt(e.pos.y);
        if (i != _hover) {
            _hover = i;
            update();
        }
        return true;
    }
    case EventType::PointerDown:
        if (const int i = rowAt(e.pos.y); i >= 0) {
            const Candidate c = _items[size_t(i)];
            _owner.pick(c); // closes this list
        }
        return true;
    case EventType::Scroll: {
        const float max = std::max(0.f, rowH() * float(_items.size()) + 4 - height());
        _scroll         = std::clamp(_scroll - e.dy * (e.precise ? 1.f : 30.f), 0.f, max);
        update();
        return true;
    }
    default:
        return false;
    }
}

// ── Dates ───────────────────────────────────────────────────────────────────

// "2026-11-27" → y m d; false when it isn't one.
bool parseDate(const std::string &s, int &y, int &m, int &d) {
    return std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 &&
           d <= 31;
}

bool parseTime(const std::string &s, int &h, int &m) {
    return std::sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60;
}

std::string twoDigits(int n) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", n);
    return b;
}

// ── The dialog ──────────────────────────────────────────────────────────────

class FormDialog final : public Dialog {
public:
    FormDialog(screens::Context &ctx, model::Form form, std::function<void(bool)> done)
        : Dialog(form.title.empty() ? std::string(tr("Workflow")) : form.title), _ctx(ctx),
          _backend(&ctx.backend()), _form(std::move(form)), _done(std::move(done)) {
        const text::Style bold  = pxFont(15, text::Weight::Bold, themed(C::FormText));
        const text::Style plain = pxFont(15, text::Weight::Regular, themed(C::FormText));
        const text::Style muted = pxFont(13, text::Weight::Regular, themed(C::FormTextMuted));
        const text::Style error = pxFont(13, text::Weight::Regular, themed(C::FormError));

        const bool needsSlack = formNeedsSlack(_form);
        if (needsSlack)
            styledLabel(
                content(),
                tr("This form has a field msga can't fill in. Start the workflow in Slack to "
                   "finish it there."),
                error
            );
        _fields.resize(_form.fields.size());
        for (size_t i = 0; i < _form.fields.size(); ++i) {
            const model::FormField &f   = _form.fields[i];
            Field                  &fv  = _fields[i];
            auto                   *col = content()->add<View>();
            col->style().spacing(6);
            if (f.kind == Kind::Note) {
                styledLabel(col, mrkdwn::parse(f.label).text, plain);
                continue;
            }
            // "Label (optional)".
            auto *head = col->add<View>();
            head->style().row().spacing(4).items(Align::Center);
            styledLabel(head, f.label, bold);
            if (f.optional)
                styledLabel(head, tr("(optional)"), muted);
            build(col, f, fv);
            if (!f.hint.empty())
                styledLabel(col, f.hint, muted);
            fv.error = col->add<Label>(std::string(), Font::Small, C::FormError);
            fv.error->setVisible(false);
            watch(fv);
        }
        _status = content()->add<Label>(std::string(), Font::Small, C::FormError);
        _status->setVisible(false);

        Button *primary;
        if (needsSlack) {
            primary          = makeButton(tr("Open in Slack"), Button::Kind::Primary);
            primary->onClick = [this] {
                if (!_form.fallbackUrl.empty() && _ctx.openUrl)
                    _ctx.openUrl(_form.fallbackUrl);
                reject();
            };
        } else {
            primary = makeButton(
                _form.submit.empty() ? std::string(tr("Submit")) : _form.submit,
                Button::Kind::Primary
            );
            primary->onClick = [this] { submit(); };
        }
        _submit       = primary;
        Button *close = makeButton(
            _form.close.empty() ? std::string(tr("Close")) : _form.close, Button::Kind::Secondary
        );
        addButtonRow(primary, close);
        onRejected = [this] {
            // Dismissed: the service stops waiting for it.
            if (!_over && _backend == &_ctx.backend())
                _backend->closeForm(_form);
            finish(false);
        };
    }

    // The first input, for the focus.
    View *firstInput() const {
        for (const Field &f : _fields)
            if (f.focus)
                return f.focus;
        return nullptr;
    }

private:
    // One field's inputs (whichever its kind has).
    struct Field {
        TextField              *text = nullptr;
        PickField              *pick = nullptr;
        std::vector<CheckBox *> boxes;
        RadioGroup             *radio = nullptr;
        Dropdown               *drop  = nullptr;
        DateTimeField          *when  = nullptr;
        CheckBox               *use   = nullptr; // an optional date: given at all?
        Label                  *error = nullptr;
        View                   *focus = nullptr;
    };

    // A field's message goes once it is changed (the next Submit checks again).
    static void watch(Field &fv) {
        Field *p     = &fv; // _fields never grows after the build: stable
        auto   clear = [p] {
            if (p->error)
                p->error->setVisible(false);
        };
        if (fv.text) {
            auto prev                = fv.text->edit().onChange; // the length counter
            fv.text->edit().onChange = [prev, clear] {
                if (prev)
                    prev();
                clear();
            };
        }
        if (fv.pick)
            fv.pick->onChange = clear;
        for (CheckBox *b : fv.boxes)
            b->onChange = [clear](bool) { clear(); };
        if (fv.drop)
            fv.drop->onChange = [clear](int) { clear(); };
        if (fv.radio)
            fv.radio->onChange = [clear](int) { clear(); };
        if (fv.when)
            fv.when->onChange = clear;
        if (fv.use) {
            auto prev        = fv.use->onChange;
            fv.use->onChange = [prev, clear](bool on) {
                if (prev)
                    prev(on);
                clear();
            };
        }
    }

    void build(View *col, const model::FormField &f, Field &fv) {
        switch (f.kind) {
        case Kind::Text:
        case Kind::Number:
            fv.text = col->add<TextField>(f.placeholder, TextField::Size::Normal);
            if (f.maxLength > 0)
                fv.text->setMaxLength(f.maxLength);
            fv.text->setText(f.initial.text);
            fv.text->onReturn = [this] { submit(); };
            fv.focus          = &fv.text->edit();
            break;
        case Kind::Paragraph:
            fv.text = col->add<TextField>(f.placeholder, true);
            fv.text->style().height(96);
            if (f.maxLength > 0)
                fv.text->setMaxLength(f.maxLength);
            fv.text->setText(f.initial.text);
            fv.focus = &fv.text->edit();
            break;
        case Kind::People:
        case Kind::Channels:
            fv.pick  = col->add<PickField>(_ctx, f);
            fv.focus = &fv.pick->edit();
            break;
        case Kind::Checkboxes:
            for (size_t k = 0; k < f.options.size(); ++k) {
                const bool on =
                    std::find(f.initial.chosen.begin(), f.initial.chosen.end(), uint32_t(k)) !=
                    f.initial.chosen.end();
                auto *b = col->add<CheckBox>(f.options[k].text, on);
                b->style().alignSelf(Align::Start);
                fv.boxes.push_back(b);
            }
            break;
        case Kind::Select:
            if (f.multi) { // several of a list: check boxes too
                for (size_t k = 0; k < f.options.size(); ++k) {
                    const bool on =
                        std::find(f.initial.chosen.begin(), f.initial.chosen.end(), uint32_t(k)) !=
                        f.initial.chosen.end();
                    auto *b = col->add<CheckBox>(f.options[k].text, on);
                    b->style().alignSelf(Align::Start);
                    fv.boxes.push_back(b);
                }
            } else {
                // The first entry is "nothing chosen".
                std::vector<std::string> names{
                    f.placeholder.empty() ? std::string(tr("Choose an option")) : f.placeholder
                };
                for (const auto &o : f.options)
                    names.push_back(o.text);
                const int at = f.initial.chosen.empty() ? 0 : int(f.initial.chosen.front()) + 1;
                fv.drop      = col->add<Dropdown>(std::move(names), at);
                fv.drop->style().alignSelf(Align::Start);
            }
            break;
        case Kind::Radio: {
            std::vector<std::string> names;
            for (const auto &o : f.options)
                names.push_back(o.text);
            fv.radio = col->add<RadioGroup>(
                names, f.initial.chosen.empty() ? -1 : int(f.initial.chosen.front())
            );
            break;
        }
        case Kind::Date:
        case Kind::Time:
        case Kind::DateTime: {
            auto *row = col->add<View>();
            row->style().row().spacing(8).items(Align::Center);
            const bool given = !f.initial.text.empty();
            if (f.optional) {
                fv.use = row->add<CheckBox>(std::string(), given);
                fv.use->setTooltip(tr("Give a value"));
            }
            fv.when = row->add<DateTimeField>(
                f.kind == Kind::Date   ? DateTimeField::Kind::Date
                : f.kind == Kind::Time ? DateTimeField::Kind::Time
                                       : DateTimeField::Kind::DateTime
            );
            const int64_t         now = base::nowSecs();
            const base::CivilTime t   = base::localTime(now);
            int                   y = t.year, mo = t.month, d = t.day, h = t.hour, mi = t.minute;
            if (f.kind == Kind::Date)
                parseDate(f.initial.text, y, mo, d);
            if (f.kind == Kind::Time)
                parseTime(f.initial.text, h, mi);
            if (f.kind == Kind::DateTime)
                fv.when->setValue(given ? std::strtoll(f.initial.text.c_str(), nullptr, 10) : now);
            else if (f.kind == Kind::Date)
                fv.when->setDate(y, mo, d);
            else
                fv.when->setTime(h, mi);
            if (fv.use) {
                fv.when->setEnabled(given);
                fv.use->onChange = [w = fv.when](bool on) { w->setEnabled(on); };
            }
            break;
        }
        case Kind::Unsupported:
            styledLabel(
                col,
                tr("This field can only be filled in in Slack."),
                pxFont(13, text::Weight::Regular, themed(C::FormTextMuted))
            );
            break;
        case Kind::Note:
            break;
        }
    }

    std::vector<model::FormAnswer> collect() const {
        std::vector<model::FormAnswer> out(_form.fields.size());
        for (size_t i = 0; i < _form.fields.size(); ++i) {
            const model::FormField &f  = _form.fields[i];
            const Field            &fv = _fields[i];
            model::FormAnswer      &a  = out[i];
            if (fv.text)
                a.text = std::string(str::trim(fv.text->text()));
            else if (fv.pick)
                a.ids = fv.pick->ids();
            else if (!fv.boxes.empty()) {
                for (size_t k = 0; k < fv.boxes.size(); ++k)
                    if (fv.boxes[k]->checked())
                        a.chosen.push_back(uint32_t(k));
            } else if (fv.radio) {
                if (fv.radio->selected() >= 0)
                    a.chosen.push_back(uint32_t(fv.radio->selected()));
            } else if (fv.drop) {
                if (fv.drop->selected() > 0)
                    a.chosen.push_back(uint32_t(fv.drop->selected() - 1));
            } else if (fv.when && (!fv.use || fv.use->checked())) {
                const DateTimeField &w = *fv.when;
                if (f.kind == Kind::Date)
                    a.text = str::concat(
                        {str::number(w.year()), "-", twoDigits(w.month()), "-", twoDigits(w.day())}
                    );
                else if (f.kind == Kind::Time)
                    a.text = str::concat({twoDigits(w.hour()), ":", twoDigits(w.minute())});
                else
                    a.text = str::number(w.value());
            }
        }
        return out;
    }

    // Each field's message ("" hides it); true when any shows.
    bool showErrors(const std::vector<std::string> &errs) {
        bool any = false;
        for (size_t i = 0; i < _fields.size(); ++i) {
            Label *l = _fields[i].error;
            if (!l)
                continue;
            const std::string &e = i < errs.size() ? errs[i] : std::string();
            l->setText(e);
            l->setVisible(!e.empty());
            any = any || !e.empty();
        }
        return any;
    }

    void submit() {
        if (_sending || _over)
            return;
        _status->setVisible(false);
        std::vector<model::FormAnswer> a = collect();
        if (showErrors(formProblems(_form, a)))
            return;
        if (_backend != &_ctx.backend()) { // another workspace is shown now
            _status->setText(tr("Switch back to the workspace this form came from to send it."));
            _status->setVisible(true);
            return;
        }
        _sending = true;
        _submit->setEnabled(false);
        std::weak_ptr<int> alive = _alive;
        _backend->submitForm(_form, std::move(a), [this, alive](model::Backend::FormResult r) {
            if (alive.expired())
                return;
            _sending = false;
            _submit->setEnabled(true);
            if (!r.error.empty()) {
                _status->setText(i18n::arg(tr("The form couldn't be sent (%1)."), r.error));
                _status->setVisible(true);
                return;
            }
            bool refused = false;
            for (const std::string &e : r.fieldErrors)
                refused = refused || !e.empty();
            if (refused) {
                showErrors(r.fieldErrors);
                return;
            }
            _over = true; // sent: nothing to close on the service
            if (!r.next.id.empty()) {
                // The next step, in this one's place.
                Window *w    = window();
                auto    done = std::move(_done);
                accept();
                if (w)
                    showWorkflowForm(_ctx, *w, std::move(r.next), std::move(done));
                return;
            }
            accept();
            finish(true);
        });
    }

    void finish(bool sent) {
        if (auto fn = std::exchange(_done, nullptr))
            fn(sent);
    }

    screens::Context         &_ctx;
    model::Backend           *_backend;
    model::Form               _form;
    std::function<void(bool)> _done;
    std::vector<Field>        _fields;
    Label                    *_status  = nullptr;
    Button                   *_submit  = nullptr;
    bool                      _sending = false, _over = false;
    std::shared_ptr<int>      _alive = std::make_shared<int>(0);
};

} // namespace

Popup *showWorkflowForm(
    screens::Context &ctx, Window &w, model::Form form, std::function<void(bool sent)> done
) {
    auto  d   = std::make_unique<FormDialog>(ctx, std::move(form), std::move(done));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    if (View *first = raw->firstInput())
        first->focus();
    return raw;
}

} // namespace shell
