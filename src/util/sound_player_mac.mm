#include "util/sound_player.h"

#import <AppKit/AppKit.h>

#include <QDir>
#include <QFileInfo>
#include <QStringList>

// macOS: NSSound covers both cases natively — named system sounds via
// +soundNamed: (the .aiff files under the Sounds dirs, same list System
// Settings shows) and arbitrary files via -initWithContentsOfFile:. We keep a
// strong reference to the currently-playing sound; an NSSound that gets
// released stops immediately.

namespace Sound::detail {

namespace {
NSSound *g_current = nil; // retains the playing sound (strong, never autoreleased)

QStringList soundDirs() {
    return {QStringLiteral("/System/Library/Sounds"),
            QStringLiteral("/Library/Sounds"),
            QDir::homePath() + QStringLiteral("/Library/Sounds")};
}

bool startPlaying(NSSound *sound) {
    if (!sound)
        return false;
    if (g_current)
        [g_current stop];
    [g_current release];
    g_current = [sound retain];
    return [g_current play];
}
} // namespace

std::vector<Entry> enumerateSystemSounds() {
    std::vector<Entry> out;
    for (const QString &dir : soundDirs()) {
        const auto files = QDir(dir).entryInfoList({"*.aiff", "*.aif", "*.wav"},
                                                   QDir::Files, QDir::Name);
        for (const QFileInfo &fi : files) {
            const QString name = fi.completeBaseName();
            // System Settings shows the bare name; +soundNamed: resolves it.
            out.push_back({QStringLiteral("system:") + name, name});
        }
    }
    return out;
}

bool playSystem(const QString &nativeId) {
    NSString *name = nativeId.toNSString();
    NSSound *sound = [NSSound soundNamed:name];
    return startPlaying(sound);
}

bool playFile(const QString &path) {
    NSString *p = path.toNSString();
    NSSound *sound = [[[NSSound alloc] initWithContentsOfFile:p byReference:YES] autorelease];
    return startPlaying(sound);
}

} // namespace Sound::detail
