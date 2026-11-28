// Between audio_common.cpp and the per-OS audio backends.
#pragma once

#include "plat/audio.h"

namespace plat::audio {

// Defined by exactly one of audio_{linux,win32,cocoa}.
std::unique_ptr<Recorder> createNativeRecorder(App &app);

// A take collected with kWavHeader free bytes at its front becomes the WAV
// file in place, with no second copy of the audio: wavInPlace() fills those
// bytes with the header for the s16le PCM that follows them.
constexpr size_t kWavHeader = 44;
void             wavInPlace(std::string &buf, int sampleRate, int channels);

// Peak |sample| of `bytes` bytes of s16le PCM, 0..1.
float peakOf(const char *pcm, size_t bytes);

} // namespace plat::audio
