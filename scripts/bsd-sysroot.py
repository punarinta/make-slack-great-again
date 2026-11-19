#!/usr/bin/env python3
"""Fetch a FreeBSD or OpenBSD amd64 sysroot for scripts/build-bsd-cross.sh.

    python3 -I scripts/bsd-sysroot.py <freebsd|openbsd> <dest-dir>

Downloads the base system's libraries and headers and the packages the
desktop build links (Wayland, xkbcommon, XCB, libdbus and what they need at
link time) from the official mirrors into <dest>/dl, checks each file
against the release's SHA256 list (FreeBSD: MANIFEST and the package
catalogue; OpenBSD: the SHA256 files), and unpacks only headers, libraries,
pkg-config files and the Wayland protocol XMLs into <dest>/root.

Everything downloaded is data: nothing from it is ever run. Archives are
unpacked through tarfile's "data" filter (no device files, no paths outside
the root), and absolute symlinks are rewritten to point inside the root.
The signatures (FreeBSD's package fingerprints, OpenBSD's signify) are not
checked: the checksums come over HTTPS from the same mirror.
"""

import base64
import hashlib
import io
import json
import os
import re
import sys
import tarfile
import urllib.request

FREEBSD_RELEASE = "14.4-RELEASE"
FREEBSD_ABI = "FreeBSD:14:amd64"
OPENBSD_RELEASE = "7.9"

# What the build links (plat's wayland/x11/services fragments) plus the XML
# protocol files; their run-time dependencies follow from the catalogue.
FREEBSD_PACKAGES = [
    "wayland", "wayland-protocols", "libxkbcommon", "libxcb", "xcb-util-cursor",
    "xcb-util", "xcb-util-image", "xcb-util-renderutil", "dbus", "libepoll-shim",
]
# Dependencies only needed to run tools or for files the build never reads.
FREEBSD_SKIP = {"python311", "python312", "python313", "perl5", "perl5.40", "perl5.42",
                "xkeyboard-config", "gettext-runtime", "indexinfo", "readline"}
OPENBSD_PACKAGES = ["wayland", "wayland-protocols", "libxkbcommon", "dbus", "epoll-shim",
                    "libffi"]
OPENBSD_SKIP = {"python", "gettext-runtime", "libiconv", "xkeyboard-config"}

# Only these trees of each archive reach the sysroot.
KEEP = re.compile(
    r"^(lib/|usr/lib/|usr/include/|usr/libdata/pkgconfig/|usr/X11R6/(include|lib)/"
    r"|usr/local/(include|lib|libdata/pkgconfig)/|usr/local/share/(wayland-protocols|pkgconfig)/)")
SKIP = re.compile(r"^usr/lib/(debug|dtrace|i18n|clang/.*/lib/.*/.*san)|^usr/lib32/|^usr/local/lib/python")


def log(msg):
    print(msg, flush=True)


def fetch(url, path):
    if os.path.exists(path):
        return path
    log(f"  get {url}")
    tmp = path + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "sysroot-fetch"})
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)
    os.replace(tmp, path)
    return path


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def pkg_sum(path):
    """FreeBSD pkg's "2$…" checksum: BLAKE2b-512 of the file in pkg's base32
    (the z-base-32 alphabet, least significant bits first)."""
    h = hashlib.blake2b()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    digest = h.digest()
    n, alpha = int.from_bytes(digest, "little"), "ybndrfg8ejkmcpqxot1uwisza345h769"
    return "2$" + "".join(alpha[(n >> (5 * i)) & 31] for i in range((len(digest) * 8 + 4) // 5))


def verify(path, want):
    if want.startswith("2$"):
        got = pkg_sum(path)
    else:
        got = sha256(path)
        if len(want) != 64:  # OpenBSD's package list has them in base64
            want = base64.b64decode(want).hex()
    if got != want.lower():
        os.remove(path)
        sys.exit(f"checksum mismatch for {path}: {got} != {want}")


def norm(name):
    """An archive path without its leading "/" or "./"."""
    name = name.lstrip("/")
    while name.startswith("./"):
        name = name[2:]
    return name


def make_filter(root, prefix):
    """tarfile filter: maps member names under `prefix`, keeps only KEEP,
    rewrites absolute symlinks to relative ones, then applies "data"."""

    def flt(member, dest):
        name = norm(member.name)
        if name.startswith("+"):
            return None
        name = prefix + name
        if not KEEP.match(name) or SKIP.match(name):
            return None
        if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
            return None
        changes = {"name": name}
        if member.issym() and member.linkname.startswith("/"):
            target = member.linkname.lstrip("/")
            changes["linkname"] = os.path.relpath(target, os.path.dirname(name) or ".")
        if member.islnk():
            changes["linkname"] = prefix + norm(member.linkname)
        member = member.replace(**changes, deep=False)
        try:
            return tarfile.data_filter(member, dest)
        except tarfile.FilterError as e:
            log(f"    skipped {name}: {e}")
            return None

    return flt


def extract(archive, root, prefix=""):
    log(f"  unpack {os.path.basename(archive)}")
    data = open(archive, "rb").read()
    if data[:4] == b"\x28\xb5\x2f\xfd":  # zstd (FreeBSD .pkg)
        import compression.zstd as zstd
        data = zstd.decompress(data)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(root, filter=make_filter(root, prefix))


def freebsd(dest):
    dl, root = os.path.join(dest, "dl"), os.path.join(dest, "root")
    os.makedirs(dl, exist_ok=True)
    os.makedirs(root, exist_ok=True)
    rel = f"https://download.freebsd.org/releases/amd64/{FREEBSD_RELEASE}"
    manifest = open(fetch(f"{rel}/MANIFEST", os.path.join(dl, "MANIFEST"))).read()
    base_sum = next(l.split()[1] for l in manifest.splitlines() if l.startswith("base.txz"))
    base = fetch(f"{rel}/base.txz", os.path.join(dl, "base.txz"))
    verify(base, base_sum)
    extract(base, root)

    repo = f"https://pkg.freebsd.org/{FREEBSD_ABI}/quarterly"
    site = fetch(f"{repo}/packagesite.pkg", os.path.join(dl, "packagesite.pkg"))
    import compression.zstd as zstd
    with tarfile.open(fileobj=io.BytesIO(zstd.decompress(open(site, "rb").read()))) as t:
        catalogue = t.extractfile("packagesite.yaml").read().decode()
    pkgs = {}
    for line in catalogue.splitlines():
        j = json.loads(line)
        pkgs[j["name"]] = j
    todo, seen = list(FREEBSD_PACKAGES), set()
    while todo:
        name = todo.pop()
        if name in seen or name in FREEBSD_SKIP:
            continue
        seen.add(name)
        todo.extend(pkgs[name].get("deps", {}).keys())
    log(f"  packages: {' '.join(sorted(seen))}")
    for name in sorted(seen):
        p = pkgs[name]
        path = fetch(f"{repo}/{p['repopath']}", os.path.join(dl, f"{name}-{p['version']}.pkg"))
        verify(path, p["sum"])
        extract(path, root)


def openbsd(dest):
    dl, root = os.path.join(dest, "dl"), os.path.join(dest, "root")
    os.makedirs(dl, exist_ok=True)
    os.makedirs(root, exist_ok=True)
    v = OPENBSD_RELEASE.replace(".", "")
    mirror = f"https://cdn.openbsd.org/pub/OpenBSD/{OPENBSD_RELEASE}"

    def sums(url, path):
        out = {}
        for l in open(fetch(url, path)).read().splitlines():
            m = re.match(r"SHA256 \((.+)\) = (\S+)", l)
            if m:
                out[m.group(1)] = m.group(2)
        return out

    setsums = sums(f"{mirror}/amd64/SHA256", os.path.join(dl, "SHA256.sets"))
    # xshare: the X headers and .pc files that are not per-architecture.
    for s in ("base", "comp", "xbase", "xshare"):
        f = f"{s}{v}.tgz"
        path = fetch(f"{mirror}/amd64/{f}", os.path.join(dl, f))
        verify(path, setsums[f])
        # The sets are rooted at "./".
        extract(path, root)

    repo = f"{mirror}/packages/amd64"
    pkgsums = sums(f"{repo}/SHA256", os.path.join(dl, "SHA256.packages"))
    files = sorted(pkgsums)

    def latest(stem):
        # "wayland" → wayland-1.24.0p2.tgz: the stem, a dash, then a digit.
        c = [f for f in files if re.match(re.escape(stem) + r"-[0-9]", f)]
        if not c:
            sys.exit(f"openbsd: no package {stem}")
        return c[-1]

    todo, seen = [latest(p) for p in OPENBSD_PACKAGES], set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        path = fetch(f"{repo}/{f}", os.path.join(dl, f))
        verify(path, pkgsums[f])
        with tarfile.open(path) as t:
            contents = t.extractfile("+CONTENTS").read().decode()
        for l in contents.splitlines():
            # @depend devel/libffi:libffi-*:libffi-3.5.2p0
            if l.startswith("@depend "):
                dep = l.split(":")[-1] + ".tgz"
                stem = re.sub(r"-[0-9].*", "", dep)
                if stem not in OPENBSD_SKIP and dep in pkgsums:
                    todo.append(dep)
                elif stem not in OPENBSD_SKIP:
                    todo.append(latest(stem))
        extract(path, root, prefix="usr/local/")
    log(f"  packages: {' '.join(sorted(seen))}")


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("freebsd", "openbsd"):
        sys.exit(__doc__)
    os_name, dest = sys.argv[1], os.path.abspath(sys.argv[2])
    log(f"{os_name} sysroot → {dest}/root")
    (freebsd if os_name == "freebsd" else openbsd)(dest)
    open(os.path.join(dest, "root", ".complete"), "w").close()
    log("done")


if __name__ == "__main__":
    main()
