/* Single translation unit for the vendored miniaudio decoder (public domain /
 * MIT-0, see LICENSE). Decoding only: MP3 (dr_mp3), WAV (dr_wav) and Ogg
 * Vorbis (stb_vorbis). FLAC (dr_flac, 46 KB) is compiled out: Slack never
 * sends it, and a shared .flac file still plays through ffmpeg. Device I/O
 * is compiled out — the static Linux release binary can't dlopen a sound
 * server, so playback goes through a helper process instead
 * (src/plat/src/audio/audio_linux.cpp). */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_FLAC
#include "miniaudio.h"

/* stb_vorbis implementation, after miniaudio so ma_dr_* symbols don't clash. */
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include "pcm_decoder.h"

/* The embedded decoders straight, not ma_decoder: every one of them reads s16
 * at the file's own rate and channel count, so miniaudio's data converter,
 * resampler and channel mapper (and the ma_decoder layer) stay out of the
 * binary. Tried in this order; MP3 last since its frame sync scan is the
 * least strict about what it accepts. */
enum { DEC_WAV, DEC_VORBIS, DEC_MP3 };

struct pcm_decoder {
    int kind;
    int channels;
    union {
        ma_dr_wav   wav;
        stb_vorbis *vorbis;
        ma_dr_mp3   mp3;
    } u;
};

pcm_decoder *pcm_decoder_open(const char *path, int *rate, int *channels, long long *total_frames) {
    pcm_decoder *d = (pcm_decoder *)ma_malloc(sizeof(pcm_decoder), NULL);
    int          err = 0;
    if (!d)
        return NULL;
    if (ma_dr_wav_init_file(&d->u.wav, path, NULL)) {
        d->kind       = DEC_WAV;
        *rate         = (int)d->u.wav.sampleRate;
        *channels     = (int)d->u.wav.channels;
        *total_frames = (long long)d->u.wav.totalPCMFrameCount;
    } else if ((d->u.vorbis = stb_vorbis_open_filename(path, &err, NULL)) != NULL) {
        stb_vorbis_info info = stb_vorbis_get_info(d->u.vorbis);
        d->kind       = DEC_VORBIS;
        *rate         = (int)info.sample_rate;
        *channels     = info.channels;
        *total_frames = (long long)stb_vorbis_stream_length_in_samples(d->u.vorbis);
    } else if (ma_dr_mp3_init_file(&d->u.mp3, path, NULL)) {
        d->kind       = DEC_MP3;
        *rate         = (int)d->u.mp3.sampleRate;
        *channels     = (int)d->u.mp3.channels;
        *total_frames = (long long)ma_dr_mp3_get_pcm_frame_count(&d->u.mp3);
    } else {
        ma_free(d, NULL);
        return NULL;
    }
    d->channels = *channels;
    if (*rate <= 0 || *channels <= 0) {
        pcm_decoder_close(d);
        return NULL;
    }
    return d;
}

long long pcm_decoder_read(pcm_decoder *d, void *buf, long long frames) {
    switch (d->kind) {
    case DEC_WAV:
        return (long long)ma_dr_wav_read_pcm_frames_s16(&d->u.wav, (ma_uint64)frames, (ma_int16 *)buf);
    case DEC_VORBIS: {
        /* Frames per call are bounded by the caller's 50 ms chunks. */
        int n = stb_vorbis_get_samples_short_interleaved(
            d->u.vorbis, d->channels, (short *)buf, (int)(frames * d->channels));
        return n > 0 ? n : 0;
    }
    default:
        return (long long)ma_dr_mp3_read_pcm_frames_s16(&d->u.mp3, (ma_uint64)frames, (ma_int16 *)buf);
    }
}

void pcm_decoder_seek(pcm_decoder *d, long long frame) {
    ma_uint64 f = (ma_uint64)(frame < 0 ? 0 : frame);
    switch (d->kind) {
    case DEC_WAV:
        ma_dr_wav_seek_to_pcm_frame(&d->u.wav, f);
        break;
    case DEC_VORBIS:
        stb_vorbis_seek(d->u.vorbis, (unsigned int)f);
        break;
    default:
        ma_dr_mp3_seek_to_pcm_frame(&d->u.mp3, f);
        break;
    }
}

void pcm_decoder_close(pcm_decoder *d) {
    if (!d)
        return;
    switch (d->kind) {
    case DEC_WAV:
        ma_dr_wav_uninit(&d->u.wav);
        break;
    case DEC_VORBIS:
        stb_vorbis_close(d->u.vorbis);
        break;
    default:
        ma_dr_mp3_uninit(&d->u.mp3);
        break;
    }
    ma_free(d, NULL);
}
