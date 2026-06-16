#include "util/sound_player.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>

// Linux has no dependency-free in-process audio path (every sound server needs
// a shared lib that won't link into the static binary), so we hand playback to
// whichever helper the desktop ships and enumerate system sounds by scanning
// the active freedesktop sound theme. Both degrade gracefully: a desktop with
// no helper / no theme simply gets the bundled chime via no usable path → the
// caller's fallback, and an empty system list (the dropdown still has the
// bundled sound).

namespace Sound::detail {

bool playFile(const QString &path); // defined below; used by playSystem

namespace {

// Base dirs that hold sound themes, most-specific first (user overrides system).
QStringList soundBaseDirs() {
    QStringList   dirs;
    const QString xdgData = qEnvironmentVariable("XDG_DATA_HOME");
    if (!xdgData.isEmpty())
        dirs << xdgData + "/sounds";
    dirs << QDir::homePath() + "/.local/share/sounds";
    dirs << "/usr/local/share/sounds";
    dirs << "/usr/share/sounds";
    return dirs;
}

// Best-effort current theme name; falls back to the freedesktop default.
QString currentThemeName() {
    QProcess p;
    p.start("gsettings", {"get", "org.gnome.desktop.sound", "theme-name"});
    if (p.waitForStarted(300) && p.waitForFinished(500)) {
        QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        out.remove('\'').remove('"');
        if (!out.isEmpty())
            return out;
    }
    return QStringLiteral("freedesktop");
}

// Directory holding a theme's event sounds (.../<theme>/stereo), or empty.
QString themeStereoDir(const QString &theme) {
    for (const QString &base : soundBaseDirs()) {
        const QString dir = base + "/" + theme + "/stereo";
        if (QFileInfo::exists(dir))
            return dir;
    }
    return {};
}

// "message-new-instant" -> "Message new instant"
QString prettify(QString name) {
    name.replace('-', ' ').replace('_', ' ');
    if (!name.isEmpty())
        name[0] = name[0].toUpper();
    return name;
}

bool spawn(const QString &program, const QStringList &args) {
    return QProcess::startDetached(program, args);
}

} // namespace

std::vector<Entry> enumerateSystemSounds() {
    std::vector<Entry> out;
    QString            dir = themeStereoDir(currentThemeName());
    if (dir.isEmpty())
        dir = themeStereoDir(QStringLiteral("freedesktop"));
    if (dir.isEmpty())
        return out;

    QDir          d(dir);
    const auto    files = d.entryInfoList({"*.oga", "*.ogg", "*.wav"}, QDir::Files, QDir::Name);
    QSet<QString> seen;
    for (const QFileInfo &fi : files) {
        const QString name = fi.completeBaseName();
        if (name.isEmpty() || seen.contains(name))
            continue;
        seen.insert(name);
        out.push_back({QStringLiteral("system:") + name, prettify(name)});
    }
    return out;
}

bool playSystem(const QString &nativeId) {
    // Prefer canberra: it understands the theme and decodes .oga natively.
    if (spawn("canberra-gtk-play", {"-i", nativeId}))
        return true;
    // Otherwise resolve the theme file and play it directly.
    QString dir = themeStereoDir(currentThemeName());
    if (dir.isEmpty())
        dir = themeStereoDir(QStringLiteral("freedesktop"));
    if (!dir.isEmpty()) {
        for (const char *ext : {".oga", ".ogg", ".wav"}) {
            const QString path = dir + "/" + nativeId + ext;
            if (QFileInfo::exists(path))
                return playFile(path);
        }
    }
    return false;
}

bool playFile(const QString &path) {
    // Try the common players in order; first one that launches wins.
    if (spawn("pw-play", {path}))
        return true;
    if (spawn("paplay", {path}))
        return true;
    if (spawn("ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", path}))
        return true;
    if (spawn("aplay", {"-q", path})) // WAV only, fine for the bundled chime
        return true;
    if (spawn("canberra-gtk-play", {"-f", path}))
        return true;
    return false;
}

} // namespace Sound::detail
