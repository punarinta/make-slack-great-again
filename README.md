# Make Slack Great Again

A fast native Slack client built in C++ with Qt6.

## About

msga is primarily a Slack client — that's its main focus and the most complete, battle-tested backend. It also supports other messaging platforms in **experimental mode**: Microsoft Teams and email. The same Slack-like UI is used for all of them, so channels, direct messages and conversations from every connected service share one consistent interface and live side by side in the same workspace rail.

![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-blue)
![C++](https://img.shields.io/badge/language-C%2B%2B20-00599C?logo=cplusplus&logoColor=white)
![Qt](https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white)
![Release](https://img.shields.io/github/v/release/punarinta/make-slack-great-again)

## Demo

![msga in action](gfx/screenshots/recording-1.webp)

## Download

[![Linux](https://img.shields.io/badge/Linux-x86__64-FCC624?logo=linux&logoColor=black)](https://msga.app/download/msga-linux-x86_64)
[![macOS](https://img.shields.io/badge/macOS-Apple%20Silicon-000000?logo=apple&logoColor=white)](https://msga.app/download/msga-macos-arm64.dmg)
[![Windows](https://img.shields.io/badge/Windows-x86__64-0078D4?logo=windows&logoColor=white)](https://msga.app/download/msga-windows-x86_64.exe)

## Connecting to Slack

Grab a [prebuilt build](#download) and you can connect Slack straight away — there are two ways to sign in:

- **Session sign-in (recommended)** — click *Sign in with your browser* and log in to Slack the normal way; msga picks the session up from a private, throwaway browser profile and closes the window. Nothing to register, nothing to build, no tokens to copy, and everything runs on your own personal quota. New messages arrive within a few seconds. (Already have the Slack desktop app on Linux? One click imports its session instead.)
- **Your own Slack app** — register a free Slack app for instant real-time push (Socket Mode), then paste its keys into **Settings → System**.

Both are covered step by step in the **[Slack setup guide](docs/SETUP_SLACK.md)**. Most people want session sign-in — it needs no setup at all.

## Or build your own version

If you'd rather build msga yourself (or you want to bake your own app keys into the binary), you register each service you want to connect and configure it before the client will connect. This is a one-time setup. Note that **Slack session sign-in needs none of this** — it works with a plain prebuilt binary.

### Step 1. Set up a messaging backend

Pick the service(s) you want and follow the matching guide. Each ends by writing the credentials into `credentials.cmake`:

- **[Slack setup](docs/SETUP_SLACK.md)** — create a Slack app, scopes, socket mode, events.
- **[Microsoft Teams setup](docs/SETUP_TEAMS.md)** — register an Entra app, Graph permissions, admin consent.

You can configure more than one — each fills in its own values in the same `credentials.cmake`, and the workspaces stack in the same rail.

The app version lives in `version.cmake` (tracked in git). Increment `MSGA_VERSION` there before each public release.

### Step 2. Configure build environment

> **Prerequisites:** CMake ≥ 3.24, Qt 6.5+ dev packages (incl. the WebSockets module), a C++20 compiler (GCC 12+ / Clang 14+ / MSVC 2022+)

> Distro Qt packages are often older than 6.5 (Ubuntu 24.04 ships 6.4). If yours is,
> install Qt from the [online installer](https://www.qt.io/download-qt-installer) —
> tick **Qt WebSockets**, it is not selected by default. A kit under `~/Qt` is picked
> up automatically; anywhere else, name it with `QT_PREFIX`:
> `QT_PREFIX=$HOME/Qt/6.9.0/gcc_64 ./scripts/build.sh`.
>
> `QT_PREFIX` works the same way for every build script (`build.sh`, `release.sh`,
> `run-tests.sh`, `coverage.sh`, `build.ps1`) and for a plain `cmake -B build -S .`.
> Changing it reconfigures the build directory instead of silently reusing the old Qt.

```sh
./scripts/configure-linux.sh    # for Linux
./scripts/configure-mac.sh      # for macOS
scripts/configure-windows.ps1   # for Windows
```

On Windows, run the PowerShell script from an **elevated** (Administrator) terminal:

```powershell
scripts\configure-windows.ps1
```

If you see _"running scripts is disabled"_, enable local scripts first (one-time, machine-wide):

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope LocalMachine
```

Or bypass the policy for a single run without changing the system setting:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\configure-windows.ps1
```

### Step 3. Build and run

```sh
./scripts/build.sh
./build/msga                            # for Linux
./build/msga.app/Contents/MacOS/msga    # for macOS
```

On Windows:

```powershell
scripts\build.ps1
build\Debug\msga.exe
```

If you see _"running scripts is disabled"_, see the execution policy note in Step 2, or run directly:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```
