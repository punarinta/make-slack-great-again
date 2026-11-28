// The stb_image subset msga-next uses; shared by the implementation file and
// every includer so declarations and definitions agree.
#pragma once
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS // callers only need success/failure
#define STBI_NO_SIMD            // the SSE2 IDCT costs code for speed chat images don't need
#define STBI_MAX_DIMENSIONS 16384
#define STBI_ASSERT(x) ((void)0)

#ifdef __cplusplus
extern "C" {
#endif
// msga (stb_image.c): decodes a GIF one composited frame at a time, handing
// each to fn (straight RGBA, canvas-sized, delay in ms) before the next is
// decoded; fn returns 0 to stop. Holds about three canvases, never the whole
// animation. Returns the number of frames handed out (0: not a GIF / none).
int msga_gif_frames(
    const unsigned char *buffer,
    int                  len,
    int (*fn)(void *ctx, const unsigned char *rgba, int w, int h, int delayMs),
    void *ctx
);
#ifdef __cplusplus
}
#endif
