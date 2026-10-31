// msga_net tests: the pure helpers, then HTTP and WebSocket against a local
// server (tests/server.py), then optional live tests.
//
// Environment switches:
//   NET_TEST_URL=http://host:port   use an already running server.py instead
//       of starting one (`python3 server.py --host 0.0.0.0 --port 8099` on
//       the Linux host, then run the Windows build under Wine or the Mac
//       build with NET_TEST_URL=http://<linux-ip>:8099). Without it the test
//       spawns `python3 NET_TEST_SERVER --tls` itself (POSIX only; on Windows
//       the server tests are skipped without it).
//   MSGA_NET_LIVE=1   also run the tests against the internet (slack.com,
//       badssl.com, a public wss:// echo).
//   NET_PROBE_URL=<url>   the "probe" case fetches it and prints the result.
// Tests that need a capability the platform or setup lacks print "skip: …"
// and pass: the local TLS server (needs openssl, and our own CA loading,
// which honours SSL_CERT_FILE — the posix transport only).
#include "support/test.h"
#include "net/net.h"
#include "net/transport.h"
#include "base/thread.h"
#include "plat/plat.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

// The posix transport is ours: exact error reasons, no Accept-Encoding,
// chunk extensions, no DHE. The OS stacks (WinHTTP, NSURLSession) differ.
#if !defined(_WIN32) && !defined(__APPLE__)
#define NET_TEST_POSIX 1
#define NET_TEST_OWN_TLS 1 // our CA loading honours SSL_CERT_FILE
#include "net/posix/posix.h"

#include <atomic>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <thread>
#endif

namespace {

// ── Harness ─────────────────────────────────────────────────────────────────

plat::App &app() {
    static std::unique_ptr<plat::App> a = plat::App::create();
    return *a;
}

int64_t msNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

// Pumps the loop until `done` or the time runs out; false on timeout.
template <class F>
bool pumpUntil(F done, int timeoutMs = 10000) {
    const int64_t end = msNow() + timeoutMs;
    while (!done()) {
        if (msNow() > end)
            return false;
        app().pump(20);
    }
    return true;
}

void pumpFor(int ms) {
    const int64_t end = msNow() + ms;
    while (msNow() < end)
        app().pump(10);
}

struct Server {
    std::string base;      // "http://127.0.0.1:port"
    std::string tlsBase;   // "https://localhost:port", empty when not available
    std::string tls12Base; // the same, capped at TLS 1.2
    std::string tlsWhy;    // why not
    std::string host;      // of base
    int         port = 0;
};

#ifndef _WIN32
pid_t g_serverPid   = -1;
int   g_serverStdin = -1;

void stopServer() {
    if (g_serverStdin >= 0)
        close(g_serverStdin); // server.py exits at EOF on stdin
    if (g_serverPid > 0) {
        kill(g_serverPid, SIGTERM);
        waitpid(g_serverPid, nullptr, 0);
    }
}

void spawnServer(Server *s) {
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0)
        return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, toChild[0], 0);
    posix_spawn_file_actions_adddup2(&fa, fromChild[1], 1);
    posix_spawn_file_actions_addclose(&fa, toChild[1]);
    posix_spawn_file_actions_addclose(&fa, fromChild[0]);
    char      py[] = "python3", script[] = NET_TEST_SERVER, tls[] = "--tls";
    char     *argv[] = {py, script, tls, nullptr};
    const int rc     = posix_spawnp(&g_serverPid, "python3", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(toChild[0]);
    close(fromChild[1]);
    if (rc != 0) {
        g_serverPid = -1;
        close(toChild[1]);
        close(fromChild[0]);
        return;
    }
    g_serverStdin = toChild[1];
    std::atexit(stopServer);
    std::string out;
    char        buf[256];
    while (out.find("READY\n") == std::string::npos) {
        const ssize_t n = read(fromChild[0], buf, sizeof buf);
        if (n <= 0)
            break;
        out.append(buf, size_t(n));
    }
    close(fromChild[0]);
    int port = 0, tlsPort = 0;
    std::sscanf(out.c_str(), "PORT %d", &port);
    if (port > 0)
        s->base = "http://127.0.0.1:" + std::to_string(port);
    const size_t t = out.find("TLS ");
    if (t != std::string::npos) {
        char bundle[512] = {};
        if (std::sscanf(out.c_str() + t, "TLS %d %511s", &tlsPort, bundle) == 2 && tlsPort > 0) {
#ifdef NET_TEST_OWN_TLS
            // Before the first TLS connection: the CA store loads once.
            base::test::setEnv("SSL_CERT_FILE", bundle);
            s->tlsBase         = "https://localhost:" + std::to_string(tlsPort);
            int          tls12 = 0;
            const size_t t12   = out.find("TLS12 ");
            if (t12 != std::string::npos && std::sscanf(out.c_str() + t12, "TLS12 %d", &tls12) == 1)
                s->tls12Base = "https://localhost:" + std::to_string(tls12);
#else
            s->tlsWhy = "the OS's TLS ignores SSL_CERT_FILE";
#endif
        } else {
            s->tlsWhy = out.substr(t + 4, out.find('\n', t) - t - 4);
        }
    }
}
#endif

const Server &server() {
    static Server s = [] {
        Server      out;
        const char *url = std::getenv("NET_TEST_URL");
        if (url && *url) {
            out.base = url;
            while (!out.base.empty() && out.base.back() == '/')
                out.base.pop_back();
            out.tlsWhy = "NET_TEST_URL is set";
        } else {
#ifndef _WIN32
            spawnServer(&out);
#endif
        }
        net::Url u;
        if (u.parse(out.base)) {
            out.host = u.host;
            out.port = u.port;
        } else {
            std::fprintf(stderr, "  no test server (python3 missing? set NET_TEST_URL)\n");
        }
        return out;
    }();
    return s;
}

bool live() {
    const char *v = std::getenv("MSGA_NET_LIVE");
    return v && std::strcmp(v, "1") == 0;
}

void skip(const char *what, std::string_view why) {
    std::fprintf(stderr, "  skip: %s (%.*s)\n", what, int(why.size()), why.data());
}

// One request, synchronously (through the loop, as the app would).
net::Response fetch(net::Request req) {
    net::Client   c(app());
    net::Response out;
    bool          done = false;
    c.send(std::move(req), [&](net::Response r) {
        out  = std::move(r);
        done = true;
    });
    if (!pumpUntil([&] { return done; }, 40000))
        out.error = "test: no answer";
    return out;
}

net::Response get(const std::string &url, std::vector<net::Header> headers = {}) {
    net::Request r;
    r.url     = url;
    r.headers = std::move(headers);
    return fetch(std::move(r));
}

bool startsWith(std::string_view s, std::string_view p) {
    return s.substr(0, p.size()) == p;
}
bool contains(std::string_view s, std::string_view p) {
    return s.find(p) != std::string_view::npos;
}

// ── Helpers: Url ────────────────────────────────────────────────────────────

TEST("url: parse") {
    net::Url u;
    REQUIRE(u.parse("HTTPS://Example.COM/a/b?c=1#frag"));
    CHECK_STR(u.scheme, "https");
    CHECK_STR(u.host, "example.com");
    CHECK(u.port == 443);
    CHECK_STR(u.target, "/a/b?c=1");
    CHECK(u.secure());
    REQUIRE(u.parse("http://[::1]:8080/x"));
    CHECK_STR(u.host, "::1");
    CHECK(u.port == 8080);
    CHECK_STR(u.str(), "http://[::1]:8080/x");
    REQUIRE(u.parse("http://host"));
    CHECK_STR(u.target, "/");
    REQUIRE(u.parse("http://host?x=1"));
    CHECK_STR(u.target, "/?x=1");
    REQUIRE(u.parse("ws://user:pw@h:81/sock"));
    CHECK_STR(u.host, "h");
    CHECK(u.port == 81);
    CHECK_FALSE(u.secure());
    REQUIRE(u.parse("wss://h/"));
    CHECK(u.port == 443);
    CHECK(u.secure());
    CHECK_FALSE(u.parse("ftp://h/"));
    CHECK_FALSE(u.parse("http:///path"));
    CHECK_FALSE(u.parse("http://h:99999/"));
    CHECK_FALSE(u.parse("http://h:0/"));
    CHECK_FALSE(u.parse("http://h:8x/"));
    CHECK_FALSE(u.parse("http://[::1/"));
    CHECK_FALSE(u.parse("no-scheme"));
}

TEST("url: str") {
    net::Url u;
    REQUIRE(u.parse("http://h:80/p"));
    CHECK_STR(u.str(), "http://h/p");
    REQUIRE(u.parse("https://h:8443"));
    CHECK_STR(u.str(), "https://h:8443/");
}

TEST("url: resolve (RFC 3986 5.4)") {
    net::Url base;
    REQUIRE(base.parse("http://a/b/c/d;p?q"));
    const char *const cases[][2] = {
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q"},
        {"g#s", "http://a/b/c/g"},
        {";x", "http://a/b/c/;x"},
        {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
        {"https://other:8443/x?y", "https://other:8443/x?y"},
    };
    for (const auto &c : cases)
        if (!CHECK_STR(base.resolve(c[0]), c[1]))
            std::fprintf(stderr, "    for ref \"%s\"\n", c[0]);
    net::Url v6;
    REQUIRE(v6.parse("http://[fe80::1]:81/a/b"));
    CHECK_STR(v6.resolve("c"), "http://[fe80::1]:81/a/c");
    CHECK_STR(v6.resolve("//[::2]/z"), "http://[::2]/z");
}

TEST("url: percent and form encoding") {
    CHECK_STR(net::percentEncode("a b/\xc3\xbc~-._"), "a%20b%2F%C3%BC~-._");
    CHECK_STR(net::percentDecode("a%20b%2f%C3%BC"), "a b/\xc3\xbc");
    CHECK_STR(net::percentDecode("100%"), "100%");
    CHECK_STR(net::percentDecode("%zz%4"), "%zz%4");
    CHECK_STR(net::percentDecode("a+b"), "a+b");
    CHECK_STR(net::formEncode({{"a", "1"}, {"b", "x y&z"}}), "a=1&b=x%20y%26z");
    CHECK_STR(net::formEncode({}), "");
    CHECK_STR(net::queryValue("?a=1&b=x+y%21&c", "b"), "x y!");
    CHECK_STR(net::queryValue("a=1&b=2", "a"), "1");
    CHECK_STR(net::queryValue("a=1&c", "c"), "");
    CHECK_STR(net::queryValue("a=1", "zz"), "");
    CHECK_STR(net::queryValue("x%5B%5D=v", "x[]"), "v");
}

TEST("response: header lookup") {
    net::Response r;
    r.headers = {{"Content-Type", "text/plain"}, {"Set-Cookie", "a=1"}, {"set-cookie", "b=2"}};
    CHECK_STR(r.header("content-type"), "text/plain");
    CHECK_STR(r.header("SET-COOKIE"), "a=1");
    CHECK_STR(r.header("missing"), "");
    r.status = 204;
    CHECK(r.ok());
    r.status = 302;
    CHECK_FALSE(r.ok());
}

TEST("response: header lines, content length (every transport's parser)") {
    std::vector<net::Header> h;
    CHECK(
        net::detail::parseHeaderLines(
            "Content-Type: text/plain\r\nX-Long: a\r\n  b\r\n\tc\r\nContent-Length:  12 "
            "\r\n\r\nbody",
            &h
        )
    );
    REQUIRE(h.size() == 3);
    CHECK_STR(h[0].name, "Content-Type");
    CHECK_STR(h[1].value, "a b c"); // folded lines join on
    CHECK_STR(net::detail::headerValue(h, "CONTENT-length"), "12");
    CHECK(net::detail::contentLength(h) == 12);
    h.clear();
    CHECK(net::detail::parseHeaderLines("A: 1\r\nB: 2", &h) && h.size() == 2); // no final CRLF
    h.clear();
    CHECK_FALSE(net::detail::parseHeaderLines("A: 1\r\nno colon\r\n\r\n", &h));
    CHECK(h.size() == 1);
    h.clear();
    CHECK_FALSE(net::detail::parseHeaderLines(" folded first\r\n", &h));
    CHECK_FALSE(net::detail::parseHeaderLines(": no name\r\n", &h));

    int64_t n = -1;
    CHECK(net::detail::parseContentLength(" 0 ", &n) && n == 0);
    CHECK(net::detail::parseContentLength("1125899906842624", &n) && n == (int64_t(1) << 50));
    CHECK_FALSE(net::detail::parseContentLength("", &n));
    CHECK_FALSE(net::detail::parseContentLength("12a", &n));
    CHECK_FALSE(net::detail::parseContentLength("-1", &n));
    CHECK_FALSE(net::detail::parseContentLength("99999999999999999999", &n));
    CHECK(net::detail::contentLength({{"Content-Length", "x"}}) == 0);
    CHECK(net::detail::contentLength({}) == 0);
}

TEST("multipart: fields, a file, the closing boundary") {
    net::Multipart m("BND");
    m.field("model", "whisper-1");
    m.file("file", "a\"b\r\n.ogg", "audio/ogg", "DATA");
    m.file("image", "x.png", {}, "PNG");
    CHECK_STR(m.contentType(), "multipart/form-data; boundary=BND");
    CHECK_STR(
        m.body(),
        "--BND\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
        "--BND\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a_b  .ogg\"\r\n"
        "Content-Type: audio/ogg\r\n\r\nDATA\r\n"
        "--BND\r\nContent-Disposition: form-data; name=\"image\"; filename=\"x.png\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\nPNG\r\n"
        "--BND--\r\n"
    );
}

#ifdef NET_TEST_POSIX
namespace {
// The voluntary context switches of thread `tid` so far (Linux /proc).
long voluntarySwitches(pid_t tid) {
    std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/status");
    std::string   line;
    while (std::getline(f, line))
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0)
            return std::atol(line.c_str() + 24);
    return -1;
}
} // namespace

// An idle WebSocket reader waits with a wake fd and no deadline: it must
// sleep until something happens (no 4×/s poll timeouts), yet an abort (flag +
// wake write) still ends the wait at once.
TEST("posix waitFd: a wake-fd wait blocks without periodic wakeups, aborts at once") {
    int data[2], wake[2];
    REQUIRE(pipe2(data, O_NONBLOCK | O_CLOEXEC) == 0);
    REQUIRE(pipe2(wake, O_NONBLOCK | O_CLOEXEC) == 0);
    std::atomic<bool>    cancel{false};
    std::atomic<pid_t>   tid{0};
    std::atomic<int>     result{-1};
    std::atomic<int64_t> returnedAt{0};
    std::thread          t([&] {
        tid.store(gettid());
        net::detail::Waiter w;
        w.cancel = &cancel;
        w.wakeFd = wake[0];
        result.store(int(net::detail::waitFd(data[0], POLLIN, w, true)));
        returnedAt.store(msNow());
    });
    while (!tid.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const long before = voluntarySwitches(tid.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const long after = voluntarySwitches(tid.load());
    CHECK(before >= 0);
    // The old 250 ms slices woke it 4 times here.
    CHECK(after - before <= 1);
    CHECK(result.load() == -1);

    const int64_t aborted = msNow();
    cancel.store(true);
    const char c = 1;
    REQUIRE(::write(wake[1], &c, 1) == 1);
    t.join();
    CHECK(result.load() == int(net::detail::Wait::Cancelled));
    CHECK(returnedAt.load() - aborted < 100);

    // A plain wake (a queued send) returns Woken; readable data returns Ready.
    cancel.store(false);
    net::detail::Waiter w;
    w.cancel = &cancel;
    w.wakeFd = wake[0];
    REQUIRE(::write(wake[1], &c, 1) == 1);
    CHECK(net::detail::waitFd(data[0], POLLIN, w, true) == net::detail::Wait::Woken);
    REQUIRE(::write(data[1], &c, 1) == 1);
    CHECK(net::detail::waitFd(data[0], POLLIN, w, true) == net::detail::Wait::Ready);
    // A deadline still times the wait out.
    char drain;
    REQUIRE(::read(data[0], &drain, 1) == 1);
    w.deadline       = net::detail::nowMs() + 50;
    const int64_t t0 = msNow();
    CHECK(net::detail::waitFd(data[0], POLLIN, w, true) == net::detail::Wait::Timeout);
    CHECK(msNow() - t0 < 500);
    for (int fd : {data[0], data[1], wake[0], wake[1]})
        ::close(fd);
}
#endif

TEST("loopback port") {
    const int p = net::freeLoopbackPort();
    CHECK(p > 1024 && p < 65536);
}

// ── HTTP ────────────────────────────────────────────────────────────────────

#ifdef _WIN32
// No server.py of our own here: without NET_TEST_URL the server tests skip.
#define NEED_SERVER()                                                                              \
    const Server &srv = server();                                                                  \
    if (srv.base.empty())                                                                          \
        return skip("needs a server", "set NET_TEST_URL");
#else
#define NEED_SERVER()                                                                              \
    const Server &srv = server();                                                                  \
    REQUIRE(!srv.base.empty())
#endif

TEST("http: content-length body") {
    NEED_SERVER();
    const net::Response r = get(srv.base + "/plain");
    CHECK_STR(r.error, "");
    CHECK(r.status == 200);
    CHECK_STR(r.body, "hello world");
    CHECK_STR(r.header("content-type"), "text/plain");
    CHECK_STR(r.url, srv.base + "/plain");
}

TEST("http: chunked with trailers") {
    NEED_SERVER();
    const std::string   want = "chunk one, chunk two, " + std::string(70000, 'x') + "end";
    const net::Response r    = get(srv.base + "/chunked");
    CHECK_STR(r.error, "");
    CHECK(r.status == 200);
    CHECK(r.body == want);
#ifdef NET_TEST_POSIX
    // Chunk extensions (";ext=1") are skipped; CFNetwork refuses them.
    const net::Response e = get(srv.base + "/chunked-ext");
    CHECK_STR(e.error, "");
    CHECK(e.body == want);
#endif
}

TEST("http: body until close") {
    NEED_SERVER();
    const net::Response r = get(srv.base + "/close");
    CHECK_STR(r.error, "");
    CHECK(startsWith(r.body, "until close "));
}

TEST("http: big body") {
    NEED_SERVER();
    const size_t        n = 5 * 1000 * 1000 + 7;
    const net::Response r = get(srv.base + "/big?n=" + std::to_string(n));
    CHECK_STR(r.error, "");
    REQUIRE(r.body.size() == n);
    bool same = true;
    for (size_t i = 0; i < n && same; i += 997)
        same = uint8_t(r.body[i]) == i % 251;
    CHECK(same);
}

TEST("http: download progress — throttled, in order, never after done") {
    NEED_SERVER();
    const int64_t n = 3 * 1000 * 1000;
    struct Tick {
        int64_t got, total;
    };
    std::vector<Tick> ticks;
    bool              done = false, late = false;
    net::Client       c(app());
    net::Request      req;
    req.url        = srv.base + "/big?ms=1000&n=" + std::to_string(n); // a second long
    req.onProgress = [&](int64_t got, int64_t total) {
        late = late || done;
        ticks.push_back({got, total});
    };
    net::Response out;
    c.send(std::move(req), [&](net::Response r) {
        out  = std::move(r);
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }, 40000));
    pumpFor(50);
    CHECK_STR(out.error, "");
    CHECK(int64_t(out.body.size()) == n);
    CHECK(!late);
    REQUIRE(ticks.size() >= 2);
    CHECK(ticks.size() <= 110); // about once a percent
    bool ordered = true, sized = true;
    for (size_t i = 0; i < ticks.size(); ++i) {
        ordered = ordered && (i == 0 || ticks[i].got > ticks[i - 1].got);
        sized   = sized && ticks[i].total == n;
    }
    CHECK(ordered);
    CHECK(sized);
    CHECK(ticks.back().got == n);

    // Unknown size (chunked): total 0. A cancelled request hears nothing more.
    ticks.clear();
    done = false;
    net::Request ch;
    ch.url        = srv.base + "/chunked";
    ch.onProgress = [&](int64_t got, int64_t total) { ticks.push_back({got, total}); };
    c.send(std::move(ch), [&](net::Response) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    REQUIRE(!ticks.empty());
    CHECK(ticks.back().total == 0);
    ticks.clear();
    net::Request big;
    big.url                 = srv.base + "/big?n=" + std::to_string(n);
    big.onProgress          = [&](int64_t got, int64_t total) { ticks.push_back({got, total}); };
    const net::RequestId id = c.send(std::move(big), [&](net::Response) { late = true; });
    c.cancel(id);
    pumpFor(300);
    CHECK(ticks.empty());
    CHECK(!late);
}

TEST("http: a handler hears a 2xx body as it comes, on the worker; other answers fill body") {
    NEED_SERVER();
    struct Collect final : net::Handler {
        std::string     got, finishedBody;
        int             pieces = 0, finishedStatus = -1;
        bool            offUi = true, refuse = false;
        std::thread::id ui = std::this_thread::get_id();
        bool            body(const char *d, size_t n) override {
            offUi = offUi && std::this_thread::get_id() != ui;
            got.append(d, n);
            ++pieces;
            return !refuse;
        }
        void finished(net::Response &r) override {
            offUi          = offUi && std::this_thread::get_id() != ui;
            finishedStatus = r.status;
            finishedBody   = r.body;
        }
    };
    auto run = [&](const std::string &url, bool stream, bool refuse = false) {
        auto h        = std::make_shared<Collect>();
        h->streamBody = stream;
        h->refuse     = refuse;
        net::Request req;
        req.url     = url;
        req.handler = h;
        return std::make_pair(fetch(std::move(req)), h);
    };
    // Content-Length, in pieces, nothing in Response::body.
    const size_t n = 3 * 1000 * 1000 + 7;
    auto [big, hb] = run(srv.base + "/big?n=" + std::to_string(n), true);
    CHECK_STR(big.error, "");
    CHECK(big.status == 200);
    CHECK(big.body.empty());
    REQUIRE(hb->got.size() == n);
    bool same = true;
    for (size_t i = 0; i < n && same; i += 997)
        same = uint8_t(hb->got[i]) == i % 251;
    CHECK(same);
    CHECK(hb->pieces > 1);
    CHECK(hb->offUi);
    CHECK(hb->finishedStatus == 200);
    // Chunked, and until the close.
    auto [ch, hc] = run(srv.base + "/chunked", true);
    CHECK_STR(ch.error, "");
    CHECK(hc->got == "chunk one, chunk two, " + std::string(70000, 'x') + "end");
    auto [cl, hl] = run(srv.base + "/close", true);
    CHECK_STR(cl.error, "");
    CHECK(startsWith(hl->got, "until close "));
    // A redirect's own body never reaches it; the final 2xx does.
    auto [rd, hr] = run(srv.base + "/r1", true);
    CHECK(rd.status == 200);
    CHECK(rd.body.empty());
    CHECK(contains(hr->got, "hop=1"));
    // Not 2xx: the body is the Response's as always.
    auto [nf, hn] = run(srv.base + "/status?code=404", true);
    CHECK(nf.status == 404);
    CHECK_STR(nf.body, "status");
    CHECK(hn->got.empty());
    CHECK_STR(hn->finishedBody, "status");
    // Not streaming: finished() sees the whole answer before `done`.
    auto [pl, hp] = run(srv.base + "/plain", false);
    CHECK_STR(pl.body, "hello world");
    CHECK_STR(hp->finishedBody, "hello world");
    CHECK(hp->got.empty());
    CHECK(hp->offUi);
    // A refusal stops the exchange.
    auto [st, hs] = run(srv.base + "/big?n=" + std::to_string(n), true, true);
    CHECK(st.status == 0);
    CHECK_STR(st.error, "sink");
    CHECK(hs->pieces == 1);
}

TEST("http: HEAD and 204 have no body") {
    NEED_SERVER();
    net::Request req;
    req.method      = "HEAD";
    req.url         = srv.base + "/plain";
    net::Response r = fetch(req);
    CHECK_STR(r.error, "");
    CHECK(r.status == 200);
    CHECK_STR(r.header("content-length"), "11");
    CHECK(r.body.empty());
    r = get(srv.base + "/nocontent");
    CHECK(r.status == 204);
    CHECK(r.body.empty());
    // and the connection is still good for the next one
    CHECK_STR(get(srv.base + "/plain").body, "hello world");
}

TEST("http: status codes are answers") {
    NEED_SERVER();
    const net::Response r = get(srv.base + "/status?code=404");
    CHECK_STR(r.error, "");
    CHECK(r.status == 404);
    CHECK_FALSE(r.ok());
    CHECK(get(srv.base + "/status?code=500").status == 500);
}

TEST("http: POST echo") {
    NEED_SERVER();
    net::Request req;
    req.method      = "POST";
    req.url         = srv.base + "/echo";
    req.body        = net::formEncode({{"a", "1"}, {"b", "x y"}});
    req.headers     = {{"Content-Type", "application/x-www-form-urlencoded"}};
    net::Response r = fetch(req);
    CHECK_STR(r.error, "");
    CHECK(contains(r.body, "\"body\": \"a=1&b=x%20y\""));
    CHECK(contains(r.body, "\"content-type\": \"application/x-www-form-urlencoded\""));
    // a body-less POST still says Content-Length: 0
    req.body.clear();
    r = fetch(req);
    CHECK(contains(r.body, "\"content-length\": \"0\""));
    // a body bigger than one write
    req.body = std::string(300000, 'q');
    r        = fetch(req);
    CHECK(contains(r.body, "\"content-length\": \"300000\""));
}

TEST("http: request headers arrive, no Accept-Encoding") {
    NEED_SERVER();
    const net::Response r = get(srv.base + "/headers", {{"X-Test", "yes"}, {"User-Agent", "t/1"}});
    CHECK(contains(r.body, "\"x-test\": \"yes\""));
    CHECK(contains(r.body, "\"user-agent\": \"t/1\""));
    CHECK(contains(r.body, "\"host\": \"" + srv.host + ":" + std::to_string(srv.port) + "\""));
#ifdef NET_TEST_POSIX
    CHECK_FALSE(contains(r.body, "accept-encoding")); // NSURLSession adds (and decodes) it
#endif
}

TEST("http: a default User-Agent on every OS") {
    NEED_SERVER();
    const net::Response r = get(srv.base + "/headers", {});
    CHECK(contains(r.body, "\"user-agent\": \"Mozilla/5.0\""));
}

TEST("http: repeated headers keep order") {
    NEED_SERVER();
    const net::Response      r = get(srv.base + "/setcookies");
    std::vector<std::string> cookies;
    int                      midAt = -1, i = 0;
    for (const auto &h : r.headers) {
        if (h.name == "Set-Cookie")
            cookies.push_back(h.value);
        if (h.name == "X-Mid")
            midAt = i;
        ++i;
    }
    REQUIRE(cookies.size() == 2);
    CHECK_STR(cookies[0], "a=1; Path=/");
    CHECK_STR(cookies[1], "b=2; Path=/; HttpOnly");
#ifndef __APPLE__
    // NSURLSession keeps only the Set-Cookie lines' relative order.
    CHECK(midAt > 0);
#else
    (void)midAt;
#endif
}

TEST("http: keep-alive reuses the connection") {
    NEED_SERVER();
    net::Client c(app());
    std::string first, second;
    c.send({.url = srv.base + "/keepalive"}, [&](net::Response r) { first = r.body; });
    REQUIRE(pumpUntil([&] { return !first.empty(); }));
    c.send({.url = srv.base + "/keepalive"}, [&](net::Response r) { second = r.body; });
    REQUIRE(pumpUntil([&] { return !second.empty(); }));
    CHECK_STR(second, first);
}

TEST("http: Connection: close is honoured") {
    NEED_SERVER();
    const std::string a = get(srv.base + "/closehdr").body;
    const std::string b = get(srv.base + "/closehdr").body;
    CHECK(!a.empty() && !b.empty());
    CHECK(a != b);
}

TEST("http: a dropped kept-alive connection is retried") {
    NEED_SERVER();
    for (int i = 0; i < 5; ++i) {
        CHECK_STR(get(srv.base + "/drop").error, "");
        CHECK_STR(get(srv.base + "/plain").body, "hello world");
    }
}

// The answer to a POST on a reused connection was lost: the request may have
// been acted on, so it must NOT be sent again (a duplicate Slack message).
TEST("http: a POST that got no answer is not re-sent") {
    NEED_SERVER();
    net::Client   c(app());
    net::Response r;
    bool          got = false;
    auto          run = [&](net::Request req) {
        got = false;
        c.send(std::move(req), [&](net::Response x) {
            r   = std::move(x);
            got = true;
        });
        REQUIRE(pumpUntil([&] { return got; }));
    };
    const int before = std::atoi(get(srv.base + "/swallowed").body.c_str());
    run({.url = srv.base + "/plain"}); // a kept-alive connection to reuse
    run({.method = "POST", .url = srv.base + "/post-swallow", .body = "x"});
    CHECK(!r.error.empty());
    CHECK(std::atoi(get(srv.base + "/swallowed").body.c_str()) == before + 1);
}

TEST("http: concurrent requests") {
    NEED_SERVER();
    net::Client c(app());
    int         done = 0, good = 0;
    for (int i = 0; i < 12; ++i)
        c.send({.url = srv.base + (i % 3 ? "/plain" : "/chunked")}, [&](net::Response r) {
            ++done;
            good += r.error.empty() && r.status == 200;
        });
    REQUIRE(pumpUntil([&] { return done == 12; }));
    CHECK(good == 12);
}

// ── Redirects ───────────────────────────────────────────────────────────────

TEST("redirect: cookies carry over the chain") {
    NEED_SERVER();
    const net::Response r = get(srv.base + "/r1", {{"Cookie", "orig=1; other=2"}});
    CHECK_STR(r.error, "");
    CHECK(r.status == 200);
    CHECK_STR(r.url, srv.base + "/final");
    CHECK(contains(r.body, "orig=1"));
    CHECK(contains(r.body, "other=2"));
    CHECK(contains(r.body, "hop=1"));
}

TEST("redirect: Authorization only to the same host") {
    NEED_SERVER();
    net::Response r = get(srv.base + "/same-host", {{"Authorization", "Bearer x"}});
    CHECK(contains(r.body, "\"authorization\": \"Bearer x\""));
    if (srv.host != "127.0.0.1")
        return skip("cross-host redirect", "needs the server on 127.0.0.1");
    r = get(srv.base + "/to-localhost", {{"Authorization", "Bearer x"}});
    CHECK_STR(r.error, "");
    CHECK(startsWith(r.url, "http://localhost:"));
    CHECK(contains(r.body, "\"host\": \"localhost:"));
    CHECK_FALSE(contains(r.body, "authorization"));
}

TEST("redirect: limits and POST to GET") {
    NEED_SERVER();
    net::Request req;
    req.url          = srv.base + "/r1";
    req.maxRedirects = 0;
    net::Response r  = fetch(req);
    CHECK(r.status == 302);
    CHECK_STR(r.header("location"), "r2");
    req.url          = srv.base + "/loop";
    req.maxRedirects = 3;
    r                = fetch(req);
    CHECK(r.status == 0);
    CHECK_STR(r.error, "too_many_redirects");
    req        = net::Request();
    req.method = "POST";
    req.url    = srv.base + "/post-redirect";
    req.body   = "x=1";
    r          = fetch(req);
    CHECK(r.status == 200);
    CHECK_STR(r.url, srv.base + "/headers");
    CHECK_FALSE(contains(r.body, "content-length\": \"3"));
}

TEST("redirect: a 307 sends the POST body again") {
    NEED_SERVER();
    net::Request req;
    req.method = "POST";
    req.url    = srv.base + "/post-307";
    req.headers.push_back({"Content-Type", "text/plain"});
    req.body              = std::string(100000, 'b') + "end";
    const net::Response r = fetch(std::move(req));
    CHECK_STR(r.error, "");
    CHECK(r.status == 200);
    CHECK_STR(r.url, srv.base + "/echo");
    CHECK(contains(r.body, "\"method\": \"POST\""));
    CHECK(contains(r.body, "bbbend\""));
    CHECK(contains(r.body, "\"content-length\": \"100003\""));
}

// ── Failures ────────────────────────────────────────────────────────────────

TEST("error: bad url") {
    CHECK(startsWith(get("nonsense").error, "url"));
    CHECK(startsWith(get("ws://127.0.0.1/").error, "url"));
}

TEST("error: dns") {
    const net::Response r = get("http://nonexistent.invalid/");
    CHECK(r.status == 0);
    if (!CHECK(startsWith(r.error, "dns")))
        std::fprintf(stderr, "    error: %s\n", r.error.c_str());
}

TEST("error: connection refused") {
    const int port = net::freeLoopbackPort();
    REQUIRE(port > 0);
    const net::Response r = get("http://127.0.0.1:" + std::to_string(port) + "/");
    CHECK(r.status == 0);
    if (!CHECK(startsWith(r.error, "connect")))
        std::fprintf(stderr, "    error: %s\n", r.error.c_str());
}

TEST("error: timeout") {
    NEED_SERVER();
    net::Request req;
    req.url            = srv.base + "/slow?ms=3000";
    req.timeoutMs      = 300;
    const int64_t t    = msNow();
    net::Response r    = fetch(req);
    const int64_t took = msNow() - t;
    CHECK_STR(r.error, "timeout");
    CHECK(r.status == 0);
    CHECK(took < 1500);
}

TEST("error: cancel — no callback, prompt") {
    NEED_SERVER();
    net::Client c(app());
    bool        called = false;
    const auto  id =
        c.send({.url = srv.base + "/slow?ms=1500"}, [&](net::Response) { called = true; });
    CHECK(id != 0);
    pumpFor(100);
    c.cancel(id);
    c.cancel(id);       // twice: ignored
    c.cancel(id + 100); // unknown: ignored
    pumpFor(2000);
    CHECK_FALSE(called);
    // the cancelled worker is free again (it honoured the flag)
    bool second = false;
    c.send({.url = srv.base + "/plain"}, [&](net::Response r) {
        second = r.body == "hello world";
    });
    CHECK(pumpUntil([&] { return second; }));
}

// A request cancelled while it waits for a free worker is never sent — not
// even over a kept-alive connection, where the request would be written out
// before anything looked at its cancel flag.
TEST("error: cancel — a queued request is never sent") {
    NEED_SERVER();
    const int   before = std::atoi(get(srv.base + "/swallowed").body.c_str());
    net::Client c(app());
    int         slow = 0;
    for (int i = 0; i < 4; ++i) // every worker busy; their connections stay warm
        c.send({.url = srv.base + "/slow?ms=600"}, [&](net::Response) { ++slow; });
    bool       called = false;
    const auto id     = c.send(
        {.method = "POST", .url = srv.base + "/post-swallow", .body = "x"},
        [&](net::Response) { called = true; }
    );
    pumpFor(50);
    c.cancel(id);
    CHECK(pumpUntil([&] { return slow == 4; }));
    pumpFor(400); // a worker free again would have sent it by now
    CHECK_FALSE(called);
    CHECK(std::atoi(get(srv.base + "/swallowed").body.c_str()) == before);
    // The workers are free for what comes next.
    bool next = false;
    c.send({.url = srv.base + "/plain"}, [&](net::Response r) { next = r.status == 200; });
    CHECK(pumpUntil([&] { return next; }));
}

// The transport sleeps on the request's Cancel object, not on a 250 ms poll
// of its flag: set() ends a blocked exchange at once.
TEST("error: cancel wakes a blocked exchange at once") {
    NEED_SERVER();
    net::Url u;
    REQUIRE(u.parse(srv.base + "/slow?ms=3000"));
    for (int round = 0; round < 3; ++round) {
        net::detail::Cancel  cancel;
        net::Request         req;
        net::Response        resp;
        std::atomic<int64_t> returned{0};
        req.url = u.str();
        base::Thread t;
        REQUIRE(t.start([&] {
            net::detail::perform(u, req, resp, cancel, {});
            returned.store(msNow());
        }));
        pumpFor(150 + round * 70); // somewhere inside a would-be 250 ms slice
        const int64_t at = msNow();
        cancel.set();
        t.join();
        CHECK_STR(resp.error, "cancelled");
        if (!CHECK(returned.load() - at < 100))
            std::fprintf(stderr, "    took %lld ms\n", (long long)(returned.load() - at));
    }
}

#ifdef NET_TEST_POSIX
namespace {
int threadCount() {
    int  n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return -1;
    while (const dirent *e = readdir(d))
        n += e->d_name[0] != '.';
    closedir(d);
    return n;
}
} // namespace
#endif

TEST("client: idle workers end; the next request starts one") {
    NEED_SERVER();
    net::detail::setWorkerIdleMs(150);
    {
        net::Client c(app());
#ifdef NET_TEST_POSIX
        const int before = threadCount();
#endif
        for (int round = 0; round < 2; ++round) {
            int done = 0;
            for (int i = 0; i < 3; ++i)
                c.send({.url = srv.base + "/plain"}, [&](net::Response r) {
                    done += r.body == "hello world";
                });
            REQUIRE(pumpUntil([&] { return done == 3; }));
#ifdef NET_TEST_POSIX
            CHECK(threadCount() > before);
#endif
#ifdef NET_TEST_POSIX
            // Past the idle time every worker ends (a busy machine may take a while).
            CHECK(pumpUntil([&] { return threadCount() == before; }, 5000));
#else
            pumpFor(500); // past the idle time: every worker has ended
#endif
        }
    }
    net::detail::setWorkerIdleMs(30000);
}

TEST("error: Client destroyed with requests in flight") {
    NEED_SERVER();
    int     called = 0;
    int64_t took   = 0;
    {
        auto c = std::make_unique<net::Client>(app());
        for (int i = 0; i < 6; ++i)
            c->send({.url = srv.base + "/slow?ms=2000"}, [&](net::Response) { ++called; });
        c->send({.url = srv.base + "/plain"}, [&](net::Response) { ++called; });
        pumpFor(150);
        const int64_t t = msNow();
        c.reset();
        took = msNow() - t;
    }
    pumpFor(500);
    CHECK(called == 0 || called == 1); // /plain may have landed before the reset
    CHECK(took < 1000);
}

// ── TLS ─────────────────────────────────────────────────────────────────────

TEST("tls: local server, hostname checked") {
    NEED_SERVER();
    if (srv.tlsBase.empty())
        return skip("local TLS", srv.tlsWhy.empty() ? "no TLS server" : srv.tlsWhy);
    net::Response r = get(srv.tlsBase + "/plain");
    CHECK_STR(r.error, "");
    CHECK_STR(r.body, "hello world");
    r = get(srv.tlsBase + "/chunked");
    CHECK(r.body.size() == 70025);
    // keep-alive over TLS (one Client, so the pool is not dropped between the
    // two calls — the last Client's destructor closes idle connections).
    {
        net::Client c(app());
        std::string a, b;
        c.send({.url = srv.tlsBase + "/keepalive"}, [&](net::Response r) { a = r.body; });
        REQUIRE(pumpUntil([&] { return !a.empty(); }));
        c.send({.url = srv.tlsBase + "/keepalive"}, [&](net::Response r) { b = r.body; });
        REQUIRE(pumpUntil([&] { return !b.empty(); }));
        CHECK_STR(b, a);
    }
    // The certificate names "localhost" only.
    net::Url u;
    REQUIRE(u.parse(srv.tlsBase));
    r = get("https://127.0.0.1:" + std::to_string(u.port) + "/plain");
    CHECK(r.status == 0);
    CHECK_STR(r.error, "tls: hostname mismatch");
    // A server that is not TLS at all.
    r = get("https://127.0.0.1:" + std::to_string(srv.port) + "/plain");
    CHECK(startsWith(r.error, "tls"));
}

#ifdef NET_TEST_POSIX
// A host name is resolved once a minute, not per connection.
TEST("dns: a reconnect within the minute skips the lookup") {
    NEED_SERVER();
    const std::string url = "http://localhost:" + std::to_string(srv.port) + "/closehdr";
    CHECK(get(url).status == 200); // may itself come from the cache (an earlier test)
    const int64_t before = net::detail::dnsLookups();
    for (int i = 0; i < 3; ++i)
        CHECK(get(url).status == 200); // Connection: close → a new connection each time
    CHECK(net::detail::dnsLookups() == before);
    // A failed name is not cached.
    const int64_t failedBefore = net::detail::dnsLookups();
    CHECK(startsWith(get("http://nonexistent.invalid/").error, "dns"));
    CHECK(startsWith(get("http://nonexistent.invalid/").error, "dns"));
    CHECK(net::detail::dnsLookups() == failedBefore + 2);
}
#endif

#ifdef NET_TEST_OWN_TLS
// The second connection offers the first one's session (a TLS 1.3 ticket,
// a TLS 1.2 session id or ticket) and the server takes it: no certificate
// chain this time. The server says whether it resumed; so does our counter.
TEST("tls: a reconnect resumes the session (TLS 1.3 and 1.2)") {
    NEED_SERVER();
    if (srv.tlsBase.empty())
        return skip("local TLS", srv.tlsWhy.empty() ? "no TLS server" : srv.tlsWhy);
    for (const std::string &base : {srv.tlsBase, srv.tls12Base}) {
        if (base.empty())
            continue;
        const net::Response first = get(base + "/tls-session");
        CHECK_STR(first.error, "");
        const int64_t before = net::detail::tlsResumptions();
        for (int i = 0; i < 2; ++i) {
            const net::Response again = get(base + "/tls-session"); // Connection: close
            CHECK_STR(again.error, "");
            if (!CHECK_STR(again.body, "reused"))
                std::fprintf(stderr, "    %s\n", base.c_str());
        }
        CHECK(net::detail::tlsResumptions() == before + 2);
    }
    // A session is per host and port, and resuming leaves the hostname check
    // in place.
    net::Url u;
    REQUIRE(u.parse(srv.tlsBase));
    const net::Response r = get("https://127.0.0.1:" + std::to_string(u.port) + "/plain");
    CHECK_STR(r.error, "tls: hostname mismatch");
}

// At exit nothing is open: the name cache, the TLS sessions and settings go,
// and a later request simply starts over (a fresh lookup, a full handshake).
TEST("releaseCaches: frees what was cached, and requests still work after") {
    NEED_SERVER();
    const std::string url = "http://localhost:" + std::to_string(srv.port) + "/closehdr";
    CHECK(get(url).status == 200); // the name is cached now
    if (!srv.tlsBase.empty())
        CHECK_STR(get(srv.tlsBase + "/tls-session").error, ""); // and a session kept
    REQUIRE(net::detail::liveStreams() == 0);
    net::releaseCaches();
    const int64_t before = net::detail::dnsLookups();
    CHECK(get(url).status == 200);
    CHECK(net::detail::dnsLookups() == before + 1);
    if (srv.tlsBase.empty())
        return skip("local TLS", srv.tlsWhy.empty() ? "no TLS server" : srv.tlsWhy);
    const net::Response again = get(srv.tlsBase + "/tls-session");
    CHECK_STR(again.error, "");
    CHECK_STR(again.body, "new");
}
#endif

// ── WebSocket ───────────────────────────────────────────────────────────────

struct WsProbe {
    net::WebSocket           ws{app()};
    bool                     opened = false, closed = false;
    int                      code = -1;
    std::string              reason;
    std::vector<std::string> texts, binaries;

    WsProbe() {
        ws.onOpen   = [this] { opened = true; };
        ws.onText   = [this](std::string t) { texts.push_back(std::move(t)); };
        ws.onBinary = [this](std::string b) { binaries.push_back(std::move(b)); };
        ws.onClosed = [this](int c, std::string r) {
            closed = true;
            code   = c;
            reason = std::move(r);
        };
    }
    bool open(const std::string &url, std::vector<net::Header> h = {}) {
        ws.open(url, std::move(h));
        return pumpUntil([&] { return opened || closed; }) && opened;
    }
    bool waitTexts(size_t n) {
        return pumpUntil([&] { return texts.size() >= n || closed; }) && texts.size() >= n;
    }
};

std::string wsBase(const Server &s) {
    return "ws" + s.base.substr(4); // http://… → ws://…
}

TEST("ws: text and binary echo") {
    NEED_SERVER();
    WsProbe p;
    REQUIRE(p.open(wsBase(srv) + "/ws"));
    CHECK(p.ws.isOpen());
    p.ws.sendText("hello");
    p.ws.sendText(std::string(70000, 'm')); // 64-bit length
    p.ws.sendText(std::string(300, 'k'));   // 16-bit length
    p.ws.sendText("");
    REQUIRE(p.waitTexts(4));
    CHECK_STR(p.texts[0], "hello");
    CHECK(p.texts[1] == std::string(70000, 'm'));
    CHECK(p.texts[2] == std::string(300, 'k'));
    CHECK_STR(p.texts[3], "");
    CHECK_FALSE(p.closed);
}

TEST("ws: upgrade carries the caller's headers") {
    NEED_SERVER();
    WsProbe p;
    REQUIRE(p.open(wsBase(srv) + "/ws", {{"Origin", "http://example"}, {"Cookie", "d=xoxd"}}));
    p.ws.sendText("headers");
    REQUIRE(p.waitTexts(1));
    CHECK(contains(p.texts[0], "\"origin\": \"http://example\""));
    CHECK(contains(p.texts[0], "\"cookie\": \"d=xoxd\""));
}

TEST("ws: server ping, fragmented message") {
    NEED_SERVER();
    WsProbe p;
    REQUIRE(p.open(wsBase(srv) + "/ws"));
    p.ws.sendText("ping-me");
    REQUIRE(p.waitTexts(1));
    CHECK_STR(p.texts[0], "pong ok");
    p.ws.sendText("fragment-me");
    REQUIRE(p.waitTexts(2));
    CHECK_STR(p.texts[1], "frag1frag2frag3");
}

TEST("ws: client pings go out in order with the messages; pongs stay silent") {
    NEED_SERVER();
    WsProbe p;
    CHECK_FALSE(p.ws.ping("not open"));
    REQUIRE(p.open(wsBase(srv) + "/ws"));
#ifdef _WIN32
    // WinHTTP has no call for a ping (it sends its own keepalives).
    CHECK_FALSE(p.ws.ping("keep"));
    p.ws.sendText("pings");
    REQUIRE(p.waitTexts(1));
    CHECK_STR(p.texts[0], "[]");
    return;
#endif
    REQUIRE(p.ws.ping("keep"));
    p.ws.sendText("between");
    REQUIRE(p.ws.ping());
    p.ws.sendText("pings");
    REQUIRE(p.waitTexts(2));
    CHECK_STR(p.texts[0], "between"); // the pongs never surface as messages
#ifdef __APPLE__
    // NSURLSession picks the payloads itself: two pings, whatever they carry.
    CHECK(std::count(p.texts[1].begin(), p.texts[1].end(), ',') == 1);
#else
    CHECK_STR(p.texts[1], "[\"keep\", \"\"]");
#endif
    CHECK(p.ws.isOpen());
}

TEST("ws: server closes with a code") {
    NEED_SERVER();
    WsProbe p;
    REQUIRE(p.open(wsBase(srv) + "/ws"));
    p.ws.sendText("close-me");
    REQUIRE(pumpUntil([&] { return p.closed; }));
    CHECK(p.code == 4001);
    CHECK_STR(p.reason, "bye");
    CHECK_FALSE(p.ws.isOpen());
    p.ws.sendText("ignored once closed");
}

TEST("ws: client close handshake") {
    NEED_SERVER();
    WsProbe p;
    REQUIRE(p.open(wsBase(srv) + "/ws"));
    p.ws.close(1000);
    const int64_t t = msNow();
    REQUIRE(pumpUntil([&] { return p.closed; }));
    CHECK(p.code == 1000);
    CHECK(msNow() - t < 2000);
}

TEST("ws: reopen after close") {
    NEED_SERVER();
    WsProbe p;
    REQUIRE(p.open(wsBase(srv) + "/ws"));
    p.ws.close();
    REQUIRE(pumpUntil([&] { return p.closed; }));
    p.opened = p.closed = false;
    REQUIRE(p.open(wsBase(srv) + "/ws"));
    p.ws.sendText("again");
    REQUIRE(p.waitTexts(1));
    CHECK_STR(p.texts[0], "again");
}

TEST("ws: destroyed while open or connecting — prompt, no callback") {
    NEED_SERVER();
    bool    any = false;
    int64_t took;
    {
        auto ws      = std::make_unique<net::WebSocket>(app());
        ws->onClosed = [&](int, std::string) { any = true; };
        ws->onText   = [&](std::string) { any = true; };
        ws->open(wsBase(srv) + "/ws");
        REQUIRE(pumpUntil([&] { return ws->isOpen(); }));
        const int64_t t = msNow();
        ws.reset();
        took = msNow() - t;
    }
    CHECK(took < 1000);
    {
        // still connecting (the handshake answer is slow to come: /slow)
        auto ws      = std::make_unique<net::WebSocket>(app());
        ws->onClosed = [&](int, std::string) { any = true; };
        ws->open(wsBase(srv) + "/slow?ms=3000");
        pumpFor(100);
        const int64_t t = msNow();
        ws.reset();
        took = msNow() - t;
    }
    pumpFor(300);
    CHECK(took < 1000);
    CHECK_FALSE(any);
}

TEST("ws: failures before open") {
    NEED_SERVER();
    {
        WsProbe p;
        CHECK_FALSE(p.open(wsBase(srv) + "/ws-reject"));
        CHECK(p.code == 0);
        CHECK_STR(p.reason, "handshake: status 403");
    }
    {
        WsProbe p;
        CHECK_FALSE(p.open("ws://127.0.0.1:" + std::to_string(net::freeLoopbackPort()) + "/"));
        CHECK(p.code == 0);
        CHECK(startsWith(p.reason, "connect"));
    }
    {
        WsProbe p;
        CHECK_FALSE(p.open("http://127.0.0.1/"));
        CHECK_STR(p.reason, "url");
    }
    {
        WsProbe p;
        p.ws.open(wsBase(srv) + "/slow?ms=3000", {}, 300);
        REQUIRE(pumpUntil([&] { return p.closed; }));
        CHECK_STR(p.reason, "timeout");
    }
}

TEST("ws: wss to the local TLS server") {
    NEED_SERVER();
    if (srv.tlsBase.empty())
        return skip("local wss", srv.tlsWhy.empty() ? "no TLS server" : srv.tlsWhy);
    WsProbe p;
    REQUIRE(p.open("wss" + srv.tlsBase.substr(5) + "/ws"));
    p.ws.sendText("over tls");
    p.ws.sendText(std::string(100000, 't')); // several TLS records each way
    p.ws.sendText("ping-me");
    REQUIRE(p.waitTexts(3));
    CHECK_STR(p.texts[0], "over tls");
    CHECK(p.texts[1] == std::string(100000, 't'));
    CHECK_STR(p.texts[2], "pong ok");
    p.ws.close(1000);
    REQUIRE(pumpUntil([&] { return p.closed; }));
    CHECK(p.code == 1000);
}

// ── Live (MSGA_NET_LIVE=1) ──────────────────────────────────────────────────

TEST("live: https slack.com api.test") {
    if (!live())
        return skip("live", "MSGA_NET_LIVE is not 1");
    const net::Response r = get("https://slack.com/api/api.test");
    CHECK_STR(r.error, "");
    CHECK(r.status == 200);
    CHECK(contains(r.body, "\"ok\":true"));
}

#ifdef NET_TEST_POSIX
// Real hosts: the next connection (the last Client's keep-alive pool is gone
// with it) resumes the TLS session. A server farm may now and then decline
// a ticket another machine issued, so one resumption in three is the bar.
TEST("live: a reconnect resumes the TLS session") {
    if (!live())
        return skip("live", "MSGA_NET_LIVE is not 1");
    for (const char *url : {"https://slack.com/api/api.test", "https://www.cloudflare.com/"}) {
        int64_t t0 = msNow();
        CHECK_STR(get(url).error, "");
        std::fprintf(stderr, "    %s: first %lld ms", url, (long long)(msNow() - t0));
        bool resumed = false;
        for (int i = 0; i < 3 && !resumed; ++i) {
            const int64_t before  = net::detail::tlsResumptions();
            t0                    = msNow();
            const net::Response r = get(url);
            resumed               = net::detail::tlsResumptions() == before + 1;
            CHECK_STR(r.error, "");
            std::fprintf(
                stderr,
                ", again %lld ms (%s)",
                (long long)(msNow() - t0),
                resumed ? "resumed" : "full handshake"
            );
        }
        std::fprintf(stderr, "\n");
        CHECK(resumed);
    }
}
#endif

TEST("live: TLS variants and refusals (badssl.com)") {
    if (!live())
        return skip("live", "MSGA_NET_LIVE is not 1");
    for (const char *ok :
         {"https://ecc256.badssl.com/",
          "https://ecc384.badssl.com/",
          "https://rsa2048.badssl.com/",
          "https://rsa4096.badssl.com/",
          "https://sha256.badssl.com/",
          "https://tls-v1-2.badssl.com:1012/",
          "https://www.google.com/",
          "https://www.cloudflare.com/",
          "https://github.com/",
          "https://www.wikipedia.org/",
          "https://letsencrypt.org/",
          "https://api.slack.com/"}) {
        const net::Response r = get(ok);
        if (!CHECK(r.error.empty() && r.status > 0))
            std::fprintf(stderr, "    %s: %s\n", ok, r.error.c_str());
    }
    const char *const bad[][2] = {
        {"https://wrong.host.badssl.com/", "tls: hostname mismatch"},
        {"https://expired.badssl.com/", "tls: certificate expired"},
        {"https://self-signed.badssl.com/", "tls: untrusted certificate"},
        {"https://untrusted-root.badssl.com/", "tls: untrusted certificate"},
        {"https://tls-v1-0.badssl.com:1010/", "tls"},
        {"https://rc4.badssl.com/", "tls"},
#ifdef NET_TEST_POSIX
        {"https://dh2048.badssl.com/", "tls"}, // we build no DHE; an OS stack may accept it
#endif
    };
    for (const auto &b : bad) {
        const net::Response r = get(b[0]);
#ifdef NET_TEST_POSIX
        const std::string_view want = b[1];
#else
        const std::string_view want = "tls"; // WinHTTP says only "secure channel error"
#endif
        if (!CHECK(r.status == 0 && startsWith(r.error, want)))
            std::fprintf(stderr, "    %s: \"%s\"\n", b[0], r.error.c_str());
    }
}

TEST("live: wss echo") {
    if (!live())
        return skip("live", "MSGA_NET_LIVE is not 1");
    WsProbe p;
    REQUIRE(p.open("wss://ws.postman-echo.com/raw"));
    p.ws.sendText("msga live echo");
    REQUIRE(pumpUntil([&] {
        for (const auto &t : p.texts)
            if (t == "msga live echo")
                return true;
        return p.closed;
    }));
    CHECK_FALSE(p.closed);
    p.ws.close(1000);
    REQUIRE(pumpUntil([&] { return p.closed; }));
    CHECK(p.code == 1000);
}

// NET_PROBE_URL=<url>: fetch it and print the outcome (for poking at a
// server by hand: `msga_net_tests probe`).
TEST("probe: NET_PROBE_URL") {
    const char *url = std::getenv("NET_PROBE_URL");
    if (!url || !*url)
        return;
    const net::Response r = get(url);
    std::fprintf(
        stderr,
        "  %s → status %d, error \"%s\", %zu bytes\n",
        url,
        r.status,
        r.error.c_str(),
        r.body.size()
    );
#ifndef _WIN32
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    std::fprintf(stderr, "  peak RSS %ld KB\n", ru.ru_maxrss);
#endif
}

} // namespace
