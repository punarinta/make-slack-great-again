// An agent session's background jobs in the conversation header
// (model::BackgroundJob, Store::jobs): one ring per job right of the title,
//
//   [avatar] Name ……………  (◔ fuzz-checkout 62%)  ☆  🔍     one job
//   [avatar] Name ……………  (◔ ◌ ◑)  ☆  🔍                     2–3 jobs
//   [avatar] Name ……………  (◔ ◌ +4)  ☆  🔍                    more
//   [avatar] Na…  (◔ 6)  ☆  🔍                                no room
//
// A ring is a pie filling up (a job that says how far along it is), a
// spinning arc (one that doesn't), a check (done), a red × (failed) or a
// square (stopped). Each has a tooltip; a click opens the jobs popover with a
// row per job: its name, command, latest line and bar. Hidden while there are
// no jobs. Done and stopped ones go after 5 s on screen (in an active
// window); a failed one stays until seen: the popover opened, or the
// session's next message.
#pragma once

#include "model/types.h"
#include "screens/common/context.h"
#include "ui/ui.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace shell {

class JobsPopover;

class JobsButton : public ui::Clickable {
public:
    // Label: ring, name, value (one job). Rings: up to three slots. Count:
    // one ring and how many.
    enum class Form : uint8_t { Label, Rings, Count };

    explicit JobsButton(screens::Context &ctx);
    ~JobsButton() override;

    void                                     setConv(model::ConvRef conv);
    model::ConvRef                           conv() const { return _conv; }
    // The jobs as shown: a failed one first, then by start.
    const std::vector<model::BackgroundJob> &jobs() const { return _jobs; }

    // The widest form that fits `room` (px); Count when none does.
    Form  fit(float room) const;
    void  setForm(Form f);
    Form  form() const { return _form; }
    // Its width in a form (padding included).
    float widthOf(Form f) const;

    // The slots as shown: indexes into jobs(), -1 for the "+N"/count text.
    const std::vector<int> &slots() const { return _slots; }
    // The tooltip of slot `i` (tests), and the whole element's name.
    std::string             slotTooltip(size_t i) const;
    std::string             summary() const;
    JobsPopover            *popover() const { return _popover; }
    void                    openPopover();

    ui::SizeF   measureContent(float availW, float availH) override;
    void        layout() override;
    void        paint(gfx::Painter &p) override;
    bool        onEvent(ui::Event &e) override;
    std::string tooltip() const override;
    ui::RectF   tooltipAnchor() const override;
    std::string accessibleName() const override { return summary(); }
    bool        tick(double nowMs) override;
    void        windowChanged() override;

    // "62%", "2m", "done", … : the label form's value.
    static std::string shortValue(const model::BackgroundJob &j, int64_t nowMs);
    // "fuzz-checkout · 62% (6200/10000) · 14m 10s" and the like.
    static std::string jobTooltip(const model::BackgroundJob &j, int64_t nowMs);
    // Paints a job's 16 px ring centred at `c`.
    static void
    paintRing(gfx::Painter &p, ui::PointF c, const model::BackgroundJob &j, double nowMs);

private:
    void reload();  // from the Store
    void rebuild(); // the labels and slots for the form
    void second();  // the 1 s tick: values, and the end states' time on screen
    void startTimer();
    bool anySpinner() const;
    int  slotAt(ui::PointF local) const;
    // The session posted a message: the failed jobs it explains are seen.
    void messageArrived();

    screens::Context                        &_ctx;
    model::ConvRef                           _conv = model::kNoConv;
    std::vector<model::BackgroundJob>        _jobs;
    std::vector<int>                         _slots;
    std::vector<float>                       _slotX; // ring centres
    Form                                     _form     = Form::Rings;
    ui::Label                               *_name     = nullptr;
    ui::Label                               *_value    = nullptr;
    ui::Label                               *_more     = nullptr;
    int                                      _hover    = -1;
    bool                                     _ticking  = false;
    plat::TimerId                            _timer    = 0;
    model::Store::ObserverId                 _observer = 0;
    std::unordered_map<std::string, int64_t> _shownMs; // ended jobs' time on screen
    JobsPopover                             *_popover = nullptr;
};

} // namespace shell
