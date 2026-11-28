#include "app/screens/common/loading_indicator.h"

#include <cmath>

namespace screens {

namespace {

constexpr int      kIntervalMs = 160;
constexpr float    kGapDeg     = 3;
// The loader's four colours: the same in every theme.
constexpr uint32_t kColors[4]  = {0xedae2f, 0x2fb27c, 0x38bced, 0xdc1a59};

} // namespace

LoadingIndicator::~LoadingIndicator() {
    if (_timer) // no onStep: the owner is being destroyed
        ui::app()->cancelTimer(_timer);
}

void LoadingIndicator::start() {
    _step = 0;
    if (!_timer)
        _timer = ui::app()->addTimer(kIntervalMs, true, [this] {
            _step = (_step + 1) % 4;
            if (_onStep)
                _onStep();
        });
    if (_onStep)
        _onStep();
}

void LoadingIndicator::stop() {
    if (!_timer)
        return;
    ui::app()->cancelTimer(_timer);
    _timer = 0;
    if (_onStep)
        _onStep();
}

void LoadingIndicator::paint(gfx::Painter &p, ui::RectF r, float diameter) const {
    const float     cx = std::floor(r.x + r.w / 2), cy = std::floor(r.y + r.h / 2);
    const float     outer = diameter / 2, inner = outer - kStroke * diameter / kDiameter;
    constexpr float kPi = 3.14159265f;
    // Each arc as a filled annulus sector, so its ends are cut flat.
    for (int i = 0; i < 4; ++i) {
        const float   a0    = (float(i) * 90 + kGapDeg) * kPi / 180;
        const float   a1    = (float(i) * 90 + 90 - kGapDeg) * kPi / 180;
        constexpr int kSegs = 16;
        gfx::Path     path;
        for (int k = 0; k <= kSegs; ++k) {
            const float a = a0 + (a1 - a0) * float(k) / kSegs; // 0 = 12 o'clock, clockwise
            const float x = cx + outer * std::sin(a), y = cy - outer * std::cos(a);
            if (k == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        for (int k = kSegs; k >= 0; --k) {
            const float a = a0 + (a1 - a0) * float(k) / kSegs;
            path.lineTo(cx + inner * std::sin(a), cy - inner * std::cos(a));
        }
        path.close();
        p.fillPath(path, gfx::rgba(kColors[i], i == _step ? 255 : 51)); // setAlphaF(0.2)
    }
}

} // namespace screens
