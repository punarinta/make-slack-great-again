// The in-app updater (app/update/updater.h): the manifest, the asset names
// the release scripts publish, the SHA-256 check, and a whole check →
// download → install against the local fake server (never msga.app).
#include "support/fake_slack_server.h"
#include "app/update/updater.h"
#include "base/crypto.h"
#include "base/file.h"
#include "support/test.h"

#include <cstdlib>
#include <string>
#include <vector>

using update::Updater;
using Kind = Updater::Event::Kind;

TEST("update: the manifest the release scripts write") {
    update::Manifest m;
    CHECK(
        update::parseManifest(
            R"({"version":37,"updatedAt":"2026-10-01T10:00:00Z","sha256":"ABCdef01"})", &m
        )
    );
    CHECK(m.version == 37);
    CHECK_STR(m.sha256, "abcdef01");
    CHECK(update::parseManifest(R"({"version":12,"updatedAt":"x"})", &m)); // before the hashes
    CHECK(m.version == 12);
    CHECK(m.sha256.empty());
    CHECK_FALSE(update::parseManifest(R"({"version":0})", &m));
    CHECK_FALSE(update::parseManifest(R"({"updatedAt":"x"})", &m));
    CHECK_FALSE(update::parseManifest("<html>404</html>", &m));
}

TEST("update: msga.app's asset names per platform") {
    CHECK_STR(update::assetFor("linux", "x86_64"), "msga-linux-x86_64");
    CHECK_STR(update::assetFor("windows", "x86_64"), "msga-windows-x86_64.exe");
    CHECK_STR(update::assetFor("macos", "arm64"), "msga-macos-arm64.dmg");
    CHECK(update::assetFor("linux", "arm64").empty());
    CHECK_STR(update::assetFor("macos", "x86_64"), "msga-macos-x86_64.dmg");
    // As scripts/release-*.sh write them and msga.app serves them (issue #95:
    // "<asset>.manifest" 404'd on Windows and macOS).
    CHECK_STR(update::manifestFor("msga-linux-x86_64"), "msga-linux-x86_64.manifest");
    CHECK_STR(update::manifestFor("msga-windows-x86_64.exe"), "msga-windows-x86_64.manifest");
    CHECK_STR(update::manifestFor("msga-macos-arm64.dmg"), "msga-macos-arm64.manifest");
    CHECK_STR(update::manifestFor("msga-macos-x86_64.dmg"), "msga-macos-x86_64.manifest");
    CHECK(update::manifestFor("").empty());
#if defined(__linux__) && defined(__x86_64__)
    CHECK_STR(update::assetUrl(), "https://msga.app/download/msga-linux-x86_64");
    CHECK_STR(update::manifestUrl(), "https://msga.app/download/msga-linux-x86_64.manifest");
#endif
}

TEST("update: the download must hash to the manifest's SHA-256") {
    const std::string body = "the new msga";
    const std::string sha  = crypto::hex(crypto::bytes(crypto::sha256(body)));
    CHECK(update::checksumMatches(body, sha));
    std::string upper = sha;
    for (char &c : upper)
        if (c >= 'a' && c <= 'f')
            c = char(c - 32);
    CHECK(update::checksumMatches(body, upper));
    CHECK_FALSE(update::checksumMatches(body + "!", sha));
    CHECK_FALSE(update::checksumMatches(body, sha.substr(1)));
    CHECK(update::checksumMatches(body, "")); // an old manifest: unchecked
}

#ifndef _WIN32

namespace {

struct Run {
    std::vector<Updater::Event> events;
    bool                        done(Kind k) const {
        for (const auto &e : events)
            if (e.kind == k)
                return true;
        return false;
    }
    bool finished() const {
        return done(Kind::UpToDate) || done(Kind::Ready) || done(Kind::Failed);
    }
};

std::string tempTarget() {
    const std::string d = base::test::makeTempDir("msga_update_");
    return d.empty() ? std::string() : d + "/msga";
}

} // namespace

TEST("update: a newer manifest downloads, verifies and replaces the binary") {
    const std::string &srv = fakeslack::server();
    if (srv.empty())
        return; // no python3
    net::Client       client(fakeslack::app());
    const std::string target = tempTarget();
    REQUIRE(file::writeAtomic(target, "old binary", 0755));
    const std::string body = "bytes of msga-new"; // what the fake serves for that path
    const std::string sha  = crypto::hex(crypto::bytes(crypto::sha256(body)));

    Updater u(fakeslack::app(), client, 36);
    u.setTarget(target);
    int64_t checked = 0;
    u.onChecked     = [&](int64_t when) { checked = when; };
    Run run;
    u.listen([&](const Updater::Event &e) { run.events.push_back(e); });

    // Same version: up to date, nothing downloaded.
    u.setUrls(srv + "/update/36/-.manifest", srv + "/files-pri/u/msga-new");
    u.checkNow();
    REQUIRE(fakeslack::pumpUntil([&] { return run.finished(); }));
    CHECK(run.events.front().kind == Kind::Started);
    CHECK(run.done(Kind::UpToDate));
    CHECK(checked > 0);
    std::string have;
    CHECK((file::readAll(target, &have) && have == "old binary"));

    // A newer one: Available, then Ready with the new bytes in place.
    run.events.clear();
    u.setUrls(srv + "/update/37/" + sha + ".manifest", srv + "/files-pri/u/msga-new");
    u.checkNow();
    REQUIRE(fakeslack::pumpUntil([&] { return run.finished(); }));
    REQUIRE(run.done(Kind::Ready));
    CHECK(run.events[1].kind == Kind::Available);
    CHECK(run.events[1].version == 37);
    // Download progress: the percentage, all of it before Ready.
    REQUIRE(run.events.size() >= 4);
    CHECK(run.events[run.events.size() - 2].kind == Kind::Progress);
    CHECK(run.events[run.events.size() - 2].percent == 100);
    CHECK(u.ready());
    CHECK_STR(u.downloadedPath(), target);
    CHECK((file::readAll(target, &have) && have == body));
    CHECK_FALSE(file::exists(target + ".part")); // streamed there, then moved into place
    file::remove(target);
    file::remove(file::dirName(target));
}

TEST("update: a checksum mismatch leaves the binary alone") {
    const std::string &srv = fakeslack::server();
    if (srv.empty())
        return;
    net::Client       client(fakeslack::app());
    const std::string target = tempTarget();
    REQUIRE(file::writeAtomic(target, "old binary", 0755));
    Updater u(fakeslack::app(), client, 36);
    u.setTarget(target);
    Run run;
    u.listen([&](const Updater::Event &e) { run.events.push_back(e); });
    const std::string wrong(64, 'a');
    u.setUrls(srv + "/update/40/" + wrong + ".manifest", srv + "/files-pri/u/msga-new");
    u.checkNow();
    REQUIRE(fakeslack::pumpUntil([&] { return run.finished(); }));
    REQUIRE(run.done(Kind::Failed));
    CHECK_STR(run.events.back().message, "Downloaded update is corrupt (checksum mismatch).");
    CHECK_FALSE(u.ready());
    std::string have;
    CHECK((file::readAll(target, &have) && have == "old binary"));
    CHECK_FALSE(file::exists(target + ".part")); // the unverified download is gone

    // A download the server refuses: nothing written, nothing left behind.
    run.events.clear();
    u.setUrls(srv + "/update/41/" + wrong + ".manifest", srv + "/files-pri/u/missing");
    u.checkNow();
    REQUIRE(fakeslack::pumpUntil([&] { return run.finished(); }));
    REQUIRE(run.done(Kind::Failed));
    CHECK_STR(run.events.back().message, "Download failed: HTTP 404");
    CHECK((file::readAll(target, &have) && have == "old binary"));
    CHECK_FALSE(file::exists(target + ".part"));

    // A target that can't be written next to: the download stops at its first bytes.
    run.events.clear();
    u.setTarget(std::string(file::dirName(target)) + "/no-such-dir/x/msga");
    REQUIRE(
        file::writeAtomic(std::string(file::dirName(target)) + "/no-such-dir", "a file, not a dir")
    );
    u.setUrls(srv + "/update/42/-.manifest", srv + "/files-pri/u/msga-new");
    u.checkNow();
    REQUIRE(fakeslack::pumpUntil([&] { return run.finished(); }));
    REQUIRE(run.done(Kind::Failed));
    CHECK_FALSE(u.ready());
    file::remove(std::string(file::dirName(target)) + "/no-such-dir");
    u.setTarget(target);

    // Not a manifest at all; and a background check with auto-checks off
    // never asks.
    run.events.clear();
    u.setUrls(srv + "/files-pri/u/manifest", srv + "/files-pri/u/msga-new");
    u.checkNow();
    REQUIRE(fakeslack::pumpUntil([&] { return run.finished(); }));
    CHECK_STR(run.events.back().message, "Could not parse version manifest.");
    run.events.clear();
    u.checkInBackground(false);
    fakeslack::pumpFor(100);
    CHECK(run.events.empty());
    CHECK_FALSE(u.busy());
    file::remove(target);
    file::remove(file::dirName(target));
}

#endif
