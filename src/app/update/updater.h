// The in-app updater: fetches this platform's manifest
// from msga.app ({"version": N, "sha256": "…"}), compares it with the running
// version, and downloads the new release asset when there is one, checking
// its SHA-256 against the manifest (older manifests carry none: unchecked).
//
//   Linux    the download replaces the running binary atomically (the kernel
//            keeps the old inode running); a restart picks it up.
//   Windows  the running .exe is renamed to <exe>.old (Windows allows that,
//            not a replace) and the new one moved into its place; the .old is
//            removed on the next start.
//   macOS    the DMG is saved to Downloads; "Open installer" opens it.
//
// Network on net::Client; the download is written to <target>.part and hashed
// on the net worker as it arrives, then checked and moved into place on a
// background thread (a failed or unverified one is removed); a job in the
// footer's list. Every callback runs on the UI thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace net {
class Client;
}
namespace plat {
class App;
}

namespace update {

// ── Pure parts (tests) ──────────────────────────────────────────────────────

struct Manifest {
    int         version = 0;
    std::string sha256; // lower-case hex; "" = the manifest has none
};
// False when it is no manifest (no positive "version").
bool        parseManifest(std::string_view json, Manifest *out);
// msga's release asset for an OS / CPU ("linux"/"windows"/"macos",
// "x86_64"/"arm64"): "msga-linux-x86_64", "msga-windows-x86_64.exe",
// "msga-macos-arm64.dmg", "msga-macos-x86_64.dmg"; "" where no release is built.
std::string assetFor(std::string_view os, std::string_view arch);
std::string asset(); // this build's
// An asset's manifest, as the release scripts publish it: the asset's name
// without its extension + ".manifest" ("msga-windows-x86_64.exe" →
// "msga-windows-x86_64.manifest").
std::string manifestFor(std::string_view asset);
// https://msga.app/download/<asset> / <manifestFor(asset)>; "" where unsupported.
std::string assetUrl();
std::string manifestUrl();
// `bytes` hash to `expectedHex` (case-insensitive); an empty expectation
// passes (a manifest from before the checks).
bool        checksumMatches(std::string_view bytes, std::string_view expectedHex);

// ── The checker ─────────────────────────────────────────────────────────────

class Updater {
public:
    Updater(plat::App &app, net::Client &client, int currentVersion);
    ~Updater();
    Updater(const Updater &)            = delete;
    Updater &operator=(const Updater &) = delete;

    // At startup (a few seconds in): silent, no-op on failure — and nothing
    // at all, not even the manifest, with automatic checks off.
    void checkInBackground(bool autoCheck);
    // Settings → "Check for updates": always ends in UpToDate, Ready or Failed.
    void checkNow();

    bool               busy() const { return _busy; } // checking or downloading
    bool               ready() const { return _ready; }
    // The new binary (Linux, Windows: the app's own path) or the DMG (macOS).
    const std::string &downloadedPath() const { return _downloaded; }

    struct Event {
        enum class Kind : uint8_t {
            Started,   // checkNow began
            UpToDate,  // the manifest's version is not newer
            Available, // `version` is newer: downloading
            Progress,  // `percent` of the download is in (servers that say its size)
            Ready,     // downloaded (and installed where the OS allows)
            Failed,    // `message` says why
        };
        Kind        kind    = Kind::Started;
        int         version = 0;
        std::string message;
        int         percent = 0;
    };
    using Listener = std::function<void(const Event &)>;
    int  listen(Listener fn); // an id for unlisten
    void unlisten(int id);

    // A manifest was read (the "Last checked" time, epoch secs): persist it.
    std::function<void(int64_t when)> onChecked;

    // Tests: where the update goes instead of the running binary / Downloads,
    // and the manifest / asset URLs instead of msga.app's.
    void setTarget(std::string path) { _target = std::move(path); }
    void setUrls(std::string manifest, std::string assetUrl);

private:
    void fetch(bool silent);
    void download(int version, std::string sha256);
    void emit(Event e);

    plat::App   &_app;
    net::Client &_client;
    int          _current;
    bool         _busy = false, _ready = false;
    std::string  _downloaded, _target, _manifestUrl, _assetUrl;
    struct Slot {
        int      id;
        Listener fn;
    };
    std::vector<Slot>     _listeners;
    int                   _nextId = 1;
    std::shared_ptr<char> _alive  = std::make_shared<char>(0);
};

} // namespace update
