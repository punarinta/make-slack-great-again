#!/bin/sh
# Rebuild src/third_party/stb/stb_image.h from a pristine upstream copy, cut
# down to the code msga can ever use (PNG, JPEG, GIF from memory, 8-bit
# RGBA out). Then re-apply the two "msga patch" GIF fixes (README there).
#
#   src/tools/stb_trim.sh path/to/upstream/stb_image.h
#
# Needs unifdef. Steps: join backslash-continued #if lines (unifdef cannot
# parse them), resolve every format/SIMD/stdio/float switch, then drop the
# public functions we never call (tools/stb_prune.py).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: stb_trim.sh upstream/stb_image.h}
out=$here/../third_party/stb/stb_image.h
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 - "$src" "$tmp/joined.h" <<'EOF'
import sys
lines = open(sys.argv[1]).read().split('\n')
out, i = [], 0
while i < len(lines):
    l = lines[i]
    if l.lstrip().startswith('#'):
        while l.endswith('\\') and i + 1 < len(lines):
            i += 1
            l = l[:-1].rstrip() + ' ' + lines[i].strip()
    out.append(l)
    i += 1
open(sys.argv[2], 'w').write('\n'.join(out))
EOF

# -x 2: exit 0 whether or not anything changed; 2 only on trouble.
unifdef -k -x 2 \
    -DSTBI_ONLY_PNG -DSTBI_ONLY_JPEG -DSTBI_ONLY_GIF \
    -USTBI_ONLY_BMP -USTBI_ONLY_PSD -USTBI_ONLY_TGA -USTBI_ONLY_HDR -USTBI_ONLY_PIC -USTBI_ONLY_PNM \
    -USTBI_ONLY_ZLIB \
    -DSTBI_NO_BMP -DSTBI_NO_PSD -DSTBI_NO_TGA -DSTBI_NO_HDR -DSTBI_NO_PIC -DSTBI_NO_PNM \
    -USTBI_NO_PNG -USTBI_NO_JPEG -USTBI_NO_GIF -USTBI_NO_ZLIB \
    -DSTBI_NO_STDIO -DSTBI_NO_LINEAR -DSTBI_NO_SIMD -USTBI_SSE2 -USTBI_NEON \
    -DSTBI_NO_FAILURE_STRINGS -USTBI_FAILURE_USERMSG -USTBI_WINDOWS_UTF8 \
    -o "$tmp/min.h" "$tmp/joined.h"

python3 "$here/stb_prune.py" "$tmp/min.h" "$out"
echo "wrote $out ($(wc -l < "$out") lines) — now re-apply the msga patches (see README)"
