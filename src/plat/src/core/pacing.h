// Frame pacing shared by the backends that pace Frames themselves: X11 and
// Win32 always, Wayland for the Frames no frame callback paces.
#pragma once

namespace plat::core {

// Milliseconds between two paced Frames on a display refreshing at
// refreshMilliHz (0 = unknown: 60 Hz). Rounded down, so pacing never runs
// slower than the display; 2..50 ms.
int frameIntervalMs(int refreshMilliHz);

} // namespace plat::core
