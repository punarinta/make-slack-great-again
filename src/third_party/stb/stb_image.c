// stb_image implementation, compiled once (-Os, see ../../gfx/CMakeLists.txt).
#include "stb_config.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// msga: frame-by-frame GIF decoding (declared in stb_config.h).
// stbi_load_gif_from_memory keeps every composited frame in one growing
// buffer (frames x w x h x 4); this hands each frame to `fn` as soon as it is
// composited and keeps no frame of its own.
// Disposal method 3 ("restore to previous") puts back the canvas as it was
// before the previous frame was drawn: g.background, which
// stbi__gif_load_next fills with the disposed canvas just before drawing
// each frame. stbi__load_gif_main passes the frame two back instead, which
// is wrong after a method-3 frame (two in a row kept the first one's pixels).
int msga_gif_frames(
    const unsigned char *buffer,
    int                  len,
    int (*fn)(void *ctx, const unsigned char *rgba, int w, int h, int delayMs),
    void *ctx
) {
   stbi__context s;
   stbi__gif     g;
   int           comp = 0, layers = 0;
   stbi__start_mem(&s, buffer, len);
   if (!stbi__gif_test(&s))
      return 0;
   memset(&g, 0, sizeof(g));
   for (;;) {
      // Null before the first frame: nothing to restore, as stb expects.
      stbi_uc *u = stbi__gif_load_next(&s, &g, &comp, 4, g.background);
      if (u == (stbi_uc *)&s)
         u = 0; // the end of the animation
      if (!u)
         break;
      ++layers;
      if (!fn(ctx, u, g.w, g.h, g.delay))
         break;
   }
   STBI_FREE(g.out);
   STBI_FREE(g.history);
   STBI_FREE(g.background);
   return layers;
}
