// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include "cache/workspace_cache.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// Each test gets a fresh fixture; the destructor removes the cache directory.
struct CacheFixture {
    const QString  teamId;
    WorkspaceCache cache;
    QString        baseDir;

    CacheFixture()
        : teamId("T_CACHE_TEST"), cache(teamId),
          baseDir(
              QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId
          ) {}
    ~CacheFixture() { QDir(baseDir).removeRecursively(); }
};

// ── Conversations ─────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadConversations returns empty when no file", "[cache][conv]") {
    CHECK(cache.loadConversations().empty());
}

TEST_CASE_METHOD(CacheFixture, "conversations round-trip all kinds", "[cache][conv]") {
    std::vector<Conversation> input = {
        Conversation{
            .id       = ConversationId{"C1"},
            .kind     = ConvKind::PublicChannel,
            .name     = "general",
            .isMember = true,
            .lastRead = "100.000",
            .unread   = 3,
        },
        Conversation{
            .id       = ConversationId{"G1"},
            .kind     = ConvKind::PrivateChannel,
            .name     = "secret",
            .isMember = true,
        },
        Conversation{
            .id     = ConversationId{"D1"},
            .kind   = ConvKind::Im,
            .name   = "U456",
            .dmUser = UserId{"U456"},
        },
    };
    cache.saveConversations(input);
    auto loaded = cache.loadConversations();
    REQUIRE(loaded.size() == 3);
    CHECK(loaded[0] == input[0]);
    CHECK(loaded[1] == input[1]);
    CHECK(loaded[2] == input[2]);
}

TEST_CASE_METHOD(
    CacheFixture, "conversation without dmUser stays nullopt after round-trip", "[cache][conv]"
) {
    std::vector<Conversation> input = {
        Conversation{
            .id   = ConversationId{"C1"},
            .kind = ConvKind::PublicChannel,
            .name = "general",
        },
    };
    cache.saveConversations(input);
    auto loaded = cache.loadConversations();
    REQUIRE(loaded.size() == 1);
    CHECK(!loaded[0].dmUser.has_value());
}

// ── Users ─────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadUsers returns empty when no file", "[cache][user]") {
    CHECK(cache.loadUsers().empty());
}

TEST_CASE_METHOD(CacheFixture, "users round-trip preserves all fields", "[cache][user]") {
    std::vector<User> input = {
        User{
            .id            = UserId{"U1"},
            .name          = "alice.smith",
            .displayName   = "Alice",
            .avatarUrl     = "https://avatar.example.com/alice.jpg",
            .isBot         = false,
            .isActive      = true,
            .isDeactivated = false,
        },
        User{
            .id            = UserId{"B1"},
            .name          = "slackbot",
            .displayName   = "Slackbot",
            .isBot         = true,
            .isActive      = false,
            .isDeactivated = false,
        },
    };
    cache.saveUsers(input);
    auto loaded = cache.loadUsers();
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0] == input[0]);
    CHECK(loaded[1] == input[1]);
}

// ── Messages ──────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadMessages returns empty when no file", "[cache][msg]") {
    CHECK(cache.loadMessages(ConversationId{"C1"}).empty());
}

TEST_CASE_METHOD(CacheFixture, "messages round-trip preserves all fields", "[cache][msg]") {
    // Build a message that exercises every serialized sub-type.
    // Note: File.prettyType and File.permalink are NOT cached — leave them empty.
    // Note: Message.replyCount is NOT cached — leave it at default 0.
    Message m;
    m.ts         = "123.456";
    m.threadRoot = QString{"100.000"};
    m.author     = UserId{"U1"};
    m.text       = TextWithEntities{"hello world", {TextEntity{EntityType::Bold, 0, 5, ""}}};
    m.edited     = true;
    m.subtype    = QString{"bot_message"};
    m.reactions  = {Reaction{"thumbsup", 2, {UserId{"U1"}, UserId{"U2"}}}};
    m.files      = {File{
             .id          = "F1",
             .name        = "img.png",
             .mimeType    = "image/png",
             .urlPrivate  = "https://files.slack.com/img.png",
             .thumbUrl    = "https://thumb.example.com/img.png",
             .imageWidth  = 640,
             .imageHeight = 480,
             .size        = 12345,
             .thumbs      = {
            FileThumb{360, 270, "https://thumb.example.com/img_360.png"},
            FileThumb{480, 360, "https://thumb.example.com/img_480.png"}
        },
    }};
    m.blocks     = {
        Block{
                .typeStr = "section",
                .text    = TextWithEntities{"block text", {TextEntity{EntityType::Italic, 0, 5, ""}}},
        },
        Block{
                .typeStr   = "table",
                .tableRows = {
                {TextWithEntities{"Header", {TextEntity{EntityType::Bold, 0, 6, ""}}},
                     TextWithEntities{"", {}}},
                {TextWithEntities{"cell", {}}, TextWithEntities{"18.2", {}}}
            },
        },
    };
    m.attachments = {Attachment{
        .fallback    = "fallback",
        .color       = "#36a64f",
        .title       = "Attachment title",
        .text        = TextWithEntities{"attach body", {}},
        .imageWidth  = 1200,
        .imageHeight = 630,
        .thumbWidth  = 360,
        .thumbHeight = 189,
    }};

    ConversationId conv{"C1"};
    cache.saveMessages(conv, {m});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == m);
}

TEST_CASE_METHOD(
    CacheFixture, "huddle_thread label is re-derived on load, not replayed", "[cache][msg]"
) {
    // Simulate a row cached before presentHuddleThread existed (or under a
    // different locale): empty text, USLACKBOT author. Loading must synthesize
    // the current-locale presentation, not trust the stored fields.
    Message m;
    m.ts      = "100.000";
    m.author  = UserId{"USLACKBOT"};
    m.subtype = QString{"huddle_thread"};

    ConversationId conv{"C1"};
    cache.saveMessages(conv, {m});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].author.value.isEmpty());
    CHECK(loaded[0].botName == "Slack");
    CHECK(loaded[0].text.text == "Huddle happened");
}

TEST_CASE_METHOD(CacheFixture, "saveMessages caps at 50 newest messages", "[cache][msg]") {
    ConversationId       conv{"C2"};
    std::vector<Message> msgs;
    for (int i = 0; i < 60; ++i) {
        Message m;
        m.ts     = QString::number(i) + ".000";
        m.author = UserId{"U1"};
        msgs.push_back(m);
    }
    cache.saveMessages(conv, msgs);
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 50);
    CHECK(loaded.front().ts == "10.000");
    CHECK(loaded.back().ts == "59.000");
}

TEST_CASE_METHOD(CacheFixture, "saveMessages with fewer than 50 keeps all", "[cache][msg]") {
    ConversationId       conv{"C3"};
    std::vector<Message> msgs;
    for (int i = 0; i < 10; ++i) {
        Message m;
        m.ts     = QString::number(i) + ".000";
        m.author = UserId{"U1"};
        msgs.push_back(m);
    }
    cache.saveMessages(conv, msgs);
    CHECK(cache.loadMessages(conv).size() == 10);
}

// ── LastConv ──────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadLastConv returns empty pair when no file", "[cache][meta]") {
    auto [conv, name] = cache.loadLastConv();
    CHECK(conv.value.isEmpty());
    CHECK(name.isEmpty());
}

TEST_CASE_METHOD(CacheFixture, "lastConv round-trip", "[cache][meta]") {
    cache.saveLastConv(ConversationId{"C42"}, "general");
    auto [conv, name] = cache.loadLastConv();
    CHECK(conv == ConversationId{"C42"});
    CHECK(name == "general");
}

// ── MeUserId ──────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadMeUserId returns empty when no file", "[cache][meta]") {
    CHECK(cache.loadMeUserId().value.isEmpty());
}

TEST_CASE_METHOD(CacheFixture, "meUserId round-trip", "[cache][meta]") {
    cache.saveMeUserId(UserId{"U777"});
    CHECK(cache.loadMeUserId() == UserId{"U777"});
}

TEST_CASE_METHOD(CacheFixture, "meUserId does not clobber other meta keys", "[cache][meta]") {
    cache.saveLastConv(ConversationId{"C42"}, "general");
    cache.saveMeUserId(UserId{"U777"});
    auto [conv, name] = cache.loadLastConv();
    CHECK(conv == ConversationId{"C42"});
    CHECK(name == "general");
    CHECK(cache.loadMeUserId() == UserId{"U777"});
}

// ── Message reminders ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadReminders returns empty when no file", "[cache][reminder]") {
    CHECK(cache.loadReminders().empty());
}

TEST_CASE_METHOD(CacheFixture, "reminders round-trip all fields", "[cache][reminder]") {
    const std::vector<MessageReminder> input = {
        MessageReminder{
            .conv       = ConversationId{"C1"},
            .ts         = "1700000000.000100",
            .dueAt      = 1700003600,
            .threadRoot = "1699999999.000001",
            .snippet    = "don't forget this",
            .fired      = true,
        },
        MessageReminder{
            .conv  = ConversationId{"D2"},
            .ts    = "1700000001.000200",
            .dueAt = 1700007200,
        },
    };
    cache.saveReminders(input);
    const auto out = cache.loadReminders();
    REQUIRE(out.size() == 2);
    CHECK(out[0] == input[0]);
    CHECK(out[1] == input[1]);
}

TEST_CASE_METHOD(
    CacheFixture, "reminders with no due date or identity are dropped on load", "[cache][reminder]"
) {
    // Guards against a corrupt meta entry resurfacing as a ghost reminder.
    cache.saveReminders(
        {MessageReminder{.conv = ConversationId{"C1"}, .ts = "1.2", .dueAt = 0},
         MessageReminder{.conv = ConversationId{}, .ts = "1.2", .dueAt = 5},
         MessageReminder{.conv = ConversationId{"C1"}, .ts = "3.4", .dueAt = 9}}
    );
    const auto out = cache.loadReminders();
    REQUIRE(out.size() == 1);
    CHECK(out[0].ts == "3.4");
}

TEST_CASE_METHOD(CacheFixture, "reminders do not clobber other meta keys", "[cache][reminder]") {
    cache.saveMeUserId(UserId{"U777"});
    cache.saveReminders({MessageReminder{.conv = ConversationId{"C1"}, .ts = "1.2", .dueAt = 5}});
    CHECK(cache.loadMeUserId() == UserId{"U777"});
    CHECK(cache.loadReminders().size() == 1);
}

// ── Images ────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "image round-trip", "[cache][img]") {
    const QByteArray data = "\x89PNG_BYTES_HERE";
    const QString    url  = "https://avatar.example.com/U1.png";
    cache.saveImage(url, data);
    CHECK(cache.loadImage(url) == data);
}

TEST_CASE_METHOD(CacheFixture, "loadImage returns empty for different url", "[cache][img]") {
    cache.saveImage("https://a.example.com/img.png", "data");
    CHECK(cache.loadImage("https://b.example.com/img.png").isEmpty());
}

TEST_CASE_METHOD(CacheFixture, "saveImage with empty data writes nothing", "[cache][img]") {
    const QString url = "https://example.com/empty.png";
    cache.saveImage(url, QByteArray{});
    CHECK(cache.loadImage(url).isEmpty());
}

TEST_CASE_METHOD(
    CacheFixture, "loadImage bumps a stale blob mtime (LRU last-used)", "[cache][img]"
) {
    const QString url = "https://avatar.example.com/U2.png";
    cache.saveImage(url, "data");

    const auto blobs = QDir(baseDir + "/images").entryInfoList(QDir::Files);
    REQUIRE(blobs.size() == 1);
    const QString blobPath = blobs.first().filePath();

    // Backdate past the 1h bump throttle, then read.
    const auto past = QDateTime::currentDateTimeUtc().addSecs(-7200);
    {
        QFile f(blobPath);
        REQUIRE(f.open(QIODevice::ReadWrite));
        REQUIRE(f.setFileTime(past, QFileDevice::FileModificationTime));
    }
    CHECK(cache.loadImage(url) == "data");
    CHECK(QFileInfo(blobPath).lastModified().secsTo(QDateTime::currentDateTimeUtc()) < 60);
}

// ── Bots ──────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadBots returns empty when no file", "[cache][bot]") {
    CHECK(cache.loadBots().empty());
}

TEST_CASE_METHOD(CacheFixture, "bots round-trip preserves name and avatar", "[cache][bot]") {
    QHash<QString, User> bots;
    bots["B001"] = User{
        UserId{"B001"},
        "jenkins",
        "Jenkins CI",
        "https://cdn.example.com/jenkins_72.png",
        /*isBot=*/true,
    };
    bots["B002"] = User{
        UserId{"B002"},
        "deploy-bot",
        "Deploy Bot",
        "",
        /*isBot=*/true,
    };

    cache.saveBots(bots);
    const auto loaded = cache.loadBots();

    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded.contains("B001"));
    CHECK(loaded["B001"].displayName == "Jenkins CI");
    CHECK(loaded["B001"].avatarUrl == "https://cdn.example.com/jenkins_72.png");
    CHECK(loaded["B001"].isBot == true);
    REQUIRE(loaded.contains("B002"));
    CHECK(loaded["B002"].displayName == "Deploy Bot");
    CHECK(loaded["B002"].avatarUrl.isEmpty());
}

TEST_CASE_METHOD(CacheFixture, "saveBots with empty map writes nothing to load", "[cache][bot]") {
    cache.saveBots({});
    CHECK(cache.loadBots().empty());
}

TEST_CASE_METHOD(CacheFixture, "loadBots skips entries with empty id", "[cache][bot]") {
    // Write one valid and one id-less entry directly, then verify only valid one loads.
    QHash<QString, User> bots;
    bots["B001"] = User{UserId{"B001"}, "bot1", "Bot One", "", true};
    cache.saveBots(bots);
    const auto loaded = cache.loadBots();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.contains("B001"));
}
