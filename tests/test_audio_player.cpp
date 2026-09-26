// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Media::AudioPlayer state machine against a scripted engine, plus the real
// platform engine's decode path on Linux (a generated WAV: load, duration,
// seek at rest — never actually played, so CI needs no sound server).

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>

#include "media/audio_player.h"

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test-audio-player");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

using Media::AudioPlayer;
using State = AudioPlayer::State;

namespace {

struct FakeEngine : Media::Engine {
    using Media::Engine::Engine;
    QStringList calls;
    QString     path;
    bool        deferLoad = false;
    qint64      pos = 0, dur = 0;
    QStringList exts{"mp3", "wav"};

    void load(const QString &p) override {
        calls << "load";
        path = p;
        if (!deferLoad)
            emit loaded();
    }
    void play() override { calls << "play"; }
    void pause() override { calls << "pause"; }
    void seek(qint64 ms) override {
        calls << QStringLiteral("seek:%1").arg(ms);
        pos = ms;
    }
    void   stop() override { calls << "stop"; }
    qint64 positionMs() const override { return pos; }
    qint64 durationMs() const override { return dur; }
    bool   supportsExtension(const QString &e) const override { return exts.contains(e); }
};

struct Rig {
    AudioPlayer            &player = AudioPlayer::instance();
    FakeEngine             *eng    = nullptr;
    QStringList             changed;
    QMetaObject::Connection conn;

    Rig() {
        auto e = std::make_unique<FakeEngine>();
        eng    = e.get();
        player.setEngineForTesting(std::move(e));
        conn = QObject::connect(&player, &AudioPlayer::statusChanged, [this](const QString &k) {
            changed << k;
        });
    }
    ~Rig() {
        QObject::disconnect(conn);
        player.stop();
        player.setEngineForTesting(nullptr);
    }
};

} // namespace

TEST_CASE("play loads the file, then starts once the engine is ready", "[audio][player]") {
    Rig r;
    r.eng->deferLoad = true;
    r.eng->dur       = 4200;

    r.player.play("F1", "/tmp/a.mp3", 5000);
    CHECK(r.player.status().key == "F1");
    CHECK(r.player.status().state == State::Loading);
    CHECK(r.player.status().durationMs == 5000); // Slack's figure until the engine knows
    CHECK(r.eng->calls.last() == "load");        // (a stop() of whatever was there precedes it)
    CHECK_FALSE(r.eng->calls.contains("play"));

    emit r.eng->loaded();
    CHECK(r.player.status().state == State::Playing);
    CHECK(r.player.status().durationMs == 4200); // engine wins once it has decoded
    CHECK(r.eng->calls.last() == "play");
    CHECK(r.changed.contains("F1"));
}

TEST_CASE("pause and resume round-trip through the engine", "[audio][player]") {
    Rig r;
    r.eng->dur = 10000;
    r.player.play("F1", "/tmp/a.mp3");
    REQUIRE(r.player.status().state == State::Playing);

    r.eng->pos = 3000;
    r.player.togglePause();
    CHECK(r.player.status().state == State::Paused);
    CHECK(r.player.status().positionMs == 3000);
    CHECK(r.eng->calls.last() == "pause");

    r.player.togglePause();
    CHECK(r.player.status().state == State::Playing);
    CHECK(r.eng->calls.last() == "play");

    // Same key while paused: play() resumes rather than reloading.
    r.player.pause();
    r.eng->calls.clear();
    r.player.play("F1", "/tmp/a.mp3");
    CHECK(r.eng->calls == QStringList{"play"});
}

TEST_CASE("ended clip restarts from the top on the next toggle", "[audio][player]") {
    Rig r;
    r.eng->dur = 2000;
    r.player.play("F1", "/tmp/a.mp3");
    emit r.eng->ended();
    CHECK(r.player.status().state == State::Ended);
    CHECK(r.player.status().positionMs == 2000);

    r.eng->calls.clear();
    r.player.togglePause();
    CHECK(r.player.status().state == State::Playing);
    CHECK(r.eng->calls == QStringList{"seek:0", "play"});
    CHECK(r.player.status().positionMs == 0);
}

TEST_CASE("seek clamps to the duration and moves the reported position", "[audio][player]") {
    Rig r;
    r.eng->dur = 5000;
    r.player.play("F1", "/tmp/a.mp3");
    r.player.seek(9000);
    CHECK(r.player.status().positionMs == 5000);
    CHECK(r.eng->calls.last() == "seek:5000");
    r.player.seek(-10);
    CHECK(r.player.status().positionMs == 0);
}

TEST_CASE(
    "a new file takes over: the old key is notified and the engine stopped", "[audio][player]"
) {
    Rig r;
    r.player.play("F1", "/tmp/a.mp3");
    r.changed.clear();
    r.eng->calls.clear();

    r.player.play("F2", "/tmp/b.mp3");
    CHECK(r.player.status().key == "F2");
    CHECK(r.player.status().state == State::Playing);
    CHECK(r.changed.first() == "F1"); // so the old chip repaints idle
    CHECK(r.changed.contains("F2"));
    CHECK(r.eng->calls.first() == "stop");
}

TEST_CASE("download bookkeeping: beginLoading → loadFailed shows an error", "[audio][player]") {
    Rig r;
    r.player.beginLoading("F1", 3000);
    CHECK(r.player.status().state == State::Loading);
    CHECK(r.player.status().durationMs == 3000);
    // Nothing reaches the engine until the bytes exist.
    CHECK_FALSE(r.eng->calls.contains("load"));
    CHECK_FALSE(r.eng->calls.contains("play"));

    r.player.loadFailed("F9", "wrong key");
    CHECK(r.player.status().state == State::Loading);
    r.player.loadFailed("F1", "Download failed");
    CHECK(r.player.status().state == State::Error);
    CHECK(r.player.status().error == "Download failed");

    // An engine failure lands in the same place.
    r.player.play("F1", "/tmp/a.mp3");
    emit r.eng->failed("boom");
    CHECK(r.player.status().state == State::Error);
    CHECK(r.player.status().error == "boom");
}

TEST_CASE("sourceUrlFor picks the first URL the engine can decode", "[audio][player]") {
    Rig  r; // FakeEngine decodes mp3 + wav only
    File f;
    f.id                 = "F1";
    f.name               = "sample-5s.mp3";
    // A real MP3 upload as Slack reports it: url_private is already the MP4
    // transcode; the original sits behind url_private_download.
    f.urlPrivate         = "https://files.slack.com/files-tmb/T1-F1-abc/sample_audio.mp4";
    f.urlPrivateDownload = "https://files.slack.com/files-pri/T1-F1/download/sample-5s.mp3";
    f.aacUrl             = f.urlPrivate;
    CHECK(r.player.sourceUrlFor(f) == f.urlPrivateDownload);

    // Query strings don't confuse the extension check.
    f.urlPrivate = "https://files.slack.com/files-pri/T1-F1/song.mp3?x=1";
    CHECK(r.player.sourceUrlFor(f) == f.urlPrivate);

    // A voice clip: WebM original, AAC transcode. Nothing decodable → the
    // engine gets url_private and reports the format error itself.
    f.urlPrivate         = "https://files.slack.com/files-pri/T1-F1/audio_message.webm";
    f.urlPrivateDownload = "https://files.slack.com/files-pri/T1-F1/download/audio_message.webm";
    f.aacUrl             = "https://files.slack.com/files-tmb/T1-F1/audio_message_audio.mp4";
    CHECK(r.player.sourceUrlFor(f) == f.urlPrivate);
    r.eng->exts << "mp4"; // the native players (and ffmpeg) take the transcode
    CHECK(r.player.sourceUrlFor(f) == f.aacUrl);
}

TEST_CASE("extensionOf handles names, URLs and query strings", "[audio][player]") {
    CHECK(AudioPlayer::extensionOf("clip.MP3") == "mp3");
    CHECK(AudioPlayer::extensionOf("https://files.slack.com/a/b/clip.m4a?t=1") == "m4a");
    CHECK(AudioPlayer::extensionOf("noext") == "");
    CHECK(AudioPlayer::extensionOf("") == "");
}

#if defined(Q_OS_LINUX)

namespace {
// 16-bit mono PCM WAV of a sine tone.
QString writeWav(const QString &path, int rate, double seconds) {
    const int   frames = (int)(rate * seconds);
    QByteArray  pcm;
    QDataStream ds(&pcm, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    for (int i = 0; i < frames; ++i)
        ds << (qint16)(8000 * std::sin(2 * M_PI * 440 * i / rate));

    QByteArray  wav;
    QDataStream w(&wav, QIODevice::WriteOnly);
    w.setByteOrder(QDataStream::LittleEndian);
    w.writeRawData("RIFF", 4);
    w << (quint32)(36 + pcm.size());
    w.writeRawData("WAVEfmt ", 8);
    w << (quint32)16 << (quint16)1 << (quint16)1 << (quint32)rate << (quint32)(rate * 2)
      << (quint16)2 << (quint16)16;
    w.writeRawData("data", 4);
    w << (quint32)pcm.size();
    w.writeRawData(pcm.constData(), pcm.size());

    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(wav);
    return path;
}

// Spin until `done` or the timeout.
bool waitFor(const bool &done, int ms) {
    QEventLoop loop;
    QTimer     guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(ms);
    while (!done && guard.isActive())
        loop.processEvents(QEventLoop::AllEvents, 50);
    return done;
}
} // namespace

TEST_CASE("Linux engine decodes a WAV in-process: duration and seek at rest", "[audio][engine]") {
    QTemporaryDir dir;
    const QString path = writeWav(dir.filePath("tone.wav"), 8000, 1.5);

    auto engine = Media::createPlatformEngine(nullptr);
    bool loaded = false, failed = false;
    QObject::connect(engine.get(), &Media::Engine::loaded, [&] { loaded = true; });
    QObject::connect(engine.get(), &Media::Engine::failed, [&](const QString &) { failed = true; });
    engine->load(path);
    REQUIRE(waitFor(loaded, 3000));
    CHECK_FALSE(failed);
    CHECK(engine->durationMs() == 1500);
    CHECK(engine->positionMs() == 0);

    engine->seek(600);
    CHECK(engine->positionMs() == 600);
    engine->seek(99999); // past the end clamps
    CHECK(engine->positionMs() == 1500);

    CHECK(engine->supportsExtension("mp3"));
    CHECK(engine->supportsExtension("flac"));
    CHECK(engine->supportsExtension("ogg"));
}

TEST_CASE("Linux engine reports garbage as a failure", "[audio][engine]") {
    QTemporaryDir dir;
    const QString path = dir.filePath("junk.mp3");
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4096, 'x'));
    }
    auto engine = Media::createPlatformEngine(nullptr);
    bool loaded = false, failed = false;
    QObject::connect(engine.get(), &Media::Engine::loaded, [&] { loaded = true; });
    QObject::connect(engine.get(), &Media::Engine::failed, [&](const QString &) { failed = true; });
    engine->load(path);
    // miniaudio rejects it synchronously; with ffmpeg installed the fallback
    // decoder is tried and fails asynchronously instead — and must not have
    // claimed loaded() in the meantime.
    if (!failed)
        waitFor(failed, 5000);
    CHECK(failed);
    CHECK_FALSE(loaded);
}

#endif // Q_OS_LINUX
