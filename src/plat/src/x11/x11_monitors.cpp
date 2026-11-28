// X11 monitors: RandR 1.5 GetMonitors (what xrandr --listmonitors shows, so
// a video wall configured as one logical monitor is one monitor), each with
// its CRTC's refresh rate, and work areas from the WM's properties.
//
// Work areas: X11 only has per-desktop ones. Mutter/Muffin publish the
// per-monitor rectangles as _GTK_WORKAREAS_D<desktop> (the GTK3 source), and
// that is used when present; otherwise the one _NET_WORKAREA rectangle of the
// current desktop is intersected with each monitor. That approximation is
// what other toolkits (GTK) do too: a panel on one monitor of a multi-monitor setup
// shrinks _NET_WORKAREA's bounding box only where it borders the edge of the
// whole screen, so a panel between two monitors is invisible to it.
#include "x11/x11_internal.h"

#include "core/pacing.h"

#include <xcb/randr.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace plat::x11 {

namespace {

Rect intersect(const Rect &a, const Rect &b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

// Edges rounded independently, so monitors that touch in physical pixels
// still touch in logical ones.
Rect toLogical(const Rect &r, double s) {
    const int x0 = int(std::lround(r.x / s)), y0 = int(std::lround(r.y / s));
    const int x1 = int(std::lround((r.x + r.w) / s)), y1 = int(std::lround((r.y + r.h) / s));
    return {x0, y0, x1 - x0, y1 - y0};
}

int modeMilliHz(const xcb_randr_mode_info_t &m) {
    if (!m.htotal || !m.vtotal)
        return 0;
    double hz = double(m.dot_clock) / (double(m.htotal) * double(m.vtotal));
    if (m.mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)
        hz *= 2;
    if (m.mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
        hz /= 2;
    return int(std::lround(hz * 1000));
}

} // namespace

void X11App::setupRandr() {
    const auto *ext = xcb_get_extension_data(_c, &xcb_randr_id);
    if (!ext || !ext->present)
        return;
    Reply ver(xcb_randr_query_version_reply(_c, xcb_randr_query_version(_c, 1, 5), nullptr));
    if (!ver || ver->major_version != 1)
        return;
    _randrMinor = int(ver->minor_version);
    _randrEvent = ext->first_event;
    if (_randrMinor >= 2) {
        // Screen resizes, CRTC (mode/position) and output (connect) changes;
        // bursts of these arrive for one hotplug and are coalesced.
        xcb_randr_select_input(
            _c,
            _root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
        );
    }
}

bool X11App::isWorkareaAtom(xcb_atom_t a) const {
    return a == _atoms[NetWorkarea] || a == _atoms[NetCurrentDesktop] ||
           (_gtkWorkareas && a == _gtkWorkareas);
}

void X11App::scheduleMonitorRefresh(bool layout) {
    _monitorsLayoutStale |= layout;
    if (_monitorsPending)
        return;
    _monitorsPending = true;
    post([this] {
        _monitorsPending = false;
        if (std::exchange(_monitorsLayoutStale, false) || !_monitorsReady)
            refreshMonitors(true);
        else
            refreshWorkAreas(true);
    });
}

void X11App::refreshMonitors(bool emitChange) {
    std::vector<MonitorEntry> out = readLayout();
    applyWorkAreas(out);
    commitMonitors(std::move(out), emitChange);
}

void X11App::refreshWorkAreas(bool emitChange) {
    // A desktop switch or a panel change: the layout is what it was.
    std::vector<MonitorEntry> out = _monitors;
    applyWorkAreas(out);
    commitMonitors(std::move(out), emitChange);
}

std::vector<X11App::MonitorEntry> X11App::readLayout() {
    std::vector<MonitorEntry> out;
    // Requests go out in batches, each answered together, rather than one
    // blocking round trip per CRTC, output and monitor.
    _refreshMilliHz = 0;
    if (_randrMinor >= 3) {
        const auto resCookie = xcb_randr_get_screen_resources_current(_c, _root);
        xcb_randr_get_monitors_cookie_t monCookie{};
        if (_randrMinor >= 5)
            monCookie = xcb_randr_get_monitors(_c, _root, 1);
        Reply res(xcb_randr_get_screen_resources_current_reply(_c, resCookie, nullptr));
        Reply mons(
            _randrMinor >= 5 ? xcb_randr_get_monitors_reply(_c, monCookie, nullptr) : nullptr
        );
        const xcb_randr_mode_info_t  *modes  = nullptr;
        const xcb_randr_crtc_t       *crtcs  = nullptr;
        int                           nModes = 0, nCrtcs = 0;
        std::vector<xcb_randr_mode_t> crtcMode; // per entry of crtcs, 0 = off
        if (res) {
            modes  = xcb_randr_get_screen_resources_current_modes(res.p);
            nModes = xcb_randr_get_screen_resources_current_modes_length(res.p);
            crtcs  = xcb_randr_get_screen_resources_current_crtcs(res.p);
            nCrtcs = xcb_randr_get_screen_resources_current_crtcs_length(res.p);
            std::vector<xcb_randr_get_crtc_info_cookie_t> cookies;
            cookies.resize(size_t(nCrtcs));
            for (int i = 0; i < nCrtcs; ++i)
                cookies[size_t(i)] = xcb_randr_get_crtc_info(_c, crtcs[i], res->config_timestamp);
            crtcMode.assign(size_t(nCrtcs), 0);
            // X11 has no frame callbacks, so Frames are paced by a timer at the
            // fastest active output's refresh rate; 60 Hz when RandR can't say.
            double best = 0;
            for (int i = 0; i < nCrtcs; ++i) {
                Reply ci(xcb_randr_get_crtc_info_reply(_c, cookies[size_t(i)], nullptr));
                if (!ci || !ci->mode)
                    continue;
                crtcMode[size_t(i)] = ci->mode;
                for (int m = 0; m < nModes; ++m)
                    if (modes[m].id == ci->mode && modes[m].htotal && modes[m].vtotal)
                        best = std::max(
                            best, double(modes[m].dot_clock) / (modes[m].htotal * modes[m].vtotal)
                        );
            }
            if (best >= 20 && best <= 500)
                _refreshMilliHz = int(std::lround(best * 1000));
        }
        // A CRTC's mode refresh, from the batch above.
        auto crtcMilliHz = [&](xcb_randr_crtc_t crtc) {
            for (int i = 0; i < nCrtcs; ++i)
                if (crtcs[i] == crtc)
                    for (int k = 0; k < nModes; ++k)
                        if (crtcMode[size_t(i)] && modes[k].id == crtcMode[size_t(i)])
                            return modeMilliHz(modes[k]);
            return 0;
        };

        // ── the monitors ────────────────────────────────────────────────────
        if (mons) {
            std::vector<xcb_atom_t>                         nameAtoms;
            std::vector<xcb_randr_get_output_info_cookie_t> outputs; // per entry
            std::vector<bool>                               hasOutput;
            for (auto it = xcb_randr_get_monitors_monitors_iterator(mons.p); it.rem;
                 xcb_randr_monitor_info_next(&it)) {
                const xcb_randr_monitor_info_t *mi = it.data;
                if (!mi->width || !mi->height)
                    continue;
                MonitorEntry e;
                e.phys      = {mi->x, mi->y, mi->width, mi->height};
                // The name atom is the server's own handle for the monitor
                // ("DP-1"), stable while it stays connected.
                e.m.id      = mi->name ? uint64_t(mi->name) : uint64_t(out.size() + 1);
                e.m.primary = mi->primary != 0;
                nameAtoms.push_back(mi->name);
                // Refresh from the first output's CRTC mode.
                const bool has = res && xcb_randr_monitor_info_outputs_length(mi) > 0;
                outputs.push_back(
                    has ? xcb_randr_get_output_info(
                              _c, xcb_randr_monitor_info_outputs(mi)[0], res->config_timestamp
                          )
                        : xcb_randr_get_output_info_cookie_t{}
                );
                hasOutput.push_back(has);
                out.push_back(std::move(e));
            }
            for (size_t i = 0; i < out.size(); ++i) {
                if (!hasOutput[i])
                    continue;
                Reply oi(xcb_randr_get_output_info_reply(_c, outputs[i], nullptr));
                if (oi && oi->crtc)
                    out[i].m.refreshMilliHz = crtcMilliHz(oi->crtc);
            }
            const auto names = atomNames(nameAtoms);
            for (size_t i = 0; i < out.size() && i < names.size(); ++i)
                out[i].m.name = names[i];
        }
    }
    _frameIntervalMs = core::frameIntervalMs(_refreshMilliHz);
    if (out.empty()) {
        // No RandR 1.5 (or no active CRTC, as on a bare Xvfb without outputs):
        // the whole screen is the one monitor. The root's live geometry, not
        // the connection setup's, which goes stale after a resize.
        MonitorEntry e;
        Reply        g(xcb_get_geometry_reply(_c, xcb_get_geometry(_c, _root), nullptr));
        e.phys             = g ? Rect{0, 0, g->width, g->height}
                               : Rect{0, 0, _screen->width_in_pixels, _screen->height_in_pixels};
        e.m.id             = 1;
        e.m.name           = "screen";
        e.m.refreshMilliHz = _refreshMilliHz; // fastest CRTC, 0 = unknown
        out.push_back(std::move(e));
    }
    // Exactly one primary: the first when the user never picked one.
    if (std::none_of(out.begin(), out.end(), [](auto &e) { return e.m.primary; }))
        out[0].m.primary = true;
    for (bool seen = false; auto &e : out) {
        e.m.primary = e.m.primary && !seen;
        seen |= e.m.primary;
    }
    return out;
}

void X11App::applyWorkAreas(std::vector<MonitorEntry> &out) {
    // The current desktop and _NET_WORKAREA in one round trip; the per-desktop
    // GTK list (its atom is interned once per desktop) in a second.
    auto cardinals = [this](xcb_atom_t prop, uint32_t longs) {
        return xcb_get_property(_c, 0, _root, prop, XCB_ATOM_CARDINAL, 0, longs);
    };
    const auto deskCookie = cardinals(_atoms[NetCurrentDesktop], 1);
    const auto netCookie  = cardinals(_atoms[NetWorkarea], 4096);
    uint32_t   desktop    = 0;
    {
        Reply r(xcb_get_property_reply(_c, deskCookie, nullptr));
        if (r && r->format == 32 && xcb_get_property_value_length(r.p) >= 4)
            desktop = *static_cast<uint32_t *>(xcb_get_property_value(r.p));
    }
    auto rectsOf = [](const xcb_get_property_reply_t *r) {
        std::vector<Rect> rects;
        if (!r || r->format != 32)
            return rects;
        const auto *v = static_cast<const uint32_t *>(
            xcb_get_property_value(const_cast<xcb_get_property_reply_t *>(r))
        );
        const int n = xcb_get_property_value_length(r) / 16;
        for (int i = 0; i < n; ++i)
            rects.push_back(
                {int(v[4 * i]), int(v[4 * i + 1]), int(v[4 * i + 2]), int(v[4 * i + 3])}
            );
        return rects;
    };
    Reply netReply(xcb_get_property_reply(_c, netCookie, nullptr));
    _gtkWorkareas = intern("_GTK_WORKAREAS_D" + std::to_string(desktop));
    std::vector<Rect> perMonitor;
    if (_gtkWorkareas) {
        Reply r(xcb_get_property_reply(_c, cardinals(_gtkWorkareas, 4096), nullptr));
        perMonitor = rectsOf(r.p);
    }
    std::vector<Rect> net     = _atoms[NetWorkarea] ? rectsOf(netReply.p) : std::vector<Rect>();
    const bool        haveNet = !net.empty();
    const Rect        netArea = haveNet ? net[desktop < net.size() ? desktop : 0] : Rect{};

    for (auto &e : out) {
        Rect wa = e.phys;
        if (!perMonitor.empty()) {
            // The list's order is the WM's monitor order; match by overlap.
            Rect best{};
            for (const Rect &r : perMonitor) {
                const Rect i = intersect(r, e.phys);
                if (i.w * i.h > best.w * best.h)
                    best = i;
            }
            if (best.w > 0)
                wa = best;
        } else if (haveNet) {
            const Rect i = intersect(netArea, e.phys);
            if (i.w > 0)
                wa = i;
        }
        e.m.bounds   = toLogical(e.phys, _scale);
        e.m.workArea = intersect(toLogical(wa, _scale), e.m.bounds);
        if (e.m.workArea.w <= 0)
            e.m.workArea = e.m.bounds;
        e.m.scale = _scale;
    }
}

void X11App::commitMonitors(std::vector<MonitorEntry> out, bool emitChange) {
    const bool changed =
        out.size() != _monitors.size() ||
        !std::equal(out.begin(), out.end(), _monitors.begin(), [](auto &a, auto &b) {
            return a.m == b.m;
        });
    _monitors      = std::move(out);
    _monitorsReady = true;
    if (changed && emitChange)
        emit({.type = EventType::MonitorsChanged});
}

std::vector<Monitor> X11App::monitors() const {
    if (!_monitorsReady)
        const_cast<X11App *>(this)->refreshMonitors(false);
    std::vector<Monitor> v;
    for (auto &e : _monitors)
        v.push_back(e.m);
    return v;
}

uint64_t X11App::monitorAt(int x, int y) const {
    if (!_monitorsReady)
        const_cast<X11App *>(this)->refreshMonitors(false);
    uint64_t best  = 0;
    long     bestD = -1;
    for (auto &e : _monitors) {
        const Rect &r  = e.phys;
        // Distance from the point to the rectangle (0 inside).
        const long  dx = x < r.x ? r.x - x : x >= r.x + r.w ? x - (r.x + r.w - 1) : 0;
        const long  dy = y < r.y ? r.y - y : y >= r.y + r.h ? y - (r.y + r.h - 1) : 0;
        const long  d  = dx * dx + dy * dy;
        if (bestD < 0 || d < bestD) {
            best  = e.m.id;
            bestD = d;
        }
    }
    return best;
}

} // namespace plat::x11
