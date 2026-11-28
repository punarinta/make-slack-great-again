#!/usr/bin/env python3
"""Per-module size of a linked msga binary, checked against budgets.

Reads an lld linker map (link with -Wl,-Map=<file>; the top-level
CMakeLists does this for every executable when MSGA_SIZE_MAP=ON) and adds up
the bytes each input archive/object contributes to allocated output sections
(.text, .rodata, .data, .eh_frame, …; .bss is reported separately because it
costs RAM, not file size).

    src/tools/size_report.py build/msga.map [--budget src/tools/size_budget.json]

Exit status 1 when a module is over its budget, so CI can gate on it.
"""
import argparse
import collections
import json
import os
import re
import sys

# lld map lines: "     VMA      LMA     Size Align Out     In      Symbol"
LINE = re.compile(r'^\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s(.*)$')


def module_of(path: str) -> str:
    """Group an input file into a module name."""
    base = os.path.basename(path.split('(')[0])
    m = re.match(r'lib(msga_\w+|plat|prim)\.a$', base)
    if m:
        return m.group(1)
    if 'third_party' in path:
        return 'third_party:' + path.split('third_party/')[1].split('/')[0]
    for lib in ('freetype', 'harfbuzz', 'mbedtls', 'mbedcrypto', 'mbedx509', 'xcb', 'xkbcommon',
                'wayland', 'dbus', 'png', 'z'):
        if re.match(rf'lib{lib}[\w-]*\.a$', base):
            return 'lib:' + lib
    if re.search(r'lib(std)?c\+\+|libgcc|libsupc|crt\w*\.o|libc\.a|libm\.a|libpthread', base):
        return 'runtime'
    if base.endswith('.o') or base.endswith('.obj'):
        # an object of an executable target: CMakeFiles/<target>.dir/…
        m = re.search(r'CMakeFiles/([\w.-]+)\.dir/', path)
        return 'exe:' + (m.group(1) if m else base)
    return 'other:' + base


def parse(map_path: str):
    sizes = collections.Counter()
    bss = collections.Counter()
    out_section = None
    with open(map_path, errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            size = int(m.group(3), 16)
            rest = m.group(5)
            depth = len(rest) - len(rest.lstrip(' '))
            item = rest.strip()
            if depth == 0:            # output section, e.g. ".text"
                out_section = item.split()[0]
                continue
            if depth != 8 or not size:  # only input sections ("        file:(.text.foo)")
                continue
            path = item.rsplit(':(', 1)[0]
            if out_section in ('.bss', '.tbss'):
                bss[module_of(path)] += size
            elif out_section and not out_section.startswith(('.debug', '.comment', '.symtab',
                                                             '.strtab', '.shstrtab')):
                sizes[module_of(path)] += size
    return sizes, bss


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--budget', help='JSON {module: max_bytes}')
    ap.add_argument('--json', help='write the report as JSON here')
    a = ap.parse_args()

    sizes, bss = parse(a.map)
    total = sum(sizes.values())
    budget = json.load(open(a.budget)) if a.budget else {}
    over = []
    print(f"{'module':32} {'KB':>9} {'%':>6} {'bss KB':>8} {'budget KB':>10}")
    for mod, b in sizes.most_common():
        lim = budget.get(mod)
        flag = ''
        if lim is not None and b > lim:
            flag = '  OVER'
            over.append(mod)
        print(f"{mod:32} {b/1024:9.1f} {100*b/max(total,1):6.1f} {bss[mod]/1024:8.1f} "
              f"{(f'{lim/1024:.0f}' if lim is not None else '-'):>10}{flag}")
    print(f"{'total (allocated, file)':32} {total/1024:9.1f}")
    if a.json:
        json.dump({'total': total, 'modules': dict(sizes), 'bss': dict(bss)}, open(a.json, 'w'),
                  indent=1, sort_keys=True)
    if over:
        print('over budget: ' + ', '.join(over), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
