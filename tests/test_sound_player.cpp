#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>
#include <QFile>
#include <QtEndian>

#include "util/sound_player.h"

// Own main: Sound::Player touches QStandardPaths / QProcess, which want a
// QCoreApplication instance.
MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    return msga_test::runCatch(argc, argv);
}

using namespace Sound;

TEST_CASE("default id is the bundled chime") {
    REQUIRE(Player::defaultId() == "bundled:notify");
}

TEST_CASE("bundled chime is a plain PCM WAV the native players can read") {
    // Windows PlaySound and aplay only take PCM WAV, so the chime must stay one.
    // rcc may store it compressed: QFile must still hand back the real bytes
    // and report the uncompressed size (bundledPath() relies on that).
    QFile f(":/sfx/notify.wav");
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray wav = f.readAll();
    REQUIRE(wav.size() > 44);
    CHECK(f.size() == wav.size());
    CHECK(wav.left(4) == "RIFF");
    CHECK(wav.mid(8, 4) == "WAVE");
    CHECK(wav.mid(12, 4) == "fmt ");
    CHECK(qFromLittleEndian<quint16>(wav.constData() + 20) == 1);  // WAVE_FORMAT_PCM
    CHECK(qFromLittleEndian<quint16>(wav.constData() + 34) == 16); // bits per sample
}

TEST_CASE("bundled sounds expose the default with a label") {
    const auto bundled = Player::instance().bundledSounds();
    REQUIRE(bundled.size() == 1);
    CHECK(bundled.front().id == Player::defaultId());
    CHECK_FALSE(bundled.front().label.isEmpty());
}

TEST_CASE("enumerated system sounds are well-formed") {
    // Count is environment-dependent (theme files present); only the contract
    // is asserted — every entry is a non-empty "system:<id>" with a label.
    for (const auto &e : Player::instance().systemSounds()) {
        CHECK(e.id.startsWith("system:"));
        CHECK(e.id.size() > QStringLiteral("system:").size());
        CHECK_FALSE(e.label.isEmpty());
    }
}

TEST_CASE("play never throws on bad or empty ids") {
    // Fire-and-forget; unresolvable ids fall back to the bundled chime. We only
    // assert it returns cleanly (no crash/throw) in a headless environment.
    CHECK_NOTHROW(Player::instance().play(QString()));
    CHECK_NOTHROW(Player::instance().play("nonsense:does-not-exist"));
    CHECK_NOTHROW(Player::instance().play("system:definitely-not-a-real-sound"));
}
