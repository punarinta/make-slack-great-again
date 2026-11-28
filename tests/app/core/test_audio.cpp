// plat audio (plat/audio.h) and the notification sounds on top of it
// (app/media/sounds.h), driven through fake helper programs: Linux audio is
// the sound server's CLI tools, found only in $PLAT_AUDIO_HELPERS, so each
// test points that at a directory of shell scripts that record what they
// were given. Nothing here reaches the speakers or the microphone.
#include "app/media/audio_errors.h"
#include "app/media/sounds.h"
#include "base/file.h"
#include "base/i18n.h"
#include "support/test.h"
#include "plat/audio.h"
#include "plat/plat.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using plat::audio::Error;
using plat::audio::Failure;

namespace {

plat::App &app() {
    static std::unique_ptr<plat::App> a = plat::App::create();
    return *a;
}

bool pumpUntil(const std::function<bool()> &done, int timeoutMs) {
    for (int waited = 0; !done(); waited += 10) {
        if (waited >= timeoutMs)
            return false;
        app().pump(10);
    }
    return true;
}

[[maybe_unused]] void pumpFor(int ms) {
    for (int waited = 0; waited < ms; waited += 10)
        app().pump(10);
}

// A fresh directory for one test's helpers and files.
struct Scratch {
    std::string dir;
    Scratch() {
        dir = base::test::makeTempDir("msga-audio-test-");
        base::test::setEnv("PLAT_AUDIO_HELPERS", dir);
    }
    ~Scratch() {
        base::test::setEnv("PLAT_AUDIO_HELPERS", "/nonexistent/msga-test-audio");
        std::string cmd = "rm -rf '" + dir + "'";
        (void)!std::system(cmd.c_str());
    }
    std::string path(std::string_view name) const { return file::join(dir, name); }
    // A helper program: a shell script with `body`.
    void        helper(const char *name, const std::string &body) const {
        const std::string p = path(name);
        file::writeAtomic(p, "#!/bin/sh\n" + body + "\n", 0755);
    }
    std::string read(std::string_view name) const {
        std::string s;
        file::readAll(path(name), &s);
        return s;
    }
};

std::string chimePath() {
    return file::join(MSGA_SOURCE_DIR, "sfx/notify.wav");
}

// The chime's PCM: its WAV file minus the 44-byte header.
[[maybe_unused]] std::string chimePcm() {
    std::string wav;
    file::readAll(chimePath(), &wav);
    return wav.size() > 44 ? wav.substr(44) : std::string();
}

struct PlayerProbe {
    bool    loaded = false, ended = false, failed = false;
    Failure failure;
    void    attach(plat::audio::Player &p) {
        p.onLoaded = [this] { loaded = true; };
        p.onEnded  = [this] { ended = true; };
        p.onFailed = [this](const Failure &f) {
            failed  = true;
            failure = f;
        };
    }
};

} // namespace

TEST("audio: wavFromPcm16 writes a plain 44-byte RIFF/WAVE header") {
    const std::string wav = plat::audio::wavFromPcm16(std::string("\x01\x02\x03\x04", 4), 16000, 1);
    REQUIRE(wav.size() == 48);
    CHECK(wav.compare(0, 4, "RIFF") == 0);
    CHECK(wav.compare(8, 8, "WAVEfmt ") == 0);
    CHECK(wav.compare(36, 4, "data") == 0);
    auto le32 = [&](size_t at) {
        const auto *p = reinterpret_cast<const unsigned char *>(wav.data() + at);
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    CHECK(le32(4) == 40);     // RIFF size
    CHECK(le32(24) == 16000); // sample rate
    CHECK(le32(28) == 32000); // byte rate
    CHECK(le32(40) == 4);     // data size
    CHECK(wav[22] == 1);      // channels
    CHECK(wav[34] == 16);     // bits
    CHECK(wav.substr(44) == std::string("\x01\x02\x03\x04", 4));
    // The chime the app bundles is such a file, and its header matches ours.
    std::string chime;
    REQUIRE(file::readAll(chimePath(), &chime));
    CHECK(chime.substr(0, 44) == plat::audio::wavFromPcm16(chimePcm(), 16000, 1).substr(0, 44));
}

#if defined(MSGA_FREEDESKTOP)
// Linux and the BSDs: audio is the sound server's CLI helpers, faked here.

TEST("audio: the player decodes in-process and pipes the PCM to pw-cat") {
    Scratch s;
    // The output helper: records its arguments and everything it is fed.
    s.helper("pw-cat", "echo \"$@\" > '" + s.path("args") + "'\ncat > '" + s.path("pcm") + "'");
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(chimePath());
    CHECK_FALSE(probe.loaded); // never inside the call
    REQUIRE(pumpUntil([&] { return probe.loaded || probe.failed; }, 5000));
    REQUIRE(probe.loaded);
    CHECK(p->durationMs() == 400); // 6400 frames at 16 kHz
    CHECK(p->positionMs() == 0);
    p->play();
    REQUIRE(pumpUntil([&] { return probe.ended || probe.failed; }, 5000));
    CHECK(probe.ended);
    CHECK_FALSE(probe.failed);
    CHECK_STR(s.read("args"), "--playback --raw --format s16 --rate 16000 --channels 1 -\n");
    CHECK(s.read("pcm") == chimePcm()); // bit-exact decode → PCM
    CHECK(p->positionMs() == 400);      // at the end
    // Played again from the start after a seek.
    p->seek(0);
    CHECK(p->positionMs() == 0);
    probe.ended = false;
    p->play();
    REQUIRE(pumpUntil([&] { return probe.ended; }, 5000));
}

TEST("audio: MP3 and Ogg Vorbis decode in-process too") {
    // Test files made by the machine's ffmpeg when it has one (never used to
    // decode here: the helpers dir has only the fake pw-cat).
    Scratch s;
    s.helper("pw-cat", "echo \"$@\" > '" + s.path("args") + "'\ncat > '" + s.path("pcm") + "'");
    for (const char *ext : {"mp3", "ogg"}) {
        const std::string out = s.path(std::string("chime.") + ext);
        const std::string cmd = "ffmpeg -nostdin -loglevel quiet -y -i '" + chimePath() + "' " +
                                (std::string(ext) == "ogg" ? "-c:a libvorbis " : "") + "'" + out +
                                "' 2>/dev/null";
        if (std::system(cmd.c_str()) != 0 || !file::exists(out)) {
            std::printf("  no ffmpeg (or no %s encoder): %s skipped\n", ext, ext);
            continue;
        }
        auto        p = plat::audio::Player::create(app());
        PlayerProbe probe;
        probe.attach(*p);
        p->load(out);
        REQUIRE(pumpUntil([&] { return probe.loaded || probe.failed; }, 5000));
        CHECK(probe.loaded);
        const int64_t ms = p->durationMs();
        std::printf("  %s: %lld ms\n", ext, (long long)ms);
        CHECK(ms >= 390 && ms <= 520); // MP3 adds encoder padding
        p->play();
        REQUIRE(pumpUntil([&] { return probe.ended || probe.failed; }, 5000));
        CHECK(probe.ended);
        CHECK_STR(s.read("args"), "--playback --raw --format s16 --rate 16000 --channels 1 -\n");
        const size_t bytes = s.read("pcm").size();
        CHECK(bytes >= 6400 * 2 * 95 / 100 && bytes <= 8400 * 2);
    }
}

TEST("audio: AAC (Slack's url_private transcode) plays through the real ffmpeg") {
    Scratch           s;
    const std::string m4a = s.path("chime.m4a");
    const std::string enc =
        "ffmpeg -nostdin -loglevel quiet -y -i '" + chimePath() + "' -c:a aac '" + m4a + "'";
    std::string ffmpeg;
    if (FILE *w = popen("command -v ffmpeg", "r")) {
        char buf[512] = {};
        if (std::fgets(buf, sizeof buf, w))
            ffmpeg = buf;
        pclose(w);
    }
    while (!ffmpeg.empty() && ffmpeg.back() == '\n')
        ffmpeg.pop_back();
    if (ffmpeg.empty() || std::system(enc.c_str()) != 0) {
        std::printf("  no ffmpeg with an AAC encoder: skipped\n");
        return;
    }
    REQUIRE(::symlink(ffmpeg.c_str(), s.path("ffmpeg").c_str()) == 0);
    s.helper("pw-cat", "echo \"$@\" > '" + s.path("args") + "'\ncat > '" + s.path("pcm") + "'");
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(m4a);
    REQUIRE(pumpUntil([&] { return probe.loaded || probe.failed; }, 10000));
    CHECK(probe.loaded);
    pumpUntil([&] { return p->durationMs() > 0; }, 3000);
    CHECK(p->durationMs() >= 390 && p->durationMs() <= 450);
    p->play();
    REQUIRE(pumpUntil([&] { return probe.ended || probe.failed; }, 10000));
    CHECK(probe.ended);
    CHECK_STR(s.read("args"), "--playback --raw --format s16 --rate 48000 --channels 2 -\n");
    const size_t bytes = s.read("pcm").size(); // 0.4 s of 48 kHz stereo s16
    CHECK(bytes >= 76800 * 95 / 100 && bytes <= 76800 * 115 / 100);
}

TEST("audio: a sink that dies hands over to the next helper") {
    Scratch s;
    s.helper("pw-cat", "exit 1"); // no PipeWire running
    s.helper("paplay", "cat > '" + s.path("pcm") + "'");
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(chimePath());
    REQUIRE(pumpUntil([&] { return probe.loaded; }, 5000));
    p->play();
    REQUIRE(pumpUntil([&] { return probe.ended || probe.failed; }, 5000));
    CHECK(probe.ended);
    CHECK(s.read("pcm") == chimePcm());
}

TEST("audio: sndio's aucat plays and records when nothing else is there") {
    Scratch s;
    s.helper("aucat", "echo \"$@\" > '" + s.path("args") + "'\ncat > '" + s.path("pcm") + "'");
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(chimePath());
    REQUIRE(pumpUntil([&] { return probe.loaded || probe.failed; }, 5000));
    REQUIRE(probe.loaded);
    p->play();
    REQUIRE(pumpUntil([&] { return probe.ended || probe.failed; }, 5000));
    CHECK(probe.ended);
    CHECK_STR(s.read("args"), "-h raw -e s16le -r 16000 -c 0:0 -i -\n");
    CHECK(s.read("pcm") == chimePcm());

    s.helper(
        "aucat",
        "echo \"$@\" > '" + s.path("args") +
            "'\ndd if=/dev/zero bs=3200 count=1 2>/dev/null; exec sleep 30"
    );
    auto r       = plat::audio::Recorder::create(app());
    bool started = false;
    r->onStarted = [&] { started = true; };
    r->start();
    REQUIRE(pumpUntil([&] { return started; }, 5000));
    CHECK_STR(s.read("args"), "-h raw -e s16le -r 16000 -c 0:0 -o -\n");
    r->cancel();
    CHECK_FALSE(r->isRecording());
}

TEST("audio: pause holds the position, seek moves it, stop drops the media") {
    Scratch s;
    // A slow sink: reads nothing for a while, so playback stays in progress.
    s.helper("pw-cat", "exec sleep 5");
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(chimePath());
    REQUIRE(pumpUntil([&] { return probe.loaded; }, 5000));
    p->play();
    pumpFor(150);
    p->pause();
    pumpFor(50);
    const int64_t at = p->positionMs();
    CHECK(at >= 0 && at <= 400);
    pumpFor(100);
    CHECK(p->positionMs() == at);
    p->seek(250);
    pumpUntil([&] { return p->positionMs() == 250; }, 2000);
    CHECK(p->positionMs() == 250);
    p->seek(99999); // clamped to the end
    pumpUntil([&] { return p->positionMs() == 400; }, 2000);
    CHECK(p->positionMs() == 400);
    p->stop();
    pumpUntil([&] { return p->durationMs() == 0; }, 2000);
    CHECK(p->durationMs() == 0);
    CHECK(p->positionMs() == 0);
    CHECK_FALSE(probe.failed);
    CHECK_FALSE(probe.ended);
}

TEST("audio: player failures come back as codes") {
    Scratch s;
    // No output helper at all.
    {
        auto        p = plat::audio::Player::create(app());
        PlayerProbe probe;
        probe.attach(*p);
        p->load(chimePath());
        REQUIRE(pumpUntil([&] { return probe.loaded; }, 5000));
        p->play();
        REQUIRE(pumpUntil([&] { return probe.failed; }, 5000));
        CHECK(probe.failure.code == Error::NoOutput);
        CHECK_STR(
            media::audioErrorText(probe.failure),
            "No audio output found (needs pw-cat, paplay or aplay)"
        );
    }
    // Not a format miniaudio decodes, and no ffmpeg.
    const std::string m4a = s.path("clip.m4a");
    file::writeAtomic(
        m4a,
        "\x00\x00\x00\x20"
        "ftypM4A not really audio"
    );
    CHECK_FALSE(plat::audio::canPlayExtension("m4a"));
    CHECK(plat::audio::canPlayExtension("mp3"));
    {
        auto        p = plat::audio::Player::create(app());
        PlayerProbe probe;
        probe.attach(*p);
        p->load(m4a);
        REQUIRE(pumpUntil([&] { return probe.failed; }, 5000));
        CHECK(probe.failure.code == Error::NeedsFfmpeg);
        CHECK_STR(
            media::audioErrorText(probe.failure), "This audio format needs ffmpeg installed to play"
        );
    }
    // ffmpeg that can't decode it either.
    s.helper("ffmpeg", "echo 'clip.m4a: Invalid data found when processing input' >&2; exit 1");
    CHECK(plat::audio::canPlayExtension("m4a"));
    {
        auto        p = plat::audio::Player::create(app());
        PlayerProbe probe;
        probe.attach(*p);
        p->load(m4a);
        REQUIRE(pumpUntil([&] { return probe.failed; }, 5000));
        CHECK_FALSE(probe.loaded);
        CHECK(probe.failure.code == Error::Unsupported);
        CHECK_STR(media::audioErrorText(probe.failure), "This audio format can't be played here");
    }
}

TEST("audio: other formats decode through ffmpeg, its banner giving the duration") {
    Scratch s;
    // 0.25 s of 48 kHz stereo silence, and the banner ffmpeg prints.
    s.helper(
        "ffmpeg",
        "echo \"$@\" > '" + s.path("ffargs") +
            "'\necho '  Duration: 00:00:01.50, start: 0.000000, bitrate: 64 kb/s' >&2\n"
            "dd if=/dev/zero bs=48000 count=1 2>/dev/null" // OpenBSD's head has no -c
    );
    s.helper("pw-cat", "cat > '" + s.path("pcm") + "'");
    const std::string m4a = s.path("clip.m4a");
    file::writeAtomic(m4a, "m4a");
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(m4a);
    REQUIRE(pumpUntil([&] { return probe.loaded || probe.failed; }, 5000));
    REQUIRE(probe.loaded);
    pumpUntil([&] { return p->durationMs() == 1500; }, 2000);
    CHECK(p->durationMs() == 1500);
    p->play();
    REQUIRE(pumpUntil([&] { return probe.ended || probe.failed; }, 5000));
    CHECK(probe.ended);
    CHECK(s.read("pcm").size() == 48000);
    CHECK(
        s.read("ffargs") == "-nostdin -hide_banner -nostats -loglevel info -i " + m4a +
                                " -vn -f s16le -ac 2 -ar 48000 pipe:1\n"
    );
}

TEST("audio: the recorder captures 16 kHz mono PCM from pw-record") {
    Scratch     s;
    // 0.3 s of a loud square wave, then wait to be stopped (SIGTERM ends it).
    std::string pcm;
    for (int i = 0; i < 4800; ++i)
        pcm += (i / 20) % 2 ? std::string("\x00\x40", 2) : std::string("\x00\xC0", 2);
    file::writeAtomic(s.path("take.raw"), pcm);
    s.helper(
        "pw-record",
        "echo \"$@\" > '" + s.path("args") + "'\ncat '" + s.path("take.raw") + "'\nexec sleep 30"
    );
    auto        r       = plat::audio::Recorder::create(app());
    bool        started = false, failed = false;
    int         levels = 0;
    float       peak   = 0;
    std::string wav;
    r->onStarted  = [&] { started = true; };
    r->onLevel    = [&](float l) { ++levels, peak = std::max(peak, l); };
    r->onFinished = [&](std::string w) { wav = std::move(w); };
    r->onFailed   = [&](const Failure &) { failed = true; };
    r->start();
    CHECK(r->isRecording());
    REQUIRE(pumpUntil([&] { return started || failed; }, 5000));
    REQUIRE(started);
    pumpUntil([&] { return levels >= 3; }, 2000);
    CHECK(levels >= 3);
    CHECK(peak > 0.45f && peak < 0.55f); // 0x4000 of 0x7fff
    r->stop();
    REQUIRE(pumpUntil([&] { return !wav.empty() || failed; }, 5000));
    CHECK_FALSE(r->isRecording());
    CHECK_STR(s.read("args"), "--raw --format s16 --rate 16000 --channels 1 -\n");
    CHECK(wav == plat::audio::wavFromPcm16(pcm, 16000, 1));
}

TEST("audio: a capture helper that can't record hands over, then fails with its words") {
    Scratch s;
    s.helper("pw-record", "echo 'pw-record: can not connect' >&2; exit 1");
    s.helper(
        "arecord", "echo 'arecord: main:850: audio open error: Device or resource busy' >&2; exit 1"
    );
    auto    r      = plat::audio::Recorder::create(app());
    bool    failed = false, started = false;
    Failure f;
    r->onStarted = [&] { started = true; };
    r->onFailed  = [&](const Failure &x) {
        failed = true;
        f      = x;
    };
    r->start();
    REQUIRE(pumpUntil([&] { return failed; }, 5000));
    CHECK_FALSE(started);
    CHECK_FALSE(r->isRecording());
    CHECK(f.code == Error::StartFailed);
    CHECK_STR(
        media::audioErrorText(f),
        "Couldn't start recording from the microphone: arecord: main:850: audio open error: "
        "Device or resource busy"
    );
}

TEST("audio: no capture helper at all, and cancel stops a recording silently") {
    Scratch s;
    {
        auto    r      = plat::audio::Recorder::create(app());
        bool    failed = false;
        Failure f;
        r->onFailed = [&](const Failure &x) {
            failed = true;
            f      = x;
        };
        r->start();
        REQUIRE(pumpUntil([&] { return failed; }, 5000));
        CHECK(f.code == Error::NoCaptureTool);
    }
    s.helper("parecord", "dd if=/dev/zero bs=3200 count=1 2>/dev/null; exec sleep 30");
    auto r       = plat::audio::Recorder::create(app());
    bool started = false, other = false;
    r->onStarted  = [&] { started = true; };
    r->onFinished = [&](std::string) { other = true; };
    r->onFailed   = [&](const Failure &) { other = true; };
    r->start();
    REQUIRE(pumpUntil([&] { return started; }, 5000));
    r->cancel();
    CHECK_FALSE(r->isRecording());
    pumpFor(100);
    CHECK_FALSE(other);
}

TEST("audio: MSGA_VOICE_FAKE_WAV records that file") {
    base::test::setEnv("MSGA_VOICE_FAKE_WAV", chimePath());
    auto r = plat::audio::Recorder::create(app());
    base::test::unsetEnv("MSGA_VOICE_FAKE_WAV");
    bool        started = false;
    int         levels  = 0;
    std::string wav;
    r->onStarted  = [&] { started = true; };
    r->onLevel    = [&](float) { ++levels; };
    r->onFinished = [&](std::string w) { wav = std::move(w); };
    r->start();
    CHECK_FALSE(started);
    REQUIRE(pumpUntil([&] { return levels >= 2; }, 3000));
    CHECK(started);
    r->stop();
    REQUIRE(pumpUntil([&] { return !wav.empty(); }, 3000));
    std::string chime;
    file::readAll(chimePath(), &chime);
    CHECK(wav == chime);
}

TEST("audio: system sounds come from the freedesktop theme, labelled from their file names") {
    Scratch           s;
    const std::string data = s.path("data");
    const std::string dir  = file::join(data, "sounds/freedesktop/stereo");
    REQUIRE(file::makeDirs(dir));
    for (const char *f : {"message-new-instant.oga", "bell.ogg", "bell.wav", "README.txt"})
        file::writeAtomic(file::join(dir, f), "x");
    const std::string oldData = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME") : "";
    base::test::setEnv("XDG_DATA_HOME", data);
    const auto list = plat::audio::systemSounds(); // no gsettings: the freedesktop theme
    REQUIRE(list.size() == 2);
    CHECK_STR(list[0].name, "bell");
    CHECK_STR(list[0].label, "Bell");
    CHECK_STR(list[1].name, "message-new-instant");
    CHECK_STR(list[1].label, "Message new instant");

    // Playing one: canberra is missing, so its theme file goes to pw-play.
    s.helper("pw-play", "echo \"$@\" >> '" + s.path("played") + "'");
    plat::audio::playSound("message-new-instant", chimePath());
    pumpUntil([&] { return !s.read("played").empty(); }, 3000);
    CHECK_STR(s.read("played"), file::join(dir, "message-new-instant.oga") + "\n");
    // An unknown one falls back to the file.
    plat::audio::playSound("no-such-sound", chimePath());
    pumpUntil([&] { return s.read("played").find("notify.wav") != std::string::npos; }, 3000);
    CHECK(s.read("played").find(chimePath()) != std::string::npos);

    // The app's list: the same sounds, as "system:" ids.
    std::vector<sounds::Entry> entries;
    bool                       done = false;
    sounds::systemSounds(app(), [&](std::vector<sounds::Entry> e) {
        entries = std::move(e);
        done    = true;
    });
    CHECK_FALSE(done);
    REQUIRE(pumpUntil([&] { return done; }, 3000));
    REQUIRE(entries.size() == 2);
    CHECK_STR(entries[0].id, "system:bell");
    CHECK_STR(entries[1].label, "Message new instant");
    if (oldData.empty())
        base::test::unsetEnv("XDG_DATA_HOME");
    else
        base::test::setEnv("XDG_DATA_HOME", oldData);
}

TEST("sounds: the bundled chime is the default and plays from the cache dir") {
    Scratch    s;
    const auto b = sounds::bundled();
    REQUIRE(b.size() == 1);
    CHECK_STR(b[0].id, sounds::kDefaultId);
    CHECK_STR(b[0].label, "msga chime");
    s.helper("pw-play", "echo \"$@\" >> '" + s.path("played") + "'");
    sounds::play(app(), "bundled:notify");
    REQUIRE(pumpUntil([&] { return !s.read("played").empty(); }, 3000));
    std::string played = s.read("played");
    if (!played.empty())
        played.pop_back();
    CHECK(file::baseName(played) == "sfx-notify.wav");
    std::string copy, chime;
    file::readAll(played, &copy);
    file::readAll(chimePath(), &chime);
    CHECK(copy == chime);
    // An unknown system sound falls back to the chime.
    file::writeAtomic(s.path("played"), "");
    sounds::play(app(), "system:nothing-like-it");
    REQUIRE(pumpUntil([&] { return !s.read("played").empty(); }, 3000));
    CHECK(s.read("played") == played + "\n");
}

#else
// macOS / Windows: the OS's player and recorder. Nothing is played or
// recorded: loading reads the file only, and $PLAT_AUDIO_HELPERS (set by the
// test harness) keeps every device closed.

TEST("audio: the OS player loads the chime and knows its length") {
    base::test::unsetEnv("PLAT_AUDIO_HELPERS"); // loading opens no device
    auto        p = plat::audio::Player::create(app());
    PlayerProbe probe;
    probe.attach(*p);
    p->load(chimePath());
    base::test::setEnv("PLAT_AUDIO_HELPERS", "/nonexistent/msga-test-audio");
    REQUIRE(pumpUntil([&] { return probe.loaded || probe.failed; }, 5000));
    CHECK(probe.loaded);
    CHECK(p->durationMs() >= 395 && p->durationMs() <= 405);
    CHECK(p->positionMs() == 0);
    p->stop();
    CHECK(plat::audio::canPlayExtension("m4a"));
    CHECK(plat::audio::canPlayExtension("mp3"));
    CHECK_FALSE(plat::audio::canPlayExtension("txt"));
}

TEST("audio: with audio off for tests, nothing opens the microphone") {
    auto    r      = plat::audio::Recorder::create(app());
    bool    failed = false;
    Failure f;
    r->onFailed = [&](const Failure &x) {
        failed = true;
        f      = x;
    };
    r->start();
    REQUIRE(pumpUntil([&] { return failed; }, 3000));
    CHECK(f.code == Error::NoMicrophone);
    CHECK_FALSE(r->isRecording());
    CHECK_STR(media::audioErrorText(f), "No microphone found");
}

TEST("audio: the OS lists its system sounds") {
    const auto list = plat::audio::systemSounds();
    CHECK(!list.empty());
    for (const auto &s : list)
        CHECK(!s.name.empty() && !s.label.empty());
    std::printf(
        "  %zu system sounds, first: %s\n", list.size(), list.empty() ? "" : list[0].label.c_str()
    );
}
#endif
