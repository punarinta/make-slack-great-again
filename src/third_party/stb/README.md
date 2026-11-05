# stb_image

`stb_image.h` v2.30 from https://github.com/nothings/stb (public domain or
MIT, see the end of the header). `stb_config.h` picks the subset (PNG, JPEG,
GIF; no stdio, HDR or float paths) and `stb_image.c` is the one translation
unit that compiles it. Only `src/gfx/image.cpp` includes it; no stb types
leave that file, so Windows/macOS can swap in WIC/ImageIO later.

**Trimmed source.** The file is not upstream as-is: `src/tools/stb_trim.sh`
removed every format we never decode (BMP, PSD, TGA, HDR, PIC, PNM), the
SSE2/NEON, stdio and float paths, and the public functions we never call
(16-bit and callback loaders, the zlib API except the one PNG uses, flip /
unpremultiply / iPhone-PNG switches, HDR stubs, failure strings). 7990 → 4723
lines; decoding output is byte-identical to the untrimmed subset on PNG
(8/16-bit, interlaced), baseline + progressive JPEG and animated GIF. To update
stb: run the script on the new upstream header, then re-apply the patches below.

Two local patches in the animated-GIF path, marked `msga patch`, and one
rule of our own frame loop (`msga_gif_frames` in `stb_image.c`):

- `stbi__load_gif_main`: `two_back` pointed *before* the output buffer
  (`out - 2 * stride`), so any GIF using disposal method 3 ("restore to
  previous") read out of bounds and crashed. It now points at the frame
  before the previous one.
- `stbi__gif_load_next`: disposal method 2 ("restore to background") put back
  what was under the previous frame's pixels — that is method 3. It now clears
  the previous frame's rectangle to transparent, as browsers do.
- `msga_gif_frames` passes `g.background` as the frame to restore for
  disposal method 3: the canvas as it was before the previous frame was
  drawn. stb's own loop passes the frame two back, which brings back a frame
  that was itself to be undone when two method-3 frames follow each other.
