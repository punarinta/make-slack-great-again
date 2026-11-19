# Porting msga anywhere

How to bring msga to an operating system it doesn't run on yet. msga needs
surprisingly little from an OS: a place to draw pixels, input events, a
network connection with TLS, threads, files and a clock. Everything else
(widgets, layout, text shaping, fonts, image decoding, painting, HTTP,
WebSockets, JSON, the Slack and Claude Code logic) is msga's own code and is
the same on every system.

The headers themselves (`src/port/include/port/*.h`) are the reference:
every function, struct and capability bit is documented there.

## 1. Pick a route

There are two ways in:

| Route | When | What you write |
|---|---|---|
| **The C port contract** | a new OS, especially one whose own compiler is C only, or whose services don't look like a desktop OS's | one set of C files implementing `src/port/include/port/*.h`, built with `-DMSGA_PLATFORM=port` |
| **A native backend** | an OS close to one msga already supports (it reuses most of an existing backend) | C++ in `src/plat/src/<backend>/`, `src/net/<backend>/` and the matching `base` files, as the existing backends are |

This page is about the first route: it is the one meant for "anywhere".

## 2. What the OS must provide

The contract groups everything msga asks of an OS into five headers:

| Header | Concept | Needed for |
|---|---|---|
| `port_sys.h` | threads, mutexes, condition variables with timed waits; a monotonic and a wall clock, the local time zone's offset; sleep; random bytes; environment; well-known folders (home, config, data, cache, temp, the executable); files and folders (read, write, sync, atomic rename, stat with modification and creation time, listing); child processes (optional); a secret store (optional) | everything |
| `port_net.h` | one blocking HTTP/1.1 exchange that streams its answer; a WebSocket client; a cancel flag that ends a blocked exchange; name resolution, sockets and TLS stay inside the port | talking to Slack (TLS is required) |
| `port_app.h` | the event loop (run, wake from another thread, timers); one or more surfaces msga paints into (CPU pixels, premultiplied 32-bit ARGB, with damage rectangles); keyboard, pointer, scroll and text-input events; and, where the OS has them, clipboard, cursors, tray, notifications, badges, monitors, single instance, URL scheme, file dialog, network and power events, opening a link | the window and the UI |
| `port_audio.h` | playing a sound file, recording 16 kHz mono audio, system sounds | voice messages, dictation, the notification chime (all optional) |
| `port_test.h` | synthesised input, reading pixels back, probes for tray and notifications | running the UI test suites on the target (optional) |

`port.h` ties them together: the ABI version, error codes, capability bits,
`port_init` and `port_shutdown`.

## 3. The rules of the contract

- **Plain C.** The headers include only `<stddef.h>` and `<stdint.h>`, so any
  C11 compiler takes them, and they compile as C++ as well.
- **Opaque handles.** Every OS object is a `struct port_x *` the port creates
  and destroys. msga never sees the OS's own descriptors or handles.
- **The caller owns memory.** Strings come back into caller buffers (with the
  full length returned, so a short buffer is retried), are valid during a
  callback, or are objects with their own free function.
- **UTF-8 everywhere**, `/` in paths.
- **Errors are negative integers** (`PORT_E_*`), `0` is success.
- **Callbacks take `void *user` first.**
- **Every struct starts with its own `size`**, so a newer msga works with an
  older port and the other way round within one major ABI version.
- **Every function exists in every port.** What the OS can't do returns
  `PORT_E_NOSYS` and its capability bit stays clear.

## 4. Capabilities: say what you have, msga adapts

`port_caps()` and `port_caps2()` return bits saying what the port can do.
msga asks before it uses a feature and quietly does without:

| Missing | msga then |
|---|---|
| several windows | uses one full-screen surface and draws its own title bar |
| system decorations, window positions, always-on-top | draws its own title bar, doesn't restore positions, hides "Pin window on top" |
| clipboard, primary selection, drag and drop, cursors | goes without copy/paste, middle-click paste, drag and drop, pointer shapes |
| inline input-method composition | accepts committed text only |
| tray, tray menu, badge | runs without a tray icon, or shows an attention dot instead of a count |
| notifications, their images, actions, withdrawal | goes without notifications or without those parts |
| single instance, URL scheme | lets every launch be its own, has no `msga://` links |
| file dialog | shows its own chooser |
| network and power events | reconnects when a request fails |
| child processes, pipes, a terminal (PTY) | no browser sign-in, no local Claude Code (see section 8) |
| a secret store, or one with a size limit (`port_info.secret_max`) | keeps credentials in its settings file |
| directory watching | checks for changes on a timer |
| audio play, capture, sounds | hides playback, has no microphone, uses the bundled chime |

TLS is the one thing a real port can't leave out: Slack is HTTPS only.

## 5. Threads and the loop

- **The loop belongs to the port.** It waits on whatever the OS waits on and
  calls msga back for events, wakes, due timers, frames and directory
  changes. msga never waits on a socket itself.
- **The loop thread** runs everything in `port_app.h` and `port_audio.h` and
  every callback from them. A callback never runs inside the call that
  caused it; it comes on a later turn of the loop.
- **Any thread** may call `port_app_wake` and anything in `port_sys.h` and
  `port_net.h`. msga does its slow work (requests, WebSocket reads, disk,
  child processes) on its own worker threads, which need 256 KiB to 1 MiB of
  stack.
- **Cancelling** a blocked exchange must end it promptly: at once where the
  OS can interrupt the wait, otherwise within 250 ms.

## 6. Writing the port, step by step

1. **Start from the reference port.** `src/port/ref/` implements every
   function of the contract and runs headless on a development machine.
   Copy it (or just its function list) into a directory of your own whose
   `CMakeLists.txt` defines the static library `msga_port_impl`.
2. **System first** (`port_sys.h`): threads, locks, clocks, files, folders,
   environment. Build only the base module (`-DMSGA_MODULES="base"`) and run
   `msga_base_tests` on the target.
3. **Network** (`port_net.h`): the HTTP exchange and the WebSocket client on
   the OS's TLS. Build `base;net` and run `msga_net_tests` (its test server
   is a Python script; on a target without Python, point `NET_TEST_URL` at
   one running elsewhere).
4. **The app** (`port_app.h`): the loop, timers and wake; then one surface
   with frames and painting; then input; then whatever services the OS has.
   Leave the rest returning `PORT_E_NOSYS` with its bit clear.
5. **Test hooks** (`port_test.h`), so the UI suites can drive the port.
6. **Audio** last.
7. **Build the whole app:**

   ```
   cmake -S . -B build-<os> -G Ninja -DMSGA_PLATFORM=port \
       -DMSGA_PORT_IMPL=/path/to/your/port \
       -DCMAKE_TOOLCHAIN_FILE=<your target's C++ toolchain>
   ```

   The port's C files can instead be built by the OS's own C compiler into a
   static library and handed to CMake as an `IMPORTED` target through
   `MSGA_PORT_IMPL`.

## 7. Hosting msga's C++ on the target

The port is C, but msga's core is C++20, cross-compiled for the target on
another machine if the target has no C++ compiler. The core is built to need
very little from a C++ runtime: no exceptions, no RTTI, no iostreams, no
locales, no regular expressions. What the target's C++ support must still
give it:

- `operator new` / `delete` on the system allocator;
- static constructors run before `main` (if the C runtime doesn't walk
  `.init_array`, a small start routine has to);
- function-local statics (`__cxa_guard_*`, which can sit on a `port_mutex`)
  and `__cxa_pure_virtual`;
- `thread_local`, used in a few places;
- a standard library configured without wide characters and locales, for the
  containers msga uses (`vector`, `string`, `string_view`, `function`,
  `unique_ptr`, `unordered_map`, `optional`, algorithms).

## 8. Things outside the contract

A few app features talk to the OS directly and are switched off or moved
onto the contract per port:

| Feature | On a new OS |
|---|---|
| self-update (replacing the running binary) | off: `-DMSGA_SELF_UPDATE=OFF`; a new version is a new download |
| a desktop launcher entry | off: `-DMSGA_INSTALL_LAUNCHER=OFF`, or the OS's own way |
| crash reports | the OS's own fault handling, or off |
| fonts | msga reads font files itself; give it the OS's font folder through the paths API, or, where the OS hands out faces by name instead of as files, list them and hand over each face's bytes (`port_font_info`, `port_font_data`, `PORT_CAP2_FONTS`) |
| spell-check dictionaries | Hunspell files from a folder the paths API names |
| memory statistics in Settings | a value from the OS, or none |
| importing a session from Slack's desktop app | off (it only exists where that app does) |
| Claude Code | `claude` won't run on a new OS: msga drives it on another machine over SSH instead, through `msga --cc-relay` on that machine (Settings → System → Claude Code → "Runs on") |

## 9. Testing

- **On the development machine**, with the reference port:

  ```
  cmake -S . -B build-port -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMSGA_BUILD_TESTS=ON -DMSGA_PLATFORM=port
  cmake --build build-port && ctest --test-dir build-port
  ```

- **One surface instead of windows:** set `MSGA_PORT_REF_SURFACE=auto` (or a
  size such as `1280x800`) and run the suites again. The reference port then
  behaves like an OS with a single full-screen surface, so the core's
  single-surface path is tested before a real port needs it.
- **The headers as plain C:** they must build with any C11 compiler at
  `-std=c11 -pedantic`; `src/port/header_check.c` includes all of them.
- **Nothing beside the contract:** `scripts/port-symbol-audit.sh <build-dir>
  <allowlist>` lists every symbol the core leaves undefined apart from
  `port_*` and fails on any the allowlist doesn't name. The reference-port
  build runs it against `scripts/port-allow-iso.txt` (ISO C and the C++
  runtime) as a ctest; for a new OS, write an allowlist of what its SDK
  exports and run the audit on the target build.
- **On the target**, the same suites, module by module as in section 6.

## 10. Shipping it

When the port runs: a release script that builds the archive into `dist/`
with its `.manifest`, its entry in `scripts/publish-release.sh`, the download
in `website/deploy.sh` and on the website's download section, a badge in the
README, and its scripts and docs in `scripts/sync-to-public.sh`.
