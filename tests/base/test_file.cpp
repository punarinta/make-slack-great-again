#include "base/file.h"
#include "support/test.h"

#include <cstdlib>
#include <string>
#include <vector>
#if !defined(_WIN32) && !defined(__PEOS__)
#include <unistd.h> // symlink
#endif

static std::string tempDir() {
    const std::string d = base::test::makeTempDir("msga_base_test_");
    return d.empty() ? "/tmp" : d;
}

TEST("file: atomic write creates parents and replaces content") {
    const std::string dir  = tempDir();
    const std::string path = file::join(dir, "a/b/c.json");
    CHECK_FALSE(file::exists(path));
    REQUIRE(file::writeAtomic(path, "first"));
    std::string got;
    REQUIRE(file::readAll(path, &got));
    CHECK(got == "first");
    REQUIRE(file::writeAtomic(path, std::string(100000, 'x')));
    REQUIRE(file::readAll(path, &got));
    CHECK(got.size() == 100000);
    CHECK(file::size(path) == 100000);
    REQUIRE(file::writeAtomic(path, "not flushed", 0644, false)); // a cache's write
    REQUIRE(file::readAll(path, &got));
    CHECK(got == "not flushed");
    CHECK(file::isDir(file::join(dir, "a/b")));
    // No temp file left behind: the folder holds the file alone.
    std::vector<file::DirEntry> left;
    REQUIRE(file::listDir(file::dirName(path), &left));
    CHECK(left.size() == 1 && left[0].name == "c.json");
    CHECK(file::remove(path));
    CHECK_FALSE(file::readAll(path, &got));
    CHECK(got.empty());
    CHECK(file::size(path) == -1);
    file::remove(file::join(dir, "a/b"));
    file::remove(file::join(dir, "a"));
    CHECK(file::remove(dir));
}

TEST("file: path strings") {
    CHECK(file::dirName("a/b/c.txt") == "a/b");
    CHECK(file::dirName("c.txt") == "");
    CHECK(file::dirName("/c.txt") == "/");
    CHECK(file::baseName("a/b/c.txt") == "c.txt");
    CHECK(file::extension("x/c.tar.gz") == "gz");
    CHECK(file::extension(".bashrc") == "");
    CHECK(file::extension("noext") == "");
    CHECK(file::join("ws", "./assets/a.png") == "ws/assets/a.png");
    CHECK(file::join("ws/", "a") == "ws/a");
#ifdef _WIN32
    CHECK(file::resolve("ws", "C:/abs/x") == "C:/abs/x");
    const std::string rooted = file::absolute("/abs/x"); // on the current drive
    CHECK(rooted.size() == 8 && rooted.compare(1, 7, ":/abs/x") == 0);
#else
    CHECK(file::resolve("ws", "/abs/x") == "/abs/x");
#endif
    CHECK(file::isAbsolute(file::absolute("rel")));
}

TEST("file: file:// URLs made from local paths and read back, percent-encoded") {
    CHECK_STR(file::toFileUrl("/a/b c"), "file:///a/b%20c");
    CHECK_STR(file::toFileUrl("/x/100%#?.txt"), "file:///x/100%25%23%3F.txt");
    CHECK_STR(file::toFileUrl("/x/a+b(1),c=d;e@f:g~h!$&'*"), "file:///x/a+b(1),c=d;e@f:g~h!$&'*");
    CHECK_STR(file::toFileUrl("/x/caf\xC3\xA9 [1]"), "file:///x/caf%C3%A9%20%5B1%5D");
    CHECK_STR(file::toFileUrl("C:/Users/me/a b.png"), "file:///C:/Users/me/a%20b.png");
    CHECK_STR(file::toFileUrl("//srv/share/x"), "file://srv/share/x");
    for (const char *p : {"/a/b c", "/x/100%#?.txt", "/x/caf\xC3\xA9 [1]", "/x/a+b;c"})
        CHECK_STR(file::fromFileUrl(file::toFileUrl(p)), p);
    CHECK_STR(file::fromFileUrl("file://localhost/a/b%20c"), "/a/b c");
    CHECK_STR(file::fromFileUrl("https://x/y"), "");
#ifdef _WIN32
    CHECK_STR(file::toFileUrl("C:\\Users\\me\\x.txt"), "file:///C:/Users/me/x.txt");
    CHECK_STR(file::fromFileUrl("file:///C:/Users/me/a%20b.png"), "C:/Users/me/a b.png");
    CHECK_STR(file::fromFileUrl("file://srv/share/x"), "//srv/share/x");
#else
    CHECK_STR(file::fromFileUrl("file:///C:/x"), "/C:/x");
#endif
}

TEST("file: list a directory and copy a file") {
    const std::string dir = tempDir();
    REQUIRE(file::writeAtomic(file::join(dir, "a.txt"), "hello"));
    REQUIRE(file::writeAtomic(file::join(dir, ".hidden"), ""));
    REQUIRE(file::makeDirs(file::join(dir, "sub")));
    std::vector<file::DirEntry> es;
    REQUIRE(file::listDir(dir, &es));
    CHECK(es.size() == 3);
    int seen = 0;
    for (const auto &e : es) {
        if (e.name == "a.txt")
            seen += CHECK(!e.isDir && e.size == 5 && !e.hidden) ? 1 : 0;
        else if (e.name == ".hidden")
            seen += CHECK(e.hidden) ? 1 : 0;
        else if (e.name == "sub")
            seen += CHECK(e.isDir && !e.hidden) ? 1 : 0;
    }
    CHECK(seen == 3);
    CHECK_FALSE(file::listDir(file::join(dir, "missing"), &es));
    CHECK(es.empty());

    CHECK(file::copy(file::join(dir, "a.txt"), file::join(dir, "sub/b.txt")));
    std::string got;
    CHECK(file::readAll(file::join(dir, "sub/b.txt"), &got) && got == "hello");
    CHECK_FALSE(file::copy(file::join(dir, "missing"), file::join(dir, "c.txt")));
    CHECK_FALSE(file::exists(file::join(dir, "c.txt")));

    file::remove(file::join(dir, "sub/b.txt"));
    file::remove(file::join(dir, "sub"));
    file::remove(file::join(dir, "a.txt"));
    file::remove(file::join(dir, ".hidden"));
    CHECK(file::remove(dir));
}

TEST("file: byte ranges and in-place overwrite") {
    const std::string dir  = tempDir();
    const std::string path = file::join(dir, "log.txt");
    REQUIRE(file::writeAtomic(path, "0123456789"));
    std::string got;
    REQUIRE(file::readRange(path, 3, 4, &got));
    CHECK(got == "3456");
    REQUIRE(file::readRange(path, 8, 100, &got));
    CHECK(got == "89");
    REQUIRE(file::readRange(path, 20, 4, &got)); // past the end: nothing
    CHECK(got.empty());
    CHECK_FALSE(file::readRange(path, -1, 4, &got));
    CHECK_FALSE(file::readRange(file::join(dir, "missing"), 0, 4, &got));
    // The same file (a writer holding it open keeps appending to it), shorter.
    REQUIRE(file::overwrite(path, "abc"));
    REQUIRE(file::readAll(path, &got));
    CHECK(got == "abc");
    CHECK_FALSE(file::overwrite(file::join(dir, "missing"), "x")); // never creates one
    file::remove(path);
    CHECK(file::remove(dir));
}

TEST("file: stat, treeBytes and removeTree, links not followed") {
    const std::string dir = tempDir();
    const std::string t = file::join(dir, "t"), a = file::join(t, "a"), b = file::join(t, "sub/b");
    REQUIRE(file::writeAtomic(a, "abc"));
    REQUIRE(file::writeAtomic(b, "12345"));
    file::Stat st;
    REQUIRE(file::stat(a, &st));
    CHECK(st.size == 3);
    CHECK_FALSE(st.isDir);
    CHECK(st.mtimeMicros > 1600000000LL * 1000000);
    CHECK(st.birthMicros > 1600000000LL * 1000000);
    CHECK(st.birthMicros <= st.mtimeMicros + 1000000);
    REQUIRE(file::stat(t, &st));
    CHECK(st.isDir);
    CHECK(st.size == 0);
    const file::Stat before = st;
    CHECK_FALSE(file::stat(file::join(dir, "missing"), &st));
    CHECK(st.mtimeMicros == before.mtimeMicros); // untouched
    CHECK_FALSE(file::stat("", &st));

    CHECK(file::treeBytes(t) == 8);
    CHECK(file::treeBytes(a) == 3);
    CHECK(file::treeBytes(file::join(dir, "missing")) == 0);
#if !defined(_WIN32) && !defined(__PEOS__) // PEOS has no links
    const std::string keep = file::join(dir, "keep/k");
    REQUIRE(file::writeAtomic(keep, "1234567"));
    REQUIRE(::symlink(file::join(dir, "keep").c_str(), file::join(t, "link").c_str()) == 0);
    CHECK(file::treeBytes(t) == 8); // the link is not followed
#endif
    CHECK(file::removeTree(t));
    CHECK_FALSE(file::exists(t));
#if !defined(_WIN32) && !defined(__PEOS__)
    CHECK(file::exists(keep)); // what the link pointed at is still there
#endif
    CHECK(file::removeTree(t)); // nothing there: done
    CHECK(file::removeTree(dir));
    CHECK_FALSE(file::exists(dir));
}
