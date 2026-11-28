#include "ui/datetime.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

constexpr float kFieldH = 32, kPadX = 8, kButtonW = 14, kRadius = 4;

int daysIn(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool       leap    = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : kDays[(m - 1) % 12];
}

std::string num(int v, bool pad2) {
    char b[8];
    std::snprintf(b, sizeof b, pad2 ? "%02d" : "%d", v);
    return b;
}

// The calendar popup under a date field: a month header with
// arrows, weekday names from the locale's first day of the week (Sunday for
// en_US, Monday for most of Europe), a 6×7 day grid; days before
// the minimum are dim; a click picks the day.
class CalendarPopup final : public Popup {
public:
    CalendarPopup(DateTimeField &f, int y, int m) : _f(f), _y(y), _m(m) {
        setRole(Role::Group);
        style().padding(8);
    }
    SizeF measureContent(float, float) override { return {kCell * 7, kHead + kCell * 7}; }
    void  paint(gfx::Painter &p) override {
        Popup::paint(p);
        layouts();
        auto tx = [&](const text::Layout &l, RectF r) {
            l.paint(
                p,
                snapPx(
                    {r.x + std::floor((r.w - l.width()) / 2),
                     r.y + std::floor((r.h - l.height()) / 2)}
                )
            );
        };
        const float ox = 8, oy = 8;
        tx(*_head, {ox, oy, kCell * 7, kHead});
        tx(*_prev, {ox, oy, kCell, kHead});
        tx(*_next, {ox + kCell * 6, oy, kCell, kHead});
        for (int i = 0; i < 7; ++i)
            tx(*_weekdays[size_t(i)], {ox + kCell * float(i), oy + kHead, kCell, kCell});
        const int first = column1();
        for (int d = 1; d <= daysIn(_y, _m); ++d) {
            const RectF r   = cell(first + d - 1);
            const bool  sel = _f.year() == _y && _f.month() == _m && _f.day() == d;
            const bool  off = before(d);
            if (sel)
                p.fillRoundRect({r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 4, ui::color(C::Accent));
            else if (_hover == d && !off)
                p.fillRoundRect(
                    {r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 4, ui::color(C::FormHighlight)
                );
            text::Layout &l = *_days[size_t(d - 1)];
            l.setColor(ui::color(sel ? C::AccentText : off ? C::FormTextFaint : C::FormText));
            tx(l, r);
        }
    }
    void styleChanged() override {
        _days.clear(); // theme or text size: shaped again on the next paint
        Popup::styleChanged();
    }
    // The hand over what a click acts on: the month arrows and pickable days.
    uint8_t cursorAt(PointF pt) const override {
        const float x = pt.x - 8, y = pt.y - 8;
        const int   d     = dayAt(x, y);
        const bool  arrow = y >= 0 && y < kHead && (x < kCell || x >= kCell * 6);
        return arrow || (d > 0 && !before(d)) ? uint8_t(plat::Cursor::Hand) : View::cursorAt(pt);
    }
    bool onEvent(Event &e) override {
        const float x = e.pos.x - 8, y = e.pos.y - 8;
        if (e.type == EventType::PointerMove) {
            const int d = dayAt(x, y);
            if (d != _hover) {
                // Only the two cells whose highlight changes.
                for (int c : {_hover, d})
                    if (c > 0)
                        update(cell(column1() + c - 1));
                _hover = d;
            }
            return true;
        }
        if (e.type == EventType::PointerUp && e.button == plat::Button::Left) {
            if (y >= 0 && y < kHead && (x < kCell || x >= kCell * 6)) {
                _m += x < kCell ? -1 : 1;
                if (_m < 1)
                    _m = 12, --_y;
                if (_m > 12)
                    _m = 1, ++_y;
                update();
                return true;
            }
            if (const int d = dayAt(x, y); d > 0 && !before(d)) {
                _f.setDate(_y, _m, d);
                close();
            }
            return true;
        }
        return Popup::onEvent(e);
    }

private:
    static constexpr float kCell = 32, kHead = 30;
    static int             weekday(int y, int m, int d) {
        return int((base::daysFromCivil(y, m, d) % 7 + 11) % 7); // 1970-01-01 was a Thursday
    }
    bool before(int d) const {
        return base::daysFromCivil(_y, _m, d) <
               base::daysFromCivil(_minY ? _minY : 1, _minM ? _minM : 1, _minD ? _minD : 1);
    }
    int dayAt(float x, float y) const {
        if (x < 0 || x >= kCell * 7 || y < kHead + kCell)
            return 0;
        const int i = int((y - kHead - kCell) / kCell) * 7 + int(x / kCell);
        const int d = i - column1() + 1;
        return d >= 1 && d <= daysIn(_y, _m) ? d : 0;
    }
    // The column of the month's first day.
    int          column1() const { return (weekday(_y, _m, 1) - base::firstDayOfWeek() + 7) % 7; }
    // Grid slot i's box (local; slot 0 = the first row's first column).
    static RectF cell(int i) {
        return {8 + kCell * float(i % 7), 8 + kHead + kCell * float(1 + i / 7), kCell, kCell};
    }
    // The static labels, shaped once per scale and text size (the header per
    // month); day numbers are recoloured per paint, never reshaped.
    void layouts() {
        const float k    = windowScale();
        const float body = ui::font(Font::Body).size;
        auto        make = [&](std::string_view s, float px, text::Weight w, C c) {
            text::AttributedText t;
            t.append(s, ui::pxFont(px, w, ui::color(c)));
            return text::Layout::build(std::move(t), {}, k);
        };
        if (_days.empty() || _scale != k || _body != body) {
            _scale = k;
            _body  = body;
            _headY = _headM = 0;
            _prev           = make("\xE2\x80\xB9", 18, text::Weight::Regular, C::FormText);
            _next           = make("\xE2\x80\xBA", 18, text::Weight::Regular, C::FormText);
            _weekdays.clear();
            for (int i = 0; i < 7; ++i)
                _weekdays.push_back(make(
                    base::weekdayShortName((i + base::firstDayOfWeek()) % 7),
                    11,
                    text::Weight::Regular,
                    C::FormTextMuted
                ));
            _days.clear();
            for (int d = 1; d <= 31; ++d)
                _days.push_back(make(str::number(d), 13, text::Weight::Regular, C::FormText));
        }
        if (_headY != _y || _headM != _m) {
            _headY = _y;
            _headM = _m;
            _head  = make(base::formatMonthYear(_y, _m), 13, text::Weight::Bold, C::FormText);
        }
    }

public:
    int _minY = 0, _minM = 0, _minD = 0;

private:
    DateTimeField                             &_f;
    int                                        _y, _m, _hover = 0;
    std::unique_ptr<text::Layout>              _head, _prev, _next;
    std::vector<std::unique_ptr<text::Layout>> _weekdays, _days;
    float                                      _scale = 0, _body = 0;
    int                                        _headY = 0, _headM = 0;
};

} // namespace

// ── DateTimeField ───────────────────────────────────────────────────────────

DateTimeField::DateTimeField(Kind k) : _kind(k) {
    setFocusable(true);
    setRole(Role::TextInput);
    style().height(kFieldH).noShrink();
    setCursor(plat::Cursor::IBeam);
}

DateTimeField::~DateTimeField() = default;

void DateTimeField::setDate(int y, int m, int d) {
    _y  = y;
    _mo = std::clamp(m, 1, 12);
    _d  = std::clamp(d, 1, daysIn(_y, _mo));
    clampDate();
    changed();
}

void DateTimeField::setTime(int h, int m) {
    _h  = std::clamp(h, 0, 23);
    _mi = std::clamp(m, 0, 59);
    clampDate();
    changed();
}

void DateTimeField::setMinimumDate(int y, int m, int d) {
    _minY = y, _minMo = m, _minD = d;
    clampDate();
    changed();
}

void DateTimeField::setValue(int64_t secs) {
    const base::CivilTime t = base::localTime(secs);
    _y = t.year, _mo = t.month, _d = t.day, _h = t.hour, _mi = t.minute;
    clampDate();
    changed();
}

int64_t DateTimeField::value() const {
    return base::fromLocal(_y, _mo, _d, _h, _mi);
}

void DateTimeField::setMinimumValue(int64_t secs) {
    _minSecs                = (secs + 59) / 60 * 60; // the field shows whole minutes
    const base::CivilTime t = base::localTime(_minSecs);
    _minY = t.year, _minMo = t.month, _minD = t.day; // the calendar's first day
    clampDate();
    changed();
}

void DateTimeField::setInvalid(bool on) {
    if (on != _invalid) {
        _invalid = on;
        update();
    }
}

void DateTimeField::clampDate() {
    if (_kind == Kind::DateTime) {
        if (_minSecs && value() < _minSecs) {
            const base::CivilTime t = base::localTime(_minSecs);
            _y = t.year, _mo = t.month, _d = t.day, _h = t.hour, _mi = t.minute;
        }
        return;
    }
    if (_kind != Kind::Date || !_minY)
        return;
    if (base::daysFromCivil(_y, _mo, _d) < base::daysFromCivil(_minY, _minMo, _minD))
        _y = _minY, _mo = _minMo, _d = _minD;
}

void DateTimeField::changed() {
    _built = false;
    _parts.clear();
    _invalid = false;
    update();
    if (onChange)
        onChange();
}

// Built again when the value, the date language or the 12/24-hour setting
// changes, not on every paint and key.
const std::vector<DateTimeField::Part> &DateTimeField::parts() const {
    const char *lang = base::dateLanguage();
    const bool  h24  = base::use24h();
    if (_parts.empty() || h24 != _parts24h || _partsLang != lang) {
        _parts     = buildParts();
        _partsLang = lang;
        _parts24h  = h24;
        _built     = false; // the sections' layouts too
    }
    return _parts;
}

std::vector<DateTimeField::Part> DateTimeField::buildParts() const {
    // Display formats in the date language (base::setDateLanguage, so a
    // language change shows at once): a date field shows the locale's short
    // date (en_US "M/d/yy", sv "yyyy-MM-dd", ja "yyyy/MM/dd"), a date-and-time
    // field "MMM d, yyyy" / "yyyy年M月d日" + the clock, the clock by
    // the 12/24-hour setting (Japanese puts the day period first).
    const bool  ja    = std::string_view(base::dateLanguage()) == "ja";
    const char *clock = base::use24h() ? "HH:mm" : ja ? "APh:mm" : "h:mm AP";
    std::string pattern;
    if (_kind == Kind::Date)
        pattern = base::shortDatePattern();
    else if (_kind == Kind::Time)
        pattern = clock;
    else
        pattern = str::concat(
            {ja ? "yyyy\xE5\xB9\xB4M\xE6\x9C\x88"
                  "d\xE6\x97\xA5"
                : "MMM d, yyyy",
             " ",
             clock}
        );
    std::vector<Part> out;
    auto              lit = [&out](std::string_view t) {
        if (out.empty() || out.back().field())
            out.push_back({{}, F::Lit});
        out.back().text += t;
    };
    const int h12 = _h % 12 == 0 ? 12 : _h % 12;
    for (size_t i = 0; i < pattern.size();) {
        const char ch = pattern[i];
        if (ch == 'A' && i + 1 < pattern.size() && pattern[i + 1] == 'P') {
            out.push_back({base::dayPeriodName(_h), F::AmPm});
            i += 2;
            continue;
        }
        size_t n = 1;
        while (i + n < pattern.size() && pattern[i + n] == ch)
            ++n;
        const bool pad = n == 2;
        if (ch == '\'') { // quoted text: a separator
            const size_t j = pattern.find('\'', i + 1);
            lit(std::string_view(pattern).substr(
                i + 1, (j == std::string::npos ? pattern.size() : j) - i - 1
            ));
            i = j == std::string::npos ? pattern.size() : j + 1;
            continue;
        }
        if (ch == 'y' && n != 2)
            out.push_back({num(_y, false), F::Year});
        else if (ch == 'y' && n == 2)
            out.push_back({num(_y % 100, true), F::Year2});
        else if (ch == 'M' && n == 3)
            out.push_back({base::monthShortName(_mo), F::MonthName});
        else if (ch == 'M' && n <= 2)
            out.push_back({num(_mo, pad), F::Month});
        else if (ch == 'd' && n <= 2)
            out.push_back({num(_d, pad), F::Day});
        else if (ch == 'H' && n <= 2)
            out.push_back({num(_h, pad), F::Hour24});
        else if (ch == 'h' && n <= 2)
            out.push_back({num(h12, pad), F::Hour12});
        else if (ch == 'm' && n <= 2)
            out.push_back({num(_mi, pad), F::Minute});
        else
            lit(pattern.substr(i, n));
        i += n;
    }
    return out;
}

int DateTimeField::sectionCount() const {
    int n = 0;
    for (const Part &p : parts())
        n += p.field();
    return n;
}

DateTimeField::F DateTimeField::sectionField(int sec) const {
    for (const Part &p : parts())
        if (p.field() && sec-- == 0)
            return p.f;
    return F::Lit;
}

void DateTimeField::setSection(int i) {
    _sec   = std::clamp(i, 0, sectionCount() - 1);
    _typed = -1;
    update();
}

void DateTimeField::step(int dir) {
    // No wrapping: sections stop at their bounds.
    switch (sectionField(_sec)) {
    case F::Month:
    case F::MonthName:
        _mo = std::clamp(_mo + dir, 1, 12);
        break;
    case F::Day:
        _d = std::clamp(_d + dir, 1, daysIn(_y, _mo));
        break;
    case F::Year:
    case F::Year2:
        _y = std::clamp(_y + dir, 1753, 7999);
        break;
    case F::Hour24:
    case F::Hour12:
        _h = std::clamp(_h + dir, 0, 23);
        break;
    case F::Minute:
        _mi = std::clamp(_mi + dir, 0, 59);
        break;
    case F::AmPm:
        _h = (_h + 12) % 24;
        break;
    case F::Lit:
        break;
    }
    _d     = std::min(_d, daysIn(_y, _mo));
    _typed = -1;
    clampDate();
    changed();
}

void DateTimeField::styleChanged() {
    _built = false;
    _parts.clear();
    update();
}

SizeF DateTimeField::measureContent(float, float) {
    return {240, kFieldH}; // minimum width 240
}

int DateTimeField::sectionAt(float x) {
    int                      sec = 0, best = 0;
    const std::vector<Part> &ps = parts();
    for (size_t i = 0; i < _xs.size() && i < ps.size(); ++i) {
        if (!ps[i].field())
            continue;
        if (x >= _xs[i])
            best = sec;
        ++sec;
    }
    return best;
}

void DateTimeField::paint(gfx::Painter &p) {
    const std::vector<Part> &ps = parts();
    if (!_built) {
        _layouts.clear();
        _xs.clear();
        const float k = windowScale();
        float       x = kPadX;
        for (const Part &pt : ps) {
            text::AttributedText t;
            t.append(pt.text, ui::pxFont(13, text::Weight::Regular, ui::color(C::FormText)));
            _layouts.push_back(text::Layout::build(std::move(t), {}, k));
            _xs.push_back(x);
            x += _layouts.back()->width();
        }
        _built = true;
    }
    const RectF b     = bounds();
    const bool  focus = focused();
    fieldFrame(
        p,
        b,
        kRadius,
        C::FormBg,
        _invalid ? C::FormError
        : focus  ? C::Accent
                 : C::FormDividerStrong
    );
    int sec = 0;
    for (size_t i = 0; i < ps.size(); ++i) {
        const text::Layout *l = _layouts[i].get();
        const float         y = std::floor((b.h - l->height()) / 2);
        if (ps[i].field() && focus && sec == _sec) // the current section, selected
            p.fillRect({_xs[i], y, std::ceil(l->width()), l->height()}, ui::color(C::Accent));
        if (ps[i].field() && focus && sec == _sec) {
            l->paintAs(p, snapPx({_xs[i], y}), ui::color(C::AccentText));
        } else {
            l->paint(p, snapPx({_xs[i], y}));
        }
        sec += ps[i].field();
    }
    // Buttons: the calendar's drop-down arrow, or the spin arrows.
    const float bx = b.w - kButtonW - 1;
    p.fillRect({bx - 1, 1, 1, b.h - 2}, ui::color(C::FormDividerStrong));
    if (_kind != Kind::Time) {
        gfx::drawIcon(
            p, gfx::Icon::ChevronDown, {bx + 1, (b.h - 12) / 2, 12, 12}, ui::color(C::FormIcon)
        );
    } else {
        p.fillRect({bx, std::floor(b.h / 2), kButtonW, 1}, ui::color(C::FormDividerStrong));
        gfx::drawIcon(p, gfx::Icon::SpinUp, {bx + 1, b.h / 4 - 6, 12, 12}, ui::color(C::FormIcon));
        gfx::drawIcon(
            p, gfx::Icon::SpinDown, {bx + 1, 3 * b.h / 4 - 6, 12, 12}, ui::color(C::FormIcon)
        );
    }
}

void DateTimeField::openCalendar() {
    if (!window())
        return;
    auto c   = std::make_unique<CalendarPopup>(*this, _y, _mo);
    c->_minY = _minY, c->_minM = _minMo, c->_minD = _minD;
    c->setAnchor(windowRect(), Popup::Place::Below);
    window()->showPopup(std::move(c));
}

bool DateTimeField::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerDown: {
        if (e.button != plat::Button::Left)
            return false;
        focus();
        const float bx = width() - kButtonW - 1;
        if (e.pos.x >= bx) {
            if (_kind != Kind::Time)
                openCalendar();
            else
                step(e.pos.y < height() / 2 ? 1 : -1);
        } else {
            setSection(sectionAt(e.pos.x));
        }
        return true;
    }
    case EventType::Scroll:
        if (e.dy != 0)
            step(e.dy > 0 ? 1 : -1);
        return true;
    case EventType::FocusIn:
    case EventType::FocusOut:
        _typed = -1;
        update();
        return false;
    case EventType::KeyDown:
        switch (e.key) {
        case plat::Key::Left:
            setSection(_sec - 1);
            return true;
        case plat::Key::Right:
            setSection(_sec + 1);
            return true;
        default:
            if (const int n = stepForKey(e.key)) {
                step(n);
                return true;
            }
            break;
        }
        if (e.key >= plat::Key::Num0 && e.key <= plat::Key::Num9) {
            const int dgt = int(e.key) - int(plat::Key::Num0);
            const int v   = _typed < 0 ? dgt : _typed * 10 + dgt;
            const F   f   = sectionField(_sec);
            bool      full;
            switch (f) {
            case F::Month:
            case F::MonthName:
                _mo  = std::clamp(v, 1, 12);
                _d   = std::min(_d, daysIn(_y, _mo));
                full = _typed >= 0 || v * 10 > 12;
                break;
            case F::Day:
                _d   = std::clamp(v, 1, daysIn(_y, _mo));
                full = _typed >= 0 || v * 10 > daysIn(_y, _mo);
                break;
            case F::Year2:
                _y   = 2000 + v % 100;
                full = _typed >= 0;
                break;
            case F::Year: // four digits, then it takes
                if (v >= 1000)
                    _y = std::clamp(v, 1753, 7999);
                full = v >= 1000;
                break;
            case F::Hour24:
                _h   = std::clamp(v, 0, 23);
                full = _typed >= 0 || v * 10 > 23;
                break;
            case F::Hour12: {
                const bool pm = _h >= 12;
                _h            = std::clamp(v, 1, 12) % 12 + (pm ? 12 : 0);
                full          = _typed >= 0 || v * 10 > 12;
                break;
            }
            case F::Minute:
                _mi  = std::clamp(v, 0, 59);
                full = _typed >= 0 || v * 10 > 59;
                break;
            default:
                return true;
            }
            clampDate();
            changed();
            if (full && _sec + 1 < sectionCount())
                setSection(_sec + 1);
            else
                _typed = full ? -1 : v;
            return true;
        }
        if (sectionField(_sec) == F::AmPm && (e.key == plat::Key::A || e.key == plat::Key::P)) {
            _h = (_h % 12) + (e.key == plat::Key::P ? 12 : 0);
            clampDate();
            changed();
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // namespace ui
