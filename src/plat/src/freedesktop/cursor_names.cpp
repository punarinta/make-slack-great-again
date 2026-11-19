#include "freedesktop/cursor_names.h"

#include <iterator>

namespace plat::linux_cursor {

namespace {

// Indexed by Cursor: one table, so the X11 and Wayland fallbacks can't drift.
constexpr const char *kNames[][6] = {
    {"default", "left_ptr"},                                                 // Arrow
    {"text", "xterm", "ibeam"},                                              // IBeam
    {"pointer", "hand2", "pointing_hand", "hand1", "hand"},                  // Hand
    {"wait", "watch"},                                                       // Wait
    {"progress", "left_ptr_watch", "half-busy", "watch"},                    // Progress
    {"crosshair", "cross"},                                                  // Crosshair
    {"not-allowed", "crossed_circle", "forbidden", "circle"},                // NotAllowed
    {"move", "fleur", "all-scroll"},                                         // Move
    {"grab", "openhand", "hand1"},                                           // Grab
    {"grabbing", "closedhand", "fleur"},                                     // Grabbing
    {"ew-resize", "sb_h_double_arrow", "h_double_arrow"},                    // ResizeH
    {"ns-resize", "sb_v_double_arrow", "v_double_arrow"},                    // ResizeV
    {"nwse-resize", "size_fdiag", "bd_double_arrow", "bottom_right_corner"}, // ResizeNWSE
    {"nesw-resize", "size_bdiag", "fd_double_arrow", "bottom_left_corner"},  // ResizeNESW
};
static_assert(std::size(kNames) == size_t(Cursor::Hidden));

} // namespace

const char *const *themeNames(Cursor c) {
    return kNames[size_t(c) < std::size(kNames) ? size_t(c) : 0];
}

} // namespace plat::linux_cursor
