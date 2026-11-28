// RemoteImages — http(s) pictures on disk, for the image caches.
//
// ImageCache (screens/messages) and Avatars (screens/shell) take a path; when
// that path is an http(s) URL they ask this class for a local copy and decode
// the file as they would any other:
//
//   std::string local = remote.cachedPath(url);    // on disk already: use it
//   if (local.empty())
//       remote.fetch(url, [](const std::string &path) { /* "" = failed */ });
//
// Files live in <cacheDir>/images (app/identity.h)/<sha256(url)>, so the same URL is
// downloaded once per machine and survives restarts. The directory is
// bounded by Settings → Storage → "Limit cache to": least recently used first,
// "used" being the file's mtime (bumped on every cache hit), swept shortly
// after start, every 30 minutes, after 32 MB of new downloads and when the
// limit changes. The other folders the old evictor covered join through
// coverDirs(): the audio player's and the opened HTML files' downloads are
// blobs swept (and cleared) with the pictures; the workspaces' cached data
// counts toward the limit but is never evicted.
//
// Downloads go through the app's one net::Client, at most kMaxParallel at a
// time (the client's 4 workers also carry the Slack API, which must not
// queue behind a channel full of screenshots); the rest wait in order, and a
// URL asked for twice is fetched once. A failed URL (no answer, an error
// status, or bytes that are not an image — Slack's sign-in page instead of a
// file) is not asked for again for kCooldownMs;
// setAuth() forgets those failures, since a workspace signing in is the
// usual cure.
//
// Auth: Slack serves files (files.slack.com) only with the workspace's token
// and, for session auth, its d cookie. The hook the accounts controller sets
// adds them per URL; nothing else here knows about Slack.
//
// Threading: UI thread only. Disk writes, mtime bumps and sweeps run on one
// worker thread; every callback runs later on the UI thread, never inside
// the call that started it.
#pragma once

#include "net/net.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace screens {

class RemoteImages {
public:
    using Done     = std::function<void(const std::string &path)>; // "" on failure
    using AuthHook = std::function<void(const std::string &url, std::vector<net::Header> &)>;
    // Tests replace the network (net::Client otherwise). Like Client::send,
    // it must answer later, never inside the call.
    using Fetch    = std::function<void(net::Request, std::function<void(net::Response)>)>;

    static constexpr int kMaxParallel = 2;
    static constexpr int kCooldownMs  = 60000;

    // `client` may be null (then every fetch fails); `dir` "" disables the
    // disk (likewise).
    RemoteImages(plat::App &app, net::Client *client, std::string dir);
    ~RemoteImages(); // cancels downloads; no Done runs afterwards
    RemoteImages(const RemoteImages &)            = delete;
    RemoteImages &operator=(const RemoteImages &) = delete;

    static bool        isRemote(std::string_view path); // http:// or https://
    // <cacheDir>/images (app/identity.h), or "" when the OS has no cache directory.
    static std::string defaultDir(plat::App &app);

    // The local copy of `url` if it is on disk (and marks it used), else "".
    std::string cachedPath(const std::string &url);
    // Downloads `url` to disk (or joins the download already running).
    void        fetch(const std::string &url, Done done);
    // A recent failure: fetch() would fail at once.
    bool        failedRecently(const std::string &url) const;

    void    setAuth(AuthHook hook);
    void    setFetch(Fetch fetch);
    // More folders under the same limit (see above): `blobs` are swept and
    // cleared like the pictures (flat folders), `kept` only count (walked
    // recursively). Paths may not exist yet.
    void    coverDirs(std::vector<std::string> blobs, std::vector<std::string> kept);
    // A file just written into a blob folder.
    void    noteWritten(const std::string &path);
    // Settings → Storage: the disk bound (sweeps when it shrinks).
    void    setLimitMb(int mb);
    // Settings → "Clear cache": deletes every blob (pictures, audio, files).
    void    clear();
    // Blob bytes on disk as of the last sweep plus what was written since
    // (the kept folders not included).
    int64_t diskBytes() const;
    // Downloads running or waiting (tests pump until this is 0).
    size_t  pending() const;
    // The next sweep, now (tests); `done` runs on the UI thread after it.
    void    sweepNow(std::function<void()> done = {});

    struct Impl;

private:
    std::shared_ptr<Impl> _impl;
};

} // namespace screens
