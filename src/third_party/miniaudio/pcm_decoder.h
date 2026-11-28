/* The slice of miniaudio plat's Linux audio player uses: decode a file to
 * interleaved s16 PCM at its own rate and channel count. A plain C API so
 * no C++ file has to parse miniaudio.h (or agree with its config macros). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcm_decoder pcm_decoder;

/* Null when the file is not MP3, WAV or Ogg Vorbis (or unreadable).
 * *total_frames is 0 when the length is unknown. */
pcm_decoder *pcm_decoder_open(const char *path, int *rate, int *channels, long long *total_frames);
/* Frames read into buf (channels * 2 bytes each); 0 at the end or on error. */
long long    pcm_decoder_read(pcm_decoder *d, void *buf, long long frames);
void         pcm_decoder_seek(pcm_decoder *d, long long frame);
void         pcm_decoder_close(pcm_decoder *d);

#ifdef __cplusplus
}
#endif
