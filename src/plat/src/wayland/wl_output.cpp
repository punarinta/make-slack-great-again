// Wayland outputs: wl_output + zxdg_output_v1 into App::monitors().
//
// Coordinates: the virtual desktop is xdg-output's logical layout (what
// sway/KWin/mutter use to arrange outputs); with fractional scaling that is
// the only place the real logical size lives, wl_output alone reports just an
// integer scale. Without xdg-output (rare: weston without the module, some
// kiosks) bounds fall back to wl_output.geometry's position and mode / scale.
//
// What Wayland does not have, and how Monitor fills it:
//   - no work area (panels are other clients' layer surfaces, invisible to
//     us): workArea = bounds;
//   - no primary output: the one at logical 0,0, else the first bound, so
//     the answer is deterministic and matches where compositors put the
//     first output by default;
//   - ids: the wl_output registry name, stable while the output is plugged
//     in (a re-plug is a new global, so a new id).
#include "wayland/wl_internal.h"

#include <algorithm>
#include <cmath>

namespace plat::wl {

namespace {

const wl_output_listener kOutputListener = {
    .geometry =
        [](void *d,
           wl_output *,
           int32_t x,
           int32_t y,
           int32_t,
           int32_t,
           int32_t,
           const char *,
           const char *,
           int32_t transform) {
            auto *o      = static_cast<Output *>(d);
            o->geomX     = x;
            o->geomY     = y;
            o->transform = transform;
        },
    .mode =
        [](void *d, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
            if (flags & WL_OUTPUT_MODE_CURRENT) {
                auto *o           = static_cast<Output *>(d);
                o->modeW          = w;
                o->modeH          = h;
                o->refreshMilliHz = refresh;
            }
        },
    .done =
        [](void *d, wl_output *) {
            static_cast<Output *>(d)->app->onOutputDone(static_cast<Output *>(d));
        },
    .scale =
        [](void *d, wl_output *, int32_t s) { static_cast<Output *>(d)->scale = std::max(1, s); },
    .name = [](void *d, wl_output *, const char *n) { static_cast<Output *>(d)->wlName = n; },
    .description =
        [](void *d, wl_output *, const char *n) { static_cast<Output *>(d)->description = n; },
};

const zxdg_output_v1_listener kXdgOutputListener = {
    .logical_position =
        [](void *d, zxdg_output_v1 *, int32_t x, int32_t y) {
            static_cast<Output *>(d)->logicalX = x;
            static_cast<Output *>(d)->logicalY = y;
        },
    .logical_size =
        [](void *d, zxdg_output_v1 *, int32_t w, int32_t h) {
            static_cast<Output *>(d)->logicalW = w;
            static_cast<Output *>(d)->logicalH = h;
        },
    // Before v3 xdg-output has its own done; from v3 on wl_output.done
    // covers both, and this one is never sent.
    .done =
        [](void *d, zxdg_output_v1 *) {
            static_cast<Output *>(d)->app->onOutputDone(static_cast<Output *>(d));
        },
    .name = [](void *d, zxdg_output_v1 *, const char *n) { static_cast<Output *>(d)->xdgName = n; },
    .description = [](void *, zxdg_output_v1 *, const char *) {},
};

} // namespace

void WlApp::bindOutput(uint32_t name, uint32_t version) {
    auto o     = std::make_unique<Output>();
    o->app     = this;
    o->name    = name;
    o->version = std::min(version, 4u); // v4: name/description
    o->output  = static_cast<wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, o->version)
    );
    wl_output_add_listener(o->output, &kOutputListener, o.get());
    bindXdgOutput(o.get());
    outputs.push_back(std::move(o));
}

// Either order works: an output bound before the manager gets its
// xdg-output when the manager arrives (onGlobal walks the outputs).
void WlApp::bindXdgOutput(Output *o) {
    if (!xdgOutputManager || o->xdgOutput)
        return;
    o->xdgOutput = zxdg_output_manager_v1_get_xdg_output(xdgOutputManager, o->output);
    zxdg_output_v1_add_listener(o->xdgOutput, &kXdgOutputListener, o);
}

void WlApp::destroyOutput(Output *o) {
    if (o->xdgOutput)
        zxdg_output_v1_destroy(o->xdgOutput);
    o->xdgOutput = nullptr;
    o->version >= 3 ? wl_output_release(o->output) : wl_output_destroy(o->output);
    o->output = nullptr;
}

void WlApp::removeOutput(uint32_t name) {
    auto it =
        std::find_if(outputs.begin(), outputs.end(), [&](auto &o) { return o->name == name; });
    if (it == outputs.end())
        return;
    for (auto *w : _windows)
        w->outputGone(it->get());
    destroyOutput(it->get());
    outputs.erase(it);
    scheduleMonitorsChanged();
}

void WlApp::onOutputDone(Output *o) {
    o->announced = true;
    // Pre-v6 compositors change a window's integer scale only through the
    // outputs it is on; re-derive (updateScale emits Resized if it moved).
    for (auto *w : std::vector<WlWindow *>(_windows))
        if (alive(w))
            w->outputChanged(o);
    scheduleMonitorsChanged();
}

void WlApp::scheduleMonitorsChanged() {
    if (!_initDone || _monitorsPending)
        return;
    _monitorsPending = true;
    // Posted, not emitted from inside libwayland's dispatch, and after the
    // whole batch (wl_output.done and xdg_output.done may both be in it).
    post([this] {
        _monitorsPending = false;
        auto now         = monitors();
        if (now == _lastMonitors)
            return;
        _lastMonitors = std::move(now);
        emit({.type = EventType::MonitorsChanged});
    });
}

std::vector<Monitor> WlApp::monitors() const {
    std::vector<Monitor> out;
    for (const auto &o : outputs) {
        if (!o->announced)
            continue;
        // Modes are in the output's native orientation; a rotated output's
        // logical box is the other way round.
        const bool rotated = (o->transform & 1) != 0;
        const int  pw = rotated ? o->modeH : o->modeW, ph = rotated ? o->modeW : o->modeH;
        Monitor    m;
        m.id             = o->name;
        m.refreshMilliHz = std::max(0, o->refreshMilliHz);
        m.name           = !o->wlName.empty()        ? o->wlName
                           : !o->xdgName.empty()     ? o->xdgName
                           : !o->description.empty() ? o->description
                                                     : "output-" + std::to_string(o->name);
        if (o->logicalW > 0 && o->logicalH > 0) {
            m.bounds = {o->logicalX, o->logicalY, o->logicalW, o->logicalH};
            // Fractional scale = physical / logical. Rounded to the 1/120
            // steps of wp_fractional_scale so 2560/1707 reads 1.5, not 1.4997.
            m.scale  = pw > 0 ? std::round(double(pw) / o->logicalW * 120.0) / 120.0 : o->scale;
        } else {
            m.scale  = o->scale;
            m.bounds = {
                o->geomX,
                o->geomY,
                std::max(1, int(std::lround(pw / m.scale))),
                std::max(1, int(std::lround(ph / m.scale)))
            };
        }
        if (!(m.scale > 0))
            m.scale = 1.0;
        m.workArea = m.bounds; // Wayland has no work area (see the top of this file)
        out.push_back(std::move(m));
    }
    auto primary = std::find_if(out.begin(), out.end(), [](const Monitor &m) {
        return m.bounds.x == 0 && m.bounds.y == 0;
    });
    if (primary == out.end())
        primary = out.begin();
    if (primary != out.end())
        primary->primary = true;
    return out;
}

// ── window side ─────────────────────────────────────────────────────────────

uint64_t WlWindow::monitor() const {
    // _outputs is in entry order and leave removes, so the back is the most
    // recently entered output that still shows us.
    for (auto it = _outputs.rbegin(); it != _outputs.rend(); ++it)
        if ((*it)->announced)
            return (*it)->name;
    for (const auto &m : app->monitors())
        if (m.primary)
            return m.id;
    return 0;
}

void WlWindow::outputChanged(Output *o) {
    if (std::find(_outputs.begin(), _outputs.end(), o) != _outputs.end())
        updateScale();
}

} // namespace plat::wl
