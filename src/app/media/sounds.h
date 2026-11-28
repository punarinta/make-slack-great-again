// Notification sounds: the chime bundled in
// the binary plus the OS's own sounds (plat/audio.h), chosen in Settings →
// Notifications and played when a notification fires.
//
// A sound's id is the persisted token (Settings::soundId): "bundled:<name>"
// for a sound shipped in the app, "system:<native>" for an OS sound.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace sounds {

struct Entry {
    std::string id;
    std::string label; // for the Settings dropdown
};

// The id used when none is configured, or when a stored one won't resolve.
inline constexpr const char *kDefaultId = "bundled:notify";

// The sounds bundled with the app (just the chime), labels translated.
std::vector<Entry> bundled();

// The OS's sounds, enumerated on a worker thread (Linux asks gsettings, a
// fresh list per call so a newly added sound shows up); done(list) on the UI
// thread, never inside this call.
void systemSounds(plat::App &app, std::function<void(std::vector<Entry>)> done);

// plat::Notification::silent for every notification the app shows, per OS:
// on macOS the notification never carries a sound (the app plays its own
// chime; one attached would double it); on Linux and Windows it goes out with
// no sound hint at all, so the notification daemon / the toast plays whatever
// the OS plays by default, besides the app's own chime.
#ifdef __APPLE__
inline constexpr bool kSilentNotifications = true;
#else
inline constexpr bool kSilentNotifications = false;
#endif

// Plays the sound `id` (empty or unresolvable: the default chime, as is a
// system sound that fails to play). Fire and forget, from the UI thread.
void play(plat::App &app, const std::string &id);

} // namespace sounds
