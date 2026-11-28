// Slack workflows (Workflow Builder) and the forms they show: the mappers
// over answers captured from Slack (a featured workflow's channel, its
// shortcut bookmark, the "Collect info in a form" modal), and SlackBackend
// starting one, showing its form and submitting it against the local fake
// Slack (tests/support/fake_slack.py), the form arriving as view_opened on
// the RTM socket. Nothing here talks to the real Slack.
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "support/fake_slack_server.h"
#include "base/json.h"
#include "base/str.h"
#include "support/test.h"
#include "net/net.h"
#include "plat/plat.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using model::FormField;
using Kind = model::FormField::Kind;

json::Document parse(const char *text) {
    json::Document d;
    CHECK(d.parse(std::string_view(text), nullptr));
    return d;
}

// The "PR for review" form as Slack sent it (view_opened's view, trimmed of
// the fields the app doesn't read).
const char *kPrForm = R"({
 "id": "V0CM8ETSNES", "type": "modal", "callback_id": "hermes_form",
 "title": {"type": "plain_text", "text": "Post a PR for review", "emoji": true},
 "submit": {"type": "plain_text", "text": "Submit", "emoji": true},
 "close": {"type": "plain_text", "text": "Close", "emoji": true},
 "blocks": [
  {"type": "input", "block_id": "vWR", "optional": false,
   "label": {"type": "plain_text", "text": "Pull request URL", "emoji": true},
   "element": {"type": "plain_text_input", "action_id": "8140d629", "multiline": false}},
  {"type": "input", "block_id": "jBcAM", "optional": true,
   "label": {"type": "plain_text", "text": "Anything reviewers should know?", "emoji": true},
   "element": {"type": "plain_text_input", "action_id": "b76d19e0", "multiline": true}},
  {"type": "input", "block_id": "yc/hy", "optional": true,
   "label": {"type": "plain_text", "text": "Reviewer", "emoji": true},
   "hint": {"type": "plain_text", "text": "Pick specific reviewers if needed.", "emoji": true},
   "element": {"type": "multi_conversations_select", "action_id": "80b9e64f",
     "placeholder": {"type": "plain_text", "text": "Select users", "emoji": true},
     "filter": {"include": ["im"]}}},
  {"type": "input", "block_id": "qqc", "optional": false,
   "label": {"type": "plain_text", "text": "Self review", "emoji": true},
   "element": {"type": "checkboxes", "action_id": "6d87ed43", "options": [
     {"text": {"type": "plain_text", "text": "Manually self reviewed", "emoji": true},
      "value": "Manually self reviewed"},
     {"text": {"type": "plain_text", "text": "AI reviewed", "emoji": true},
      "value": "AI reviewed"},
     {"text": {"type": "plain_text", "text": "Didn’t self review", "emoji": true},
      "value": "Didn’t self review"}]}}]
})";

// A second form step as views.submit answered with it: a dropdown whose
// options' text is rich_text, and a date.
const char *kStepTwo = R"({
 "id": "V0CM57Z449K", "type": "modal",
 "title": {"type": "plain_text", "text": "Step two", "emoji": true},
 "submit": {"type": "plain_text", "text": "Submit", "emoji": true},
 "close": {"type": "plain_text", "text": "Close", "emoji": true},
 "blocks": [
  {"type": "input", "block_id": "sfIsr", "optional": false,
   "label": {"type": "plain_text", "text": "Size", "emoji": true},
   "element": {"type": "static_select", "action_id": "b912dabe",
    "placeholder": {"type": "plain_text", "text": "Select an option", "emoji": true},
    "options": [
     {"text": {"type": "rich_text", "block_id": "f5", "elements": [{"type": "rich_text_section",
        "elements": [{"type": "text", "text": "Small"}]}]}, "value": "Small"},
     {"text": {"type": "rich_text", "block_id": "fb", "elements": [{"type": "rich_text_section",
        "elements": [{"type": "text", "text": "Large"}]}]}, "value": "Large"}]}},
  {"type": "input", "block_id": "dt", "optional": true,
   "label": {"type": "plain_text", "text": "When", "emoji": true},
   "element": {"type": "datepicker", "action_id": "81339c1d"}},
  {"type": "section", "block_id": "s1", "text": {"type": "mrkdwn", "text": "Thanks &amp; *bye*"}},
  {"type": "input", "block_id": "up", "optional": true,
   "label": {"type": "plain_text", "text": "Screenshot", "emoji": true},
   "element": {"type": "file_input", "action_id": "f1"}}]
})";

} // namespace

TEST("slack workflows: a workflow form maps every field Slack gave it") {
    const json::Document d = parse(kPrForm);
    const model::Form    f = slack::mapjson::toForm(d.root());
    CHECK_STR(f.id, "V0CM8ETSNES");
    CHECK_STR(f.title, "Post a PR for review");
    CHECK_STR(f.submit, "Submit");
    CHECK_STR(f.close, "Close");
    REQUIRE(f.fields.size() == 4);
    CHECK(f.fields[0].kind == Kind::Text);
    CHECK_STR(f.fields[0].label, "Pull request URL");
    CHECK_FALSE(f.fields[0].optional);
    CHECK_STR(f.fields[0].blockId, "vWR");
    CHECK_STR(f.fields[0].actionId, "8140d629");
    CHECK(f.fields[1].kind == Kind::Paragraph);
    CHECK(f.fields[1].optional);
    // A conversations picker of DMs only is a people picker.
    CHECK(f.fields[2].kind == Kind::People);
    CHECK(f.fields[2].multi);
    CHECK_STR(f.fields[2].hint, "Pick specific reviewers if needed.");
    CHECK_STR(f.fields[2].placeholder, "Select users");
    CHECK(f.fields[3].kind == Kind::Checkboxes);
    REQUIRE(f.fields[3].options.size() == 3);
    CHECK_STR(f.fields[3].options[2].text, "Didn’t self review");
}

TEST("slack workflows: answers go back as views.submit's state, options as they came") {
    const json::Document           d = parse(kPrForm);
    const model::Form              f = slack::mapjson::toForm(d.root());
    std::vector<model::FormAnswer> a(4);
    a[0].text   = "https://github.com/example/repo/pull/1";
    a[2].ids    = {"U0CM458JTAP"};
    a[3].chosen = {1};
    json::Document s;
    REQUIRE(s.parse(slack::mapjson::formState(f, a), nullptr));
    const json::Value v = s.root()["values"];
    CHECK_STR(v["vWR"]["8140d629"]["type"].str(), "plain_text_input");
    CHECK_STR(v["vWR"]["8140d629"]["value"].str(), "https://github.com/example/repo/pull/1");
    // Nothing typed: null, as Slack's own client sends it.
    CHECK(v["jBcAM"]["b76d19e0"]["value"].isNull());
    const json::Value people = v["yc/hy"]["80b9e64f"];
    CHECK_STR(people["type"].str(), "multi_conversations_select");
    REQUIRE(people["selected_conversations"].size() == 1);
    CHECK_STR(people["selected_conversations"][0].str(), "U0CM458JTAP");
    const json::Value boxes = v["qqc"]["6d87ed43"]["selected_options"];
    REQUIRE(boxes.size() == 1);
    CHECK_STR(boxes[0]["value"].str(), "AI reviewed");
    CHECK_STR(boxes[0]["text"]["text"].str(), "AI reviewed");
    CHECK(boxes[0]["text"]["emoji"].boolean());
}

TEST("slack workflows: rich_text options, dates, notes and fields this app can't fill in") {
    const json::Document d = parse(kStepTwo);
    const model::Form    f = slack::mapjson::toForm(d.root());
    REQUIRE(f.fields.size() == 4);
    CHECK(f.fields[0].kind == Kind::Select);
    CHECK_FALSE(f.fields[0].multi);
    REQUIRE(f.fields[0].options.size() == 2);
    CHECK_STR(f.fields[0].options[1].text, "Large");
    CHECK(f.fields[1].kind == Kind::Date);
    CHECK(f.fields[2].kind == Kind::Note);
    CHECK_STR(f.fields[2].label, "Thanks &amp; *bye*"); // mrkdwn, as written
    CHECK(f.fields[3].kind == Kind::Unsupported);
    CHECK_STR(f.fields[3].type, "file_input");

    std::vector<model::FormAnswer> a(4);
    a[0].chosen = {1};
    a[1].text   = "2026-11-27";
    json::Document s;
    REQUIRE(s.parse(slack::mapjson::formState(f, a), nullptr));
    const json::Value v   = s.root()["values"];
    const json::Value opt = v["sfIsr"]["b912dabe"]["selected_option"];
    CHECK_STR(opt["value"].str(), "Large");
    // The rich_text goes back untouched.
    CHECK_STR(opt["text"]["type"].str(), "rich_text");
    CHECK_STR(v["dt"]["81339c1d"]["selected_date"].str(), "2026-11-27");
    // Notes and unfillable fields aren't answers.
    CHECK_FALSE(v.has("s1"));
    CHECK_FALSE(v.has("up"));
}

TEST("slack workflows: every element type's initial answer and its state key") {
    const json::Document d = parse(R"({"id": "V1", "blocks": [
      {"type": "input", "block_id": "a", "label": {"type": "plain_text", "text": "A"},
       "element": {"type": "users_select", "action_id": "x", "initial_user": "UMIRA"}},
      {"type": "input", "block_id": "b", "label": {"type": "plain_text", "text": "B"},
       "element": {"type": "multi_channels_select", "action_id": "x",
                   "initial_channels": ["C1", "C2"]}},
      {"type": "input", "block_id": "c", "label": {"type": "plain_text", "text": "C"},
       "element": {"type": "conversations_select", "action_id": "x",
                   "initial_conversation": "C9"}},
      {"type": "input", "block_id": "d", "label": {"type": "plain_text", "text": "D"},
       "element": {"type": "radio_buttons", "action_id": "x",
         "options": [{"text": {"type": "plain_text", "text": "One"}, "value": "1"},
                     {"text": {"type": "plain_text", "text": "Two"}, "value": "2"}],
         "initial_option": {"text": {"type": "plain_text", "text": "Two"}, "value": "2"}}},
      {"type": "input", "block_id": "e", "label": {"type": "plain_text", "text": "E"},
       "element": {"type": "number_input", "action_id": "x", "is_decimal_allowed": true,
                   "initial_value": "1.5"}},
      {"type": "input", "block_id": "f", "label": {"type": "plain_text", "text": "F"},
       "element": {"type": "rich_text_input", "action_id": "x"}},
      {"type": "input", "block_id": "g", "label": {"type": "plain_text", "text": "G"},
       "element": {"type": "datetimepicker", "action_id": "x", "initial_date_time": 1800000000}},
      {"type": "input", "block_id": "h", "label": {"type": "plain_text", "text": "H"},
       "element": {"type": "multi_static_select", "action_id": "x", "option_groups": [
         {"label": {"type": "plain_text", "text": "G1"},
          "options": [{"text": {"type": "plain_text", "text": "P"}, "value": "p"},
                      {"text": {"type": "plain_text", "text": "Q"}, "value": "q"}]}],
         "initial_options": [{"text": {"type": "plain_text", "text": "Q"}, "value": "q"}]}},
      {"type": "input", "block_id": "i", "label": {"type": "plain_text", "text": "I"},
       "element": {"type": "timepicker", "action_id": "x", "initial_time": "09:30"}}]})");
    const model::Form    f = slack::mapjson::toForm(d.root());
    REQUIRE(f.fields.size() == 9);
    CHECK(f.fields[0].kind == Kind::People);
    CHECK_FALSE(f.fields[0].multi);
    REQUIRE(f.fields[0].initial.ids.size() == 1);
    CHECK_STR(f.fields[0].initial.ids[0], "UMIRA");
    CHECK(f.fields[1].kind == Kind::Channels);
    CHECK(f.fields[1].initial.ids.size() == 2);
    CHECK(f.fields[2].kind == Kind::Channels); // not DMs only
    CHECK(f.fields[3].kind == Kind::Radio);
    REQUIRE(f.fields[3].initial.chosen.size() == 1);
    CHECK(f.fields[3].initial.chosen[0] == 1);
    CHECK(f.fields[4].kind == Kind::Number);
    CHECK(f.fields[4].decimal);
    CHECK_STR(f.fields[4].initial.text, "1.5");
    CHECK(f.fields[5].kind == Kind::Paragraph);
    CHECK(f.fields[6].kind == Kind::DateTime);
    CHECK_STR(f.fields[6].initial.text, "1800000000");
    CHECK(f.fields[7].kind == Kind::Select);
    CHECK(f.fields[7].multi);
    REQUIRE(f.fields[7].options.size() == 2); // the group's, flattened
    REQUIRE(f.fields[7].initial.chosen.size() == 1);
    CHECK(f.fields[7].initial.chosen[0] == 1);
    CHECK(f.fields[8].kind == Kind::Time);

    // Each answered with its initial one.
    std::vector<model::FormAnswer> a;
    for (const FormField &x : f.fields)
        a.push_back(x.initial);
    a[5].text = "Hello";
    json::Document s;
    REQUIRE(s.parse(slack::mapjson::formState(f, a), nullptr));
    const json::Value v = s.root()["values"];
    CHECK_STR(v["a"]["x"]["selected_user"].str(), "UMIRA");
    CHECK(v["b"]["x"]["selected_channels"].size() == 2);
    CHECK_STR(v["c"]["x"]["selected_conversation"].str(), "C9");
    CHECK_STR(v["d"]["x"]["selected_option"]["value"].str(), "2");
    CHECK_STR(v["e"]["x"]["value"].str(), "1.5");
    const json::Value rich = v["f"]["x"]["rich_text_value"];
    CHECK_STR(rich["type"].str(), "rich_text");
    CHECK_STR(rich["elements"][0]["elements"][0]["text"].str(), "Hello");
    CHECK(v["g"]["x"]["selected_date_time"].integer() == 1800000000);
    CHECK_STR(v["h"]["x"]["selected_options"][0]["value"].str(), "q");
    CHECK_STR(v["i"]["x"]["selected_time"].str(), "09:30");
    // Empty ones: null and [].
    std::vector<model::FormAnswer> none(f.fields.size());
    REQUIRE(s.parse(slack::mapjson::formState(f, none), nullptr));
    const json::Value e = s.root()["values"];
    CHECK(e["a"]["x"]["selected_user"].isNull());
    CHECK(e["b"]["x"]["selected_channels"].size() == 0);
    CHECK(e["d"]["x"]["selected_option"].isNull());
    CHECK(e["g"]["x"]["selected_date_time"].isNull());
}

TEST("slack workflows: errors by block, the trigger of a link, featured and bookmarked ones") {
    const json::Document d    = parse(kPrForm);
    const model::Form    f    = slack::mapjson::toForm(d.root());
    const json::Document e    = parse(R"({"vWR": "Not a link", "nope": "x"})");
    const auto           errs = slack::mapjson::formErrors(f, e.root());
    REQUIRE(errs.size() == 4);
    CHECK_STR(errs[0], "Not a link");
    CHECK(errs[1].empty());

    CHECK_STR(
        slack::mapjson::workflowTriggerOf(
            "https://slack.com/shortcuts/Ft0CMA79FDJ6/e78e4a9295f8a1f06456f403a3723269"
        ),
        "Ft0CMA79FDJ6"
    );
    CHECK(slack::mapjson::workflowTriggerOf("https://slack.com/shortcuts/").empty());
    CHECK(slack::mapjson::workflowTriggerOf("https://example.com/Ft0/x").empty());

    // conversations.info's properties name the featured ones.
    model::Store              st;
    const json::Document      c    = parse(R"({"id": "C0CM45AB9R9", "name": "new-channel",
      "is_channel": true, "properties": {"tabs": [{"type": "workflows", "id": "workflows"}],
      "channel_workflows": [{"workflow_trigger_id": "Ft0CMA79FDJ6", "title": "PR &amp; review"},
                            {"title": "no trigger"}]}})");
    const model::Conversation conv = slack::mapjson::toConversation(c.root(), st);
    REQUIRE(conv.featuredWorkflows.size() == 1);
    CHECK_STR(conv.featuredWorkflows[0].trigger, "Ft0CMA79FDJ6");
    CHECK_STR(conv.featuredWorkflows[0].title, "PR & review");

    // A shortcut bookmark is a workflow, by its shortcut_id or its link.
    const json::Document b = parse(
        R"([
      {"id": "Bk1", "type": "shortcut", "title": "Post a PR link",
       "link": "https://slack.com/shortcuts/Ft0CMA79FDJ6/e78e", "shortcut_id": "Ft0CMA79FDJ6",
       "rank": "U"},
      {"id": "Bk2", "type": "shortcut", "title": "Two forms",
       "link": "https://slack.com/shortcuts/Ft0CM49AQAFM/a3dc", "rank": "V"},
      {"id": "Bk3", "type": "link", "title": "Docs", "link": "https://docs.example", "rank": "W"}])"
    );
    const auto marks = slack::mapjson::toBookmarks(b.root());
    REQUIRE(marks.size() == 3);
    CHECK_STR(marks[0].workflow, "Ft0CMA79FDJ6");
    CHECK_STR(marks[1].workflow, "Ft0CM49AQAFM");
    CHECK(marks[2].workflow.empty());
}

// ── SlackBackend against the fake Slack ─────────────────────────────────────
#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpFor;
using fakeslack::pumpUntil;
using model::ConvRef;
using Link = model::Backend::PresenceLink;

void set(const std::string &json) {
    ctl("POST", "/_ctl/set", json);
}
void script(const char *method, const std::string &responses) {
    ctl("POST",
        "/_ctl/script",
        std::string("{\"method\":\"") + method + "\",\"responses\":" + responses + "}");
}

struct Log {
    json::Document doc;
    Log() { doc.parse(ctl("GET", "/_ctl/log").body, nullptr); }
    int count(std::string_view method) const {
        int n = 0;
        for (json::Value r : doc.root())
            n += r["method"].str() == method;
        return n;
    }
    json::Value get(std::string_view method, int n = 0) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method && n-- == 0)
                return r;
        return {};
    }
};

int count(std::string_view method) {
    return Log().count(method);
}

// The open RTM socket's id ("" when none).
std::string openRtm() {
    json::Document d;
    d.parse(ctl("GET", "/_ctl/ws").body, nullptr);
    std::string id;
    for (json::Value c : d.root())
        if (c["open"].boolean() && c["conn"].str().substr(0, 4) == "rtm-")
            id = std::string(c["conn"].str());
    return id;
}

// A frame from Slack on the open RTM socket.
void push(const std::string &frame) {
    std::string body = R"({"conn":")" + openRtm() + R"(","text":)";
    json::escapeString(body, frame);
    body += '}';
    ctl("POST", "/_ctl/ws/send", std::move(body));
}

const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true}],
   "response_metadata": {"next_cursor": ""}},
 "stars.list": {"ok": true, "items": []},
 "emoji.list": {"ok": true, "emoji": {}},
 "usergroups.list": {"ok": true, "usergroups": []},
 "users.getPresence": {"ok": true, "presence": "active", "online": true},
 "client.counts": {"ok": true, "channels": [], "ims": [], "mpims": []},
 "saved.list": {"ok": true, "saved_items": []},
 "subscriptions.thread.getView": {"ok": true, "threads": [], "total_unread_replies": 0},
 "conversations.history": {"ok": true, "messages": [], "has_more": false},
 "workflows.triggers.info": {"ok": true, "trigger": {"id": "Ft1", "type": "shortcut",
     "name": "PR for review", "shortcut_url": "https://slack.com/shortcuts/Ft1/abc",
     "workflow": {"title": "PR for review", "description": "Ask for a code review"}}},
 "workflows.triggers.trip": {"ok": true, "trigger_execution_id": "Tr1",
     "function_execution_id": "Fx1"}
})";

struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;

    explicit Env(bool session = true) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        ctl("POST", "/_ctl/reset");
        set(kWorkspace);
        slack::Credentials cr;
        cr.token  = session ? "xoxc-test" : "xoxp-test";
        cr.cookie = session ? "xoxd-test" : "";
        cr.teamId = "T1";
        be        = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
    }
    ~Env() {
        be.reset();
        base::test::unsetEnv("MSGA_SLACK_TEST_SPEEDUP");
    }
    bool connect() {
        bool called = false, ok = false;
        be->connect([&](bool o, const std::string &) {
            called = true;
            ok     = o;
        });
        return pumpUntil([&] { return called && !be->connecting(); }) && ok;
    }
    ConvRef conv(const char *id) const { return store.findConversation(id); }
};

bool haveServer() {
    if (!fakeslack::server().empty())
        return true;
    std::fprintf(stderr, "  skip: no fake Slack (python3 missing)\n");
    return false;
}

// A start under way: its answer, once it came.
struct Started {
    bool        called = false;
    model::Form form;
    std::string error;
};

// Starts trigger Ft1 from C1 and waits for the trip; the trip's client token.
std::string startAndTrip(Env &e, Started &s, int trips = 1) {
    e.be->startWorkflow(e.conv("C1"), "Ft1", [&s](model::Form f, std::string err) {
        s.called = true;
        s.form   = std::move(f);
        s.error  = std::move(err);
    });
    if (!pumpUntil([trips] { return count("workflows.triggers.trip") >= trips; }, 5000))
        return {};
    return std::string(
        Log().get("workflows.triggers.trip", trips - 1)["form"]["client_token"].str()
    );
}

std::string viewOpened(const std::string &token, const char *view) {
    return str::concat(
        {R"({"type":"view_opened","view_id":"V0CM8ETSNES","client_token":")",
         token,
         R"(","view":)",
         view,
         "}"}
    );
}

} // namespace

TEST("slack workflows: a start holds the RTM link, trips, and its form comes as view_opened") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.be->capabilities().workflows);
    // Native presence: no socket of its own until a workflow needs one.
    pumpFor(300);
    CHECK(openRtm().empty());
    Started           s;
    const std::string token = startAndTrip(e, s);
    REQUIRE(!token.empty());
    CHECK(token.starts_with("msga-"));
    const Log         log;
    const json::Value info = log.get("workflows.triggers.info")["form"];
    CHECK_STR(info["trigger_id"].str(), "Ft1");
    const json::Value trip = log.get("workflows.triggers.trip")["form"];
    CHECK_STR(trip["url"].str(), "https://slack.com/shortcuts/Ft1/abc");
    CHECK_STR(trip["run_precheck"].str(), "true");
    json::Document ctx;
    REQUIRE(ctx.parse(std::string(trip["context"].str()), nullptr));
    CHECK_STR(ctx.root()["location"].str(), "slash_command");
    CHECK_STR(ctx.root()["channel_id"].str(), "C1");
    // The socket is up (it had to be before the trip).
    REQUIRE(!openRtm().empty());
    CHECK(e.be->presenceLink() == Link::Active);
    // Somebody else's form (another client's token) is not ours.
    push(viewOpened("web-123", kPrForm));
    pumpFor(300);
    CHECK_FALSE(s.called);
    push(viewOpened(token, kPrForm));
    REQUIRE(pumpUntil([&] { return s.called; }));
    CHECK(s.error.empty());
    CHECK_STR(s.form.id, "V0CM8ETSNES");
    CHECK_STR(s.form.title, "Post a PR for review");
    REQUIRE(s.form.fields.size() == 4);
    CHECK_STR(s.form.fallbackUrl, "https://lumen.slack.com/archives/C1");
    // The hold is over: Native drops the link again.
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Off; }));
    // The trigger's link is known now: a second start asks Slack nothing more.
    Started again;
    REQUIRE(!startAndTrip(e, again, 2).empty());
    CHECK(count("workflows.triggers.info") == 1);
}

TEST("slack workflows: submits go once each; the next step, field errors, a failure, done") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const json::Document pr   = parse(kPrForm);
    model::Form          form = slack::mapjson::toForm(pr.root());
    form.fallbackUrl          = "https://lumen.slack.com/archives/C1";
    script(
        "views.submit",
        str::concat(
            {R"([{"ok":true,"response_action":"update","view":)",
             kStepTwo,
             R"(},
          {"ok":true,"response_action":"errors","errors":{"sfIsr":"Pick one"}},
          {"ok":false,"error":"internal_error"},
          {"ok":true,"view":null,"response_action":"clear"}])"}
        )
    );
    std::vector<model::FormAnswer> a(4);
    a[0].text                         = "https://github.com/x/y/pull/1";
    a[3].chosen                       = {0};
    bool                       called = false;
    model::Backend::FormResult r;
    auto                       take = [&](model::Backend::FormResult x) {
        called = true;
        r      = std::move(x);
    };
    e.be->submitForm(form, a, take);
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(r.error.empty());
    CHECK_STR(r.next.id, "V0CM57Z449K");
    CHECK_STR(r.next.title, "Step two");
    CHECK_STR(r.next.fallbackUrl, "https://lumen.slack.com/archives/C1");
    const Log         log; // the values below point into it
    const json::Value sent = log.get("views.submit")["form"];
    CHECK_STR(sent["view_id"].str(), "V0CM8ETSNES");
    CHECK(std::string(sent["client_token"].str()).starts_with("msga-"));
    json::Document state;
    REQUIRE(state.parse(std::string(sent["state"].str()), nullptr));
    CHECK_STR(
        state.root()["values"]["vWR"]["8140d629"]["value"].str(), "https://github.com/x/y/pull/1"
    );

    // The next step refused a field.
    const model::Form two = r.next;
    called                = false;
    e.be->submitForm(two, std::vector<model::FormAnswer>(two.fields.size()), take);
    REQUIRE(pumpUntil([&] { return called; }));
    REQUIRE(r.fieldErrors.size() == two.fields.size());
    CHECK_STR(r.fieldErrors[0], "Pick one");
    CHECK(r.next.id.empty());

    // A failure is reported, never sent again.
    called = false;
    e.be->submitForm(two, std::vector<model::FormAnswer>(two.fields.size()), take);
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_STR(r.error, "internal_error");
    pumpFor(500);
    CHECK(count("views.submit") == 3);

    // "clear": done.
    called = false;
    e.be->submitForm(two, std::vector<model::FormAnswer>(two.fields.size()), take);
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(r.error.empty());
    CHECK(r.next.id.empty());
    CHECK(r.fieldErrors.empty());

    e.be->closeForm(two);
    REQUIRE(pumpUntil([] { return count("views.close") == 1; }));
    CHECK_STR(Log().get("views.close")["form"]["view_id"].str(), "V0CM57Z449K");
}

TEST("slack workflows: a run without a form, a failed run, a refused trip, an OAuth token") {
    if (!haveServer())
        return;
    {
        Env e;
        REQUIRE(e.connect());
        // It ran to the end without asking anything.
        Started s;
        REQUIRE(!startAndTrip(e, s, 1).empty());
        push(R"({"type":"workflow_execution_outcome_changed","trigger_execution_id":"Tr1",
                 "workflow_instance_status":"PENDING"})");
        pumpFor(300);
        CHECK_FALSE(s.called); // still running
        push(R"({"type":"workflow_execution_outcome_changed","trigger_execution_id":"Tr1",
                 "workflow_instance_status":"SUCCESS"})");
        REQUIRE(pumpUntil([&] { return s.called; }));
        CHECK(s.form.id.empty());
        CHECK(s.error.empty());
        REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Off; }));

        // One that failed on its way.
        Started f;
        REQUIRE(!startAndTrip(e, f, 2).empty());
        push(R"({"type":"workflow_execution_outcome_changed","trigger_execution_id":"Tr1",
                 "workflow_instance_status":"FAILED"})");
        REQUIRE(pumpUntil([&] { return f.called; }));
        CHECK(f.form.id.empty());
        CHECK_STR(f.error, "failed");

        // Slack refuses the trip.
        script("workflows.triggers.trip", R"([{"ok":false,"error":"invalid_url"}])");
        Started r;
        REQUIRE(!startAndTrip(e, r, 3).empty());
        REQUIRE(pumpUntil([&] { return r.called; }));
        CHECK_STR(r.error, "invalid_url");
        REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Off; }));
        pumpFor(300);
        CHECK(count("workflows.triggers.trip") == 3); // never repeated

        // An unknown trigger never trips.
        script("workflows.triggers.info", R"([{"ok":false,"error":"trigger_not_found"}])");
        Started u;
        e.be->startWorkflow(e.conv("C1"), "FtGone", [&u](model::Form, std::string err) {
            u.called = true;
            u.error  = std::move(err);
        });
        REQUIRE(pumpUntil([&] { return u.called; }));
        CHECK_STR(u.error, "trigger_not_found");
        CHECK(count("workflows.triggers.trip") == 3);
    }
    {
        Env e(false); // an OAuth token: the web client's methods are not for it
        REQUIRE(e.connect());
        CHECK_FALSE(e.be->capabilities().workflows);
        Started s;
        e.be->startWorkflow(e.conv("C1"), "Ft1", [&s](model::Form, std::string err) {
            s.called = true;
            s.error  = std::move(err);
        });
        REQUIRE(pumpUntil([&] { return s.called; }));
        CHECK_STR(s.error, "not_supported");
        CHECK(count("workflows.triggers.trip") == 0);
    }
}

TEST("slack workflows: the channel's featured workflows follow conversations.info as it opens") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    script(
        "conversations.info",
        R"([{"ok":true,"channel":{"id":"C1","name":"general","is_channel":true,
              "properties":{"channel_workflows":[{"workflow_trigger_id":"Ft1","title":"PR for review"}]}}},
            {"ok":true,"channel":{"id":"C1","name":"general","is_channel":true}}])"
    );
    bool called = false;
    e.be->loadChannelCanvas(c, [&](std::string) { called = true; });
    REQUIRE(pumpUntil([&] { return called; }));
    REQUIRE(e.store.conversation(c).featuredWorkflows.size() == 1);
    CHECK_STR(e.store.conversation(c).featuredWorkflows[0].title, "PR for review");
    // Unfeatured since: gone.
    called = false;
    e.be->loadChannelCanvas(c, [&](std::string) { called = true; });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(e.store.conversation(c).featuredWorkflows.empty());
}

TEST("slack workflows: a workflow's title and description, looked up once") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    model::WorkflowLink got;
    bool                called = false;
    e.be->lookUpWorkflow("Ft1", [&](model::WorkflowLink l) {
        called = true;
        got    = std::move(l);
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_STR(got.trigger, "Ft1");
    CHECK_STR(got.title, "PR for review");
    CHECK_STR(got.description, "Ask for a code review");
    called = false;
    e.be->lookUpWorkflow("Ft1", [&](model::WorkflowLink) { called = true; });
    CHECK(called); // from what it knows: at once
    CHECK(count("workflows.triggers.info") == 1);
}

#endif // !_WIN32
