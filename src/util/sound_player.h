#pragma once

#include <QString>
#include <vector>

namespace Sound {

// One selectable notification sound. `id` is the stable, persisted token in the
// form "system:<native>" (an OS sound, enumerated at runtime) or
// "bundled:<name>" (a sound shipped inside the app, qrc :/sfx/<name>.wav).
// `label` is the human-facing name for the Settings dropdown.
struct Entry {
    QString id;
    QString label;
};

// Cross-platform notification sound playback, abstracting the OS-native audio
// path (Windows: PlaySound/winmm, macOS: NSSound, Linux: spawn paplay/pw-play/
// canberra). No Qt Multimedia dependency, so it stays compatible with the
// fully-static Linux release binary.
//
// OS "system" sounds are enumerated dynamically per platform; one chime ships
// bundled in the binary as the cross-platform default. Playback never throws
// and silently no-ops on a platform with no usable audio path.
class Player {
public:
    static Player &instance();

    // Dynamically enumerated OS system sounds. May be empty (platform exposes
    // none, or enumeration found nothing).
    std::vector<Entry> systemSounds() const;

    // Sounds bundled with the app. Currently just the default chime.
    std::vector<Entry> bundledSounds() const;

    // Id used when none is configured, or when a stored id won't resolve.
    static QString defaultId();

    // Play the sound identified by `id`. An empty or unresolvable id falls back
    // to defaultId(). Fire-and-forget; returns immediately.
    void play(const QString &id);

private:
    Player() = default;
    bool playId(const QString &id);
};

} // namespace Sound
