#include "base/json.h"
#include "base/file.h"
#include "support/test.h"

#include <cmath>

TEST("json: parse a Slack-shaped payload") {
    json::Document d;
    std::string    err;
    REQUIRE(d.parse(
        std::string(R"({
        "ok": true, "ts": "1758445200.123456", "count": 42, "ratio": 0.5,
        "big": 12345678901234567890, "neg": -7, "exp": 1e3, "none": null,
        "user": {"id": "U1", "name": "mira"},
        "list": [1, "two", [3], {}, []]
    })"),
        &err
    ));
    const json::Value r = d.root();
    CHECK(r.isObject());
    CHECK(r.size() == 10);
    CHECK(r["ok"].boolean());
    CHECK(r["ts"].str() == "1758445200.123456");
    CHECK(r["count"].integer() == 42);
    CHECK(r["count"].isInt());
    CHECK(r["ratio"].number() == 0.5);
    CHECK(r["big"].type() == json::Type::Double); // beyond int64
    CHECK(r["neg"].integer() == -7);
    CHECK(r["exp"].integer() == 1000); // integral double accepted as integer
    CHECK(r["none"].isNull());
    CHECK(r["user"]["name"].str() == "mira");
    CHECK(r["list"].size() == 5);
    CHECK(r["list"][1].str() == "two");
    CHECK(r["list"][2][0].integer() == 3);
    CHECK(r["list"][3].isObject());
    CHECK(r["list"][4].isArray());
    CHECK(r["list"][4].size() == 0);
    // Missing keys and wrong types fall back to defaults instead of failing.
    CHECK_FALSE(r["nope"].exists());
    CHECK(r["nope"]["deeper"][3].str("dflt") == "dflt");
    CHECK(r["ts"].integer(-1) == -1);
    CHECK_FALSE(r["list"][99].exists());
    int n = 0;
    for (json::Value m : r["user"]) {
        CHECK((n == 0 ? m.key() == "id" : m.key() == "name"));
        ++n;
    }
    CHECK(n == 2);
}

TEST("json: string escapes decode in place") {
    json::Document d;
    REQUIRE(d.parse(
        std::string(
            R"(["a\"b\\c\/d\n\t", "\u00e5\u20ac", "\ud83d\ude80", "\ud800x", "k\u0000z", "\ud800\u0041"])"
        ),
        nullptr
    ));
    const auto r = d.root();
    CHECK(r[0].str() == "a\"b\\c/d\n\t");
    CHECK(r[1].str() == "å€");
    CHECK(r[2].str() == "🚀");
    CHECK(r[3].str() == "\xEF\xBF\xBDx"); // lone surrogate
    CHECK(r[4].str() == std::string_view("k\0z", 3));
    CHECK(
        r[5].str() == "\xEF\xBF\xBD"
                      "A"
    ); // high surrogate + non-low escape
}

TEST("json: plain runs after escapes move into place") {
    json::Document d;
    REQUIRE(d.parse(std::string(R"(["ab\ncdef\u00e9ghij\"k", "plain", "x\\"])"), nullptr));
    CHECK(d.root()[0].str() == "ab\ncdef\xC3\xA9ghij\"k");
    CHECK(d.root()[1].str() == "plain");
    CHECK(d.root()[2].str() == "x\\");
}

TEST("json: one document re-parsed, and numbers next to delimiters") {
    json::Document d;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(d.parse(std::string(R"({"a":[1.5,2e3,-0.25],"b":1E-2})"), nullptr));
        CHECK(d.root()["a"][0].number() == 1.5);
        CHECK(d.root()["a"][1].number() == 2000);
        CHECK(d.root()["a"][2].number() == -0.25);
        CHECK(d.root()["b"].number() == 0.01);
        REQUIRE(d.parse(std::string("3.75"), nullptr));
        CHECK(d.root().number() == 3.75);
        CHECK_FALSE(d.parse(std::string("[1,"), nullptr));
        CHECK_FALSE(d.root().exists());
    }
}

TEST("json: keys with escapes") {
    json::Document d;
    REQUIRE(d.parse(std::string(R"({"a\u0062c": 1, "x": {"y": [true]}})"), nullptr));
    CHECK(d.root()["abc"].integer() == 1);
    CHECK(d.root()["x"]["y"][0].boolean());
}

TEST("json: malformed input is rejected with a position") {
    const char *bad[] = {
        "",         "{",           "[1,]",     "{\"a\":1,}", "{\"a\" 1}",     "[01]",  "[1.]",
        "[.5]",     "[1e]",        "[-]",      "tru",        "nul",           "\"abc", "\"a\x01\"",
        "\"\\x\"",  "\"\\u12G4\"", "[1] 2",    "{1:2}",      "[\"a\" \"b\"]", "NaN",   "[Infinity]",
        "'single'", "[1,,2]",      "{\"a\":}",
    };
    for (const char *b : bad) {
        json::Document d;
        std::string    err;
        if (!CHECK_FALSE(d.parse(std::string(b), &err)))
            std::fprintf(stderr, "    accepted: %s\n", b);
        CHECK(!err.empty());
        CHECK_FALSE(d.root().exists());
    }
    json::Document d;
    std::string    err;
    CHECK_FALSE(d.parse(std::string("{\n  \"a\": [1,\n  ]\n}"), &err));
    CHECK_STR(err, "line 3, column 3: unexpected character");
}

TEST("json: nesting depth is bounded") {
    std::string deep(json::Document::kMaxDepth + 10, '[');
    deep.append(json::Document::kMaxDepth + 10, ']');
    json::Document d;
    std::string    err;
    CHECK_FALSE(d.parse(deep, &err));
    CHECK(err.find("too deep") != std::string::npos);
    std::string ok(100, '[');
    ok.append(100, ']');
    CHECK(d.parse(ok, &err));
}

TEST("json: compact and pretty round trips") {
    const std::string src =
        R"({"a":1,"b":[true,false,null],"c":{"d":"x\"y\n","e":-2.5,"f":1.0,"g":{}},"h":[]})";
    json::Document d;
    REQUIRE(d.parse(src, nullptr));
    CHECK_STR(json::write(d.root()), src);

    const std::string pretty = json::write(d.root(), true);
    CHECK_STR(
        pretty,
        "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    false,\n    null\n  ],\n"
        "  \"c\": {\n    \"d\": \"x\\\"y\\n\",\n    \"e\": -2.5,\n    \"f\": 1.0,\n"
        "    \"g\": {}\n  },\n  \"h\": []\n}"
    );
    json::Document again;
    REQUIRE(again.parse(pretty, nullptr));
    CHECK_STR(json::write(again.root()), src);
}

TEST("json: writer escapes and numbers") {
    json::Writer w;
    w.beginObject()
        .key("s")
        .value("tab\there \x01 ü")
        .key("i")
        .value(int64_t(-9007199254740993))
        .key("d")
        .value(0.1)
        .key("nan")
        .value(std::nan(""))
        .key("arr")
        .beginArray()
        .value(3)
        .value(false)
        .null()
        .endArray()
        .endObject();
    CHECK_STR(
        w.str(),
        R"({"s":"tab\there \u0001 ü","i":-9007199254740993,"d":0.1,"nan":null,"arr":[3,false,null]})"
    );
    json::Document d;
    REQUIRE(d.parse(w.str(), nullptr));
    CHECK(d.root()["i"].integer() == -9007199254740993);
    CHECK(d.root()["d"].number() == 0.1);
}

TEST("json: parse a file; a missing file and invalid JSON fail") {
    const std::string tmp  = base::test::makeTempDir("msga_json_test_");
    const std::string dir  = tmp.empty() ? "/tmp" : tmp;
    const std::string good = file::join(dir, "good.json");
    json::Document    d;
    std::string       err = "stale";
    REQUIRE(file::writeAtomic(good, R"({"a": [1, 2]})"));
    REQUIRE(d.parseFile(good, &err));
    CHECK(d.root()["a"][1].integer() == 2);
    CHECK(err.empty());
    // Missing: false and no error text (callers tell it from bad JSON so).
    err = "stale";
    CHECK_FALSE(d.parseFile(file::join(dir, "missing.json"), &err));
    CHECK(err.empty());
    CHECK_FALSE(d.root().exists());
    const std::string bad = file::join(dir, "bad.json");
    REQUIRE(file::writeAtomic(bad, "{\"a\": "));
    REQUIRE(d.parseFile(good));
    CHECK_FALSE(d.parseFile(bad, &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(d.root().exists());
    // An empty file is no JSON either.
    const std::string empty = file::join(dir, "empty.json");
    REQUIRE(file::writeAtomic(empty, ""));
    CHECK_FALSE(d.parseFile(empty));
    file::remove(good);
    file::remove(bad);
    file::remove(empty);
}
