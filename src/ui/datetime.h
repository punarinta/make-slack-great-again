// Date/time input fields: the reminder dialog's date field (with a calendar
// popup) and time field, and the schedule-send popup's date-and-time field.
//
// DateTimeField shows sections ("9/30/26", "8:45 PM" / "20:45", "Sep 30,
// 2026 8:45 PM"), laid out by a base::formatCivil pattern in the date language
// (base/time.h: the locale's short date, so Japanese reads "2026/09/30", sv
// "2026-09-30"; "2026年9月30日 午後8:45"), and the clock follows the
// 12/24-hour setting; the focused section is highlighted, Left/Right move
// between sections, Up/Down, the wheel and the arrows step it, digits type
// into it. A date or date-time field opens a month calendar from its
// drop-down arrow; a time field has spin arrows.
#pragma once

#include "ui/widgets.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ui {

class DateTimeField : public View {
public:
    enum class Kind : uint8_t { Date, Time, DateTime };
    explicit DateTimeField(Kind k);
    ~DateTimeField() override;

    // Date: year/month/day; Time: hour/minute (24-hour values).
    void    setDate(int year, int month, int day);
    void    setTime(int hour, int minute);
    void    setMinimumDate(int year, int month, int day);
    // DateTime: the value as epoch seconds (local wall clock), and the
    // earliest one allowed (rounded up to
    // the whole minute the field can show).
    void    setValue(int64_t secs);
    int64_t value() const;
    void    setMinimumValue(int64_t secs);
    // A red frame until the next change: the value was refused.
    void    setInvalid(bool on);
    int     year() const { return _y; }
    int     month() const { return _mo; }
    int     day() const { return _d; }
    int     hour() const { return _h; }
    int     minute() const { return _mi; }
    int     section() const { return _sec; }
    void    setSection(int i);
    void    step(int dir); // the current section, ±1 (wrapping within it)

    std::function<void()> onChange;

    SizeF measureContent(float availW, float availH) override;
    void  paint(gfx::Painter &p) override;
    bool  onEvent(Event &e) override;
    void  styleChanged() override;

private:
    // A pattern token: an editable section, or literal text (Lit).
    enum class F : uint8_t {
        Lit,
        Year,
        Year2,
        Month,
        MonthName,
        Day,
        Hour24,
        Hour12,
        Minute,
        AmPm
    };
    struct Part {
        std::string text;
        F           f = F::Lit;
        bool        field() const { return f != F::Lit; }
    };
    int                      sectionCount() const;
    F                        sectionField(int sec) const;
    const std::vector<Part> &parts() const; // cached
    std::vector<Part>        buildParts() const;
    int                      sectionAt(float x);
    void                     clampDate();
    void                     changed();
    void                     openCalendar();

    mutable std::vector<Part>                  _parts;     // empty: to build
    mutable std::string                        _partsLang; // … in this date language
    std::vector<std::unique_ptr<text::Layout>> _layouts;
    std::vector<float>                         _xs; // left of each part
    Kind                                       _kind;
    int                                        _y = 2026, _mo = 1, _d = 1, _h = 0, _mi = 0;
    int                                        _minY = 0, _minMo = 0, _minD = 0;
    int64_t                                    _minSecs = 0; // DateTime
    int          _sec = 0, _typed = -1; // digits typed into the current section so far
    mutable bool _built = false, _parts24h = false;
    bool         _invalid = false;
};

} // namespace ui
