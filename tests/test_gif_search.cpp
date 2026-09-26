// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests for the GIPHY wire format behind the composer's GIF picker. Two details
// of that format are easy to get wrong and both fail silently — an empty picker
// rather than an error: GIPHY sends rendition dimensions as JSON *strings*, and
// the `original` rendition carries no `url` key at all. Rendition coverage also
// varies per GIF, so the fallback chains have to survive gaps.

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include "network/gif_search.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>

using net::GifResult;
using net::GifSearch;

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga");
    app.setOrganizationName("msga");
    // SecretStore's QSettings backend hardcodes QSettings("msga", "msga") — the
    // real store the running app uses — so a test that touches the key can read
    // and delete the developer's actual one.
    //
    // The setPath() below only redirects that on Linux. It is documented to do
    // nothing for NativeFormat — the registry on Windows, CFPreferences on
    // macOS — and the two-argument QSettings constructor ignores
    // setDefaultFormat as well (measured: defaultFormat() reports Ini while the
    // object still reports Native). On those two platforms the KeyGuard further
    // down is what keeps the real key intact.
    static QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    return msga_test::runCatch(argc, argv);
}

namespace {

// A response shaped like GIPHY's, with every rendition the picker looks for.
QByteArray fullBody() {
    return R"({
      "data": [
        {
          "id": "abc123",
          "title": "Happy Dancing GIF",
          "alt_text": "a cat dancing",
          "images": {
            "fixed_width_downsampled": {
              "url": "https://media.giphy.com/abc/200w_d.gif",
              "width": "200", "height": "150", "size": "40000"
            },
            "fixed_width": {
              "url": "https://media.giphy.com/abc/200w.gif",
              "width": "200", "height": "150", "size": "900000"
            },
            "downsized": {
              "url": "https://media.giphy.com/abc/downsized.gif",
              "width": "480", "height": "360", "size": "1500000"
            },
            "downsized_medium": {
              "url": "https://media.giphy.com/abc/downsized-medium.gif",
              "width": "480", "height": "360", "size": "4000000"
            },
            "original": {
              "width": "480", "height": "360", "frames": "20",
              "mp4": "https://media.giphy.com/abc/giphy.mp4"
            }
          }
        }
      ],
      "pagination": {"total_count": 1, "count": 1, "offset": 0},
      "meta": {"status": 200, "msg": "OK"}
    })";
}

} // namespace

TEST_CASE("parseResponse reads renditions and string dimensions", "[gif]") {
    const QList<GifResult> out = GifSearch::parseResponse(fullBody());
    REQUIRE(out.size() == 1);

    // Preview prefers the downsampled rendition — same 200px width as the full
    // one at a fraction of the bytes, and the grid never shows it larger.
    CHECK(out[0].previewUrl == "https://media.giphy.com/abc/200w_d.gif");
    // "200"/"150" are strings in the payload; read as ints they come out 0 and
    // the masonry would fall back to square cells for every result.
    CHECK(out[0].previewSize == QSize(200, 150));
    // Send size is the 200px-wide rendition Slack's own GIF picker posts.
    CHECK(out[0].postUrl == "https://media.giphy.com/abc/200w.gif");
    // alt_text wins over title when both are present.
    CHECK(out[0].description == "a cat dancing");
}

TEST_CASE("parseResponse sends a downsized rendition when fixed_width is missing", "[gif]") {
    // "downsized" (<2 MB) before "downsized_medium" (<5 MB).
    const QByteArray body = R"({"data":[{
      "id": "d",
      "images": {
        "fixed_width_small": {"url": "https://media.giphy.com/d/100w.gif",
                              "width": "100", "height": "75"},
        "downsized": {"url": "https://media.giphy.com/d/downsized.gif"},
        "downsized_medium": {"url": "https://media.giphy.com/d/downsized-medium.gif"}
      }}]})";

    const QList<GifResult> out = GifSearch::parseResponse(body);
    REQUIRE(out.size() == 1);
    CHECK(out[0].postUrl == "https://media.giphy.com/d/downsized.gif");
}

TEST_CASE("parseResponse never posts the url-less original rendition", "[gif]") {
    // `original` has no "url" key — only mp4/webp. Reaching for it would post an
    // empty string, i.e. send a blank message.
    const QByteArray body = R"({"data":[{
      "id": "x",
      "images": {
        "fixed_width": {"url": "https://media.giphy.com/x/200w.gif",
                        "width": "200", "height": "200"},
        "original": {"width": "480", "height": "480",
                     "mp4": "https://media.giphy.com/x/giphy.mp4"}
      }}]})";

    const QList<GifResult> out = GifSearch::parseResponse(body);
    REQUIRE(out.size() == 1);
    CHECK(out[0].postUrl == "https://media.giphy.com/x/200w.gif");
    CHECK_FALSE(out[0].postUrl.isEmpty());
}

TEST_CASE("parseResponse falls back down the preview chain", "[gif]") {
    // Only preview_gif present — the last resort, but still displayable.
    const QByteArray body = R"({"data":[{
      "id": "y",
      "title": "fallback",
      "images": {"preview_gif": {"url": "https://media.giphy.com/y/preview.gif",
                                 "width": "80", "height": "60"}}}]})";

    const QList<GifResult> out = GifSearch::parseResponse(body);
    REQUIRE(out.size() == 1);
    CHECK(out[0].previewUrl == "https://media.giphy.com/y/preview.gif");
    CHECK(out[0].previewSize == QSize(80, 60));
    // No send-size rendition at all, so the preview doubles as the post URL
    // rather than the entry being dropped.
    CHECK(out[0].postUrl == "https://media.giphy.com/y/preview.gif");
    CHECK(out[0].description == "fallback"); // title, since alt_text is absent
}

TEST_CASE("parseResponse skips entries with no usable rendition", "[gif]") {
    // A GIF offering only mp4 renditions cannot be painted by the grid; emitting
    // it half-filled would leave a permanently grey cell.
    const QByteArray body = R"({"data":[
      {"id": "ok", "images": {"fixed_width": {"url": "https://m.giphy.com/ok.gif",
                                              "width": "200", "height": "100"}}},
      {"id": "novideo", "images": {"looping": {"mp4": "https://m.giphy.com/x.mp4"}}},
      {"id": "empty", "images": {}}
    ]})";

    const QList<GifResult> out = GifSearch::parseResponse(body);
    REQUIRE(out.size() == 1);
    CHECK(out[0].previewUrl == "https://m.giphy.com/ok.gif");
}

TEST_CASE("parseResponse tolerates malformed and empty payloads", "[gif]") {
    CHECK(GifSearch::parseResponse("").isEmpty());
    CHECK(GifSearch::parseResponse("not json at all").isEmpty());
    CHECK(GifSearch::parseResponse(R"({"meta":{"status":401}})").isEmpty());
    CHECK(GifSearch::parseResponse(R"({"data":[]})").isEmpty());
}

TEST_CASE("requestUrl targets search when a query is given", "[gif]") {
    const QUrl      url(GifSearch::requestUrl("cat party", 30, "KEY123"));
    const QUrlQuery q(url);

    CHECK(url.host() == "api.giphy.com");
    CHECK(url.path() == "/v1/gifs/search");
    CHECK(q.queryItemValue("q", QUrl::FullyDecoded) == "cat party");
    CHECK(q.queryItemValue("api_key") == "KEY123");
    CHECK(q.queryItemValue("limit") == "30");
    CHECK(q.queryItemValue("rating") == "pg-13"); // explicit tier stays out
}

TEST_CASE("requestUrl targets trending when the query is blank", "[gif]") {
    // The picker opens on trending, and GIPHY 400s on /search with an empty q.
    for (const QString &blank : {QString(), QStringLiteral("   ")}) {
        const QUrl      url(GifSearch::requestUrl(blank, 30, "KEY123"));
        const QUrlQuery q(url);
        CHECK(url.path() == "/v1/gifs/trending");
        CHECK_FALSE(q.hasQueryItem("q"));
    }
}

TEST_CASE("requestUrl clamps the limit and the query length", "[gif]") {
    // GIPHY rejects limit > 50 and q longer than 50 characters.
    CHECK(QUrlQuery(QUrl(GifSearch::requestUrl("x", 500, "K"))).queryItemValue("limit") == "50");
    CHECK(QUrlQuery(QUrl(GifSearch::requestUrl("x", 0, "K"))).queryItemValue("limit") == "1");

    const QString   longQuery(120, QLatin1Char('a'));
    const QUrlQuery q(QUrl(GifSearch::requestUrl(longQuery, 30, "K")));
    CHECK(q.queryItemValue("q", QUrl::FullyDecoded).size() == 50);
}

TEST_CASE("requestUrl percent-encodes a query that would break the URL", "[gif]") {
    const QString raw = QStringLiteral("rock & roll?=#yes");
    const QUrl    url(GifSearch::requestUrl(raw, 5, "K"));
    // The separators must survive a round trip rather than splitting the query.
    CHECK(QUrlQuery(url).queryItemValue("q", QUrl::FullyDecoded) == raw);
    CHECK(QUrlQuery(url).queryItemValue("api_key") == "K");
}

// ── Key resolution ───────────────────────────────────────────────────────────

namespace {

// Puts the developer's real key back. On Windows and macOS these tests operate
// on the live store (see main), so this is load-bearing rather than tidiness.
// Restores on scope exit, including when Catch2 unwinds a failed REQUIRE.
struct KeyGuard {
    QString saved = GifSearch::userApiKey();
    ~KeyGuard() { GifSearch::setUserApiKey(saved); }
};

} // namespace

TEST_CASE("the user key round-trips and clearing restores the build default", "[gif]") {
    const KeyGuard guard;

    // No key is baked into a test build, so the user key is the only source and
    // configured() tracks it exactly. That is what decides whether the picker
    // opens on the setup form or on trending.
    GifSearch::setUserApiKey("");
    CHECK(GifSearch::userApiKey().isEmpty());
    CHECK_FALSE(GifSearch::configured());

    GifSearch::setUserApiKey("  abc123  "); // surrounding space is a paste artifact
    CHECK(GifSearch::userApiKey() == "abc123");
    CHECK(GifSearch::apiKey() == "abc123");
    CHECK(GifSearch::configured());

    GifSearch::setUserApiKey("");
    CHECK_FALSE(GifSearch::configured());
}

TEST_CASE("KeyGuard puts the previous key back", "[gif]") {
    // The guard is the only thing standing between this suite and the
    // developer's real GIPHY key, so it gets its own test.
    GifSearch::setUserApiKey("outer-key");
    {
        const KeyGuard guard;
        GifSearch::setUserApiKey("inner-key");
        CHECK(GifSearch::userApiKey() == "inner-key");
    }
    CHECK(GifSearch::userApiKey() == "outer-key");
    GifSearch::setUserApiKey("");
}

TEST_CASE("searching with no key fails as a key problem, not a network one", "[gif]") {
    const KeyGuard guard;

    // The picker routes on this flag: keyRejected sends the user back to the
    // setup form, anything else shows a plain error they cannot act on.
    GifSearch::setUserApiKey("");
    REQUIRE_FALSE(GifSearch::configured());

    GifSearch  api;
    QSignalSpy spy(&api, &GifSearch::failed);
    api.search("cats");

    REQUIRE(spy.count() == 1);
    CHECK(spy.at(0).at(0).toString() == "cats"); // tagged with its query
    CHECK_FALSE(spy.at(0).at(1).toString().isEmpty());
    CHECK(spy.at(0).at(2).toBool()); // keyRejected
}

// ── Safety properties ────────────────────────────────────────────────────────

TEST_CASE("errorMessage never carries the api key or the request URL", "[gif]") {
    const KeyGuard guard;

    // The whole reason this helper exists rather than passing Qt's
    // errorString() through: that string embeds the request URL, and the URL
    // carries api_key=. The picker renders these verbatim, so a leak here puts
    // the user's secret on screen — and into any screenshot of the bug.
    GifSearch::setUserApiKey("SUPER-SECRET-KEY");

    for (const int status : {0, 400, 404, 429, 500, 502}) {
        const QString msg = GifSearch::errorMessage(status);
        CHECK_FALSE(msg.isEmpty()); // must still say something actionable
        CHECK_FALSE(msg.contains("SUPER-SECRET-KEY"));
        CHECK_FALSE(msg.contains("api_key", Qt::CaseInsensitive));
        CHECK_FALSE(msg.contains("://")); // no URL of any kind
    }
}
