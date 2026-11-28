#include "app/update/updater.h"

#include "app/model/jobs.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "net/net.h"
#include "plat/plat.h"

#include <algorithm>

#ifdef _WIN32
#include "base/winstr.h"

#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

using i18n::arg;
using i18n::tr;

namespace update {

namespace {

constexpr const char *kBase              = "https://msga.app/download/";
// The whole binary in one answer: minutes, not the default 30 s.
constexpr int         kDownloadTimeoutMs = 10 * 60'000;

#ifdef _WIN32
using base::widePath;
#endif

// Why the download couldn't be written next to `target` (at `part`).
std::string writeFailed(const std::string &target, const std::string &part) {
#if defined(_WIN32)
    (void)target;
    return arg(tr("Cannot write update to %1"), part);
#elif defined(__APPLE__)
    (void)part;
    return arg(tr("Cannot write update to %1"), target);
#else
    (void)part;
    return arg(tr("Could not replace binary: %1"), target);
#endif
}

// The download, written to <target>.part on the net worker as it arrives
// and hashed on the way (net::Handler); checked and moved into place on a
// worker once it is all there. One that never got there removes its file.
class PartFile final : public net::Handler {
public:
    explicit PartFile(std::string t) : target(std::move(t)), part(target + ".part") {
        streamBody = true;
    }
    ~PartFile() override { discard(); }

    bool body(const char *data, size_t n) override {
        if (!open())
            return false;
        sha.update({data, n});
        if (put(data, n))
            return true;
        error = writeFailed(target, part);
        return false;
    }
    // A worker's, after the last byte: flushed to disk and closed. False:
    // `error` says why.
    bool finish() {
        if (!open())
            return false;
        if (close(true))
            return true;
        error = writeFailed(target, part);
        return false;
    }
    // Closes and removes what was written (a failure, a bad hash).
    void discard() {
        close(false);
        if (_opened && !_installed)
            file::remove(part);
        _opened = false;
    }
    void installed() { _installed = true; }

    const std::string target, part;
    crypto::Sha256    sha;
    std::string       error; // why a write failed

private:
    // Made (or truncated) at the first byte, on the worker; false once
    // anything failed.
    bool open() {
        if (_opened)
            return isOpen() && error.empty();
        _opened = true;
        file::makeDirs(file::dirName(part));
#if defined(_WIN32)
        _fd = CreateFileW(
            widePath(part).c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
#else
#ifdef __APPLE__
        const mode_t mode = 0644; // the DMG
#else
        const mode_t mode = 0755; // the binary
#endif
        _fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
#endif
        if (!isOpen())
            error = writeFailed(target, part);
        return error.empty();
    }
    bool put(const char *data, size_t n) {
        while (n) {
#ifdef _WIN32
            DWORD w = 0;
            if (!WriteFile(_fd, data, DWORD(std::min<size_t>(n, 1u << 30)), &w, nullptr) || !w)
                return false;
#else
            const ssize_t w = ::write(_fd, data, n);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                return false;
#endif
            data += w;
            n -= size_t(w);
        }
        return true;
    }
    // durable: flushed to disk first (before the rename, or a crash can
    // leave the renamed file empty). False when anything failed.
    bool close(bool durable) {
        if (!isOpen())
            return false;
#ifdef _WIN32
        bool ok = !durable || FlushFileBuffers(_fd);
        ok      = CloseHandle(_fd) && ok;
        _fd     = INVALID_HANDLE_VALUE;
#else
        bool ok = !durable || ::fsync(_fd) == 0;
        ok      = ::close(_fd) == 0 && ok;
        _fd     = -1;
#endif
        return ok;
    }
#ifdef _WIN32
    bool   isOpen() const { return _fd != INVALID_HANDLE_VALUE; }
    HANDLE _fd = INVALID_HANDLE_VALUE;
#else
    bool isOpen() const { return _fd >= 0; }
    int  _fd = -1;
#endif
    bool _opened = false, _installed = false;
};

// Puts the verified download (closed, at part.part) in place (a worker
// thread); "" or why not.
std::string install(PartFile &part) {
    const std::string &target = part.target, &tmp = part.part;
#if defined(_WIN32)
    // Windows locks a running .exe against replacing, not against renaming:
    // the current one moves aside first, then the new one takes its name.
    const std::string backup = target + ".old";
    file::remove(backup);
    if (!MoveFileExW(widePath(target).c_str(), widePath(backup).c_str(), MOVEFILE_REPLACE_EXISTING))
        return arg(
            tr("Could not move current binary \xE2\x80\x94 check file permissions on %1"), target
        );
    if (!MoveFileExW(widePath(tmp).c_str(), widePath(target).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(widePath(backup).c_str(), widePath(target).c_str(), 0); // best-effort restore
        return arg(tr("Could not place new binary at %1"), target);
    }
#else
    // Renamed over the target (Linux: the binary, macOS: the DMG): the
    // directory entry swaps atomically, a running process keeps its inode.
    if (::rename(tmp.c_str(), target.c_str()) != 0)
        return writeFailed(target, tmp);
#endif
    part.installed();
    return {};
}

bool digestMatches(const std::array<uint8_t, 32> &digest, std::string_view expectedHex) {
    if (expectedHex.empty())
        return true; // older manifests carry no hash: unchecked
    return str::iequals(crypto::hex(crypto::bytes(digest)), expectedHex);
}

} // namespace

bool parseManifest(std::string_view text, Manifest *out) {
    json::Document d;
    if (!d.parse(std::string(text), nullptr))
        return false;
    const int64_t v = d.root()["version"].integer();
    if (v <= 0 || v > 1'000'000'000)
        return false;
    out->version = int(v);
    out->sha256  = str::asciiLower(d.root()["sha256"].str());
    return true;
}

std::string assetFor(std::string_view os, std::string_view arch) {
    if (os == "linux" && arch == "x86_64")
        return "msga-linux-x86_64";
    if (os == "windows" && arch == "x86_64")
        return "msga-windows-x86_64.exe";
    if (os == "macos" && arch == "arm64")
        return "msga-macos-arm64.dmg";
    if (os == "macos" && arch == "x86_64")
        return "msga-macos-x86_64.dmg";
    return {};
}

std::string asset() {
#if defined(_WIN32)
    const char *os = "windows";
#elif defined(__APPLE__)
    const char *os = "macos";
#elif defined(__linux__)
    const char *os = "linux";
#else
    const char *os = "";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char *arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char *arch = "arm64";
#else
    const char *arch = "";
#endif
    return assetFor(os, arch);
}

std::string assetUrl() {
    const std::string a = asset();
    return a.empty() ? std::string() : kBase + a;
}

std::string manifestFor(std::string_view asset) {
    if (asset.empty())
        return {};
    // The release scripts name it after the asset without its extension.
    return str::concat({asset.substr(0, asset.rfind('.')), ".manifest"});
}

std::string manifestUrl() {
    const std::string m = manifestFor(asset());
    return m.empty() ? std::string() : kBase + m;
}

bool checksumMatches(std::string_view bytes, std::string_view expectedHex) {
    return digestMatches(crypto::sha256(bytes), expectedHex);
}

// ── Updater ─────────────────────────────────────────────────────────────────

Updater::Updater(plat::App &app, net::Client &client, int currentVersion)
    : _app(app), _client(client), _current(currentVersion), _manifestUrl(manifestUrl()),
      _assetUrl(assetUrl()) {
#if defined(__APPLE__)
    const std::string dl = app.standardDir(plat::StandardDir::Downloads);
    if (!dl.empty() && !asset().empty())
        _target = file::join(dl, asset());
#else
    _target = base::executablePath();
#endif
#ifdef _WIN32
    // The backup the previous update's rename-away step left.
    if (!_target.empty())
        model::runInBackground(app, [old = _target + ".old"] { file::remove(old); }, [] {});
#endif
}

Updater::~Updater() = default;

void Updater::setUrls(std::string manifest, std::string asset) {
    _manifestUrl = std::move(manifest);
    _assetUrl    = std::move(asset);
}

int Updater::listen(Listener fn) {
    _listeners.push_back({_nextId, std::move(fn)});
    return _nextId++;
}

void Updater::unlisten(int id) {
    std::erase_if(_listeners, [id](const Slot &s) { return s.id == id; });
}

void Updater::emit(Event e) {
    const std::vector<Slot> copy = _listeners; // a listener may unlisten
    for (const Slot &s : copy)
        if (s.fn)
            s.fn(e);
}

void Updater::checkInBackground(bool autoCheck) {
    if (!autoCheck || _busy || _ready)
        return;
    fetch(true);
}

void Updater::checkNow() {
    if (_busy)
        return;
    emit({Event::Kind::Started, 0, {}});
    fetch(false);
}

void Updater::fetch(bool silent) {
    if (_manifestUrl.empty() || _target.empty()) {
        if (!silent)
            emit(
                {Event::Kind::Failed,
                 0,
                 tr("Automatic updates are not supported on this platform.")}
            );
        return;
    }
    _busy = true;
    net::Request req;
    req.url                   = _manifestUrl;
    std::weak_ptr<char> alive = _alive;
    _client.send(std::move(req), [this, alive, silent](net::Response r) {
        if (alive.expired())
            return;
        _busy = false;
        if (!r.ok()) {
            LOG_WARN("update", "manifest: %s (HTTP %d)", r.error.c_str(), r.status);
            if (!silent)
                emit(
                    {Event::Kind::Failed,
                     0,
                     r.error.empty() ? "HTTP " + str::number(int64_t(r.status)) : r.error}
                );
            return;
        }
        Manifest m;
        if (!parseManifest(r.body, &m)) {
            if (!silent)
                emit({Event::Kind::Failed, 0, tr("Could not parse version manifest.")});
            return;
        }
        if (onChecked)
            onChecked(base::nowSecs());
        if (m.version <= _current) {
            emit({Event::Kind::UpToDate, m.version, {}});
            return;
        }
        emit({Event::Kind::Available, m.version, {}});
        download(m.version, std::move(m.sha256));
    });
}

void Updater::download(int version, std::string sha256) {
    _busy = true;
    const int job =
        model::jobs().begin(arg(tr("Downloading %1"), "msga " + str::number(int64_t(version))));
    net::Request req;
    req.url                   = _assetUrl;
    req.timeoutMs             = kDownloadTimeoutMs;
    std::weak_ptr<char> alive = _alive;
    // Download progress: only when the server says the size.
    req.onProgress            = [this, alive, last = -1](int64_t got, int64_t total) mutable {
        if (alive.expired() || total <= 0)
            return;
        const int pct = int(got * 100 / total);
        if (pct == last)
            return;
        last = pct;
        Event e;
        e.kind    = Event::Kind::Progress;
        e.percent = pct;
        emit(std::move(e));
    };
    // Written and hashed on the net worker as it arrives (PartFile).
    auto part   = std::make_shared<PartFile>(_target);
    req.handler = part;
    _client.send(std::move(req), [this, alive, job, sha256, part](net::Response r) {
        if (alive.expired()) { // the file goes with the last PartFile reference
            model::jobs().end(job);
            return;
        }
        auto err = std::make_shared<std::string>();
        if (!r.ok())
            *err = !part->error.empty() ? part->error
                   : r.error.empty()
                       ? arg(tr("Download failed: %1"), "HTTP " + str::number(int64_t(r.status)))
                       : arg(tr("Download failed: %1"), r.error);
        // Closed, checked and moved into place off the UI thread (an fsync of
        // tens of megabytes); a failed or unverified one is removed there.
        model::runInBackground(
            _app,
            [part, err, sha256] {
                if (err->empty() && part->finish()) {
                    if (!digestMatches(part->sha.finish(), sha256))
                        *err = tr("Downloaded update is corrupt (checksum mismatch).");
                    else
                        *err = install(*part);
                } else if (err->empty()) {
                    *err = part->error;
                }
                part->discard();
            },
            [this, alive, job, err] {
                model::jobs().end(job);
                if (alive.expired())
                    return;
                _busy = false;
                if (!err->empty()) {
                    LOG_WARN("update", "%s", err->c_str());
                    emit({Event::Kind::Failed, 0, *err});
                    return;
                }
                _ready      = true;
                _downloaded = _target;
                emit({Event::Kind::Ready, 0, {}});
            }
        );
    });
}

} // namespace update
