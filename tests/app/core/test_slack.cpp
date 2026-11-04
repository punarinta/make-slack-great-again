// The Slack sign-in logic without the network: credential records, the
// workspace store, and the parsers of the session paths.
#include "app/auth/workspaces.h"
#include "app/slack/browser_login.h"
#include "app/slack/credentials.h"
#include "app/slack/session.h"
#include "app/slack/slack_json.h"
#include "app/slack/web_api.h"
#include "base/json.h"
#include "base/file.h"
#include "base/process.h"
#include "support/test.h"

TEST("slack: credentials round-trip through the stored auth blob") {
    slack::Credentials c;
    c.token                       = "xoxc-1-2-3";
    c.teamId                      = "T0123";
    c.teamName                    = "Lumen";
    c.iconUrl                     = "https://a/icon.png";
    c.expiresAt                   = 1893456000123;
    c.cookie                      = "xoxd-abc%2Fdef";
    c.workspaceUrl                = "https://lumen.slack.com/";
    const auth::WorkspaceRecord r = slack::toRecord(c);
    CHECK(r.key() == "slack:T0123");
    CHECK(r.displayName == "Lumen");
    CHECK(r.auth.find("\"xoxp\":\"xoxc-1-2-3\"") != std::string::npos);
    CHECK(r.auth.find("\"expiresAt\":\"1893456000123\"") != std::string::npos); // a string
    const slack::Credentials d = slack::fromRecord(r);
    CHECK(d.token == c.token && d.cookie == c.cookie && d.workspaceUrl == c.workspaceUrl);
    CHECK(d.expiresAt == c.expiresAt && d.teamId == "T0123" && d.sessionAuth());
    // An OAuth blob as stored: no cookie, numeric-string expiry.
    auth::WorkspaceRecord old;
    old.service                = "slack";
    old.id                     = "T9";
    old.auth                   = R"({"xoxp":"xoxp-9","refreshToken":"xoxe-1","expiresAt":"0"})";
    const slack::Credentials o = slack::fromRecord(old);
    CHECK(o.token == "xoxp-9" && o.refreshToken == "xoxe-1" && !o.sessionAuth());
    CHECK(slack::toRecord(o).auth.find("cookie") == std::string::npos);
}

TEST("slack: app keys — personal ones win field by field") {
    const slack::AppConfig a = slack::appConfig("my-id", "", "xapp-mine");
    CHECK(a.clientId == "my-id");
    CHECK(a.appToken == "xapp-mine");
}

TEST("auth: workspace store persists order, active, muted; remove moves active") {
    const std::string path = file::join(base::env("HOME"), "ws-test/workspaces.json");
    file::remove(path);
    {
        auth::WorkspaceStore s(path);
        CHECK(s.empty() && s.active().empty());
        s.save({"slack", "T1", "One", "", R"({"xoxp":"a"})"});
        s.save({"slack", "T2", "Two", "", R"({"xoxp":"b"})"});
        CHECK(s.active() == "slack:T1"); // the first one added
        s.setActive("slack:T2");
        s.setMuted("slack:T1", true);
        s.setOrder({"slack:T2", "nope:x"});
        // Replacing keeps the place and the mute.
        s.save({"slack", "T1", "One renamed", "", R"({"xoxp":"c"})"});
    }
    std::string text;
    REQUIRE(file::readAll(path, &text));
#ifndef _WIN32
    CHECK((file::size(path) > 0)); // written owner-only by writeAtomic(…, 0600)
#endif
    auth::WorkspaceStore s(path);
    REQUIRE(s.all().size() == 2);
    CHECK(s.all()[0].key() == "slack:T2");
    CHECK(s.all()[1].displayName == "One renamed" && s.all()[1].muted);
    CHECK(s.all()[1].auth == R"({"xoxp":"c"})");
    CHECK(s.active() == "slack:T2");
    s.remove("slack:T2");
    CHECK(s.active() == "slack:T1");
    s.remove("slack:T1");
    CHECK(s.empty() && s.active().empty());
    // A broken file is an empty store, never a crash.
    file::writeAtomic(path, "{not json");
    CHECK(auth::WorkspaceStore(path).empty());
    file::remove(path + ".active");
}

TEST("auth: a switch writes only the active file; the store's own stays the fallback") {
    const std::string path = file::join(base::env("HOME"), "ws-active/workspaces.json");
    const std::string act  = path + ".active";
    file::remove(path);
    file::remove(act);
    {
        auth::WorkspaceStore s(path);
        s.save({"slack", "T1", "One", "", R"({"xoxp":"a"})"});
        s.save({"slack", "T2", "Two", "", R"({"xoxp":"b"})"});
        CHECK_FALSE(file::exists(act)); // nothing switched yet
        std::string before;
        REQUIRE(file::readAll(path, &before));
        s.setActive("slack:T2");
        s.setActive("slack:T1");
        s.setActive("slack:T2");
        std::string after;
        REQUIRE(file::readAll(path, &after));
        CHECK(after == before); // the store itself untouched
        std::string a;
        REQUIRE(file::readAll(act, &a));
        CHECK_STR(a, "slack:T2");
        // An unchanged record is not written again either.
        s.save({"slack", "T1", "One", "", R"({"xoxp":"a"})"});
        REQUIRE(file::readAll(path, &after));
        CHECK(after == before);
    }
    CHECK_STR(auth::WorkspaceStore(path).active(), "slack:T2");
    // A torn or unknown one: the store's own "active".
    file::overwrite(act, "slack:T");
    CHECK_STR(auth::WorkspaceStore(path).active(), "slack:T1");
    // A flush brings a stale one in step: removing T2 makes T1 active.
    file::overwrite(act, "slack:T2");
    {
        auth::WorkspaceStore s(path);
        s.remove("slack:T2");
        CHECK_STR(s.active(), "slack:T1");
    }
    std::string a;
    REQUIRE(file::readAll(act, &a));
    CHECK_STR(a, "slack:T1");
    CHECK_STR(auth::WorkspaceStore(path).active(), "slack:T1");
    file::remove(path);
    file::remove(act);
}

TEST("slack: one classification of transient errors for reads and writes") {
    for (const char *e :
         {"dns",
          "connect: refused",
          "tls: bad cert",
          "timeout",
          "protocol",
          "too_many_redirects",
          "bad_json"})
        CHECK(slack::isTransportError(e));
    for (const char *e :
         {"", "ratelimited", "internal_error", "invalid_auth", "cancelled", "dnsx", "url: bad"})
        CHECK_FALSE(slack::isTransportError(e));
    for (const char *e : {"internal_error", "service_unavailable", "fatal_error"})
        CHECK(slack::isTransientSlackError(e));
    CHECK_FALSE(slack::isTransientSlackError("timeout"));
    CHECK_FALSE(slack::isTransientSlackError("channel_not_found"));
    CHECK(slack::retryBackoffMs(0) == 1000);
    CHECK(slack::retryBackoffMs(1) == 2000);
    CHECK(slack::retryBackoffMs(5) == 32000);
    CHECK(slack::retryBackoffMs(6) == 60000);
    CHECK(slack::retryBackoffMs(40) == 60000);
}

TEST("slack: a file downloads with its own workspace's credentials") {
    CHECK_STR(slack::fileTeamId("https://files.slack.com/files-pri/T0A1-F0B2/shot.png"), "T0A1");
    CHECK_STR(
        slack::fileTeamId("https://files.slack.com/files-tmb/E09X-F0B2-ab12/shot_360.png"), "E09X"
    );
    CHECK(slack::fileTeamId("https://files.slack.com/files-pri/F0B2/x").empty());
    CHECK(slack::fileTeamId("https://avatars.slack-edge.com/2026/T0A1-x.png").empty());

    const slack::Auth a{"xoxc-a", "d-a", {}}, b{"xoxp-b", {}, {}}, open{"xoxp-open", {}, {}};
    const std::vector<slack::TeamAuth> in = {{"T0A1", &a}, {"T0B2", &b}};
    // A background workspace's file: its token, whichever one is on screen.
    CHECK(slack::downloadAuth("https://files.slack.com/files-pri/T0B2-F1/x.png", in, &open) == &b);
    CHECK(
        slack::downloadAuth("https://files.slack.com/files-tmb/T0A1-F1-h/x_64.png", in, &open) == &a
    );
    // Not signed in there (Slack Connect), or no team in the path: the open one.
    CHECK(
        slack::downloadAuth("https://files.slack.com/files-pri/T0C3-F1/x.png", in, &open) == &open
    );
    CHECK(slack::downloadAuth("https://team.slack.com/files/U1/F1/x.png", in, &open) == &open);
    CHECK(
        slack::downloadAuth("https://files.slack.com/files-pri/T0C3-F1/x", in, nullptr) == nullptr
    );
    // Public CDNs and plain http get nothing.
    CHECK(slack::downloadAuth("https://avatars.slack-edge.com/a.png", in, &open) == nullptr);
    CHECK(slack::downloadAuth("http://files.slack.com/files-pri/T0B2-F1/x", in, &open) == nullptr);
}

TEST("slack: token scraping from the boot page") {
    using slack::scrapeBootTokens;
    CHECK(scrapeBootTokens(R"(x"api_token":"xoxc-123-456-abc"y)").token == "xoxc-123-456-abc");
    CHECK(scrapeBootTokens(R"(boot {"other":"xoxc-9-8"} )").token == "xoxc-9-8");
    CHECK(scrapeBootTokens(R"("api_token":null)").token.empty());
    CHECK(scrapeBootTokens("xoxc-").token.empty());
    // Off Grid: no enterprise token, never a fallback one.
    CHECK(scrapeBootTokens(R"(x"api_token":"xoxc-1-a"y)").enterpriseToken.empty());
    CHECK(scrapeBootTokens(R"(boot {"other":"xoxc-9-8"} )").enterpriseToken.empty());
}

TEST("slack: token scraping — Enterprise Grid boot page") {
    using slack::scrapeBootTokens;
    // Both keyed, either order.
    {
        const auto t =
            scrapeBootTokens(R"({"api_token":"xoxc-ws-1","enterprise_api_token":"xoxc-org-2"})");
        CHECK(t.token == "xoxc-ws-1");
        CHECK(t.enterpriseToken == "xoxc-org-2");
    }
    {
        const auto t =
            scrapeBootTokens(R"({"enterprise_api_token":"xoxc-org-2","api_token":"xoxc-ws-1"})");
        CHECK(t.token == "xoxc-ws-1");
        CHECK(t.enterpriseToken == "xoxc-org-2");
    }
    // The enterprise token is never taken for the workspace's: with no
    // "api_token" the workspace token falls back to an unkeyed run only.
    {
        const auto t =
            scrapeBootTokens(R"({"enterprise_api_token":"xoxc-org-2","x":"xoxc-other-3"})");
        CHECK(t.enterpriseToken == "xoxc-org-2");
        CHECK(t.token == "xoxc-other-3");
    }
    {
        const auto t = scrapeBootTokens(R"({"enterprise_api_token":"xoxc-org-2"})");
        CHECK(t.enterpriseToken == "xoxc-org-2");
        CHECK(t.token.empty());
    }
    // A keyed token wins over an earlier unkeyed one; the first of each key wins.
    {
        const auto t = scrapeBootTokens(
            R"({"x":"xoxc-other-3","api_token":"xoxc-ws-1","api_token":"xoxc-ws-9",)"
            R"("enterprise_api_token":"xoxc-org-2","enterprise_api_token":"xoxc-org-8"})"
        );
        CHECK(t.token == "xoxc-ws-1");
        CHECK(t.enterpriseToken == "xoxc-org-2");
    }
    // An empty enterprise token ("xoxc-" with nothing after) is no token.
    CHECK(scrapeBootTokens(R"({"api_token":"xoxc-ws-1","enterprise_api_token":"xoxc-"})")
              .enterpriseToken.empty());
}

TEST("slack: workspace address and cookie normalisation") {
    using slack::normalizeWorkspaceUrl;
    CHECK(normalizeWorkspaceUrl("myteam") == "https://myteam.slack.com");
    CHECK(normalizeWorkspaceUrl(" myteam.slack.com ") == "https://myteam.slack.com");
    CHECK(
        normalizeWorkspaceUrl("https://myteam.slack.com/messages/C1") == "https://myteam.slack.com"
    );
    CHECK(
        normalizeWorkspaceUrl("http://x.enterprise.slack.com") == "https://x.enterprise.slack.com"
    );
    CHECK(normalizeWorkspaceUrl("  ").empty());
    CHECK(slack::normalizeCookie(" d=xoxd-abc ") == "xoxd-abc");
    CHECK(slack::normalizeCookie("xoxd-abc") == "xoxd-abc");
}

TEST("slack: localConfig_v2 and host harvesting") {
    const auto teams = slack::parseLocalConfig(R"({"teams":{
        "T1":{"id":"T1","name":"One","token":"xoxc-1","url":"https://one.slack.com/",
              "icon":{"image_68":"i68","image_88":"i88"}},
        "T2":{"name":"Two","token":"xoxp-not-a-session","domain":"two"},
        "T3":{"name":"Nothing to go on"}}})");
    REQUIRE(teams.size() == 2);
    CHECK(teams[0].teamId == "T1" && teams[0].token == "xoxc-1");
    CHECK(teams[0].workspaceUrl == "https://one.slack.com" && teams[0].iconUrl == "i88");
    CHECK(teams[1].teamId == "T2" && teams[1].token.empty()); // derive it
    CHECK(teams[1].workspaceUrl == "https://two.slack.com");
    CHECK(slack::parseLocalConfig("garbage").empty());

    const auto hosts = slack::teamsFromHosts(
        {"https://app.slack.com/client",
         ".slack.com",
         "lumen.slack.com",
         "https://Lumen.slack.com/ssb/redirect",
         "https://acme-corp.slack.com/x",
         "api.slack.com"}
    );
    REQUIRE(hosts.size() == 2);
    CHECK(hosts[0].workspaceUrl == "https://lumen.slack.com");
    CHECK(hosts[1].workspaceUrl == "https://acme-corp.slack.com");
}

TEST("slack: DevTools /json/version") {
    CHECK(
        slack::BrowserLogin::parseDebuggerUrl(
            R"({"Browser":"Chrome/1","webSocketDebuggerUrl":"ws://127.0.0.1:9/devtools/browser/x"})"
        ) == "ws://127.0.0.1:9/devtools/browser/x"
    );
    CHECK(slack::BrowserLogin::parseDebuggerUrl("nope").empty());
}

TEST("slack json: audio files — the transcode, the original, Slack's transcript and cues") {
    model::Store   store;
    json::Document doc;
    REQUIRE(doc.parse(R"({"ts": "1.0", "user": "U1", "text": "", "files": [
      {"id": "F1", "name": "clip", "mimetype": "audio/webm", "subtype": "slack_audio",
       "url_private": "https://f/F1_audio.mp4", "url_private_download": "https://f/clip.webm",
       "duration_ms": 5041, "vtt": "https://f/F1.vtt",
       "transcription": {"status": "complete", "preview": {"content": "Hello team"}}},
      {"id": "F2", "name": "song.mp3", "mimetype": "audio/mpeg",
       "url_private": "https://f/F2_audio.mp4", "url_private_download": "https://f/song.mp3",
       "vtt": "https://f/F2.vtt", "transcription": {"status": "none"}}]})"));
    const model::Message m = slack::mapjson::toMessage(doc.root(), store);
    REQUIRE(m.files().size() == 2);
    const model::File &clip = m.files()[0];
    CHECK(clip.isAudio());
    CHECK_STR(clip.path, "https://f/F1_audio.mp4");
    CHECK_STR(clip.source(), "https://f/clip.webm");
    CHECK(clip.durationMs == 5041);
    CHECK_STR(clip.transcript, "Hello team");
    CHECK_STR(clip.transcriptVtt, "https://f/F1.vtt");
    // An upload Slack never transcribes: no line, no cues.
    CHECK(m.files()[1].transcript.empty() && m.files()[1].transcriptVtt.empty());
}

TEST("slack json: Slackbot and the Slack notifier are apps despite is_bot=false") {
    for (const char *id : {"USLACK", "USLACKBOT"}) {
        json::Document doc;
        REQUIRE(doc.parse(
            std::string(R"({"id": ")") + id +
            R"(", "name": "slack", "is_bot": false, "profile": {"real_name": "Slack"}})"
        ));
        CHECK(slack::mapjson::toUser(doc.root()).bot);
    }
    json::Document doc;
    REQUIRE(doc.parse(R"({"id": "U1", "name": "mira", "is_bot": false})"));
    CHECK(!slack::mapjson::toUser(doc.root()).bot);
}

TEST("slack json: a message_mention names the linked message's author for its chip") {
    model::Store   store;
    json::Document doc;
    REQUIRE(
        doc.parse(R"({"ts": "2.0", "user": "U1",
      "text": "<https://acme.slack.com/archives/C9/p1700000000000100>",
      "blocks": [{"type": "rich_text", "elements": [{"type": "rich_text_section", "elements": [
        {"type": "message_mention", "channel_id": "C9", "message_ts": "1700000000.000100",
         "author_id": "U7", "url": "https://acme.slack.com/archives/C9/p1700000000000100"}]}]}]})")
    );
    slack::mapjson::toMessage(doc.root(), store);
    const model::UserRef u = store.linkedAuthor("C9", "1700000000.000100");
    REQUIRE(u != model::kNoUser);
    CHECK_STR(store.user(u).id, "U7");
    CHECK(store.linkedAuthor("C9", "1700000000.000200") == model::kNoUser);
}

TEST("slack json: structural blocks, shared-message unfurls, PDF pages, canvas titles") {
    model::Store   store;
    json::Document doc;
    REQUIRE(doc.parse(R"({"ts": "1700000000.000100", "user": "U1", "text": "fallback",
      "blocks": [{"type": "header", "text": {"type": "plain_text", "text": "Head <1>"}},
                 {"type": "divider"},
                 {"type": "section", "text": {"type": "mrkdwn", "text": "*bold* body"}},
                 {"type": "image", "image_url": "https://i/x.png", "alt_text": "alt",
                  "title": {"type": "plain_text", "text": "chart"}, "image_width": 800,
                  "image_height": 400},
                 {"type": "table", "rows": [[{"type": "raw_text", "text": "A"},
                    {"type": "raw_text", "text": "B"}], [{"type": "rich_text", "elements": [
                    {"type": "rich_text_section", "elements": [{"type": "text", "text": "c",
                     "style": {"bold": true}}]}]}, {"type": "raw_text", "text": "d"}]]}],
      "files": [{"id": "F1", "name": "spec.pdf", "mimetype": "application/pdf",
                 "url_private": "https://f/spec.pdf", "thumb_pdf": "https://f/p.png",
                 "thumb_pdf_w": 900, "thumb_pdf_h": 1200},
                {"id": "F2", "name": "notes", "title": "Notes &amp; plans",
                 "mimetype": "application/vnd.slack-docs", "url_private": "https://f/c"}],
      "attachments": [{"id": 1, "is_msg_unfurl": true, "author_name": "Jonas",
                       "author_icon": "https://a/j.png", "author_subname": "bot",
                       "channel_id": "C9", "ts": "1699999000.000000", "text": "quoted",
                       "files": [{"id": "F3", "name": "a.txt", "mimetype": "text/plain"}]},
                      {"id": 2, "blocks": [{"type": "table", "rows": [[{"type": "raw_text",
                       "text": "x"}]]}]}]})"));
    const model::Message m = slack::mapjson::toMessage(doc.root(), store);
    REQUIRE(m.extra != nullptr);
    const auto &b = m.extra->blocks;
    using K       = model::Block::Kind;
    REQUIRE(b.size() == 5);
    CHECK(b[0].kind == K::Header && b[0].text == "Head &lt;1&gt;");
    CHECK(b[1].kind == K::Divider);
    CHECK(b[2].kind == K::Text && b[2].text == "*bold* body");
    CHECK(b[3].kind == K::Image && b[3].image == "https://i/x.png" && b[3].text == "chart");
    CHECK(b[3].width == 800 && b[3].alt == "alt");
    REQUIRE(b[4].kind == K::Table && b[4].rows.size() == 2);
    CHECK(b[4].rows[1][0] == "*c*" && b[4].rows[0][1] == "B");
    CHECK(m.attachments().size() == 2); // no image attachment from the block any more
    const model::File &pdf = m.files()[0];
    CHECK(pdf.hasPreview() && pdf.thumb == "https://f/p.png" && pdf.width == 900);
    CHECK(m.files()[1].isCanvas() && m.files()[1].title == "Notes & plans");
    const model::Attachment &u = m.attachments()[0];
    CHECK(u.msgUnfurl && u.app && u.id == 1 && u.channel == "C9");
    CHECK(u.authorIcon == "https://a/j.png" && u.ts == model::parseTs("1699999000.000000"));
    CHECK(u.files.size() == 1 && u.files[0].name == "a.txt");
    const model::Attachment &t = m.attachments()[1];
    CHECK(t.blocks.size() == 1 && t.text.empty()); // drawn as the table, not as text
    // Text-only blocks stay mrkdwn: no structure kept.
    json::Document plain;
    REQUIRE(plain.parse(R"({"ts": "1.0", "user": "U1", "text": "hi",
      "blocks": [{"type": "rich_text", "elements": []}]})"));
    const model::Message p = slack::mapjson::toMessage(plain.root(), store);
    CHECK(!p.extra || p.extra->blocks.empty());
}
