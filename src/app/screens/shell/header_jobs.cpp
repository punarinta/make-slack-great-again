#include "screens/shell/header_jobs.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "screens/shell/typing_indicator.h"
#include "ui/controls.h"
#include "ui/scroll.h"

#include <algorithm>
#include <cmath>

using namespace ui;
using i18n::tr;
using model::BackgroundJob;
using State = model::BackgroundJob::State;

namespace shell {

namespace {

constexpr float   kH = 28, kPad = 6, kRing = 16, kRingGap = 8, kTextGap = 6, kMaxName = 160;
constexpr int64_t kEndShownMs = 5000; // a done or stopped job, on screen
constexpr double  kTurnMs     = 1200; // the spinner's one turn
constexpr float   kPi         = 3.14159265f;

int64_t nowMs() {
    return base::nowMicros() / 1000;
}

bool indeterminate(const BackgroundJob &j) {
    return j.state == State::Running && j.progress < 0;
}

// An arc of the circle (c, r) from angle a0 to a1 (radians, 0 = 12 o'clock,
// clockwise), as a polyline.
void arcPath(gfx::Path &path, PointF c, float r, float a0, float a1, bool moveFirst = true) {
    const int n = std::max(2, int(std::ceil(std::fabs(a1 - a0) / (kPi / 24))));
    for (int i = 0; i <= n; ++i) {
        const float a = a0 + (a1 - a0) * float(i) / float(n);
        const float x = c.x + r * std::sin(a), y = c.y - r * std::cos(a);
        if (i == 0 && moveFirst)
            path.moveTo(x, y);
        else
            path.lineTo(x, y);
    }
}

// "40s", "2m", "1h 5m": the label form's clock.
std::string shortElapsed(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms / 1000);
    if (s < 60)
        return i18n::arg(tr("%1s"), str::number(s));
    if (s < 3600)
        return i18n::arg(tr("%1m"), str::number(s / 60));
    return i18n::arg(tr("%1h %2m"), str::number(s / 3600), str::number(s / 60 % 60));
}

std::string percent(float progress) {
    // Floor: never 100% while it runs.
    return str::concat(
        {str::number(int64_t(std::floor(std::clamp(progress, 0.f, 1.f) * 100))), "%"}
    );
}

// The header element's and the popover's: shown on screen, in an active window.
bool shownActive(const View *v) {
    const Window *w = v->window();
    if (!w || !w->isActive())
        return false;
    for (; v; v = v->parent())
        if (!v->visible())
            return false;
    return true;
}

// A job's ring as a view of its own (the popover's rows).
class RingView final : public View {
public:
    RingView() {
        style().size(kRing, kRing).noShrink();
        setHitTransparent(true);
    }
    void setJob(const BackgroundJob &j) {
        _job = j;
        if (indeterminate(_job))
            startTicking(); // a no-op while it ticks already
        update();
    }
    void paint(gfx::Painter &p) override {
        JobsButton::paintRing(p, {kRing / 2, kRing / 2}, _job, double(nowMs()));
    }
    bool tick(double) override {
        update();
        return indeterminate(_job);
    }

private:
    BackgroundJob _job;
};

// A job's 3 px bar.
class BarView final : public View {
public:
    BarView() {
        style().height(3).flex(1);
        setHitTransparent(true);
    }
    void setProgress(float f) {
        _f = f;
        update();
    }
    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 1.5f, color(C::Border));
        RectF done = b;
        done.w     = b.w * std::clamp(_f, 0.f, 1.f);
        if (done.w > 0)
            p.fillRoundRect(done, 1.5f, color(C::Progress));
    }

private:
    float _f = 0;
};

} // namespace

// ── The popover ─────────────────────────────────────────────────────────────

// "Background jobs", then a row per job in the order they had when it
// opened (new ones below): its ring, name and time or end, its command (or
// who reports it), its latest line and, with a fraction, its bar.
class JobsPopover final : public Popup {
public:
    JobsPopover(screens::Context &ctx, model::ConvRef conv) : _ctx(ctx), _conv(conv) {
        style().width(380).padding(0, 6, 0, 4);
        View *head = add<View>();
        head->style().row().padding(14, 6, 14, 8).items(Align::Center);
        auto *title = head->add<Label>(tr("Background jobs"), Font::Heading, C::Text);
        title->style().flex(1);
        _summary = head->add<Label>("", Font::Small, C::TextMuted);
        _scroll  = add<ScrollView>();
        _scroll->setLayoutBoundary(false);
        _scroll->style().maxH = 6 * 92;
        _rows                 = _scroll->content();
        refresh();
    }
    // The jobs as they are now.
    void refresh() {
        const auto &jobs = _ctx.store().jobs(_conv);
        // Keep the order: a job keeps its row; a new one goes below.
        for (const BackgroundJob &j : jobs)
            if (std::find(_order.begin(), _order.end(), j.id) == _order.end())
                _order.push_back(j.id);
        // Failed first, as in the header, when it opens.
        if (!_ordered) {
            std::stable_partition(_order.begin(), _order.end(), [&](const std::string &id) {
                const auto it = std::find_if(jobs.begin(), jobs.end(), [&](const BackgroundJob &j) {
                    return j.id == id;
                });
                return it != jobs.end() && it->state == State::Failed;
            });
            _ordered = true;
        }
        // Not under the pointer on a Stop button: rebuilt, it would lose the click.
        for (const Button *b : _stops)
            if (b->hovered())
                return;
        _rows->clearChildren();
        _stops.clear();
        int running = 0, failed = 0, done = 0;
        for (const std::string &id : _order) {
            const auto it = std::find_if(jobs.begin(), jobs.end(), [&](const BackgroundJob &j) {
                return j.id == id;
            });
            if (it == jobs.end())
                continue;
            addRow(*it);
            running += it->state == State::Running;
            failed += it->state == State::Failed;
            done += it->state == State::Done;
            if (it->state == State::Failed)
                _seenFailed.push_back(it->id);
        }
        std::string s;
        const auto  add = [&](std::string part) {
            s += s.empty() ? part : str::concat({", ", part});
        };
        if (running)
            add(i18n::trn("%n running", "%n running", running));
        if (failed)
            add(i18n::trn("%n failed", "%n failed", failed));
        if (done && !running && !failed)
            add(i18n::trn("%n done", "%n done", done));
        _summary->setText(std::move(s));
        invalidateLayout();
    }
    // Stop job `id` ("name"), once the user confirms.
    void confirmStop(const std::string &id, const std::string &name) {
        Window *w = window();
        if (!w)
            return;
        auto d = Dialog::confirm(
            tr("Stop background job"),
            i18n::arg(
                tr("Stop \xE2\x80\x9C%1\xE2\x80\x9D? The session is told that it was stopped.\nA "
                   "job that writes files (a build, a migration) may leave them half-done."),
                name
            ),
            tr("Stop job"),
            Button::Kind::Danger,
            themed(C::TextMuted)
        );
        screens::Context &ctx  = _ctx;
        const auto        conv = _conv;
        d->onAccepted          = [&ctx, conv, id] { ctx.backend.stopJob(conv, id); };
        close(); // the dialog in its place
        w->showPopup(std::move(d));
    }
    size_t                          stopButtons() const { return _stops.size(); }
    // The failed jobs it showed: seen once it closes.
    const std::vector<std::string> &seenFailed() const { return _seenFailed; }
    size_t                          rowCount() const { return (_rows->childCount() + 1) / 2; }

private:
    void addRow(const BackgroundJob &j) {
        const int64_t now = nowMs();
        // A rule between rows.
        if (_rows->childCount() > 0) {
            View *rule = _rows->add<View>();
            rule->setBackground(C::FormDivider);
            rule->style().height(1);
        }
        View *row = _rows->add<View>();
        row->style().row().padding(14, 9, 14, 10).spacing(10);
        auto *ring = row->add<RingView>();
        ring->setJob(j);
        ring->style().margin.t = 1;
        View *col              = row->add<View>();
        col->style().flex(1).spacing(2);
        View *line1 = col->add<View>();
        line1->style().row().spacing(8).items(Align::Center);
        auto *name = line1->add<Label>(j.name, Font::ControlBold, C::Text);
        name->setMaxLines(1);
        name->style().flex(1);
        name->style().shrink = 1;
        std::string end;
        C           endColor = C::TextMuted;
        Font        endFont  = Font::Small;
        switch (j.state) {
        case State::Running:
            end = TypingIndicator::formatElapsed(now - j.startedMs);
            break;
        case State::Done:
            end = i18n::arg(
                tr("done \xC2\xB7 %1"), TypingIndicator::formatElapsed(j.endedMs - j.startedMs)
            );
            break;
        case State::Failed:
            end = i18n::arg(tr("failed \xC2\xB7 exit code %1"), str::number(int64_t(j.exitCode)));
            endColor = C::Danger;
            endFont  = Font::SmallSemibold;
            break;
        case State::Stopped:
            end = tr("stopped");
            break;
        }
        line1->add<Label>(std::move(end), endFont, endColor)->style().noShrink();
        // A running command can be stopped from here (asked first).
        if (j.state == State::Running && !j.report && _ctx.backend.canStopJob(_conv, j.id)) {
            auto *stop = line1->add<Button>(tr("Stop"), Button::Kind::Ghost, Button::Form::Small);
            stop->setTooltip(tr("Stop this background job"));
            stop->style().noShrink();
            stop->onClick = [this, id = j.id, name = j.name] { confirmStop(id, name); };
            _stops.push_back(stop);
        }
        auto *cmd = col->add<Label>(
            j.report ? i18n::arg(tr("Reported by %1"), sessionName()) : j.command,
            j.report ? Font::Small : Font::Mono,
            C::TextFaint
        );
        // The whole command, wrapped: up to five lines, then cut.
        cmd->setMaxLines(5);
        if (!j.line.empty()) {
            auto *text = col->add<Label>(j.line, Font::Small, C::TextMuted);
            text->setMaxLines(2);
        }
        if (j.state == State::Running && j.progress >= 0) {
            View *bar                                                   = col->add<View>();
            bar->style().row().spacing(8).items(Align::Center).margin.t = 4;
            bar->add<BarView>()->setProgress(j.progress);
            auto *pct = bar->add<Label>(percent(j.progress), Font::Small, C::TextMuted);
            pct->setAlign(text::LayoutOptions::Align::Right);
            pct->style().width(34).noShrink();
        }
    }
    std::string sessionName() const {
        const auto &st = _ctx.store();
        if (_conv >= st.conversationCount())
            return {};
        const auto &c = st.conversation(_conv);
        return std::string(c.isDirect() ? st.user(c.dmUser).label() : std::string_view(c.name));
    }

    screens::Context        &_ctx;
    model::ConvRef           _conv;
    Label                   *_summary = nullptr;
    ScrollView              *_scroll  = nullptr;
    View                    *_rows    = nullptr;
    std::vector<std::string> _order, _seenFailed;
    bool                     _ordered = false;
    std::vector<Button *>    _stops; // in the rows now
};

// ── The header element ──────────────────────────────────────────────────────

JobsButton::JobsButton(screens::Context &ctx) : _ctx(ctx) {
    setLook({C::None, C::Hover, C::Pressed, C::Hover, metric(M::RadiusM)});
    setRole(Role::Button);
    style().height(kH).noShrink();
    _name = add<Label>("", Font::Control, C::TextMuted);
    _name->setMaxLines(1);
    _name->setHitTransparent(true);
    _value = add<Label>("", Font::Control, C::TextMuted);
    _value->setMaxLines(1);
    _value->setHitTransparent(true);
    _more = add<Label>("", Font::SmallSemibold, C::TextMuted);
    _more->setHitTransparent(true);
    setVisible(false);
    onClick   = [this] { openPopover(); };
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        if (ch.conv != _conv || _conv == model::kNoConv)
            return;
        if (ch.kind == model::ChangeKind::Jobs)
            reload();
        else if (ch.kind == model::ChangeKind::Append && ch.thread == 0)
            messageArrived();
    });
}

JobsButton::~JobsButton() {
    _ctx.store.unobserve(_observer);
    _ctx.app.cancelTimer(_timer);
}

void JobsButton::setConv(model::ConvRef conv) {
    if (conv == _conv)
        return;
    _conv = conv;
    _shownMs.clear();
    if (_popover)
        _popover->close();
    reload();
}

void JobsButton::reload() {
    _jobs = _conv == model::kNoConv ? std::vector<BackgroundJob>{} : _ctx.store().jobs(_conv);
    std::stable_partition(_jobs.begin(), _jobs.end(), [](const BackgroundJob &j) {
        return j.state == State::Failed;
    });
    std::erase_if(_shownMs, [&](const auto &kv) {
        return std::none_of(_jobs.begin(), _jobs.end(), [&](const BackgroundJob &j) {
            return j.id == kv.first;
        });
    });
    const bool show = !_jobs.empty();
    if (show != visible())
        setVisible(show);
    if (show)
        startTimer();
    else if (_timer) {
        _ctx.app.cancelTimer(_timer);
        _timer = 0;
    }
    if (_form == Form::Label && _jobs.size() != 1)
        _form = Form::Rings;
    rebuild();
    if (_popover)
        _popover->refresh();
    // A spinner turns every frame. startTicking() is a no-op while it ticks
    // already; hiding the element took it off the window's list (and its
    // flag), so it is asked again rather than remembered here.
    if (anySpinner() && visible())
        startTicking();
    // The header picks the form anew (its room may have changed).
    invalidateLayout();
    if (parent())
        parent()->invalidateLayout();
}

void JobsButton::startTimer() {
    if (!_timer)
        _timer = _ctx.app.addTimer(1000, true, [this] { second(); });
}

bool JobsButton::anySpinner() const {
    return std::any_of(_jobs.begin(), _jobs.end(), indeterminate);
}

void JobsButton::second() {
    // Done and stopped jobs go after a while on screen; failed ones wait to
    // be seen.
    if (shownActive(this))
        for (const BackgroundJob &j : _jobs)
            if (j.state == State::Done || j.state == State::Stopped) {
                int64_t &ms = _shownMs[j.id];
                ms += 1000;
                if (ms >= kEndShownMs) {
                    _ctx.store().dismissJob(_conv, j.id); // reloads us
                    return;
                }
            }
    // The clocks.
    if (_form == Form::Label && !_jobs.empty())
        _value->setText(shortValue(_jobs[0], nowMs()));
    if (_popover)
        _popover->refresh();
    update();
}

void JobsButton::messageArrived() {
    // The session's own message after a job failed explains it: seen.
    const auto &st = _ctx.store();
    if (_conv >= st.conversationCount())
        return;
    const auto &msgs = st.conversation(_conv).messages;
    if (msgs.empty() || msgs.back().user == st.me)
        return;
    const int64_t            at = msgs.back().ts / 1000;
    std::vector<std::string> seen;
    for (const BackgroundJob &j : _jobs)
        if (j.state == State::Failed && at >= j.endedMs)
            seen.push_back(j.id);
    for (const std::string &id : seen)
        _ctx.store().dismissJob(_conv, id);
}

void JobsButton::openPopover() {
    Window *w = window();
    if (!w || _jobs.empty() || _popover)
        return;
    auto p        = std::make_unique<JobsPopover>(_ctx, _conv);
    _popover      = p.get();
    // 10 px under the element, its right edge at the element's.
    const RectF r = windowRect();
    p->setAnchor({r.x + r.w - 380, r.y, 380, r.h + 10}, Popup::Place::Below);
    p->onClosed = [this, conv = _conv, raw = _popover] {
        if (_popover != raw)
            return;
        const std::vector<std::string> seen = raw->seenFailed();
        _popover                            = nullptr;
        setChecked(false);
        for (const std::string &id : seen)
            _ctx.store().dismissJob(conv, id);
    };
    setChecked(true);
    w->showPopup(std::move(p));
}

// ── Forms and layout ────────────────────────────────────────────────────────

float JobsButton::widthOf(Form f) const {
    const size_t n = _jobs.size();
    if (n == 0)
        return 0;
    switch (f) {
    case Form::Label: {
        if (n != 1)
            return kInf;
        const float name  = std::min(kMaxName, std::ceil(_name->measure(kInf, kInf).w));
        const float value = std::ceil(_value->measure(kInf, kInf).w);
        return kPad + kRing + kTextGap + name + kTextGap + value + kPad;
    }
    case Form::Rings: {
        if (n <= 3)
            return kPad + float(n) * kRing + float(n - 1) * kRingGap + kPad;
        Label probe(str::concat({"+", str::number(int64_t(n - 2))}), Font::SmallSemibold);
        return kPad + 2 * kRing + kRingGap + kTextGap + std::ceil(probe.measure(kInf, kInf).w) +
               kPad;
    }
    case Form::Count: {
        Label probe(str::number(int64_t(n)), Font::SmallSemibold);
        return kPad + kRing + kTextGap + std::ceil(probe.measure(kInf, kInf).w) + kPad;
    }
    }
    return 0;
}

JobsButton::Form JobsButton::fit(float room) const {
    if (_jobs.size() == 1 && widthOf(Form::Label) <= room)
        return Form::Label;
    if (_jobs.size() == 1 || widthOf(Form::Rings) <= room)
        return Form::Rings; // one ring is as small as the count form
    return Form::Count;
}

void JobsButton::setForm(Form f) {
    if (f == Form::Label && _jobs.size() != 1)
        f = Form::Rings;
    if (f == _form)
        return;
    _form = f;
    rebuild();
}

void JobsButton::rebuild() {
    const size_t n = _jobs.size();
    _slots.clear();
    const bool label = _form == Form::Label && n == 1;
    _name->setVisible(label);
    _value->setVisible(label);
    if (label) {
        _slots.push_back(0);
        _name->setText(_jobs[0].name);
        _value->setText(shortValue(_jobs[0], nowMs()));
        const bool failed = _jobs[0].state == State::Failed;
        _value->setColor(failed ? C::Danger : C::TextMuted);
        _value->setFont(failed ? Font::ControlBold : Font::Control);
    } else if (_form == Form::Count && n > 1) {
        _slots = {0, -1};
    } else if (n <= 3) {
        for (size_t i = 0; i < n; ++i)
            _slots.push_back(int(i));
    } else {
        _slots = {0, 1, -1};
    }
    const bool more = std::find(_slots.begin(), _slots.end(), -1) != _slots.end();
    _more->setVisible(more);
    if (more) {
        const bool anyFailed = std::any_of(_jobs.begin(), _jobs.end(), [](const BackgroundJob &j) {
            return j.state == State::Failed;
        });
        _more->setText(
            _form == Form::Count ? str::number(int64_t(n))
                                 : str::concat({"+", str::number(int64_t(n - 2))})
        );
        _more->setColor(_form == Form::Count && anyFailed ? C::Danger : C::TextMuted);
    }
    _hover = -1;
    invalidateLayout();
    update();
}

SizeF JobsButton::measureContent(float, float) {
    return {
        _jobs.empty() ? 0
                      : widthOf(_form == Form::Label && _jobs.size() != 1 ? Form::Rings : _form),
        kH
    };
}

void JobsButton::layout() {
    _slotX.clear();
    float x = kPad;
    for (const int s : _slots) {
        if (s < 0) {
            const SizeF m = _more->measure(kInf, kInf);
            x += kTextGap - kRingGap;
            _more->setFrame({x, std::round((kH - m.h) / 2), std::ceil(m.w), m.h});
            _slotX.push_back(x + m.w / 2);
            x += std::ceil(m.w);
            continue;
        }
        _slotX.push_back(x + kRing / 2);
        x += kRing + kRingGap;
    }
    if (_form == Form::Label && _jobs.size() == 1) {
        x              = kPad + kRing + kTextGap;
        const SizeF nm = _name->measure(kInf, kInf);
        const float nw = std::min(kMaxName, std::ceil(nm.w));
        _name->setFrame({x, std::round((kH - nm.h) / 2), nw, nm.h});
        x += nw + kTextGap;
        const SizeF vm = _value->measure(kInf, kInf);
        _value->setFrame({x, std::round((kH - vm.h) / 2), std::ceil(vm.w), vm.h});
    }
}

void JobsButton::paint(gfx::Painter &p) {
    Clickable::paint(p); // the hover / open wash
    const double now = double(nowMs());
    for (size_t i = 0; i < _slots.size() && i < _slotX.size(); ++i)
        if (_slots[i] >= 0)
            paintRing(p, {_slotX[i], kH / 2}, _jobs[size_t(_slots[i])], now);
}

bool JobsButton::tick(double) {
    update();
    return anySpinner() && visible();
}

void JobsButton::windowChanged() {
    if (window() && anySpinner() && visible())
        startTicking();
}

// ── Rings ───────────────────────────────────────────────────────────────────

void JobsButton::paintRing(gfx::Painter &p, PointF c, const BackgroundJob &j, double nowMs) {
    switch (j.state) {
    case State::Running:
        if (j.progress >= 0) {
            // A pie: the outline, an inner disc, the wedge done so far.
            p.strokeCircle(c, 8, 1.5f, color(C::Progress));
            p.fillCircle(c, 4.5f, color(C::Border));
            const float f = std::clamp(j.progress, 0.f, 1.f);
            if (f > 0) {
                gfx::Path wedge;
                wedge.moveTo(c.x, c.y);
                arcPath(wedge, c, 4.5f, 0, f * 2 * kPi, false);
                wedge.close();
                p.fillPath(wedge, color(C::Progress));
            }
        } else {
            // A quarter arc going round on its track.
            p.strokeCircle(c, 8, 2, color(C::Border));
            const float a0 = float(std::fmod(nowMs, kTurnMs) / kTurnMs) * 2 * kPi;
            gfx::Path   arc;
            arcPath(arc, c, 7, a0, a0 + kPi / 2);
            p.strokePath(arc, 2, color(C::Progress));
        }
        break;
    case State::Done: {
        p.strokeCircle(c, 8, 2, color(C::Online));
        gfx::Path check;
        check.moveTo(c.x - 3.2f, c.y + 0.2f);
        check.lineTo(c.x - 0.8f, c.y + 2.6f);
        check.lineTo(c.x + 3.4f, c.y - 2.4f);
        p.strokePath(check, 1.8f, color(C::Online));
        break;
    }
    case State::Failed: {
        p.fillCircle(c, 8, color(C::Danger));
        gfx::Path x;
        x.moveTo(c.x - 2.8f, c.y - 2.8f);
        x.lineTo(c.x + 2.8f, c.y + 2.8f);
        x.moveTo(c.x + 2.8f, c.y - 2.8f);
        x.lineTo(c.x - 2.8f, c.y + 2.8f);
        p.strokePath(x, 1.8f, gfx::Color(0xffffffff));
        break;
    }
    case State::Stopped:
        p.strokeCircle(c, 8, 2, color(C::Border));
        p.fillRoundRect({c.x - 3, c.y - 3, 6, 6}, 1.2f, color(C::TextMuted));
        break;
    }
}

// ── Words ───────────────────────────────────────────────────────────────────

std::string JobsButton::shortValue(const BackgroundJob &j, int64_t now) {
    switch (j.state) {
    case State::Running:
        return j.progress >= 0 ? percent(j.progress) : shortElapsed(now - j.startedMs);
    case State::Done:
        return tr("done");
    case State::Failed:
        return tr("failed");
    case State::Stopped:
        return tr("stopped");
    }
    return {};
}

std::string JobsButton::jobTooltip(const BackgroundJob &j, int64_t now) {
    switch (j.state) {
    case State::Running: {
        std::string what = j.progress >= 0 ? percent(j.progress) : std::string(tr("running"));
        if (j.progress >= 0 && !j.ratio.empty())
            what = str::concat({what, " (", j.ratio, ")"});
        return str::concat(
            {j.name,
             " \xC2\xB7 ",
             what,
             " \xC2\xB7 ",
             TypingIndicator::formatElapsed(now - j.startedMs)}
        );
    }
    case State::Done:
        return str::concat(
            {j.name,
             " \xC2\xB7 ",
             i18n::arg(tr("done in %1"), TypingIndicator::formatElapsed(j.endedMs - j.startedMs))}
        );
    case State::Failed:
        return str::concat(
            {j.name,
             " \xC2\xB7 ",
             i18n::arg(tr("failed, exit code %1"), str::number(int64_t(j.exitCode)))}
        );
    case State::Stopped:
        return str::concat({j.name, " \xC2\xB7 ", tr("stopped")});
    }
    return {};
}

std::string JobsButton::summary() const {
    const int64_t failed = std::count_if(_jobs.begin(), _jobs.end(), [](const BackgroundJob &j) {
        return j.state == State::Failed;
    });
    std::string   s = i18n::trn("%n background job", "%n background jobs", int64_t(_jobs.size()));
    if (failed)
        s = str::concat({s, ", ", i18n::trn("%n failed", "%n failed", failed)});
    return s;
}

std::string JobsButton::slotTooltip(size_t i) const {
    if (i >= _slots.size())
        return {};
    if (_slots[i] >= 0)
        return jobTooltip(_jobs[size_t(_slots[i])], nowMs());
    if (_form == Form::Count)
        return summary();
    return i18n::trn(
        "%n more background job", "%n more background jobs", int64_t(_jobs.size()) - 2
    );
}

int JobsButton::slotAt(PointF local) const {
    if (_form == Form::Label)
        return _slots.empty() ? -1 : 0;
    int   best = -1;
    float d    = kInf;
    for (size_t i = 0; i < _slotX.size(); ++i)
        if (std::fabs(local.x - _slotX[i]) < d) {
            d    = std::fabs(local.x - _slotX[i]);
            best = int(i);
        }
    return best;
}

bool JobsButton::onEvent(Event &e) {
    if (e.type == EventType::PointerMove || e.type == EventType::PointerEnter) {
        const int s = slotAt(e.pos);
        if (s != _hover) {
            _hover = s;
            if (Window *w = window())
                w->rearmTooltip();
        }
    } else if (e.type == EventType::PointerLeave) {
        _hover = -1;
    }
    return Clickable::onEvent(e);
}

std::string JobsButton::tooltip() const {
    if (_popover)
        return {};
    return _hover >= 0 ? slotTooltip(size_t(_hover)) : summary();
}

RectF JobsButton::tooltipAnchor() const {
    const RectF r = windowRect();
    if (_hover < 0 || size_t(_hover) >= _slotX.size() || _form == Form::Label)
        return r;
    return {r.x + _slotX[size_t(_hover)] - kRing / 2, r.y, kRing, r.h};
}

} // namespace shell
