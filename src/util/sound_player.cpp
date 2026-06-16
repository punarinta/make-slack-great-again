#include "util/sound_player.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace Sound {

// Platform hooks — implemented per-OS in sound_player_{mac.mm,win.cpp,linux.cpp}.
namespace detail {
std::vector<Entry> enumerateSystemSounds();
bool               playSystem(const QString &nativeId); // returns false if it couldn't play
bool               playFile(const QString &path);       // absolute path to a real file
} // namespace detail

namespace {

// The single bundled chime. Synthesized by scripts/gen-notify-sound.py.
constexpr char kDefaultBundled[] = "notify";

// Native audio APIs can't read a qrc resource, so extract the bundled WAV to a
// real file under the cache dir once and reuse it.
QString bundledPath(const QString &name) {
    const QString src = QStringLiteral(":/sfx/%1.wav").arg(name);
    if (!QFile::exists(src))
        return {};
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty())
        return {};
    QDir().mkpath(dir);
    const QString dst = dir + QStringLiteral("/sfx-%1.wav").arg(name);
    if (!QFile::exists(dst)) {
        if (!QFile::copy(src, dst))
            return {};
        QFile::setPermissions(dst, QFile::ReadOwner | QFile::WriteOwner);
    }
    return dst;
}

} // namespace

Player &Player::instance() {
    static Player p;
    return p;
}

QString Player::defaultId() {
    return QStringLiteral("bundled:") + QLatin1String(kDefaultBundled);
}

std::vector<Entry> Player::bundledSounds() const {
    return {{defaultId(), QCoreApplication::translate("Sound", "msga chime")}};
}

std::vector<Entry> Player::systemSounds() const {
    return detail::enumerateSystemSounds();
}

bool Player::playId(const QString &id) {
    if (id.startsWith(QLatin1String("system:")))
        return detail::playSystem(id.mid(7));
    if (id.startsWith(QLatin1String("bundled:"))) {
        const QString path = bundledPath(id.mid(8));
        return !path.isEmpty() && detail::playFile(path);
    }
    return false;
}

void Player::play(const QString &id) {
    const QString want = id.isEmpty() ? defaultId() : id;
    if (playId(want))
        return;
    // Fall back to the always-present bundled chime.
    if (want != defaultId())
        playId(defaultId());
}

} // namespace Sound
