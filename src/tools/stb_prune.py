"""Remove stb_image public functions msga never calls (declarations and
definitions). Internals stay: they're still reached from the four entry
points we use (stbi_load_from_memory, stbi_load_gif_from_memory,
stbi_info_from_memory, stbi_image_free)."""
import re, sys

src, dst = sys.argv[1], sys.argv[2]
REMOVE = [
    'stbi_load_from_callbacks', 'stbi_load_16_from_memory', 'stbi_load_16_from_callbacks',
    'stbi_is_hdr_from_callbacks', 'stbi_is_hdr_from_memory', 'stbi_hdr_to_ldr_gamma',
    'stbi_hdr_to_ldr_scale', 'stbi_ldr_to_hdr_gamma', 'stbi_ldr_to_hdr_scale',
    'stbi_info_from_callbacks', 'stbi_is_16_bit_from_memory', 'stbi_is_16_bit_from_callbacks',
    'stbi_set_unpremultiply_on_load', 'stbi_set_unpremultiply_on_load_thread',
    'stbi_convert_iphone_png_to_rgb', 'stbi_convert_iphone_png_to_rgb_thread',
    'stbi_set_flip_vertically_on_load', 'stbi_set_flip_vertically_on_load_thread',
    'stbi_zlib_decode_malloc_guesssize',
    'stbi_zlib_decode_malloc', 'stbi_zlib_decode_buffer', 'stbi_zlib_decode_noheader_malloc',
    'stbi_zlib_decode_noheader_buffer', 'stbi_failure_reason',
]
lines = open(src).read().split('\n')
out, i, removed = [], 0, {n: 0 for n in REMOVE}
name_re = re.compile(r'^\s*(?:STBIDEF|extern|static)?[\w\s\*]*?\b(' + '|'.join(map(re.escape, REMOVE)) + r')\s*\(')
while i < len(lines):
    l = lines[i]
    m = name_re.match(l) if not l.lstrip().startswith(('//', '#', '*')) else None
    if m and 'STBIDEF' in l:
        name = m.group(1)
        # collect until ';' (declaration) or a full brace-balanced body
        j, text = i, l
        while ';' not in text and '{' not in text and j + 1 < len(lines):
            j += 1
            text += lines[j]
        if '{' in text:
            depth = text.count('{') - text.count('}')
            while depth > 0 and j + 1 < len(lines):
                j += 1
                depth += lines[j].count('{') - lines[j].count('}')
        removed[name] += 1
        i = j + 1
        continue
    out.append(l)
    i += 1
open(dst, 'w').write('\n'.join(out))
for n, c in removed.items():
    print(f"{c}  {n}")
