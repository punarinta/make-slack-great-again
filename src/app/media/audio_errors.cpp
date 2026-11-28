#include "app/media/audio_errors.h"

#include "base/i18n.h"
#include "base/str.h"

using i18n::tr;

namespace media {

std::string audioErrorText(const plat::audio::Failure &f) {
    using E             = plat::audio::Error;
    const char *summary = nullptr;
    switch (f.code) {
    case E::Os:
        if (!f.detail.empty())
            return f.detail;
        summary = tr("Audio playback failed");
        break;
    case E::Unavailable:
        summary = tr("Audio playback is unavailable");
        break;
    case E::Unsupported:
        summary = tr("This audio format can't be played here");
        break;
    case E::NeedsFfmpeg:
        summary = tr("This audio format needs ffmpeg installed to play");
        break;
    case E::NoOutput:
        summary = tr("No audio output found (needs pw-cat, paplay or aplay)");
        break;
    case E::OutputFailed:
        summary = tr("Audio output failed");
        break;
    case E::PlaybackFailed:
        summary = tr("Audio playback failed");
        break;
    case E::NoCaptureTool:
        summary =
            tr("No audio capture tool found (install PipeWire, PulseAudio or ALSA utilities)");
        break;
    case E::NoMicrophone:
        summary = tr("No microphone found");
        break;
    case E::MicrophoneBusy:
        summary = tr("The microphone is in use by another app");
        break;
    case E::MicrophoneDenied:
        summary =
            tr("Microphone access is blocked. Allow msga in System Settings \xE2\x86\x92 Privacy & "
               "Security \xE2\x86\x92 Microphone.");
        break;
    case E::MicrophoneOpen:
#ifdef _WIN32
        // The OS's own text goes in parentheses.
        summary =
            tr("Couldn't open the microphone. Check that microphone access is allowed in Windows "
               "Settings \xE2\x86\x92 Privacy & security \xE2\x86\x92 Microphone.");
        return f.detail.empty() ? std::string(summary)
                                : str::concat({summary, " (", f.detail, ")"});
#else
        summary = tr("Couldn't open the microphone");
        break;
#endif
    case E::StartFailed:
        summary = tr("Couldn't start recording from the microphone");
        break;
    case E::NoAudio:
        summary = tr("The microphone isn't delivering any audio");
        break;
    case E::Stopped:
        summary = tr("Recording stopped unexpectedly");
        break;
    case E::ReadFailed:
        summary = tr("Couldn't read the recording");
        break;
    case E::CaptureFailed:
        summary = tr("Recording from the microphone failed");
        break;
    }
    // A capture helper's last stderr line follows.
    const bool withDetail =
        f.code == E::NoAudio || f.code == E::StartFailed || f.code == E::Stopped;
    return f.detail.empty() || !withDetail ? std::string(summary)
                                           : str::concat({summary, ": ", f.detail});
}

} // namespace media
