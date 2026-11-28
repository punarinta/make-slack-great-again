// The loading indicator: a 52 px ring of four
// brand-coloured quarter arcs (7 px, flat ends, 3° gaps), clockwise from 12
// o'clock; every 160 ms the next arc lights up (the rest at 20 %).
// Not a view: whoever shows it paints it into a rect and repaints on onStep.
#pragma once

#include "ui/ui.h"

#include <functional>

namespace screens {

class LoadingIndicator {
public:
    explicit LoadingIndicator(std::function<void()> onStep) : _onStep(std::move(onStep)) {}
    ~LoadingIndicator();
    LoadingIndicator(const LoadingIndicator &)            = delete;
    LoadingIndicator &operator=(const LoadingIndicator &) = delete;

    void start(); // from the first arc
    void stop();
    bool running() const { return _timer != 0; }
    int  step() const { return _step; }
    // Centred in `r` (logical px); `diameter` scales the ring (the voice
    // strip's 14-px spinner is the 52-px one painted scaled down).
    void paint(gfx::Painter &p, ui::RectF r, float diameter = kDiameter) const;

    static constexpr float kDiameter = 52, kStroke = 7;

private:
    std::function<void()> _onStep;
    plat::TimerId         _timer = 0;
    int                   _step  = 0;
};

} // namespace screens
