// macOS audio (ARC):
//   playback  AVAudioPlayer: pause/seek/position built in; decodes MP3,
//             AAC/M4A, WAV, AIFF, FLAC, CAF. Delegate callbacks land on the
//             main thread (the App's).
//   capture   AVAudioRecorder writing 16 kHz mono s16le LinearPCM to a temp
//             .wav, with metering polled on a timer for onLevel. Microphone
//             access goes through TCC: the first start() asks (the bundle
//             needs NSMicrophoneUsageDescription; a hardened-runtime signature
//             also the com.apple.security.device.audio-input entitlement), and
//             a denial fails with Error::MicrophoneDenied.
//   sounds    NSSound: named system sounds via +soundNamed: (the Sounds
//             folders, the list System Settings shows) and files.
#include "audio/audio_internal.h"

#include "plat/plat.h"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace plat::audio {
namespace {

// $PLAT_AUDIO_HELPERS set (tests): no device is ever opened, nothing plays.
bool nullAudio() {
    const char *v = std::getenv("PLAT_AUDIO_HELPERS");
    return v && *v;
}

NSString *nsString(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]
               ?: @"";
}

std::string utf8(NSString *s) {
    const char *c = s.UTF8String;
    return c ? c : "";
}

} // namespace
} // namespace plat::audio

// Forwards AVAudioPlayer's delegate calls to a block.
@interface PlatAudioPlayerDelegate : NSObject <AVAudioPlayerDelegate>
@property(nonatomic, copy) void (^finished)(BOOL ok, NSString *error);
@end

@implementation PlatAudioPlayerDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)player successfully:(BOOL)flag {
    if (self.finished)
        self.finished(flag, nil);
}
- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer *)player error:(NSError *)error {
    if (self.finished)
        self.finished(NO, error.localizedDescription ?: @"");
}
@end

// Forwards AVAudioRecorder's encode error to a block (any thread).
@interface PlatAudioRecorderDelegate : NSObject <AVAudioRecorderDelegate>
@property(nonatomic, copy) void (^failed)(NSString *error);
@end

@implementation PlatAudioRecorderDelegate
- (void)audioRecorderEncodeErrorDidOccur:(AVAudioRecorder *)recorder error:(NSError *)error {
    if (self.failed)
        self.failed(error.localizedDescription ?: @"");
}
@end

namespace plat::audio {
namespace {

// ── Playback ────────────────────────────────────────────────────────────────

class MacPlayer final : public Player {
public:
    explicit MacPlayer(App &app) : _app(app) {}
    ~MacPlayer() override {
        *_alive = false;
        stop();
    }

    void load(const std::string &path) override {
        stop();
        const uint64_t gen = ++_generation;
        if (nullAudio()) {
            post(gen, [](MacPlayer &p) { p.fail({Error::Unavailable, {}}); });
            return;
        }
        NSError *err = nil;
        _player =
            [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:nsString(path)]
                                                   error:&err];
        if (!_player) {
            Failure f = err ? Failure{Error::Os, utf8(err.localizedDescription)}
                            : Failure{Error::Unsupported, {}};
            post(gen, [f](MacPlayer &p) { p.fail(f); });
            return;
        }
        _delegate                   = [[PlatAudioPlayerDelegate alloc] init];
        std::shared_ptr<bool> alive = _alive;
        App                  *app   = &_app;
        MacPlayer            *self  = this;
        _delegate.finished          = ^(BOOL ok, NSString *error) {
          // Already on the main thread; posted anyway so a callback never
          // runs inside an AVFoundation call of ours.
          std::string text = error ? utf8(error) : std::string();
          app->post([self, alive, gen, ok, text] {
              if (!*alive || gen != self->_generation)
                  return;
              if (ok) {
                  if (self->onEnded)
                      self->onEnded();
              } else {
                  self->fail(
                      text.empty() ? Failure{Error::PlaybackFailed, {}} : Failure{Error::Os, text}
                  );
              }
          });
        };
        _player.delegate = _delegate;
        [_player prepareToPlay];
        post(gen, [](MacPlayer &p) {
            if (p.onLoaded)
                p.onLoaded();
        });
    }

    void play() override { [_player play]; }
    void pause() override { [_player pause]; }
    void seek(int64_t ms) override {
        if (_player)
            _player.currentTime = double(ms) / 1000.0;
    }
    void stop() override {
        ++_generation;
        if (_player) {
            [_player stop];
            _player.delegate = nil;
        }
        _player   = nil;
        _delegate = nil;
    }

    int64_t positionMs() const override {
        return _player ? int64_t(_player.currentTime * 1000.0) : 0;
    }
    int64_t durationMs() const override { return _player ? int64_t(_player.duration * 1000.0) : 0; }

private:
    void fail(const Failure &f) {
        if (onFailed)
            onFailed(f);
    }
    template <class F>
    void post(uint64_t gen, F fn) {
        _app.post([this, alive = _alive, gen, fn = std::move(fn)] {
            if (*alive && gen == _generation)
                fn(*this);
        });
    }

    App                     &_app;
    std::shared_ptr<bool>    _alive      = std::make_shared<bool>(true);
    AVAudioPlayer           *_player     = nil;
    PlatAudioPlayerDelegate *_delegate   = nil;
    uint64_t                 _generation = 0;
};

// ── Capture ─────────────────────────────────────────────────────────────────

// Core Audio's WAV writer may add chunks (FLLR padding) before "data"; hand
// out the same plain 44-byte-header file as the other platforms, rewritten
// in place (the audio moves down over the extra chunks, no second copy).
std::string normalizedWav(std::string file) {
    auto le32 = [&](size_t at) {
        const auto *p = reinterpret_cast<const unsigned char *>(file.data() + at);
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    if (file.size() < 12 || file.compare(0, 4, "RIFF") != 0 || file.compare(8, 4, "WAVE") != 0)
        return file;
    size_t pos = 12;
    while (pos + 8 <= file.size()) {
        const uint32_t len = le32(pos + 4);
        if (file.compare(pos, 4, "data") == 0) {
            const size_t n = std::min<size_t>(len, file.size() - pos - 8) & ~size_t(1);
            if (pos + 8 < kWavHeader) // no room for the header: not a file we wrote
                return wavFromPcm16(
                    std::string_view(file).substr(pos + 8, n),
                    Recorder::kSampleRate,
                    Recorder::kChannels
                );
            file.resize(pos + 8 + n);
            file.erase(0, pos + 8 - kWavHeader);
            wavInPlace(file, Recorder::kSampleRate, Recorder::kChannels);
            return file;
        }
        pos += 8 + size_t(len) + (len & 1);
    }
    return file;
}

class MacRecorder final : public Recorder {
public:
    explicit MacRecorder(App &app) : _app(app) {}
    ~MacRecorder() override {
        *_alive = false;
        cancel();
    }

    void start() override {
        if (_recording)
            return;
        _recording         = true;
        const uint64_t gen = ++_generation;
        if (nullAudio()) {
            post(gen, [](MacRecorder &r) { r.deny({Error::NoMicrophone, {}}); });
            return;
        }
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized:
            // Posted, so onStarted never fires inside the caller's start().
            post(gen, [](MacRecorder &r) { r.begin(); });
            return;
        case AVAuthorizationStatusNotDetermined: {
            // Shows the system prompt; the answer arrives on an arbitrary queue.
            App                  *app   = &_app;
            std::shared_ptr<bool> alive = _alive;
            MacRecorder          *self  = this;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                                       app->post([self, alive, gen, granted] {
                                           if (!*alive || gen != self->_generation)
                                               return;
                                           if (granted)
                                               self->begin();
                                           else
                                               self->deny({Error::MicrophoneDenied, {}});
                                       });
                                     }];
            return;
        }
        default: // denied, restricted
            post(gen, [](MacRecorder &r) { r.deny({Error::MicrophoneDenied, {}}); });
            return;
        }
    }

    void stop() override {
        if (!_recording)
            return;
        const uint64_t gen = ++_generation;
        if (!_rec) { // still waiting for the permission answer
            _recording = false;
            post(gen, [](MacRecorder &r) {
                if (r.onFinished)
                    r.onFinished(wavFromPcm16({}, kSampleRate, kChannels));
            });
            return;
        }
        [_rec stop]; // finalizes the file synchronously
        std::string file;
        bool        ok = false;
        if (FILE *f = std::fopen(_path.c_str(), "rb")) {
            // Sized up front, read straight into place.
            struct stat st{};
            if (::fstat(::fileno(f), &st) == 0 && st.st_size > 0)
                file.resize(size_t(st.st_size));
            file.resize(std::fread(file.data(), 1, file.size(), f));
            ok = !std::ferror(f);
            std::fclose(f);
        }
        teardown();
        if (ok)
            post(gen, [wav = normalizedWav(std::move(file))](MacRecorder &r) mutable {
                if (r.onFinished)
                    r.onFinished(std::move(wav));
            });
        else
            post(gen, [](MacRecorder &r) { r.deny({Error::ReadFailed, {}}); });
    }

    void cancel() override {
        ++_generation;
        if (_rec)
            [_rec stop];
        teardown();
    }

    bool isRecording() const override { return _recording; }

private:
    template <class F>
    void post(uint64_t gen, F fn) {
        _app.post([this, alive = _alive, gen, fn = std::move(fn)]() mutable {
            if (*alive && gen == _generation)
                fn(*this);
        });
    }

    void deny(const Failure &f) {
        teardown();
        if (onFailed)
            onFailed(f);
    }

    void begin() {
        static int counter = 0;
        char       name[96];
        std::snprintf(name, sizeof name, "msga-voice-%d-%d.wav", int(::getpid()), ++counter);
        _path = utf8(NSTemporaryDirectory()) + "/" + name;
        std::remove(_path.c_str());
        NSDictionary *settings = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVSampleRateKey : @(double(kSampleRate)),
            AVNumberOfChannelsKey : @(kChannels),
            AVLinearPCMBitDepthKey : @16,
            AVLinearPCMIsFloatKey : @NO,
            AVLinearPCMIsBigEndianKey : @NO,
            AVLinearPCMIsNonInterleaved : @NO,
        };
        NSError *err = nil;
        _rec         = [[AVAudioRecorder alloc] initWithURL:[NSURL fileURLWithPath:nsString(_path)]
                                                   settings:settings
                                                      error:&err];
        if (!_rec) {
            deny(
                err ? Failure{Error::Os, utf8(err.localizedDescription)}
                    : Failure{Error::MicrophoneOpen, {}}
            );
            return;
        }
        _delegate                   = [[PlatAudioRecorderDelegate alloc] init];
        App                  *app   = &_app;
        std::shared_ptr<bool> alive = _alive;
        MacRecorder          *self  = this;
        const uint64_t        gen   = _generation;
        _delegate.failed            = ^(NSString *error) {
          std::string text = utf8(error);
          app->post([self, alive, gen, text] {
              if (!*alive || gen != self->_generation)
                  return;
              self->cancel();
              if (self->onFailed)
                  self->onFailed(
                      text.empty() ? Failure{Error::CaptureFailed, {}} : Failure{Error::Os, text}
                  );
          });
        };
        _rec.delegate        = _delegate;
        _rec.meteringEnabled = YES;
        if (![_rec prepareToRecord] || ![_rec record]) {
            deny({Error::StartFailed, {}});
            return;
        }
        _meter = _app.addTimer(50, true, [this] {
            if (!_rec)
                return;
            [_rec updateMeters];
            const float db = [_rec peakPowerForChannel:0]; // -160 (silence) .. 0 dBFS
            if (onLevel)
                onLevel(std::clamp(float(std::pow(10.0, db / 20.0)), 0.0f, 1.0f));
        });
        if (onStarted)
            onStarted();
    }

    // Releases the recorder and deletes the temp file; reports nothing.
    void teardown() {
        if (_meter)
            _app.cancelTimer(_meter);
        _meter     = 0;
        _recording = false;
        if (_rec)
            _rec.delegate = nil;
        _rec      = nil;
        _delegate = nil;
        if (!_path.empty())
            std::remove(_path.c_str());
        _path.clear();
    }

    App                       &_app;
    std::shared_ptr<bool>      _alive    = std::make_shared<bool>(true);
    AVAudioRecorder           *_rec      = nil;
    PlatAudioRecorderDelegate *_delegate = nil;
    std::string                _path;
    TimerId                    _meter      = 0;
    uint64_t                   _generation = 0;
    bool                       _recording  = false;
};

// ── Notification sounds ─────────────────────────────────────────────────────

NSSound *g_current = nil; // the playing sound: a released NSSound stops at once

bool startPlaying(NSSound *sound) {
    if (!sound)
        return false;
    [g_current stop];
    g_current = sound;
    return [g_current play];
}

} // namespace

std::unique_ptr<Player> Player::create(App &app) {
    return std::make_unique<MacPlayer>(app);
}

bool canPlayExtension(std::string_view ext) {
    constexpr const char *k[] = {
        "mp3", "m4a", "mp4", "aac", "wav", "aif", "aiff", "flac", "caf", "alac"
    };
    for (const char *e : k)
        if (ext == e)
            return true;
    return false;
}

std::unique_ptr<Recorder> createNativeRecorder(App &app) {
    return std::make_unique<MacRecorder>(app);
}

std::vector<SystemSound> systemSounds() {
    std::vector<SystemSound> out;
    const char              *home   = std::getenv("HOME");
    const std::string        dirs[] = {
        "/System/Library/Sounds",
        "/Library/Sounds",
        std::string(home ? home : "") + "/Library/Sounds"
    };
    for (const std::string &dir : dirs) {
        std::vector<std::string> names;
        if (DIR *d = ::opendir(dir.c_str())) {
            while (const dirent *e = ::readdir(d)) {
                const std::string f   = e->d_name;
                const size_t      dot = f.rfind('.');
                if (dot == std::string::npos || dot == 0 || e->d_type == DT_DIR)
                    continue;
                const std::string ext = f.substr(dot);
                if (ext == ".aiff" || ext == ".aif" || ext == ".wav")
                    names.push_back(f);
            }
            ::closedir(d);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &f : names) {
            // System Settings shows the bare name; +soundNamed: resolves it.
            const std::string name = f.substr(0, f.rfind('.'));
            out.push_back({name, name});
        }
    }
    return out;
}

void playSound(const std::string &name, const std::string &fallbackFile) {
    if (nullAudio())
        return;
    if (!name.empty() && startPlaying([NSSound soundNamed:nsString(name)]))
        return;
    if (!fallbackFile.empty())
        startPlaying([[NSSound alloc] initWithContentsOfFile:nsString(fallbackFile)
                                                 byReference:YES]);
}

} // namespace plat::audio
