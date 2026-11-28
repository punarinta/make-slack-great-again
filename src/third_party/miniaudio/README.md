# miniaudio (decoders only)

`miniaudio.h` v0.11.25 from https://github.com/mackron/miniaudio and
`stb_vorbis.c` (public domain / MIT-0 and public domain / MIT, see `LICENSE`),
the same files the Qt app vendored. Linux builds only: plat's audio player
(`src/plat/src/audio/audio_linux.cpp`) decodes MP3, WAV and Ogg Vorbis
with them in-process and pipes the PCM to the sound server's CLI helper.

`miniaudio_impl.c` is the one translation unit: device I/O, threading,
encoding, generation, the resource manager, node graph and engine are compiled
out (the static binary can't dlopen a sound server anyway). It also defines
the four-function C API in `pcm_decoder.h`, so no C++ file parses miniaudio.h
or has to agree with its config macros. Unmodified upstream otherwise.
