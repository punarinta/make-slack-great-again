#pragma once

// Set (or clear, when count <= 0) the macOS Dock tile badge — the red pill with
// a number that the OS draws over the app's Dock icon. Independent of
// notification authorization, so it works even when the user has denied
// notifications. Implemented in desktop_notifier_mac.mm; the sole caller guards
// it behind Q_OS_MACOS, so there is no non-mac implementation to link.
void macSetDockBadge(int count);
