#!/usr/bin/env python3
"""Compile the UI icon SVGs into compact path data for src/gfx.

    tools/icons.py <svg dir> <out dir> [Name=file.svg ...]   (CMake target `icons`)

Extra Name=file.svg icons (e.g. the tray plane, gfx/icon_tray.svg) are
appended after the directory's, monochrome: their literal colours become
currentColor, so drawIcon's tint paints them. Name=color:file.svg keeps the
file's colours instead (the app logo, gfx/icon.svg).

Writes <out dir>/icons_generated.h (enum class Icon, kIconCount) and
icons_generated.cpp (one byte blob + offsets). The binary carries no SVG
renderer: arcs, rects, lines and polylines become M/L/C/Z and circle ops here,
and gfx::drawIcon fills or strokes them (round caps and joins) at runtime.

Supported SVG subset — everything the icons in gfx/ui use, plus a little:
  <svg viewBox>, <g>, <path d> (all commands incl. arcs), <circle>, <ellipse>,
  <rect x y width height rx ry>, <line>, <polyline>, <polygon>;
  fill / stroke (none | currentColor | #rgb | #rrggbb), stroke-width,
  transform (matrix/translate/scale/rotate). Anything else is reported.

Blob format, per icon (offsets[i] .. offsets[i+1]):
  varint viewBox width, height (in 1/UNIT viewBox units)
  items: flags byte (1 fill, 2 stroke, 4 literal colour)
         [varint stroke width] [4 bytes ARGB, big-endian]
         ops until End: byte = op | (repeat - 1) << 3, op in
           0 M x y, 1 L x y, 2 C x1 y1 x2 y2 x y, 3 Z, 4 circle cx cy r, 5 End
         coordinates are zigzag varint deltas from the previous x (resp. y)
         in the icon; circle r is a plain varint.
"""
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

UNIT = 32  # quantisation: 1/32 viewBox unit = 1/64 px at 48 physical px per 24 units

OP_M, OP_L, OP_C, OP_Z, OP_CIRCLE, OP_END = range(6)
SVG_NS = '{http://www.w3.org/2000/svg}'

warnings = []


def warn(name, msg):
    warnings.append(f'{name}: {msg}')


# ── path data ───────────────────────────────────────────────────────────────
NUM = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def tokenize(d):
    i, out = 0, []
    while i < len(d):
        c = d[i]
        if c.isalpha():
            out.append(c)
            i += 1
        elif c in ' ,\t\r\n':
            i += 1
        else:
            m = NUM.match(d, i)
            if not m:
                raise ValueError(f'bad path data at {d[i:i + 10]!r}')
            out.append(float(m.group()))
            i = m.end()
    return out


def arc_to_cubics(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    """SVG endpoint arc → list of cubic control tuples (SVG spec F.6)."""
    if rx == 0 or ry == 0 or (x1 == x2 and y1 == y2):
        return [(x1, y1, x2, y2, x2, y2)] if (x1, y1) != (x2, y2) else []
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if fa == fs:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0:
        dt -= 2 * math.pi
    elif fs and dt < 0:
        dt += 2 * math.pi
    n = max(1, math.ceil(abs(dt) / (math.pi / 2) - 1e-9))
    step = dt / n
    k = 4 / 3 * math.tan(step / 4)
    out = []

    def pt(t):
        x, y = rx * math.cos(t), ry * math.sin(t)
        return cp * x - sp * y + cx, sp * x + cp * y + cy

    def deriv(t):
        x, y = -rx * math.sin(t), ry * math.cos(t)
        return cp * x - sp * y, sp * x + cp * y

    t = t1
    for i in range(n):
        ta, tb = t, t + step
        ax, ay = pt(ta)
        bx, by = pt(tb)
        dax, day = deriv(ta)
        dbx, dby = deriv(tb)
        if i == n - 1:
            bx, by = x2, y2
        out.append((ax + k * dax, ay + k * day, bx - k * dbx, by - k * dby, bx, by))
        t = tb
    return out


def parse_path(d):
    """→ list of ops: ('M',x,y) ('L',x,y) ('C',x1,y1,x2,y2,x,y) ('Z',)"""
    toks = tokenize(d)
    ops = []
    i = 0
    cx = cy = sx = sy = 0.0
    cmd = None
    last_ctrl = None  # (x, y, kind) for S/T reflection

    def nums(n):
        nonlocal i
        vals = toks[i:i + n]
        if len(vals) < n or any(isinstance(v, str) for v in vals):
            raise ValueError('path data truncated')
        i += n
        return vals

    def flag():
        # Arc flags may be packed without separators ("a1 1 0 01 1 2 2").
        nonlocal i
        v = toks[i]
        if isinstance(v, float) and v in (0.0, 1.0):
            i += 1
            return int(v)
        raise ValueError('bad arc flag')

    while i < len(toks):
        t = toks[i]
        if isinstance(t, str):
            cmd = t
            i += 1
            if cmd in 'zZ':
                ops.append(('Z',))
                cx, cy = sx, sy
                last_ctrl = None
                continue
        elif cmd is None:
            raise ValueError('path starts with a number')
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = (cx, cy) if rel else (0.0, 0.0)
        if c == 'M':
            x, y = nums(2)
            cx, cy = x + ox, y + oy
            sx, sy = cx, cy
            ops.append(('M', cx, cy))
            cmd = 'l' if rel else 'L'  # implicit lineto after moveto
            last_ctrl = None
        elif c == 'L':
            x, y = nums(2)
            cx, cy = x + ox, y + oy
            ops.append(('L', cx, cy))
            last_ctrl = None
        elif c == 'H':
            (x,) = nums(1)
            cx = x + (cx if rel else 0)
            ops.append(('L', cx, cy))
            last_ctrl = None
        elif c == 'V':
            (y,) = nums(1)
            cy = y + (cy if rel else 0)
            ops.append(('L', cx, cy))
            last_ctrl = None
        elif c in 'CS':
            if c == 'C':
                x1, y1, x2, y2, x, y = nums(6)
                x1, y1 = x1 + ox, y1 + oy
            else:
                x2, y2, x, y = nums(4)
                if last_ctrl and last_ctrl[2] == 'C':
                    x1, y1 = 2 * cx - last_ctrl[0], 2 * cy - last_ctrl[1]
                else:
                    x1, y1 = cx, cy
            x2, y2, x, y = x2 + ox, y2 + oy, x + ox, y + oy
            ops.append(('C', x1, y1, x2, y2, x, y))
            last_ctrl = (x2, y2, 'C')
            cx, cy = x, y
        elif c in 'QT':
            if c == 'Q':
                qx, qy, x, y = nums(4)
                qx, qy = qx + ox, qy + oy
            else:
                x, y = nums(2)
                if last_ctrl and last_ctrl[2] == 'Q':
                    qx, qy = 2 * cx - last_ctrl[0], 2 * cy - last_ctrl[1]
                else:
                    qx, qy = cx, cy
            x, y = x + ox, y + oy
            # Quadratic → cubic, exactly.
            ops.append(('C', cx + 2 / 3 * (qx - cx), cy + 2 / 3 * (qy - cy),
                        x + 2 / 3 * (qx - x), y + 2 / 3 * (qy - y), x, y))
            last_ctrl = (qx, qy, 'Q')
            cx, cy = x, y
        elif c == 'A':
            rx, ry, phi = nums(3)
            fa = flag()
            fs = flag()
            x, y = nums(2)
            x, y = x + ox, y + oy
            for cub in arc_to_cubics(cx, cy, rx, ry, phi, fa, fs, x, y):
                ops.append(('C',) + cub)
            cx, cy = x, y
            last_ctrl = None
        else:
            raise ValueError(f'unknown path command {cmd}')
    return ops


def tokenize_flags_fix(d):
    """Split packed arc flags ("01" → "0 1") so the generic tokenizer sees them."""
    # Walk commands; inside a/A argument lists, split flag digits.
    parts = re.split(r'([a-zA-Z])', d)
    res = []
    cmd = ''
    for p in parts:
        if len(p) == 1 and p.isalpha():
            cmd = p
            res.append(p)
            continue
        if cmd in 'aA':
            nums = []
            j = 0
            idx = 0
            while j < len(p):
                if p[j] in ' ,\t\r\n':
                    j += 1
                    continue
                k = idx % 7
                if k in (3, 4) and p[j] in '01':
                    nums.append(p[j])
                    j += 1
                else:
                    m = NUM.match(p, j)
                    if not m:
                        raise ValueError('bad arc args')
                    nums.append(m.group())
                    j = m.end()
                idx += 1
            res.append(' '.join(nums))
        else:
            res.append(p)
    return ' '.join(res)


# ── geometry helpers ────────────────────────────────────────────────────────
def parse_transform(s, name):
    m = [1, 0, 0, 1, 0, 0]
    for fn, args in re.findall(r'(\w+)\s*\(([^)]*)\)', s or ''):
        a = [float(v) for v in NUM.findall(args)]
        if fn == 'matrix' and len(a) == 6:
            t = a
        elif fn == 'translate':
            t = [1, 0, 0, 1, a[0], a[1] if len(a) > 1 else 0]
        elif fn == 'scale':
            t = [a[0], 0, 0, a[1] if len(a) > 1 else a[0], 0, 0]
        elif fn == 'rotate':
            r = math.radians(a[0])
            c, s_ = math.cos(r), math.sin(r)
            t = [c, s_, -s_, c, 0, 0]
            if len(a) == 3:
                t = mul([1, 0, 0, 1, a[1], a[2]], mul(t, [1, 0, 0, 1, -a[1], -a[2]]))
        else:
            warn(name, f'unsupported transform {fn}')
            continue
        m = mul(m, t)
    return m


def mul(a, b):
    return [a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
            a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]]


def apply(m, x, y):
    return m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]


def is_identity(m):
    return m == [1, 0, 0, 1, 0, 0]


def ellipse_ops(cx, cy, rx, ry):
    k = 0.5522847498
    return [('M', cx + rx, cy),
            ('C', cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry),
            ('C', cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy),
            ('C', cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry),
            ('C', cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy), ('Z',)]


def rect_ops(x, y, w, h, rx, ry):
    if rx <= 0 and ry <= 0:
        return [('M', x, y), ('L', x + w, y), ('L', x + w, y + h), ('L', x, y + h), ('Z',)]
    rx, ry = min(rx, w / 2), min(ry, h / 2)
    k = 0.5522847498
    return [('M', x + rx, y), ('L', x + w - rx, y),
            ('C', x + w - rx + k * rx, y, x + w, y + ry - k * ry, x + w, y + ry),
            ('L', x + w, y + h - ry),
            ('C', x + w, y + h - ry + k * ry, x + w - rx + k * rx, y + h, x + w - rx, y + h),
            ('L', x + rx, y + h),
            ('C', x + rx - k * rx, y + h, x, y + h - ry + k * ry, x, y + h - ry),
            ('L', x, y + ry),
            ('C', x, y + ry - k * ry, x + rx - k * rx, y, x + rx, y), ('Z',)]


def transform_ops(ops, m):
    out = []
    for op in ops:
        if op[0] in 'MLC':
            pts = op[1:]
            q = []
            for j in range(0, len(pts), 2):
                q.extend(apply(m, pts[j], pts[j + 1]))
            out.append((op[0],) + tuple(q))
        else:
            out.append(op)
    return out


MONO = False  # set while loading an extra (monochrome) icon


def parse_color(v, name):
    """→ None (none), 'cur' (currentColor), or 0xAARRGGBB."""
    if v is None or v == 'currentColor':
        return 'cur'
    v = v.strip()
    if v == 'none':
        return None
    if MONO:
        return 'cur'
    if re.fullmatch(r'#[0-9a-fA-F]{6}', v):
        return 0xff000000 | int(v[1:], 16)
    if re.fullmatch(r'#[0-9a-fA-F]{3}', v):
        return 0xff000000 | int(''.join(c * 2 for c in v[1:]), 16)
    if v in ('black', 'white'):
        return 0xff000000 if v == 'black' else 0xffffffff
    warn(name, f'unsupported colour {v!r}, using currentColor')
    return 'cur'


# ── SVG walk ────────────────────────────────────────────────────────────────
KNOWN_ATTRS = {'d', 'cx', 'cy', 'r', 'rx', 'ry', 'x', 'y', 'width', 'height', 'x1', 'y1', 'x2', 'y2',
               'points', 'fill', 'stroke', 'stroke-width', 'stroke-linecap', 'stroke-linejoin',
               'transform', 'id', 'viewBox', 'xmlns', 'class'}


def load_icon(path):
    name = os.path.basename(path)
    root = ET.parse(path).getroot()
    vb = [float(v) for v in NUM.findall(root.get('viewBox', ''))]
    if len(vb) != 4:
        vb = [0, 0, float(root.get('width', 24)), float(root.get('height', 24))]
    items = []  # (fill, stroke, width, ops)

    def walk(el, style, m):
        tag = el.tag.replace(SVG_NS, '')
        for a in el.attrib:
            if a not in KNOWN_ATTRS:
                warn(name, f'ignored attribute {a} on <{tag}>')
        st = dict(style)
        for k in ('fill', 'stroke', 'stroke-width', 'stroke-linecap', 'stroke-linejoin'):
            if el.get(k) is not None:
                st[k] = el.get(k)
        if el.get('transform'):
            m = mul(m, parse_transform(el.get('transform'), name))
        if tag in ('svg', 'g'):
            for ch in el:
                walk(ch, st, m)
            return
        g = lambda k, d=0.0: float(el.get(k, d))
        circle = None
        if tag == 'path':
            ops = parse_path(tokenize_flags_fix(el.get('d', '')))
        elif tag == 'circle':
            circle = (g('cx'), g('cy'), g('r'))
            ops = ellipse_ops(*circle, circle[2])
        elif tag == 'ellipse':
            ops = ellipse_ops(g('cx'), g('cy'), g('rx'), g('ry'))
        elif tag == 'rect':
            rx, ry = el.get('rx'), el.get('ry')
            rx = float(rx) if rx is not None else (float(ry) if ry is not None else 0)
            ry = float(ry) if ry is not None else rx
            ops = rect_ops(g('x'), g('y'), g('width'), g('height'), rx, ry)
        elif tag == 'line':
            ops = [('M', g('x1'), g('y1')), ('L', g('x2'), g('y2'))]
        elif tag in ('polyline', 'polygon'):
            v = [float(n) for n in NUM.findall(el.get('points', ''))]
            ops = [('M', v[0], v[1])] + [('L', v[j], v[j + 1]) for j in range(2, len(v) - 1, 2)]
            if tag == 'polygon':
                ops.append(('Z',))
        else:
            warn(name, f'unsupported element <{tag}> skipped')
            return
        fill = parse_color(st.get('fill'), name)
        stroke = parse_color(st.get('stroke', 'none'), name)
        width = float(st.get('stroke-width', 1))
        if stroke is not None:
            for k in ('stroke-linecap', 'stroke-linejoin'):
                if st.get(k, 'round') != 'round':
                    warn(name, f'{k}={st[k]} drawn as round')
        # Uniform transforms keep circles circles; the stroke scales with them.
        sc = math.sqrt(abs(m[0] * m[3] - m[1] * m[2]))
        if circle and (is_identity(m) or (m[1] == m[2] == 0 and m[0] == m[3])):
            cx, cy = apply(m, circle[0], circle[1])
            ops = [('O', cx, cy, circle[2] * sc)]
        else:
            ops = transform_ops(ops, m)
        # Items are positioned relative to the viewBox origin.
        items.append((fill, stroke, width * sc, ops))

    # The root's own fill/stroke seed the inherited style. SVG's initial fill
    # is black; for these UI icons (mdi window buttons) that means "the icon
    # colour", so an unset fill is currentColor.
    walk(root, {'fill': 'currentColor', 'stroke': 'none'}, [1, 0, 0, 1, -vb[0], -vb[1]])
    return vb, items


# ── encoding ────────────────────────────────────────────────────────────────
def varint(v, out):
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


def encode(vb, items):
    out = bytearray()
    varint(round(vb[2] * UNIT), out)
    varint(round(vb[3] * UNIT), out)
    pen = [0, 0]

    def coord(axis, v):
        q = round(v * UNIT)
        d = q - pen[axis]
        pen[axis] = q
        varint(((-d) << 1) - 1 if d < 0 else d << 1, out)

    for fill, stroke, width, ops in items:
        if fill is None and stroke is None:
            continue
        # One colour per item: fill and stroke only share an item when they
        # agree (bookmark-filled); otherwise split into two items.
        parts = []
        if fill is not None and stroke is not None and fill != stroke:
            parts = [(fill, None), (None, stroke)]
        else:
            parts = [(fill, stroke)]
        for f, s in parts:
            col = f if f is not None else s
            flags = (1 if f is not None else 0) | (2 if s is not None else 0) | (4 if col != 'cur' else 0)
            out.append(flags)
            if s is not None:
                varint(round(width * UNIT), out)
            if col != 'cur':
                out.extend(col.to_bytes(4, 'big'))
            j = 0
            while j < len(ops):
                op = ops[j]
                k = op[0]
                if k in 'LC':
                    n = 1
                    while j + n < len(ops) and ops[j + n][0] == k and n < 32:
                        n += 1
                    out.append((OP_L if k == 'L' else OP_C) | ((n - 1) << 3))
                    for o in ops[j:j + n]:
                        for t in range(1, len(o), 2):
                            coord(0, o[t])
                            coord(1, o[t + 1])
                    j += n
                    continue
                if k == 'M':
                    out.append(OP_M)
                    coord(0, op[1])
                    coord(1, op[2])
                elif k == 'Z':
                    out.append(OP_Z)
                elif k == 'O':
                    out.append(OP_CIRCLE)
                    coord(0, op[1])
                    coord(1, op[2])
                    varint(round(op[3] * UNIT), out)
                j += 1
            out.append(OP_END)
    return bytes(out)


def enum_name(fname):
    base = os.path.splitext(os.path.basename(fname))[0]
    return ''.join(p[:1].upper() + p[1:] for p in re.split(r'[-_ ]+', base) if p)


def main():
    global MONO
    if len(sys.argv) < 3 or any('=' not in a for a in sys.argv[3:]):
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    files = sorted(f for f in os.listdir(src) if f.endswith('.svg'))
    blob, offsets, names = bytearray(), [], []
    for f in files:
        vb, items = load_icon(os.path.join(src, f))
        offsets.append(len(blob))
        blob += encode(vb, items)
        names.append(enum_name(f))
    for extra in sys.argv[3:]:
        name, path = extra.split('=', 1)
        keep = path.startswith('color:')
        if keep:
            path = path[len('color:'):]
        MONO = not keep
        vb, items = load_icon(path)
        MONO = False
        offsets.append(len(blob))
        blob += encode(vb, items)
        names.append(name)
    offsets.append(len(blob))
    otype = 'uint16_t' if len(blob) < 65536 else 'uint32_t'

    hdr = ['// Generated by src/tools/icons.py from gfx/ui/*.svg — do not edit;',
           '// regenerate with `cmake --build <dir> --target icons`.',
           '#pragma once', '', '#include <cstdint>', '', 'namespace gfx {', '',
           'enum class Icon : uint16_t {']
    hdr += [f'    {n},' for n in names]
    hdr += ['};', f'constexpr int kIconCount = {len(names)};', '',
            '// Path data for drawIcon() (format: src/tools/icons.py).',
            'namespace iconData {',
            f'constexpr int kUnit = {UNIT}; // quanta per viewBox unit',
            f'extern const uint8_t  kData[{len(blob)}];',
            f'extern const {otype} kOffsets[{len(offsets)}];',
            '} // namespace iconData', '', '} // namespace gfx', '']

    cpp = ['// Generated by src/tools/icons.py from gfx/ui/*.svg — do not edit.',
           '#include "gfx/icons_generated.h"', '', 'namespace gfx {', '',
           f'// {len(blob)} bytes of path data for {len(names)} icons.',
           f'const uint8_t iconData::kData[{len(blob)}] = {{']
    for i in range(0, len(blob), 24):
        cpp.append('    ' + ', '.join(str(b) for b in blob[i:i + 24]) + ',')
    cpp += ['};', f'const {otype} iconData::kOffsets[{len(offsets)}] = {{']
    for i in range(0, len(offsets), 16):
        cpp.append('    ' + ', '.join(str(o) for o in offsets[i:i + 16]) + ',')
    cpp += ['};', '', '} // namespace gfx', '']

    os.makedirs(dst, exist_ok=True)
    for fn, lines in (('icons_generated.h', hdr), ('icons_generated.cpp', cpp)):
        p = os.path.join(dst, fn)
        text = '\n'.join(lines)
        old = open(p).read() if os.path.exists(p) else None
        if old != text:
            with open(p, 'w') as fh:
                fh.write(text)
    print(f'icons: {len(names)} icons, {len(blob)} bytes of path data')
    for w in sorted(set(warnings)):
        print('  warning:', w)
    return 0


if __name__ == '__main__':
    sys.exit(main())
