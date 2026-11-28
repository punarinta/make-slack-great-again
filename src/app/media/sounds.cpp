#include "app/media/sounds.h"

#include "app/identity.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "plat/audio.h"
#include "plat/plat.h"

#include <memory>

using i18n::tr;

// The chime (sfx/notify.wav, synthesized by scripts/gen-notify-sound.py),
// compiled in by src/tools/embed.cmake.
namespace sounds::data {
extern const unsigned char kNotifyWav[];
extern const unsigned      kNotifyWav_size;
} // namespace sounds::data

namespace sounds {

namespace {

constexpr std::string_view kSystem = "system:";

// Native audio APIs can't read from our binary, so the bundled WAV is written
// to a real file under the cache dir once and reused there. A size
// change (a release re-rendered the chime) rewrites it.
// BLOCKING: run off the UI thread.
std::string bundledPath(const std::string &dir) {
    if (dir.empty())
        return {};
    const std::string path = file::join(dir, "sfx-notify.wav");
    if (file::size(path) == int64_t(data::kNotifyWav_size))
        return path;
    const std::string_view bytes(
        reinterpret_cast<const char *>(data::kNotifyWav), data::kNotifyWav_size
    );
    if (!file::makeDirs(dir) || !file::writeAtomic(path, bytes, 0600))
        return {};
    return path;
}

} // namespace

std::vector<Entry> bundled() {
    return {{kDefaultId, tr("msga chime")}};
}

void systemSounds(plat::App &app, std::function<void(std::vector<Entry>)> done) {
    auto list = std::make_shared<std::vector<Entry>>();
    model::runInBackground(
        app,
        [list] {
            for (auto &s : plat::audio::systemSounds())
                list->push_back({str::concat({kSystem, s.name}), std::move(s.label)});
        },
        [list, done = std::move(done)] {
            if (done)
                done(std::move(*list));
        }
    );
}

void play(plat::App &app, const std::string &id) {
    // A system sound plays by name; the chime is the fallback for anything
    // that doesn't play (and for an unknown id).
    std::string name;
    if (str::startsWith(id, kSystem))
        name = id.substr(kSystem.size());
    auto path = std::make_shared<std::string>();
    model::runInBackground(
        app,
        [path, dir = identity::cacheDir(app)] { *path = bundledPath(dir); },
        [path, name] { plat::audio::playSound(name, *path); }
    );
}

} // namespace sounds
