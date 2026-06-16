#include "util/sound_player.h"

#include <QSettings>
#include <QStringList>

#include <windows.h>
// <mmsystem.h> must follow <windows.h>.
#include <mmsystem.h>

// Windows: PlaySound (winmm) plays both named system events (SND_ALIAS, the
// AppEvents scheme) and arbitrary files (SND_FILENAME). We enumerate the
// configured event sounds straight from the registry — the same source the
// Sound control panel uses — and surface only events that have a sound
// assigned.

namespace Sound::detail {

namespace {

constexpr char kApps[]   = "HKEY_CURRENT_USER\\AppEvents\\Schemes\\Apps\\.Default";
constexpr char kLabels[] = "HKEY_CURRENT_USER\\AppEvents\\EventLabels";

QString expandEnv(const QString &raw) {
    if (!raw.contains('%'))
        return raw;
    std::wstring in = raw.toStdWString();
    wchar_t      buf[1024];
    DWORD        n = ExpandEnvironmentStringsW(in.c_str(), buf, 1024);
    if (n == 0 || n > 1024)
        return raw;
    return QString::fromWCharArray(buf);
}

QString currentWav(const QString &event) {
    QSettings     apps(QString::fromLatin1(kApps), QSettings::NativeFormat);
    // The unnamed (default) value of <event>\.Current holds the wav path.
    const QString path = apps.value(event + "/.Current/Default").toString();
    return expandEnv(path);
}

QString labelFor(const QString &event) {
    QSettings     labels(QString::fromLatin1(kLabels), QSettings::NativeFormat);
    const QString l = labels.value(event + "/Default").toString();
    return l.isEmpty() ? event : l;
}

} // namespace

std::vector<Entry> enumerateSystemSounds() {
    std::vector<Entry> out;
    QSettings          apps(QString::fromLatin1(kApps), QSettings::NativeFormat);
    const QStringList  events = apps.childGroups();
    for (const QString &event : events) {
        if (currentWav(event).isEmpty())
            continue; // event has "(None)" assigned — nothing to play
        out.push_back({QStringLiteral("system:") + event, labelFor(event)});
    }
    return out;
}

bool playSystem(const QString &nativeId) {
    const std::wstring alias = nativeId.toStdWString();
    if (PlaySoundW(alias.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT))
        return true;
    // Fall back to the resolved file path.
    const QString wav = currentWav(nativeId);
    if (wav.isEmpty())
        return false;
    return playFile(wav);
}

bool playFile(const QString &path) {
    const std::wstring p = path.toStdWString();
    return PlaySoundW(p.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

} // namespace Sound::detail
