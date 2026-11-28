#include "support/test.h"
#include "base/str.h"

TEST("str: simplified folds runs of Unicode whitespace into one space and trims") {
    CHECK_STR(str::simplified("  a \t\n b\r\n\nc  "), "a b c");
    CHECK_STR(str::simplified(""), "");
    CHECK_STR(str::simplified(" \t\n "), "");
    CHECK_STR(str::simplified("one"), "one");
    // \f, \v, NBSP, the em space and the ideographic space are whitespace too.
    CHECK_STR(
        str::simplified(
            "a\f\vb\xC2\xA0\xC2\xA0"
            "c\xE2\x80\x83"
            "d\xE3\x80\x80"
        ),
        "a b c d"
    );
    // Non-space multi-byte text passes through untouched.
    CHECK_STR(
        str::simplified(" r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s  \xF0\x9F\x8D\xA4 "),
        "r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s \xF0\x9F\x8D\xA4"
    );
}

TEST("str: decodeEntities named and numeric references") {
    CHECK_STR(
        str::decodeEntities("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;"),
        "a & b <c> \"d\" 'e'"
    );
    CHECK_STR(str::decodeEntities("&AMP; &Lt; &NBSP;"), "& < \xC2\xA0");
    CHECK_STR(str::decodeEntities("&#65;&#x42;&#X43;&#x1F600;"), "ABC\xF0\x9F\x98\x80");
    // nbspAsSpace turns only the references into spaces, not a literal U+00A0.
    CHECK_STR(str::decodeEntities("a&nbsp;b&#160;c\xC2\xA0", true), "a b c\xC2\xA0");
    CHECK_STR(
        str::decodeEntities("a&nbsp;b", false),
        "a\xC2\xA0"
        "b"
    );
}

TEST("str: decodeEntities leaves malformed references literal") {
    CHECK_STR(str::decodeEntities("no refs here"), "no refs here");
    CHECK_STR(str::decodeEntities("AT&T"), "AT&T");
    CHECK_STR(str::decodeEntities("a & b; c"), "a & b; c");
    CHECK_STR(str::decodeEntities("&unknown;"), "&unknown;");
    CHECK_STR(str::decodeEntities("&#0; &#; &#x;"), "&#0; &#; &#x;");
    CHECK_STR(str::decodeEntities("&verylongname;"), "&verylongname;"); // ';' too far away
    CHECK_STR(str::decodeEntities("&amp"), "&amp");
    CHECK_STR(str::decodeEntities("&&amp;"), "&&");
    // Out of range or surrogate numbers decode as U+FFFD, never as invalid UTF-8.
    CHECK_STR(str::decodeEntities("&#xD800;"), "\xEF\xBF\xBD");
    CHECK_STR(str::decodeEntities("&#9999999;"), "\xEF\xBF\xBD");
    // Digits stop at the first non-digit, as the old readers did.
    CHECK_STR(str::decodeEntities("&#65x;"), "A");
}

TEST("str: byteSize styles and rounding") {
    using str::ByteSize;
    // Whole: everything floored (the old composer chip and Settings).
    CHECK_STR(str::byteSize(0), "0 B");
    CHECK_STR(str::byteSize(1023), "1023 B");
    CHECK_STR(str::byteSize(1024), "1 KB");
    CHECK_STR(str::byteSize(2047), "1 KB");
    CHECK_STR(str::byteSize(1024 * 1024 - 1), "1023 KB");
    CHECK_STR(str::byteSize(1024 * 1024), "1 MB");
    CHECK_STR(str::byteSize(int64_t(1024) * 1024 * 1024 * 3 / 2), "1536 MB");

    // File: "" when unknown, floored KB, one MB decimal below 10.
    CHECK_STR(str::byteSize(0, ByteSize::File), "");
    CHECK_STR(str::byteSize(-5, ByteSize::File), "");
    CHECK_STR(str::byteSize(512, ByteSize::File), "512 B");
    CHECK_STR(str::byteSize(81 * 1024 - 1, ByteSize::File), "80 KB");
    CHECK_STR(str::byteSize(int64_t(1.45 * 1024 * 1024), ByteSize::File), "1.4 MB");
    CHECK_STR(str::byteSize(int64_t(9.94 * 1024 * 1024), ByteSize::File), "9.9 MB");
    CHECK_STR(str::byteSize(int64_t(10.6 * 1024 * 1024), ByteSize::File), "11 MB");
    CHECK_STR(str::byteSize(int64_t(1024) * 1024 * 1024 * 3, ByteSize::File), "3072 MB");

    // Exact: rounded KB, one decimal MB and GB (the file browser).
    CHECK_STR(str::byteSize(812, ByteSize::Exact), "812 B");
    CHECK_STR(str::byteSize(1535, ByteSize::Exact), "1 KB");
    CHECK_STR(str::byteSize(1536, ByteSize::Exact), "2 KB");
    CHECK_STR(str::byteSize(int64_t(12.5 * 1024 * 1024), ByteSize::Exact), "12.5 MB");
    CHECK_STR(str::byteSize(int64_t(1024) * 1024 * 1024 * 2, ByteSize::Exact), "2.0 GB");
}

TEST("str: trim and case") {
    CHECK(str::trim(" \t a b \r\n") == "a b");
    CHECK(str::trim("   ").empty());
    CHECK_STR(str::asciiLower("AbC\xC3\x84"), "abc\xC3\x84");
}

TEST("str: trimSpace takes Unicode whitespace off both ends") {
    CHECK(str::trimSpace("\xC2\xA0\t a b\xE2\x80\x83\n\xE3\x80\x80") == "a b");
    CHECK(str::trimSpace("\xC2\xA0\xC2\xA0").empty());
    CHECK(str::trimSpace("").empty());
    CHECK(str::trimSpace("x") == "x");
    // Not whitespace: a non-breaking letter-like character stays.
    CHECK(str::trimSpace(" \xC3\xA4 ") == "\xC3\xA4");
}

TEST("str: hexDigit and one character's asciiLower") {
    CHECK(str::hexDigit('0') == 0);
    CHECK(str::hexDigit('9') == 9);
    CHECK(str::hexDigit('a') == 10);
    CHECK(str::hexDigit('F') == 15);
    for (char c : {'g', 'G', '/', ':', '@', '`', ' ', '\0', '\xC3'})
        CHECK(str::hexDigit(c) == -1);
    CHECK(str::asciiLower('Q') == 'q');
    CHECK(str::asciiLower('q') == 'q');
    CHECK(str::asciiLower('@') == '@');
    CHECK(str::asciiLower('\xC3') == '\xC3');
}

TEST("str: split and Splitter keep empty parts") {
    const auto parts = [](const std::vector<std::string_view> &v) {
        std::string out;
        for (std::string_view p : v)
            out += str::concat({"[", p, "]"});
        return out;
    };
    CHECK_STR(parts(str::split("a,,b", ',')), "[a][][b]");
    CHECK_STR(parts(str::split("a\n", '\n')), "[a][]");
    CHECK_STR(parts(str::split("", ',')), "[]");
    CHECK_STR(parts(str::split("abc", ',')), "[abc]");
    str::Splitter    sp(",x,", ',');
    std::string      got;
    std::string_view p;
    while (sp.next(&p))
        got += str::concat({"[", p, "]"});
    CHECK_STR(got, "[][x][]");
    CHECK_FALSE(sp.next(&p));
}

TEST("str: escapeHtml") {
    CHECK_STR(str::escapeHtml("a <b> & \"c\" 'd'"), "a &lt;b&gt; &amp; \"c\" 'd'");
    CHECK_STR(str::escapeHtml("a \"c\"", true), "a &quot;c&quot;");
    CHECK_STR(str::escapeHtml("r\xC3\xA4k"), "r\xC3\xA4k");
    std::string out = "x";
    str::appendEscapedHtml(&out, "<&>");
    CHECK_STR(out, "x&lt;&amp;&gt;");
}

TEST("str: percentEncode and percentDecode") {
    CHECK_STR(str::percentEncode("a b/c?d=e&f~g.h_i-j"), "a%20b%2Fc%3Fd%3De%26f~g.h_i-j");
    CHECK_STR(str::percentEncode("/a b/c", "/"), "/a%20b/c");
    CHECK_STR(str::percentEncode("\xC3\xA4"), "%C3%A4");
    CHECK_STR(str::percentEncode(std::string_view("a\0b", 3)), "a%00b");
    CHECK_STR(str::percentDecode("a%20b%2fc%C3%A4+d"), "a b/c\xC3\xA4+d");
    // A malformed or cut escape stays as it is.
    CHECK_STR(str::percentDecode("100% %zz %4"), "100% %zz %4");
    for (std::string_view s : {"", "plain", "a b&c=d/\xF0\x9F\x98\x80", "%25"})
        CHECK_STR(str::percentDecode(str::percentEncode(s)), std::string(s));
}

TEST("str: decodeEntities looks for a ';' only nearby") {
    // Every '&' used to scan to the end for its ';': quadratic on this.
    const std::string many = std::string(200000, '&') + ";";
    CHECK(str::decodeEntities(many) == many);
    CHECK_STR(str::decodeEntities("&#x1F600;&amp;"), "\xF0\x9F\x98\x80&");
    CHECK_STR(str::decodeEntities("&#12345678;"), "\xEF\xBF\xBD");  // ';' 10 bytes on: still read
    CHECK_STR(str::decodeEntities("&#123456789;"), "&#123456789;"); // 11: literal
}
