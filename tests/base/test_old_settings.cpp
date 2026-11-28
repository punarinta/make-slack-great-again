// old_settings.h: the INI and registry encodings against a real settings
// file of an earlier version (old_settings_fixture.h), and the Linux store in the test's temporary
// XDG_CONFIG_HOME.
#include "base/file.h"
#include "base/old_settings.h"
#include "base/process.h"
#include "support/test.h"
#include "support/old_settings_fixture.h"

#include <string>

using oldsettings::Value;

namespace {

std::string at(const oldsettings::Map &m, const char *key) {
    const auto it = m.find(key);
    return it == m.end() ? std::string("<none>") : it->second.text();
}

// UTF-16LE bytes, as RegQueryValueExW returns them.
std::string utf16(std::u16string_view s, bool nul) {
    std::string out;
    for (char16_t c : s) {
        out.push_back(char(c & 0xFF));
        out.push_back(char(c >> 8));
    }
    if (nul)
        out.append(2, '\0');
    return out;
}

} // namespace

TEST("old settings: the INI file as earlier versions read it") {
    const oldsettings::Map m = oldsettings::parseIni(oldsettings_fixture::kIni);
    // [General] keys have no section prefix.
    CHECK_STR(at(m, "active"), "slack:T0123");
    CHECK(m.at("storeVersion").toInt(0) == 2);
    // A list; a one-element list reads back as a string (toList wraps it).
    const auto ws = m.at("workspaces").toList();
    REQUIRE(ws.size() == 3);
    CHECK_STR(ws[1], "claude-code:local");
    CHECK((m.at("llm/customProviders").toList() == std::vector<std::string>{"custom-ab12cd34"}));
    // %3A in a key, quotes and \" escapes in a value.
    CHECK_STR(
        at(m, "workspace/slack:T0123/auth"),
        R"({"xoxp":"xoxc-1-2","refreshToken":"","expiresAt":"0","cookie":"xoxd-a%2Fb=c;d"})"
    );
    CHECK_STR(at(m, "workspace/slack:T0123/displayName"), "Acme, Inc.");
    CHECK_STR(
        at(m, "workspace/slack:T0456/displayName"),
        "\xC3\x9Cn\xC3\xAF"
        "c\xC3\xB6"
        "d\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC"
    );
    CHECK(m.at("workspace/slack:T0123/muted").toBool(false));
    CHECK_FALSE(m.at("updates/autoCheck").toBool(true));
    CHECK(m.at("updates/lastChecked").toInt(0) == 1767225600);
    CHECK_STR(at(m, "credentials/slackClientSecret"), "sec\"ret\\x");
    CHECK_STR(at(m, "gif/giphy/apiKey"), "@giphy"); // "@@" undone
    CHECK_STR(at(m, "spaces/lead"), " padded ");
    CHECK_STR(
        at(m, "multi/line"),
        "a\nb\tc\x01"
        "d"
    ); // \x1 then an escaped digit
    // A key that is also a group: claudeCode/lastDir and claudeCode/lastDir/<role>.
    CHECK_STR(at(m, "claudeCode/lastDir"), "/home/u/src");
    CHECK_STR(at(m, "claudeCode/lastDir/architect"), "/home/u/src/a b");
    CHECK_STR(at(m, "claudeCode/recentDirs/2/path"), "/home/u/x,y");
    const auto gl = m.at("voice/glossary").toList();
    REQUIRE(gl.size() == 2);
    CHECK_STR(gl[1], "Kubernetes, k8s");
    // @ByteArray with \x and \0 escapes: saved window geometry bytes.
    const Value &g = m.at("window/geometry");
    CHECK(g.kind == Value::Kind::Bytes);
    REQUIRE(g.s.size() == 66);
    CHECK(uint8_t(g.s[0]) == 0x01 && uint8_t(g.s[1]) == 0xd9 && uint8_t(g.s[3]) == 0xcb);
    CHECK(uint8_t(g.s[48]) == 0x0a); // "\n" inside a byte array
    CHECK(uint8_t(g.s[65]) == 0x97);
    CHECK(m.find("nope") == m.end());
}

TEST("old settings: one key rewritten, everything else kept, still readable") {
    const std::string text = oldsettings_fixture::kIni;
    // Replace in place.
    const std::string v1   = R"({"xoxp":"xoxc-NEW","cookie":"a;b, c"})";
    std::string       t    = oldsettings::setIniValue(text, "workspace/slack:T0123/auth", &v1);
    CHECK(
        t.find(R"(slack%3AT0123\auth="{\"xoxp\":\"xoxc-NEW\",\"cookie\":\"a;b, c\"}")") !=
        std::string::npos
    );
    oldsettings::Map m = oldsettings::parseIni(t);
    CHECK_STR(at(m, "workspace/slack:T0123/auth"), v1);
    CHECK(m.size() == oldsettings::parseIni(text).size());
    CHECK_STR(
        at(m, "workspace/slack:T0456/displayName"),
        at(oldsettings::parseIni(text), "workspace/slack:T0456/displayName")
    );
    // A new key in an existing section, one in a new section, one in [General].
    const std::string v2 = "@at", v3 = "line\nbreak", v4 = "g";
    t = oldsettings::setIniValue(t, "llm/openai/apiKey", &v2);
    t = oldsettings::setIniValue(t, "brand new/key", &v3);
    t = oldsettings::setIniValue(t, "toplevel", &v4);
    m = oldsettings::parseIni(t);
    CHECK_STR(at(m, "llm/openai/apiKey"), "@at");
    CHECK(t.find("openai\\apiKey=@@at") != std::string::npos);
    CHECK_STR(at(m, "brand new/key"), "line\nbreak");
    CHECK(t.find("[brand%20new]\nkey=line\\nbreak") != std::string::npos);
    CHECK_STR(at(m, "toplevel"), "g");
    CHECK_STR(at(m, "active"), "slack:T0123");
    // Removal takes the line and nothing else.
    t = oldsettings::setIniValue(t, "workspace/slack:T0123/auth", nullptr);
    m = oldsettings::parseIni(t);
    CHECK(m.find("workspace/slack:T0123/auth") == m.end());
    CHECK_STR(at(m, "workspace/slack:T0123/displayName"), "Acme, Inc.");
    // An empty file gets a section.
    CHECK_STR(oldsettings::setIniValue("", "a/b", &v4), "[a]\nb=g\n");
    CHECK_STR(oldsettings::setIniValue("", "x", &v4), "[General]\nx=g\n");
}

TEST("old settings: registry values as earlier versions read them") {
    using oldsettings::decodeRegistry;
    // REG_SZ, a string.
    CHECK_STR(decodeRegistry(1, utf16(u"xoxc-é", true)).text(), "xoxc-\xC3\xA9");
    CHECK_STR(decodeRegistry(1, utf16(u"@@x", true)).text(), "@x");
    // REG_MULTI_SZ, a string list.
    const Value l = decodeRegistry(7, utf16(std::u16string(u"a\0bc\0\0", 6), false));
    CHECK((l.kind == Value::Kind::List && l.list == std::vector<std::string>{"a", "bc"}));
    // REG_DWORD / REG_QWORD.
    CHECK(decodeRegistry(4, std::string("\x2a\0\0\0", 4)).toInt(0) == 42);
    CHECK(decodeRegistry(11, std::string("\x00\x10\x5e\x69\0\0\0\0", 8)).toInt(0) == 1767772160);
    // REG_BINARY: a @ByteArray string with NULs, as UTF-16.
    const Value b = decodeRegistry(3, utf16(std::u16string(u"@ByteArray(\x01\0\xd9)", 15), false));
    CHECK(b.kind == Value::Kind::Bytes && b.s == std::string("\x01\0\xd9", 3));
}

TEST("old settings: value conversions") {
    CHECK(oldsettings::decodeString("true").toBool(false));
    CHECK_FALSE(oldsettings::decodeString("false").toBool(true));
    CHECK_FALSE(oldsettings::decodeString("0").toBool(true));
    CHECK(Value().toBool(true));
    CHECK(oldsettings::decodeString("12").toInt(0) == 12);
    CHECK(oldsettings::decodeString("x12").toInt(7) == 7);
    CHECK(oldsettings::decodeString("@Invalid()").kind == Value::Kind::None);
    CHECK_STR(oldsettings::encodeString("@a"), "@@a");
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST("old settings: the Linux store is msga.conf under XDG_CONFIG_HOME") {
    const std::string path = oldsettings::iniPath();
    CHECK_STR(
        path,
        file::join(
            base::env("XDG_CONFIG_HOME").empty() ? file::join(base::env("HOME"), ".config")
                                                 : base::env("XDG_CONFIG_HOME"),
            "msga/msga.conf"
        )
    );
    file::remove(path);
    CHECK(oldsettings::load().empty());
    REQUIRE(oldsettings::write("workspace/slack:T1/auth", "{\"xoxp\":\"a\"}"));
    CHECK((file::size(path) > 0));
    CHECK_FALSE(file::exists(path + ".lock")); // released
    CHECK_STR(oldsettings::get("workspace/slack:T1/auth").text(), "{\"xoxp\":\"a\"}");
    REQUIRE(oldsettings::write("workspace/slack:T1/displayName", "One"));
    CHECK(oldsettings::load().size() == 2);
    REQUIRE(oldsettings::remove("workspace/slack:T1/auth"));
    CHECK(oldsettings::get("workspace/slack:T1/auth").kind == Value::Kind::None);
    CHECK_STR(oldsettings::get("workspace/slack:T1/displayName").text(), "One");
    // The store named after the application alone is a separate file.
    CHECK_STR(oldsettings::iniPath("MSGA"), file::join(file::dirName(path), "MSGA.conf"));
    file::remove(path);
}

// get() keeps the parsed file between calls: a change of the same size, by
// us or another process, within one timestamp tick, is still seen.
TEST("old settings: get() sees every change to the file") {
    const std::string path = oldsettings::iniPath();
    file::remove(path);
    CHECK(oldsettings::get("a/k").kind == Value::Kind::None);
    REQUIRE(oldsettings::write("a/k", "one"));
    CHECK_STR(oldsettings::get("a/k").text(), "one");
    REQUIRE(oldsettings::write("a/k", "two"));
    CHECK_STR(oldsettings::get("a/k").text(), "two");
    std::string text;
    REQUIRE(file::readAll(path, &text));
    const size_t at = text.find("two");
    REQUIRE(at != std::string::npos);
    text.replace(at, 3, "six"); // another process's edit
    REQUIRE(file::overwrite(path, text));
    CHECK_STR(oldsettings::get("a/k").text(), "six");
    file::remove(path);
    CHECK(oldsettings::get("a/k").kind == Value::Kind::None);
}
#endif
