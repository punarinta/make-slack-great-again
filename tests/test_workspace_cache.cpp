// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include "cache/workspace_cache.h"

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
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

TEST_CASE_METHOD(CacheFixture, "conversation star survives a round-trip", "[cache][conv][star]") {
    // No conversation listing reports is_starred, so the cache is what carries a
    // star across a restart (issue #48).
    std::vector<Conversation> input = {
        Conversation{
            .id        = ConversationId{"C1"},
            .kind      = ConvKind::PublicChannel,
            .name      = "general",
            .isStarred = true,
        },
        Conversation{
            .id   = ConversationId{"C2"},
            .kind = ConvKind::PublicChannel,
            .name = "random",
        },
    };
    cache.saveConversations(input);
    auto loaded = cache.loadConversations();
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].isStarred == true);
    CHECK(loaded[1].isStarred == false);
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

TEST_CASE_METHOD(CacheFixture, "usergroups round-trip", "[cache][user]") {
    CHECK(cache.loadUsergroups().empty());
    const std::vector<Usergroup> input = {
        Usergroup{
            .id     = "S1",
            .handle = "eng-oncall",
            .name   = "Engineering on-call",
            .users  = {UserId{"U1"}, UserId{"U2"}}
        },
        Usergroup{.id = "S2", .handle = "design", .name = "Design"},
    };
    cache.saveUsergroups(input);
    CHECK(cache.loadUsergroups() == input);
}

// ── Messages ──────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(CacheFixture, "loadMessages returns empty when no file", "[cache][msg]") {
    CHECK(cache.loadMessages(ConversationId{"C1"}).empty());
}

TEST_CASE_METHOD(CacheFixture, "messages round-trip preserves all fields", "[cache][msg]") {
    // Build a message that exercises every serialized sub-type.
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
        .id                 = "F1",
        .name               = "img.png",
        .mimeType           = "image/png",
        .urlPrivate         = "https://files.slack.com/img.png",
        .urlPrivateDownload = "https://files.slack.com/download/img.png",
        .thumbUrl           = "https://thumb.example.com/img.png",
        .imageWidth         = 640,
        .imageHeight        = 480,
        .size               = 12345,
        .thumbs =
            {FileThumb{360, 270, "https://thumb.example.com/img_360.png"},
             FileThumb{480, 360, "https://thumb.example.com/img_480.png"}},
        .durationMs        = 5041,
        .aacUrl            = "https://files.slack.com/files-tmb/T1-F1/img_audio.mp4",
        .subtype           = "slack_audio",
        .transcriptStatus  = "complete",
        .transcriptPreview = "Test, test, battery.",
        .transcriptVttUrl  = "https://files.slack.com/files-tmb/T1-F1/file.vtt",
    }};
    m.botId      = "B0BL4TE9M44";
    m.blocks     = {
        Block{
            .typeStr = "section",
            .text    = TextWithEntities{"block text", {TextEntity{EntityType::Italic, 0, 5, ""}}},
        },
        // A pressable Block Kit button: without action/block id and value a
        // cached copy could only explain, never press.
        Block{
            .typeStr = "actions",
            .buttons = {BotButton{
                .text     = "Try again",
                .style    = "primary",
                .actionId = "retry_failed_task",
                .blockId  = "aXl4u",
                .value    = R"({"runId":40})",
            }},
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
        // Classic bot "fields" rows — a Jenkins poll's whole body lives here.
        // They were once not serialized at all, so every cached copy fell
        // back to the plain `fallback` string (and lost its emoji images).
        .fields      = {
            AttachmentField{
                .title = "Lunch",
                .value =
                    TextWithEntities{
                        "pick :no-lunch:", {TextEntity{EntityType::Emoji, 5, 10, "no-lunch"}}
                    },
            },
            AttachmentField{.title = "", .value = TextWithEntities{"untitled", {}}}
        },
    }};

    ConversationId conv{"C1"};
    cache.saveMessages(conv, {m});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == m);
}

TEST_CASE_METHOD(
    CacheFixture, "attachment footer icon and ts survive a cache round trip", "[cache][msg]"
) {
    Message m;
    m.ts          = "150.000";
    m.author      = UserId{"U1"};
    m.attachments = {Attachment{
        .text       = TextWithEntities{"Pull request opened", {}},
        .footer     = "<https://github.com/Hitta/data-collector|Hitta/data-collector>",
        .footerIcon = "https://slack.github.com/static/img/favicon-neutral.png",
        .msgDate    = 1755690000000000LL,
    }};

    ConversationId conv{"C4"};
    cache.saveMessages(conv, {m});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == m);
}

TEST_CASE_METHOD(CacheFixture, "message unfurl survives a cache round trip", "[cache][msg]") {
    // The card needs the quoted author/channel/time and the quoted message's
    // files — dropping them on load would silently demote the card to a bare
    // preview for every message read back from disk.
    Message m;
    m.ts          = "200.000";
    m.author      = UserId{"U1"};
    m.attachments = {Attachment{
        .authorName    = "CityCity",
        .text          = TextWithEntities{"Pre-booking error", {}},
        .isMsgUnfurl   = true,
        .authorIcon    = "https://a.slack-edge.com/img/bot_48.png",
        .authorSubname = "citycity",
        .channelId     = "C0401QDC20K",
        .msgDate       = 1787145280873039LL,
        .files         = {File{
            .name       = "file.txt.json",
            .mimeType   = "text/plain",
            .prettyType = "JSON",
            .permalink  = "https://team.slack.com/files/U1/F1/file.txt.json",
            .size       = 375,
        }},
    }};

    ConversationId conv{"C3"};
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
    CHECK(loaded[0].botName == "A huddle happened");
    CHECK(loaded[0].text.text == "A huddle happened");
}

TEST_CASE_METHOD(CacheFixture, "huddle summary and canvas title survive caching", "[cache][msg]") {
    Message m;
    m.ts      = "100.000";
    m.subtype = QString{"huddle_thread"};
    m.huddle  = HuddleInfo{
        .attendees = {UserId{"U1"}, UserId{"U2"}},
        .startSec  = 1790161232,
        .endSec    = 1790161718,
        .ended     = true
    };
    File canvas;
    canvas.id       = "F1";
    canvas.mimeType = "application/vnd.slack-docs";
    canvas.title    = ":headphones: Huddle notes with <@U1>";
    m.files         = {canvas};
    presentHuddleThread(m);

    ConversationId conv{"C1"};
    cache.saveMessages(conv, {m});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].huddle == m.huddle);
    REQUIRE(loaded[0].files.size() == 1);
    CHECK(loaded[0].files[0].title == canvas.title);
}

TEST_CASE_METHOD(CacheFixture, "link preview classification survives caching", "[cache][msg]") {
    const QString url = "https://example.com/article";
    Message       message;
    message.ts          = "300.000";
    message.text        = TextWithEntities{url, {{EntityType::Link, 0, int(url.size()), url}}};
    message.attachments = {
        Attachment{.title = "Web preview", .titleLink = url, .isLinkPreview = true},
        Attachment{.title = "Bot content", .titleLink = url},
        Attachment{.title = "Shared message", .titleLink = url, .isMsgUnfurl = true},
    };
    const ConversationId conv{"C_PREVIEWS"};
    cache.saveMessages(conv, {message});
    auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == message);

    // Upgrade a cache written before the unfurl flag existed. A linked title
    // alone must not hide bot content, and shared-message cards stay visible.
    const auto path = baseDir + "/messages/" + conv.value + ".json";
    QFile      file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto messages = QJsonDocument::fromJson(file.readAll()).array();
    file.close();
    auto msg         = messages[0].toObject();
    auto attachments = msg["at"].toArray();
    for (int i = 0; i < attachments.size(); ++i) {
        auto attachment  = attachments[i].toObject();
        attachment["lp"] = false; // v1 explicitly exempted app unfurls
        attachment.remove("lpv");
        // Slack canonicalises the unfurl target: host case, "www.", tracking
        // params and a trailing slash must not defeat the backfill.
        if (i == 0)
            attachment["tl"] = "https://WWW.example.com/article/?utm_source=x";
        if (i == 1)
            attachment["tl"] = "https://example.com/build";
        attachments[i] = attachment;
    }
    msg["at"]   = attachments;
    messages[0] = msg;
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(messages).toJson());
    file.close();
    loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].attachments.size() == 3);
    CHECK(loaded[0].attachments[0].isLinkPreview);
    CHECK_FALSE(loaded[0].attachments[1].isLinkPreview);
    CHECK_FALSE(loaded[0].attachments[2].isLinkPreview);

    // Legacy bot posts can repeat their attachment's URL in the body. Without
    // explicit unfurl metadata, preserve their content until history refreshes.
    for (const auto &key : {"bn", "st"}) {
        auto bot    = msg;
        bot[key]    = QString(key) == "bn" ? "Build bot" : "bot_message";
        messages[0] = bot;
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(messages).toJson());
        file.close();
        loaded = cache.loadMessages(conv);
        REQUIRE(loaded.size() == 1);
        REQUIRE(loaded[0].attachments.size() == 3);
        CHECK_FALSE(loaded[0].attachments[0].isLinkPreview);
    }
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
            .conv         = ConversationId{"C1"},
            .ts           = "1700000000.000100",
            .dueAt        = 1700003600,
            .savedAt      = 1700000000,
            .threadRoot   = "1699999999.000001",
            .snippet      = "don't forget this",
            .author       = UserId{"U42"},
            .botName      = "Deploy Bot",
            .botAvatarUrl = "https://example.com/bot.png",
            .fired        = true,
        },
        MessageReminder{
            .conv  = ConversationId{"D2"},
            .ts    = "1700000001.000200",
            .dueAt = 1700007200,
        },
        // A plain "Save for later" bookmark: no due date, but still a saved item.
        MessageReminder{
            .conv    = ConversationId{"C3"},
            .ts      = "1700000002.000300",
            .dueAt   = 0,
            .savedAt = 1700000002,
            .snippet = "read this later",
        },
    };
    cache.saveReminders(input);
    const auto out = cache.loadReminders();
    REQUIRE(out.size() == 3);
    CHECK(out[0] == input[0]);
    CHECK(out[1] == input[1]);
    CHECK(out[2] == input[2]);
}

TEST_CASE_METHOD(CacheFixture, "reminder previews round-trip", "[cache][reminder]") {
    QHash<QString, ReminderPreview> input;
    input.insert(
        "C1\t1700000000.000100",
        ReminderPreview{
            .threadRoot   = "1699999999.000001",
            .snippet      = "don't forget this",
            .author       = UserId{"U42"},
            .botName      = "Deploy Bot",
            .botAvatarUrl = "https://example.com/bot.png",
        }
    );
    // Nothing worth remembering: dropped rather than stored as an empty shell.
    input.insert("C1\t1700000000.000200", ReminderPreview{});

    cache.saveReminderPreviews(input);
    const auto out = cache.loadReminderPreviews();
    REQUIRE(out.size() == 1);
    CHECK(out.value("C1\t1700000000.000100") == input.value("C1\t1700000000.000100"));
}

TEST_CASE_METHOD(
    CacheFixture, "reminders without an identity are dropped on load", "[cache][reminder]"
) {
    // Guards against a corrupt meta entry resurfacing as a ghost item. A missing
    // due date is NOT corruption any more: it is a plain "Save for later".
    cache.saveReminders(
        {MessageReminder{.conv = ConversationId{"C1"}, .ts = "1.2", .dueAt = 0},
         MessageReminder{.conv = ConversationId{}, .ts = "1.2", .dueAt = 5},
         MessageReminder{.conv = ConversationId{"C1"}, .ts = "", .dueAt = 5},
         MessageReminder{.conv = ConversationId{"C1"}, .ts = "3.4", .dueAt = 9}}
    );
    const auto out = cache.loadReminders();
    REQUIRE(out.size() == 2);
    CHECK(out[0].ts == "1.2");
    CHECK(out[0].dueAt == 0);
    CHECK(out[1].ts == "3.4");
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

TEST_CASE_METHOD(CacheFixture, "AI transcripts round-trip by file id", "[cache][stt]") {
    CHECK(cache.loadAiTranscripts().isEmpty());
    QHash<QString, AiTranscript> in;
    in.insert("F1", {"Тест, раз, два, три.", "OpenAI"});
    in.insert("F2", {"hello", "Local LLM"});
    in.insert("F3", {"", "OpenAI"}); // empty text is never a transcript
    cache.saveAiTranscripts(in);

    const auto out = cache.loadAiTranscripts();
    CHECK(out.size() == 2);
    CHECK(out.value("F1") == AiTranscript{"Тест, раз, два, три.", "OpenAI"});
    CHECK(out.value("F2").provider == "Local LLM");
    CHECK(!out.contains("F3"));

    // Other meta.json keys survive the write.
    cache.saveFollowedThreads({"C1\t1.0"});
    CHECK(cache.loadAiTranscripts().size() == 2);
    CHECK(cache.loadFollowedThreads() == QStringList{"C1\t1.0"});
}

TEST_CASE_METHOD(CacheFixture, "user probe times round-trip", "[cache][user]") {
    CHECK(cache.loadUserProbeTimes().isEmpty());
    const qint64 big = 1'790'000'000'000; // a 2026 Unix-ms stamp: must not truncate to int
    cache.saveUserProbeTimes({{"W0EXT1", big}, {"USLACK", 0}});
    const auto out = cache.loadUserProbeTimes();
    CHECK(out.size() == 2);
    CHECK(out.value("W0EXT1") == big);
    CHECK(out.value("USLACK") == 0);
    // Lives in meta.json next to the other small blobs without clobbering them.
    cache.saveDeadConvIds({"C_DEAD"});
    CHECK(cache.loadUserProbeTimes().size() == 2);
    CHECK(cache.loadDeadConvIds() == QStringList{"C_DEAD"});
}

TEST_CASE_METHOD(CacheFixture, "attachment ids survive caching", "[cache][msg]") {
    // Positional ids address chat.deleteAttachment; a cached copy without them
    // (or from a service that sends none) reads back as 0, never as garbage.
    Message message;
    message.ts          = "310.000";
    message.text        = TextWithEntities{"links", {}};
    message.attachments = {
        Attachment{.id = 1, .title = "First", .isLinkPreview = true},
        Attachment{.id = 2, .title = "Second", .isLinkPreview = true},
        Attachment{.title = "No id"},
    };
    const ConversationId conv{"C_ATTACH_IDS"};
    cache.saveMessages(conv, {message});
    const auto loaded = cache.loadMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == message);
    REQUIRE(loaded[0].attachments.size() == 3);
    CHECK(loaded[0].attachments[0].id == 1);
    CHECK(loaded[0].attachments[1].id == 2);
    CHECK(loaded[0].attachments[2].id == 0);
}
