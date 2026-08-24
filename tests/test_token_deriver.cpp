// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Session-token derivation from a `d` cookie. The regression that matters here:
// Slack serves the *logged-in* workspace boot page with HTTP 403 (Qt reports
// ContentAccessDenied) while still embedding "api_token":"xoxc-…". Gating the
// scrape on reply->error() threw that token away and reported a perfectly good
// cookie as unverifiable — every manual cookie sign-in failed that way.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSettings>
#include <QTemporaryDir>

#include "backend/slack/session_import/token_deriver.h"
#include "fake_http_server.h"

using namespace slack::session;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());
    return Catch::Session().run(argc, argv);
}

static bool waitFor(std::function<bool()> pred, int timeoutMs = 5000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// A boot page trimmed to the shape that matters: the token lives in a big JSON
// blob inline in the HTML.
static QByteArray bootPage(const QByteArray &apiToken) {
    return "<!DOCTYPE html><html><head><title>Acme Slack</title></head><body>"
           "<script>var boot_data = {\"team_id\":\"T1\",\"api_token\":" +
           apiToken + ",\"no_login\":false};</script></body></html>";
}

namespace {

struct Result {
    bool                      done = false;
    QList<slack::Credentials> valid;
    QString                   error;
};

// Wire a deriver to `out` and point both the boot fetch and the API at `server`.
TokenDeriver *deriverFor(FakeHttpServer &server, Result &out, QObject *parent) {
    auto *d = new TokenDeriver(parent);
    d->setApiBase(server.baseUrl());
    QObject::connect(
        d,
        &TokenDeriver::finished,
        d,
        [&out](const QList<slack::Credentials> &valid, const QString &error) {
            out.valid = valid;
            out.error = error;
            out.done  = true;
        }
    );
    return d;
}

} // namespace

TEST_CASE("a boot page served with HTTP 403 still yields its session token", "[token_deriver]") {
    FakeHttpServer server;
    // 1) workspace root: Slack's real-world answer — 403 *with* the logged-in page.
    server.enqueueStatus(
        403, "Forbidden", "text/html; charset=utf-8", bootPage("\"xoxc-1-2-tok\"")
    );
    // 2) auth.test  3) team.info (icon is best-effort)
    server.enqueue(R"({"ok":true,"team_id":"T1","team":"Acme","url":"https://acme.slack.com/"})");
    server.enqueue(R"({"ok":true,"team":{"icon":{"image_88":"https://x/i.png"}}})");

    QObject     owner;
    Result      out;
    auto       *d = deriverFor(server, out, &owner);
    TeamSession cand;
    cand.workspaceUrl = server.baseUrl();
    d->run("xoxd-thecookie", {cand});

    REQUIRE(waitFor([&] { return out.done; }));
    CHECK(out.error.isEmpty());
    REQUIRE(out.valid.size() == 1);
    CHECK(out.valid[0].xoxp == "xoxc-1-2-tok");
    CHECK(out.valid[0].cookie == "xoxd-thecookie");
    CHECK(out.valid[0].teamId == "T1");
    CHECK(out.valid[0].teamName == "Acme");
    // auth.test's url is authoritative over the address the user typed.
    CHECK(out.valid[0].workspaceUrl == "https://acme.slack.com/");
    CHECK(out.valid[0].iconUrl == "https://x/i.png");
    // Boot page first, then the API calls — the token must reach auth.test.
    REQUIRE(server.requestPaths.size() == 3);
    CHECK(server.requestPaths[1] == "/auth.test");
    CHECK(server.requestPaths[2] == "/team.info");
}

TEST_CASE("a 200 boot page is scraped the same way", "[token_deriver]") {
    FakeHttpServer server;
    server.enqueueStatus(200, "OK", "text/html; charset=utf-8", bootPage("\"xoxc-ok\""));
    server.enqueue(R"({"ok":true,"team_id":"T2","team":"Beta"})");

    QObject     owner;
    Result      out;
    auto       *d = deriverFor(server, out, &owner);
    TeamSession cand;
    cand.workspaceUrl = server.baseUrl();
    cand.iconUrl      = "https://x/have.png"; // set ⇒ team.info is skipped
    d->run("xoxd-c", {cand});

    REQUIRE(waitFor([&] { return out.done; }));
    REQUIRE(out.valid.size() == 1);
    CHECK(out.valid[0].xoxp == "xoxc-ok");
    CHECK(server.requestPaths.size() == 2); // no team.info
}

TEST_CASE("a logged-out boot page is reported as a stale cookie", "[token_deriver]") {
    FakeHttpServer server;
    // What Slack actually serves without a valid `d`: same 403, api_token null,
    // no xoxc- run anywhere — so the scrape must come back empty, not guess.
    server.enqueueStatus(403, "Forbidden", "text/html; charset=utf-8", bootPage("null"));

    QObject     owner;
    Result      out;
    auto       *d = deriverFor(server, out, &owner);
    TeamSession cand;
    cand.workspaceUrl = server.baseUrl();
    d->run("xoxd-expired", {cand});

    REQUIRE(waitFor([&] { return out.done; }));
    CHECK(out.valid.isEmpty());
    // Drives the dialog's "cookie has probably expired" message.
    CHECK(out.error.toStdString() == "token_not_found");
    CHECK(server.requestPaths.size() == 1); // never bothered auth.test
}

TEST_CASE("an unreachable workspace address is reported as a network failure", "[token_deriver]") {
    FakeHttpServer server; // serves the API base; the boot host below is dead
    QObject        owner;
    Result         out;
    auto          *d = deriverFor(server, out, &owner);
    TeamSession    cand;
    cand.workspaceUrl = "http://127.0.0.1:1/"; // nothing can bind port 1 unprivileged
    d->run("xoxd-c", {cand});

    REQUIRE(waitFor([&] { return out.done; }));
    CHECK(out.valid.isEmpty());
    CHECK(out.error.toStdString() == "network");
    CHECK(server.requestCount == 0); // never got as far as auth.test
}

TEST_CASE("a connection dropped mid-flight is retried, not lost", "[token_deriver]") {
    FakeHttpServer server;
    server.dropConnections = 1; // first attempt closed without a response

    QObject     owner;
    Result      out;
    auto       *d = deriverFor(server, out, &owner);
    TeamSession cand;
    cand.workspaceUrl = server.baseUrl();
    d->run("xoxd-c", {cand});

    REQUIRE(waitFor([&] { return out.done; }));
    // Qt silently retransmits a GET whose connection died mid-flight (the same
    // behaviour that forces every write method to POST). So the boot fetch happens
    // twice and it's the *second* answer that decides the outcome — here the fake
    // server's queue is empty by then, so no token comes out of it.
    CHECK(server.requestCount == 2);
    CHECK(out.valid.isEmpty());
    CHECK(out.error.toStdString() == "token_not_found");
}

TEST_CASE("a rejected token surfaces Slack's own error", "[token_deriver]") {
    FakeHttpServer server;
    server.enqueueStatus(403, "Forbidden", "text/html; charset=utf-8", bootPage("\"xoxc-stale\""));
    server.enqueue(R"({"ok":false,"error":"invalid_auth"})");

    QObject     owner;
    Result      out;
    auto       *d = deriverFor(server, out, &owner);
    TeamSession cand;
    cand.workspaceUrl = server.baseUrl();
    d->run("xoxd-c", {cand});

    REQUIRE(waitFor([&] { return out.done; }));
    CHECK(out.valid.isEmpty());
    CHECK(out.error.toStdString() == "invalid_auth"); // dialog: "That session was rejected…"
}

TEST_CASE("one dead candidate doesn't sink the rest", "[token_deriver]") {
    FakeHttpServer server;
    server.enqueueStatus(403, "Forbidden", "text/html", bootPage("null")); // candidate 1: no token
    server.enqueueStatus(403, "Forbidden", "text/html", bootPage("\"xoxc-good\"")); // candidate 2
    server.enqueue(R"({"ok":true,"team_id":"T3","team":"Gamma"})");
    server.enqueue(R"({"ok":true,"team":{"icon":{"image_88":"https://x/g.png"}}})");

    QObject     owner;
    Result      out;
    auto       *d = deriverFor(server, out, &owner);
    TeamSession a, b;
    a.workspaceUrl = server.baseUrl();
    b.workspaceUrl = server.baseUrl();
    d->run("xoxd-c", {a, b});

    REQUIRE(waitFor([&] { return out.done; }));
    REQUIRE(out.valid.size() == 1);
    CHECK(out.valid[0].xoxp == "xoxc-good");
    CHECK(out.error.isEmpty()); // partial success reports no error
}
