// App-wide inline audio playback: the audio cards
// in the message lists. One file plays at a time; playback goes on across
// conversation switches, and every card showing the same file (the channel's,
// the thread panel's) paints the same status, keyed by the file id.
//
// The player never touches the network: the screens download the bytes
// (with the workspace's credentials) into the audio cache, marking the key
// Loading meanwhile, and hand the local path to play(). Durations from the
// service (`duration_ms`) are shown until the engine knows better.
//
// UI thread only; observers run on it after every status change.
#pragma once

#include "app/model/types.h"
#include "base/observers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
namespace audio {
class Player;
struct Failure;
} // namespace audio
} // namespace plat

namespace media {

class AudioPlayer {
public:
    enum class State : uint8_t { Idle, Loading, Playing, Paused, Ended, Error };
    struct Status {
        std::string key; // file id; "" when nothing is loaded
        State       state      = State::Idle;
        int64_t     positionMs = 0;
        int64_t     durationMs = 0; // 0 = unknown
        std::string error;          // State::Error only
    };

    explicit AudioPlayer(plat::App &app);
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer &)            = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    const Status &status() const { return _status; }
    bool isCurrent(const std::string &key) const { return !key.empty() && _status.key == key; }

    // The screens are fetching the bytes: the card shows Loading. A later
    // play() or loadFailed() for the same key resolves it; another key
    // supersedes it.
    void beginLoading(const std::string &key, int64_t knownDurationMs);
    void loadFailed(const std::string &key, const std::string &error);
    // Loads the local file and plays it. Same key while paused = resume.
    void play(const std::string &key, const std::string &localPath, int64_t knownDurationMs = 0);
    void togglePause(); // Playing → pause; Paused / Ended → resume (Ended restarts)
    void pause();
    void resume();
    void seek(int64_t ms);
    void stop();

    // Which of the file's sources to fetch for playback: the first of
    // `path` (Slack's url_private — for audio uploads already the AAC
    // transcode) and the original upload (url_private_download) whose
    // extension this platform can decode; `path` otherwise. A Linux build
    // without ffmpeg can play the original .mp3/.wav, not the transcode.
    std::string        sourceFor(const model::File &f);
    // plat::audio::canPlayExtension, remembered per extension (on Linux it
    // looks for ffmpeg on PATH on every call).
    bool               canPlay(const std::string &ext);
    // The lower-case extension of a file name or URL path ("mp3"); "" if none.
    static std::string extensionOf(std::string_view nameOrUrl);
    // Where a download of `url` for file `f` lives: <cacheDir>/audio/
    // <id>-<hash of url>.<ext> (the original and Slack's transcode differ;
    // the players sniff by the extension). "" without a cache directory.
    std::string        cachePath(const model::File &f, const std::string &url) const;

    // Anything about `key` changed (state, position, duration); also called
    // for the previous key when another file takes over, so both repaint.
    using ObserverId = base::Observers::Id;
    ObserverId observe(std::function<void(const std::string &key)> fn);
    void       unobserve(ObserverId id);

    // Tests: replaces plat::audio::Player::create.
    std::function<std::unique_ptr<plat::audio::Player>()> playerFactory;

private:
    void        ensureEngine();
    void        setState(State s);
    void        tick();
    void        changed(); // for _status.key
    void        notify(const std::string &key);
    std::string takeOver(const std::string &key); // the previous key, if different

    plat::App                                &_app;
    std::unique_ptr<plat::audio::Player>      _engine;
    Status                                    _status;
    int64_t                                   _knownDurationMs = 0;
    uint64_t                                  _tick            = 0;
    std::vector<std::pair<std::string, bool>> _playable; // extension → canPlay
    base::Observers                           _observers;
};

} // namespace media
