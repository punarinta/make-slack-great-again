// plat audio: playing a local file (inline audio clips), recording the
// microphone (voice input) and short notification sounds. No toolkit audio
// stack and no new shared libraries; each OS uses its own:
//
//   playback   Linux   in-process decode (vendored miniaudio: MP3/WAV/
//                      Vorbis; `ffmpeg` on PATH for the rest, e.g. Slack's
//                      AAC clips) piped as raw PCM into the sound server's CLI
//                      (pw-cat / paplay / aplay). The static release binary
//                      cannot load a sound server's shared library.
//              macOS   AVAudioPlayer (AVFoundation)
//              Windows MFPlay (Media Foundation)
//   capture    Linux   pw-record / parecord / arecord writing raw PCM to a pipe
//              macOS   AVAudioRecorder (needs NSMicrophoneUsageDescription and,
//                      signed with the hardened runtime, the audio-input
//                      entitlement)
//              Windows waveIn (winmm)
//   sounds     Linux   canberra-gtk-play / pw-play / paplay / ffplay / aplay,
//                      system sounds from the freedesktop sound theme
//              macOS   NSSound, system sounds from the Sounds folders
//              Windows PlaySound, system sounds from the AppEvents scheme
//
// Threading: like the rest of plat, every call is made on the App's thread
// and every callback runs there (posted, never from inside the call that
// caused it). systemSounds() is the one blocking call: run it off the UI
// thread. Objects must be destroyed before their App.
//
// Errors are codes, not text: the app words them (and translates them).
// `detail` carries what the OS or the helper program said, when anything.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {

class App;

namespace audio {

enum class Error : uint8_t {
    Unavailable,      // no playback engine here ("Audio playback is unavailable")
    Unsupported,      // the file can't be decoded here
    NeedsFfmpeg,      // Linux: this format plays only with ffmpeg on PATH
    NoOutput,         // Linux: none of pw-cat / paplay / aplay could be started
    OutputFailed,     // Linux: the output helper died while playing
    PlaybackFailed,   // the OS player reported an error mid-playback
    NoCaptureTool,    // Linux: none of pw-record / parecord / arecord exists
    NoMicrophone,     // no capture device
    MicrophoneBusy,   // the device is in use by another app
    MicrophoneDenied, // the OS privacy setting blocks us (macOS)
    MicrophoneOpen,   // the device could not be opened (Windows: check privacy settings)
    StartFailed,      // capture could not start
    NoAudio,          // the device opened but delivers nothing
    Stopped,          // capture ended on its own after audio had arrived
    ReadFailed,       // the finished recording could not be read back
    CaptureFailed,    // the OS recorder failed mid-recording
    Os,               // `detail` is the OS's own complete message (macOS)
};

struct Failure {
    Error       code = Error::Unavailable;
    std::string detail; // OS error text or the helper's last stderr line; may be empty
};

// ── Playback ────────────────────────────────────────────────────────────────
// One local file at a time, with pause, seek and position.
class Player {
public:
    static std::unique_ptr<Player> create(App &app);
    virtual ~Player() = default;

    // Starts loading `path`, replacing any current media: onLoaded once
    // play() may be called (durationMs() known where the backend can tell),
    // or onFailed.
    virtual void    load(const std::string &path) = 0;
    virtual void    play()                        = 0; // start, or resume after pause()
    virtual void    pause()                       = 0;
    virtual void    seek(int64_t ms)              = 0;
    virtual void    stop()                        = 0; // release the media
    virtual int64_t positionMs() const            = 0;
    virtual int64_t durationMs() const            = 0; // 0 = unknown

    std::function<void()>                onLoaded;
    std::function<void()>                onEnded; // played to the end
    std::function<void(const Failure &)> onFailed;
};

// Whether this platform's player can decode files with this lower-case
// extension ("mp3", "m4a", …); on Linux it depends on ffmpeg being on PATH.
bool canPlayExtension(std::string_view ext);

// ── Capture ─────────────────────────────────────────────────────────────────
// The microphone as 16 kHz mono signed 16-bit PCM (what speech-to-text
// wants; nothing to encode). One recording at a time.
class Recorder {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kChannels   = 1;

    // Test builds (PLAT_TEST_HOOKS): when $MSGA_VOICE_FAKE_WAV names a
    // readable file, a fake recorder that "records" that file is returned.
    static std::unique_ptr<Recorder> create(App &app);
    virtual ~Recorder() = default;

    // onStarted once audio flows, or onFailed (no device, no permission, no
    // capture helper).
    virtual void start()             = 0;
    // Ends the recording: onFinished with everything captured so far.
    virtual void stop()              = 0;
    // Drops the recording; no callback follows.
    virtual void cancel()            = 0;
    virtual bool isRecording() const = 0;

    std::function<void()>                onStarted;
    std::function<void(float peak)>      onLevel;    // latest peak 0..1, about 20 times a second
    std::function<void(std::string wav)> onFinished; // a RIFF/WAVE file, 16 kHz mono s16le
    std::function<void(const Failure &)> onFailed;
};

// Raw little-endian s16 PCM wrapped in a 44-byte RIFF/WAVE header.
std::string wavFromPcm16(std::string_view pcm, int sampleRate, int channels);

// ── Notification sounds ─────────────────────────────────────────────────────
struct SystemSound {
    std::string name;  // the OS's id: a theme event, an AppEvents event, a sound name
    std::string label; // what the OS's own sound settings call it
};

// The OS's selectable sounds (may be empty). BLOCKING: Linux asks gsettings
// for the sound theme, Windows reads the registry. Call off the UI thread.
std::vector<SystemSound> systemSounds();

// Fire and forget: the system sound `name` when it is non-empty and plays,
// else the sound file `fallbackFile` (a WAV every backend can play). Returns
// at once; on Linux the lookup and the helper run on a short-lived thread.
void playSound(const std::string &name, const std::string &fallbackFile);

} // namespace audio
} // namespace plat
