# msga sources

msga (Slack + Claude Code), built on its own platform layer `plat/`. The
**first goal is the smallest possible binary and resource use**; every choice
below serves that. Design record: `../docs/platform-layer-plan.md` (the plat
layer) and `docs/` here as modules land.

## Layout

| Dir     | What                                                                  |
|---------|-----------------------------------------------------------------------|
| `base/` | UTF-8, JSON, files, logging, time — the only "core library"           |
| `net/`  | HTTP requests + WebSockets: WinHTTP / NSURLSession, own client on Linux |
| `gfx/`  | CPU painter on a premultiplied ARGB32 canvas, image decoding, icons   |
| `text/` | font discovery + fallback, shaping, glyph raster cache, rich layout   |
| `ui/`   | the toolkit: view tree, layout, input routing, scroll, text editing   |
| `app/`  | model, fake (fixture) backend, mrkdwn, the screens                    |
| `tools/`| build-time generators (icons) and the size report                     |
| `plat/` | the thin OS layer: windows, input, loop, desktop services             |
| `prim/` | string, UTF-8 and hash primitives shared by plat and base           |

Each module is a static library with its own `CMakeLists.txt`; the top-level
`../CMakeLists.txt` includes the ones that exist. Dependencies only go downwards:
`app → ui → text → gfx → base`, `app → net → base`, and everything may use `plat`.

The tests live outside `src/`, in `../tests/`: one directory per module
(`tests/base/`, `tests/app/screens/shell/`, …, `tests/plat/` for plat's
standalone build), shared helpers in `tests/support/`, and everything a test
reads from disk in `tests/assets/` (the fake workspace's `fixture.json` and
its media, the SVG fixtures).

## Size rules (enforced in review and by `tools/size_report.py`)

- Build: `-Oz` (`-Os` where the compiler has no `-Oz`), no exceptions, no RTTI,
  no unwind tables, function/data sections + gc-sections, identical-code
  folding, packed relocations; releases build with clang. A file that is
  provably hot (the rasterizer, JSON parser) may opt into `-O2` in its
  CMakeLists with a comment saying why.
- Function bodies live in `.cpp` files; headers declare. Only trivial one-liners
  inline.
- No template-heavy libraries (no rpl, no Boost, no `std::regex`, no
  `<iostream>`/`<fstream>`/`<sstream>`, no `std::locale`, no `std::filesystem`,
  no floating-point `std::to_chars`/`from_chars`: in a static binary each pulls
  in 50–350 KB of libstdc++). `std::vector`/`std::string`/`std::string_view`
  /`std::unique_ptr`/`std::function` are fine; don't instantiate a container for
  every tiny type when an index or a flat array would do.
- Prefer a `switch` on an enum over `std::variant` visitors, and data tables
  over hand-written code (UI descriptions, JSON field mapping, settings).
- Lambdas stored in `std::function` are fine where they carry real behaviour;
  don't wrap every trivial handler in its own type-erased object.
- No bundled data the OS already has (fonts, emoji, spell dictionaries, CAs).
  Rarely used tables are stored compressed and unpacked on first use.
- Every module reports its size; growth beyond the module budget fails CI.

## Style

Same as the repo: clang-format (LLVM-based, 4 spaces, 100 cols;
`../scripts/clang-format-env.sh`), `_member` fields, camelCase methods,
comments explain *why*. UI text is sentence case ("Add channels", not "Add
Channels").

## Build

```
scripts/build.sh --test         # or: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DMSGA_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

Releases (into `dist/`, with a `.manifest` for the updater and the symbols
kept beside it): `scripts/release-linux-static.sh` (static musl binary, Docker),
`scripts/release-windows.sh` (static `.exe`, cross-compiled in Docker),
`scripts/release-mac.sh` on a Mac or `scripts/release-mac-remote.sh` from Linux
(DMG), then `scripts/publish-release.sh`. `scripts/release.sh` is the local
release-flags build with the size report. macOS, Windows, the static Linux
build and the BSDs compile FreeType and HarfBuzz themselves
(`text/font_libs.cmake`).

FreeBSD and OpenBSD (amd64) cross-build from Linux with the host's clang +
lld: `scripts/build-bsd-cross.sh freebsd|openbsd [--release]` fetches a
sysroot once (`scripts/bsd-sysroot.py` → `build-bsd-sysroot/`) and builds
into `build-<os>/` with `plat/cmake/bsd-clang.cmake`. On a BSD itself,
`sh scripts/configure-bsd.sh` installs the packages and `scripts/build.sh`
builds natively with the base clang and lld. `scripts/release-bsd.sh
freebsd|openbsd` cross-builds the release into
`dist/msga-<os>-x86_64.tar.gz` (the binary, its icons, a
`.desktop` file and a README naming the runtime packages; `.manifest` and
`.debug` beside it), which `publish-release.sh --only freebsd|openbsd` uploads. Code that differs by
desktop family tests `MSGA_FREEDESKTOP` (Linux and the BSDs: XDG, X11/Wayland,
D-Bus); `__linux__` is only for what Linux alone has. A new OS without any of
these backends implements the C port contract instead (`-DMSGA_PLATFORM=port`,
`../docs/port-contract.md`); `../docs/portability.md` ties the two together
with the Claude Code relay.

The fake backend (`app/fake/`) is opt-in: `-DMSGA_BUILD_TESTS=ON` builds the
test suites on it (on `../tests/assets/fixture.json`). Without it nothing of
it is compiled, and a release `msga` never contains it.
