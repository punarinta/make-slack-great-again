// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QDir>
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
    const QString teamId;
    WorkspaceCache cache;
    QString baseDir;

    CacheFixture()
        : teamId("T_CACHE_TEST")
        , cache(teamId)
        , baseDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + "/cache/" + teamId)
    {}
    ~CacheFixture() {
        QDir(baseDir).removeRecursively();
    }
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

TEST_CASE_METHOD(CacheFixture, "conversation without dmUser stays nullopt after round-trip", "[cache][conv]") {
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
    m.text       = TextWithEntities{
        "hello world",
        { TextEntity{EntityType::Bold, 0, 5, ""} }
    };
    m.edited     = true;
    m.subtype    = QString{"bot_message"};
    m.reactions  = { Reaction{"thumbsup", 2, {UserId{"U1"}, UserId{"U2"}}} };
    m.files      = { File{
        .id          = "F1",
        .name        = "img.png",
        .mimeType    = "image/png",
        .urlPrivate  = "https://files.slack.com/img.png",
        .thumbUrl    = "https://thumb.example.com/img.png",
        .imageWidth  = 640,
        .imageHeight = 480,
        .size        = 12345,
    }};
    m.blocks     = { Block{
        .typeStr  = "section",
        .text     = TextWithEntities{"block text", { TextEntity{EntityType::Italic, 0, 5, ""} }},
    }};
    m.attachments = { Attachment{
        .fallback = "fallback",
        .color    = "#36a64f",
        .title    = "Attachment title",
        .text     = TextWithEntities{"attach body", {}},
    }};

    ConversationId conv{"C1"};
    cache.saveMessages(conv, {m});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == m);
}

TEST_CASE_METHOD(CacheFixture, "saveMessages caps at 50 newest messages", "[cache][msg]") {
    ConversationId conv{"C2"};
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
    CHECK(loaded.back().ts  == "59.000");
}

TEST_CASE_METHOD(CacheFixture, "saveMessages with fewer than 50 keeps all", "[cache][msg]") {
    ConversationId conv{"C3"};
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

// ── Bots ──────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadBots returns empty when no file", "[cache][bot]") {
    CHECK(cache.loadBots().empty());
}

TEST_CASE_METHOD(CacheFixture, "bots round-trip preserves name and avatar", "[cache][bot]") {
    QHash<QString, User> bots;
    bots["B001"] = User{
        UserId{"B001"}, "jenkins", "Jenkins CI",
        "https://cdn.example.com/jenkins_72.png",
        /*isBot=*/true,
    };
    bots["B002"] = User{
        UserId{"B002"}, "deploy-bot", "Deploy Bot", "",
        /*isBot=*/true,
    };

    cache.saveBots(bots);
    const auto loaded = cache.loadBots();

    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded.contains("B001"));
    CHECK(loaded["B001"].displayName == "Jenkins CI");
    CHECK(loaded["B001"].avatarUrl   == "https://cdn.example.com/jenkins_72.png");
    CHECK(loaded["B001"].isBot       == true);
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
