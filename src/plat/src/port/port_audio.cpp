// plat audio (plat/audio.h) on the port contract's port_audio.h. The port
// posts every callback to the loop thread, as plat's contract wants; this
// file only adapts the types (the recorder's PCM becomes a WAV file here).
#include "audio/audio_internal.h"
#include "port/port_internal.h"

namespace plat::audio {

namespace {

Failure failure(int code, const char *detail) {
    return {
        code >= 0 && code <= int(Error::Os) ? Error(code) : Error::Unavailable, detail ? detail : ""
    };
}
static_assert(PORT_AUDIO_OS == int(Error::Os), "port audio errors out of step with plat");

class PortPlayer final : public Player {
public:
    explicit PortPlayer(App &app) {
        port_player_callbacks cb{};
        cb.size      = sizeof cb;
        cb.user      = this;
        cb.on_loaded = [](void *u) {
            if (auto &f = static_cast<PortPlayer *>(u)->onLoaded)
                f();
        };
        cb.on_ended = [](void *u) {
            if (auto &f = static_cast<PortPlayer *>(u)->onEnded)
                f();
        };
        cb.on_failed = [](void *u, int code, const char *detail) {
            if (auto &f = static_cast<PortPlayer *>(u)->onFailed)
                f(failure(code, detail));
        };
        port_player_create(portApp(app), &cb, &_p);
    }
    ~PortPlayer() override { port_player_destroy(_p); }

    void load(const std::string &path) override {
        if (_p)
            port_player_load(_p, path.c_str());
    }
    void play() override {
        if (_p)
            port_player_play(_p);
    }
    void pause() override {
        if (_p)
            port_player_pause(_p);
    }
    void seek(int64_t ms) override {
        if (_p)
            port_player_seek(_p, ms);
    }
    void stop() override {
        if (_p)
            port_player_stop(_p);
    }
    int64_t positionMs() const override { return _p ? port_player_position_ms(_p) : 0; }
    int64_t durationMs() const override { return _p ? port_player_duration_ms(_p) : 0; }

private:
    port_player *_p = nullptr;
};

class PortRecorder final : public Recorder {
public:
    explicit PortRecorder(App &app) {
        port_recorder_callbacks cb{};
        cb.size       = sizeof cb;
        cb.user       = this;
        cb.on_started = [](void *u) {
            if (auto &f = static_cast<PortRecorder *>(u)->onStarted)
                f();
        };
        cb.on_level = [](void *u, float peak) {
            if (auto &f = static_cast<PortRecorder *>(u)->onLevel)
                f(peak);
        };
        cb.on_finished = [](void *u, const void *pcm, size_t bytes) {
            auto *r       = static_cast<PortRecorder *>(u);
            r->_recording = false;
            if (r->onFinished)
                r->onFinished(wavFromPcm16(
                    std::string_view(static_cast<const char *>(pcm), bytes), kSampleRate, kChannels
                ));
        };
        cb.on_failed = [](void *u, int code, const char *detail) {
            auto *r       = static_cast<PortRecorder *>(u);
            r->_recording = false;
            if (r->onFailed)
                r->onFailed(failure(code, detail));
        };
        port_recorder_create(portApp(app), &cb, &_r);
    }
    ~PortRecorder() override { port_recorder_destroy(_r); }

    void start() override {
        if (!_r || _recording)
            return;
        _recording = true;
        port_recorder_start(_r);
    }
    void stop() override {
        if (_r && _recording)
            port_recorder_stop(_r);
    }
    void cancel() override {
        _recording = false;
        if (_r)
            port_recorder_cancel(_r);
    }
    bool isRecording() const override { return _recording; }

private:
    port_recorder *_r         = nullptr;
    bool           _recording = false;
};

} // namespace

std::unique_ptr<Player> Player::create(App &app) {
    return std::make_unique<PortPlayer>(app);
}

bool canPlayExtension(std::string_view ext) {
    return port_audio_can_play(std::string(ext).c_str()) != 0;
}

std::unique_ptr<Recorder> createNativeRecorder(App &app) {
    return std::make_unique<PortRecorder>(app);
}

std::vector<SystemSound> systemSounds() {
    std::vector<SystemSound> out;
    port_sound_list(
        [](void *u, const char *name, const char *label) {
            static_cast<std::vector<SystemSound> *>(u)->push_back({name, label});
        },
        &out
    );
    return out;
}

void playSound(const std::string &name, const std::string &fallbackFile) {
    port_sound_play(name.c_str(), fallbackFile.c_str());
}

} // namespace plat::audio
