// The platform-neutral half of plat audio: the WAV wrapper and (test builds
// only, PLAT_TEST_HOOKS) the fake recorder behind $MSGA_VOICE_FAKE_WAV. The
// real backends are in audio_{linux,win32,cocoa}.
#include "audio/audio_internal.h"

#include "plat/plat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace plat::audio {

namespace {

void putLe16(char *p, uint32_t v) {
    p[0] = char(v & 0xff);
    p[1] = char((v >> 8) & 0xff);
}
void putLe32(char *p, uint32_t v) {
    putLe16(p, v & 0xffff);
    putLe16(p + 2, v >> 16);
}

#ifdef PLAT_TEST_HOOKS
// "Records" a fixed WAV file: onStarted on the next loop turn, a gentle
// onLevel pulse while running, and the file's bytes (as they are) on stop().
// Lets tests and headless runs exercise voice input without a microphone.
class FakeRecorder final : public Recorder {
public:
    FakeRecorder(App &app, std::string path) : _app(app), _path(std::move(path)) {}
    ~FakeRecorder() override { cancel(); }

    void start() override {
        if (_recording)
            return;
        _recording  = true;
        _tick       = 0;
        _startTimer = _app.addTimer(0, false, [this] {
            _startTimer = 0;
            if (onStarted)
                onStarted();
            if (!_recording)
                return; // stopped from onStarted
            _pulse = _app.addTimer(50, true, [this] {
                ++_tick;
                if (onLevel)
                    onLevel(float(0.5 + 0.4 * std::sin(_tick * 0.6)));
            });
        });
    }

    void stop() override {
        if (!_recording)
            return;
        cancel();
        std::string wav;
        if (FILE *f = std::fopen(_path.c_str(), "rb")) {
            char   buf[16384];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                wav.append(buf, n);
            const bool ok = !std::ferror(f);
            std::fclose(f);
            if (ok) {
                post([wav = std::move(wav)](FakeRecorder &r) mutable {
                    if (r.onFinished)
                        r.onFinished(std::move(wav));
                });
                return;
            }
        }
        post([](FakeRecorder &r) {
            if (r.onFailed)
                r.onFailed({Error::ReadFailed, {}});
        });
    }

    void cancel() override {
        _recording = false;
        ++_generation;
        if (_startTimer)
            _app.cancelTimer(_startTimer);
        if (_pulse)
            _app.cancelTimer(_pulse);
        _startTimer = _pulse = 0;
    }

    bool isRecording() const override { return _recording; }

private:
    // Callbacks never run inside the call that caused them.
    template <class F>
    void post(F fn) {
        const uint64_t gen = _generation;
        _app.addTimer(
            0,
            false,
            [this, alive = std::weak_ptr<int>(_alive), gen, fn = std::move(fn)]() mutable {
                if (!alive.expired() && gen == _generation)
                    fn(*this);
            }
        );
    }

    App                 &_app;
    std::string          _path;
    std::shared_ptr<int> _alive      = std::make_shared<int>(0); // posted callbacks check it
    TimerId              _startTimer = 0, _pulse = 0;
    int                  _tick       = 0;
    uint64_t             _generation = 0;
    bool                 _recording  = false;
};
#endif

} // namespace

void wavInPlace(std::string &buf, int sampleRate, int channels) {
    const auto     dataSize   = uint32_t(buf.size() - kWavHeader);
    const uint32_t blockAlign = uint32_t(channels) * 2;
    char          *h          = buf.data();
    std::memcpy(h, "RIFF", 4);
    putLe32(h + 4, 36 + dataSize);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    putLe32(h + 16, 16); // fmt chunk size
    putLe16(h + 20, 1);  // PCM
    putLe16(h + 22, uint32_t(channels));
    putLe32(h + 24, uint32_t(sampleRate));
    putLe32(h + 28, uint32_t(sampleRate) * blockAlign); // byte rate
    putLe16(h + 32, blockAlign);
    putLe16(h + 34, 16); // bits per sample
    std::memcpy(h + 36, "data", 4);
    putLe32(h + 40, dataSize);
}

std::string wavFromPcm16(std::string_view pcm, int sampleRate, int channels) {
    std::string wav;
    wav.reserve(kWavHeader + pcm.size());
    wav.assign(kWavHeader, '\0');
    wav.append(pcm);
    wavInPlace(wav, sampleRate, channels);
    return wav;
}

float peakOf(const char *pcm, size_t bytes) {
    int         peak = 0;
    const auto *p    = reinterpret_cast<const unsigned char *>(pcm);
    for (size_t i = 0; i + 1 < bytes; i += 2)
        peak = std::max(peak, std::abs(int(int16_t(uint16_t(p[i] | p[i + 1] << 8)))));
    return std::min(1.0f, float(peak) / 32767.0f);
}

std::unique_ptr<Recorder> Recorder::create(App &app) {
#ifdef PLAT_TEST_HOOKS
    if (const char *fake = std::getenv("MSGA_VOICE_FAKE_WAV"); fake && *fake)
        if (FILE *f = std::fopen(fake, "rb")) {
            std::fclose(f);
            return std::make_unique<FakeRecorder>(app, fake);
        }
#endif
    return createNativeRecorder(app);
}

} // namespace plat::audio
