#!/usr/bin/env python3
"""msga's translations: source strings -> .po files -> compiled-in tables.

    tools/i18n.py update    extract the strings, merge them into every
                            app/i18n/<code>.po, regenerate languages_generated.cpp
    tools/i18n.py check     fail if a .po or the generated table is out of date
                            (ctest i18n_up_to_date runs this)
    tools/i18n.py missing [file…]
                            list the strings (of those files) that no .po has a
                            translation for, live or obsolete; changes nothing
    (CMake: cmake --build <dir> --target translations  ==  update)

Strings are found in src/{base,ui,app} (demos, the fake backend, size probes and the
gallery excluded) as the literal arguments of
    tr("...")          i18n::tr: one string
    trn("...", "...")  i18n::trn: singular (the key) and plural
    N_("...")          marks a literal translated elsewhere (tr(table[i]))
Adjacent literals concatenate, C escapes ("\\xE2\\x80\\xA6") are decoded.
A call whose argument is not a literal is skipped: its strings need N_().

The .po files are ordinary gettext catalogues (edit them with any PO editor;
Poedit, Lokalize, a text editor). `update` keeps every translation whose
msgid still exists, adds new msgids untranslated, and moves the rest to the
end as obsolete (#~) entries, so nothing translated is ever lost. Fuzzy
entries are not compiled in. The header's Plural-Forms becomes C code.

The generated table is one raw-deflate blob per language (the PNG decoder's
inflate unpacks it on first use); the format is described in base/i18n.h.
Every msgid's FNV-1a hash must be unique — `update` refuses otherwise.
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
I18N_DIR = os.path.join(SRC, 'app', 'i18n')
GENERATED = os.path.join(I18N_DIR, 'languages_generated.cpp')
SCAN_DIRS = ['base', 'ui', 'app']
SKIP_PARTS = {'tests', 'demo', 'fake', 'gallery', 'probe', 'third_party', 'i18n'}


# ── C++ source scanning ─────────────────────────────────────────────────────

CALL = re.compile(r'(?<![\w.>:])(?:i18n::)?(trn|tr|N_)\s*\(')
SIMPLE_ESC = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, '"': 34, "'": 39, 'a': 7,
              'b': 8, 'f': 12, 'v': 11, '?': 63}


def skip_space(s, i):
    while i < len(s):
        if s[i].isspace():
            i += 1
        elif s.startswith('//', i):
            i = s.find('\n', i)
            i = len(s) if i < 0 else i
        elif s.startswith('/*', i):
            i = s.find('*/', i) + 2
        else:
            break
    return i


def parse_literal(s, i):
    """One "..." at s[i] -> (bytes, index after it)."""
    out = bytearray()
    i += 1
    while s[i] != '"':
        c = s[i]
        if c != '\\':
            out += c.encode('utf-8')
            i += 1
            continue
        e = s[i + 1]
        if e == 'x':
            j = i + 2
            while j < len(s) and s[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(int(s[i + 2:j], 16) & 0xFF)
            i = j
        elif e in '01234567':
            j = i + 1
            while j < i + 4 and s[j] in '01234567':
                j += 1
            out.append(int(s[i + 1:j], 8) & 0xFF)
            i = j
        else:
            out.append(SIMPLE_ESC[e])
            i += 2
    return bytes(out), i + 1


def parse_literals(s, i):
    """Adjacent literals from s[i] -> (text or None, index after)."""
    i = skip_space(s, i)
    if i >= len(s) or s[i] != '"':
        return None, i
    data = b''
    while i < len(s) and s[i] == '"':
        lit, i = parse_literal(s, i)
        data += lit
        i = skip_space(s, i)
    return data.decode('utf-8'), i


def source_files():
    for d in SCAN_DIRS:
        for root, dirs, files in os.walk(os.path.join(SRC, d)):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_PARTS)
            for f in sorted(files):
                if f.endswith(('.cpp', '.h', '.mm', '.inc')):
                    yield os.path.join(root, f)


def extract():
    """-> {msgid: {'plural': str or None, 'refs': [file, ...]}} in source order."""
    found = {}
    for path in source_files():
        text = open(path, encoding='utf-8').read()
        if path.endswith(os.path.join('base', 'i18n.h')):
            continue  # the N_ definition itself
        rel = os.path.relpath(path, os.path.dirname(SRC))
        for m in CALL.finditer(text):
            msgid, i = parse_literals(text, m.end())
            if msgid is None:
                continue
            plural = None
            if m.group(1) == 'trn':
                if i >= len(text) or text[i] != ',':
                    continue
                plural, i = parse_literals(text, i + 1)
                if plural is None:
                    sys.exit(f'{rel}: trn("{msgid}", …) needs a literal plural')
            e = found.setdefault(msgid, {'plural': None, 'refs': []})
            if plural is not None:
                if e['plural'] not in (None, plural):
                    sys.exit(f'{rel}: "{msgid}" has two different plurals')
                e['plural'] = plural
            if rel not in e['refs']:
                e['refs'].append(rel)
    return found


# ── .po files ───────────────────────────────────────────────────────────────

def po_unquote(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\':
            e = s[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', 'r': '\r'}.get(e, e))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def po_quote(s):
    s = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r')
    lines = s.split('\n')
    if len(lines) == 1:
        return f'"{s}"'
    parts = [l + '\\n' for l in lines[:-1]] + ([lines[-1]] if lines[-1] else [])
    return '""\n' + '\n'.join(f'"{p}"' for p in parts)


class Entry:
    def __init__(self):
        self.msgid = None
        self.plural = None
        self.msgstr = {}  # form index -> text
        self.comments = []  # translator comments (# ...), kept verbatim
        self.refs = []
        self.flags = set()
        self.obsolete = False


def read_po(path, text=None):
    """-> (header, entries) of the .po at `path`, or of `text` when given."""
    entries, cur, field = [], Entry(), None

    def flush():
        nonlocal cur
        if cur.msgid is not None:
            entries.append(cur)
        cur = Entry()

    lines = open(path, encoding='utf-8') if text is None else text.splitlines(keepends=True)
    for raw in lines:
        line = raw.rstrip('\n')
        obsolete = line.startswith('#~')
        if obsolete:
            line = line[2:].lstrip()
        if not line.strip():
            flush()
            field = None
            continue
        if line.startswith('#,'):
            cur.flags |= {f.strip() for f in line[2:].split(',')}
        elif line.startswith('#:'):
            cur.refs += line[2:].split()
        elif line.startswith('#'):
            if line.startswith('# ') or line == '#':
                cur.comments.append(line)
        else:
            cur.obsolete |= obsolete
            m = re.match(r'(msgid_plural|msgid|msgstr(?:\[(\d+)\])?|msgctxt)\s+"(.*)"$', line)
            if m:
                kw = m.group(1)
                if kw == 'msgid' and cur.msgid is not None:
                    flush()
                    cur.obsolete = obsolete
                field = (kw, int(m.group(2) or 0)) if kw.startswith('msgstr') else kw
                val = po_unquote(m.group(3))
            else:
                val = po_unquote(re.match(r'"(.*)"$', line.strip()).group(1))
            if field == 'msgid':
                cur.msgid = (cur.msgid or '') + val if m is None else val
            elif field == 'msgid_plural':
                cur.plural = (cur.plural or '') + val if m is None else val
            elif isinstance(field, tuple):
                cur.msgstr[field[1]] = cur.msgstr.get(field[1], '') + val if m is None else val
    flush()
    header = next((e for e in entries if e.msgid == ''), None)
    return header, [e for e in entries if e.msgid != '']


def header_field(header, name):
    m = re.search(rf'^{name}:\s*(.*)$', header.msgstr.get(0, ''), re.M)
    return m.group(1).strip() if m else ''


def nplurals(header):
    m = re.search(r'nplurals\s*=\s*(\d+)', header_field(header, 'Plural-Forms'))
    return int(m.group(1)) if m else 2


def write_entry(out, e):
    pre = '#~ ' if e.obsolete else ''
    for c in e.comments:
        out.append(c)
    if e.refs and not e.obsolete:
        out.append('#: ' + ' '.join(e.refs))
    if e.flags:
        out.append('#, ' + ', '.join(sorted(e.flags)))

    def kv(key, val):
        q = po_quote(val).split('\n')
        out.append(f'{pre}{key} {q[0]}')
        out.extend(pre + l for l in q[1:])

    kv('msgid', e.msgid)
    if e.plural is not None:
        kv('msgid_plural', e.plural)
        for k in sorted(e.msgstr) or [0]:
            kv(f'msgstr[{k}]', e.msgstr.get(k, ''))
    else:
        kv('msgstr', e.msgstr.get(0, ''))
    out.append('')


def merged_po(path, found):
    header, entries = read_po(path)
    n = nplurals(header)
    old = {e.msgid: e for e in entries}
    out = []
    write_entry(out, header)
    for msgid, info in found.items():
        e = old.pop(msgid, None) or Entry()
        e.msgid, e.obsolete, e.refs = msgid, False, info['refs']
        if info['plural'] is not None:
            e.plural = info['plural']
            e.msgstr = {k: e.msgstr.get(k, '') for k in range(n)}
        else:
            e.plural = None
            e.msgstr = {0: e.msgstr.get(0, '')}
        write_entry(out, e)
    for e in old.values():
        if any(e.msgstr.values()):  # untranslated leftovers carry nothing
            e.obsolete = True
            write_entry(out, e)
    return '\n'.join(out)


# ── the generated table ─────────────────────────────────────────────────────

def fnv1a(s):
    h = 2166136261
    for c in s.encode('utf-8'):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def table_blob(entries, forms):
    rows = []
    for e in entries:
        if e.obsolete or 'fuzzy' in e.flags or not any(e.msgstr.values()):
            continue
        texts = [e.msgstr.get(k, '') for k in range(forms if e.plural is not None else 1)]
        rows.append((fnv1a(e.msgid), texts + [''] * (forms - len(texts))))
    rows.sort()
    blob = struct.pack('<I', len(rows)) + b''.join(struct.pack('<I', h) for h, _ in rows)
    for _, texts in rows:
        for t in texts:
            blob += t.encode('utf-8') + b'\0'
    return blob, len(rows)


def plural_expr(header):
    m = re.search(r'plural\s*=\s*([^;]+)', header_field(header, 'Plural-Forms'))
    expr = m.group(1).strip() if m else '(n != 1)'
    if not re.fullmatch(r'[n0-9\s()!=<>&|%?:+\-*/]+', expr):
        sys.exit(f'unexpected Plural-Forms expression: {expr}')
    return expr


def generated_cpp(pos):
    out = ['// Generated by src/tools/i18n.py from src/app/i18n/*.po — do not edit.',
           '#include "app/i18n/languages.h"', '', '#include "base/i18n.h"', '',
           'namespace app_i18n {', '', 'namespace {', '']
    names = []
    for code, (header, entries) in pos:
        forms = nplurals(header)
        blob, count = table_blob(entries, forms)
        z = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        packed = z.compress(blob) + z.flush()
        ident = re.sub(r'\W', '_', code)
        out.append(f'// {count} translations: {len(blob)} bytes, {len(packed)} deflated.')
        out.append(f'const unsigned char k_{ident}[{len(packed)}] = {{')
        for i in range(0, len(packed), 24):
            out.append('    ' + ', '.join(str(b) for b in packed[i:i + 24]) + ',')
        out.append('};')
        out.append(f'int {ident}Plural(int64_t n) {{')
        out.append(f'    return int({plural_expr(header)});')
        out.append('}')
        out.append(f'std::string {ident}Load() {{')
        out.append(f'    return inflate(k_{ident}, sizeof k_{ident}, {len(blob)});')
        out.append('}')
        out.append(f'const i18n::Language k_{ident}Lang{{"{code}", {ident}Plural, {forms}, '
                   f'{ident}Load}};')
        out.append('')
        names.append(ident)
    out += ['} // namespace', '', 'void registerLanguages() {']
    out += [f'    i18n::registerLanguage(&k_{n}Lang);' for n in names]
    out += ['}', '', '} // namespace app_i18n', '']
    return '\n'.join(out)


def po_files():
    return sorted(os.path.join(I18N_DIR, f) for f in os.listdir(I18N_DIR) if f.endswith('.po'))


def check_hashes(found):
    seen = {}
    for msgid in found:
        h = fnv1a(msgid)
        if h in seen and seen[h] != msgid:
            sys.exit(f'hash collision: "{seen[h]}" and "{msgid}" — reword one')
        seen[h] = msgid


def build():
    """-> {path: wanted content} for every file `update` writes."""
    found = extract()
    check_hashes(found)
    want, pos = {}, []
    for path in po_files():
        text = merged_po(path, found)
        want[path] = text
        # Parsed in memory: `check` must not write (the release builds mount
        # the source tree read-only).
        header, entries = read_po(path, text)
        pos.append((os.path.basename(path)[:-3], (header, entries)))
    want[GENERATED] = generated_cpp(pos)
    return want, found, pos


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd not in ('update', 'check', 'missing'):
        sys.exit(__doc__)
    if cmd == 'missing':
        only = {os.path.relpath(os.path.abspath(f), os.path.dirname(SRC)) for f in sys.argv[2:]}
        for path in po_files():
            _, entries = read_po(path)
            have = {e.msgid for e in entries if any(e.msgstr.values())}
            for msgid, info in extract().items():
                if msgid not in have and (not only or only & set(info['refs'])):
                    print(f'{os.path.basename(path)}: {msgid!r}  ({", ".join(info["refs"])})')
        return
    want, found, pos = build()
    stale = [p for p, t in want.items()
             if not os.path.exists(p) or open(p, encoding='utf-8').read() != t]
    if cmd == 'check':
        for p in stale:
            print(f'out of date: {os.path.relpath(p, SRC)}', file=sys.stderr)
        if stale:
            sys.exit('run: src/tools/i18n.py update   (or cmake --build <dir> --target translations)')
        return
    for p in stale:
        open(p, 'w', encoding='utf-8').write(want[p])
    for code, (header, entries) in pos:
        live = [e for e in entries if not e.obsolete]
        done = sum(1 for e in live if 'fuzzy' not in e.flags and any(e.msgstr.values()))
        print(f'{code}: {done}/{len(live)} translated, '
              f'{sum(1 for e in entries if e.obsolete)} obsolete')
    print(f'{len(found)} strings; wrote {len(stale)} file(s)')


if __name__ == '__main__':
    main()
