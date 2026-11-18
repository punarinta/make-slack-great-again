// Cursor theme names for the two Linux backends: libxcursor on X11 and
// wl_cursor on Wayland read the same theme directories.
#pragma once

#include "plat/plat.h"

namespace plat::linux_cursor {

// The theme names to try for `c`, best first: the CSS/freedesktop name, then
// the legacy X11 names older themes (and Adwaita's fallbacks) still use.
// nullptr-terminated; Hidden gets Arrow's names.
const char *const *themeNames(Cursor c);

} // namespace plat::linux_cursor
