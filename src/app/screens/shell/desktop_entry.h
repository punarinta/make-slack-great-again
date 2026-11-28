// The Linux launcher entry msga installs for itself on every start:
// ~/.local/share/applications/msga.desktop
// — visible in the application menu, the msga:// handler, named after the
// app id so the window, its notifications and the launcher badge match it —
// and the logo at icons/hicolor/256x256/apps/msga.png, which the entry names
// by its absolute path. Both are rewritten only when they change (a moved
// binary); then update-desktop-database refreshes the MIME cache.
//
// Then the msga:// scheme is registered with plat, which finds the entry and
// makes it the default handler. Elsewhere only the latter: the Windows
// registry, the macOS bundle's Info.plist.
#pragma once

#include <string>

namespace plat {
class App;
}

namespace shell {

// Off the UI thread; registers the scheme when done.
void installDesktopEntry(plat::App &app);

// The entry's text: the template with Exec= and Icon= filled in (Exec
// quoted when the path has a space).
std::string desktopEntry(const std::string &exePath, const std::string &icon);

} // namespace shell
