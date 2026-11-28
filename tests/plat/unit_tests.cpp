// Backend-independent tests: the loop core (timers, posting) and the event
// contract, driven through the headless backend.
#include "core/hash.h"
#include "core/loop_core.h"
#include "core/image_util.h"
#include "core/input.h"
#include "core/pacing.h"
#include "core/strings.h"
#include "core/transfer.h"
#include "core/wire.h"
#include "plat/plat.h"
#include "plat/testing.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace plat;
using plat_test::runCase;

namespace plat::testing_internal {
void    setHeadlessScale(Window &w, double s);
int     headlessFrames(Window &w);
Cursor  headlessCursor(Window &w);
HitArea headlessLastHit(Window &w);
} // namespace plat::testing_internal
namespace ti = plat::testing_internal;

namespace {

std::unique_ptr<App> headless() {
#if defined(_WIN32)
    _putenv_s("PLAT_BACKEND", "headless");
#else
    setenv("PLAT_BACKEND", "headless", 1);
#endif
    return App::create();
}

// Pump until pred() or the deadline; returns pred().
bool pumpUntil(App &app, const std::function<bool()> &pred, int ms = 1000) {
    const auto end = core::Clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && core::Clock::now() < end)
        app.pump(10);
    return pred();
}

void testTimersOrderAndCancel() {
    core::LoopCore   core;
    std::vector<int> order;
    core.addTimer(20, false, [&] { order.push_back(2); });
    core.addTimer(5, false, [&] { order.push_back(1); });
    const TimerId cancelled = core.addTimer(10, false, [&] { order.push_back(99); });
    core.cancelTimer(cancelled);
    CHECK(core.msUntilNextTimer() >= 0);
    CHECK(core.msUntilNextTimer() <= 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(core.msUntilNextTimer() == 0);
    core.runDueTimers();
    CHECK((order == std::vector<int>{1, 2}));
    CHECK(core.msUntilNextTimer() == -1);
}

void testRepeatingTimerRearmsAndSelfCancels() {
    core::LoopCore core;
    int            fired = 0;
    TimerId        id    = 0;
    id                   = core.addTimer(1, true, [&] {
        if (++fired == 3)
            core.cancelTimer(id); // cancelling yourself mid-callback must be safe
    });
    for (int i = 0; i < 20 && fired < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        core.runDueTimers();
    }
    CHECK(fired == 3);
    CHECK(core.msUntilNextTimer() == -1);
}

void testTimersAddedByCallbacksWaitAndTiesKeepOrder() {
    core::LoopCore   core;
    std::vector<int> order;
    // Same interval: due together, run in creation order.
    core.addTimer(0, false, [&] {
        order.push_back(1);
        core.addTimer(0, false, [&] { order.push_back(3); }); // next round
    });
    core.addTimer(0, false, [&] { order.push_back(2); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    core.runDueTimers();
    CHECK((order == std::vector<int>{1, 2}));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    core.runDueTimers();
    CHECK((order == std::vector<int>{1, 2, 3}));
    // Cancelling the earliest moves the wait out to the next one.
    const TimerId early = core.addTimer(5, false, [] {});
    core.addTimer(1000, false, [] {});
    CHECK(core.msUntilNextTimer() <= 5);
    core.cancelTimer(early);
    CHECK(core.msUntilNextTimer() > 900);
}

void testClampTimeout() {
    core::LoopCore core;
    CHECK(core.clampTimeout(-1) == -1);
    CHECK(core.clampTimeout(50) == 50);
    core.addTimer(1000, false, [] {});
    CHECK(core.clampTimeout(-1) > 900);
    CHECK(core.clampTimeout(10) == 10);
}

// A closure whose captures, when destroyed, post `depth` more like it — from
// a thread joined meanwhile when `viaThread`, like a retired net::WebSocket
// whose reader posts its close while the destructor joins it.
struct OnDestroy {
    std::function<void()> fn;
    ~OnDestroy() { fn(); }
};
std::function<void()>
reposting(core::LoopCore &c, int depth, bool viaThread, int &destroyed, bool &ran) {
    auto token = std::make_shared<OnDestroy>();
    token->fn  = [&c, depth, viaThread, &destroyed, &ran] {
        ++destroyed;
        if (depth == 0)
            return;
        auto next = reposting(c, depth - 1, viaThread, destroyed, ran);
        if (viaThread)
            std::thread([&] { c.post(std::move(next)); }).join();
        else
            c.post(std::move(next));
    };
    return [token, &ran] { ran = true; };
}

void testShutdownDrainsClosuresThatPost() {
    int  destroyed = 0;
    bool ran       = false;
    {
        core::LoopCore c;
        c.post(reposting(c, 3, false, destroyed, ran));
        c.post(reposting(c, 3, true, destroyed, ran));
        c.addTimer(1000, false, reposting(c, 3, true, destroyed, ran));
        c.shutdown();
        CHECK(destroyed == 12);
        CHECK(!c.hasPosted());
        CHECK(c.msUntilNextTimer() == -1);
        // Closed: a late post is dropped at once.
        c.post(reposting(c, 0, false, destroyed, ran));
        CHECK(destroyed == 13);
        CHECK(!c.hasPosted());
        // The destructor alone drains too.
        core::LoopCore d;
        d.post(reposting(d, 2, true, destroyed, ran));
    }
    CHECK(destroyed == 16);
    CHECK(!ran);
}

void testPostFromThreadsWakesLoop() {
    auto                     app = headless();
    std::atomic<int>         ran{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i)
                app->post([&] { ++ran; });
        });
    for (auto &t : threads)
        t.join();
    // A blocking pump(-1) must return once posted work exists.
    app->pump(-1);
    CHECK(ran == 400);
}

void testRunQuitFromTimer() {
    auto app   = headless();
    int  ticks = 0;
    app->addTimer(1, true, [&] {
        if (++ticks == 5)
            app->quit();
    });
    app->run();
    CHECK(ticks == 5);
}

void testPostedWorkDoesNotStarve() {
    // A closure that re-posts itself runs once per iteration, not forever.
    auto                  app   = headless();
    int                   count = 0;
    std::function<void()> again = [&] {
        ++count;
        app->post(again);
    };
    app->post(again);
    app->pump(0);
    CHECK(count == 1);
    app->pump(0);
    CHECK(count == 2);
}

void testFrameContract() {
    auto app    = headless();
    int  frames = 0;
    app->setEventHandler([&](const Event &e) {
        if (e.type == EventType::Frame) {
            ++frames;
            Canvas c = e.window->beginPaint();
            for (int y = 0; y < c.height; ++y)
                for (int x = 0; x < c.width; ++x)
                    c.pixels[size_t(y) * c.stride + x] = x < c.width / 2 ? 0xffff0000 : 0xff0000ff;
            e.window->endPaint({});
        }
    });
    auto win = app->createWindow({.size = {100, 50}});
    CHECK(pumpUntil(*app, [&] { return frames == 1; }));
    // No request → no frame.
    app->pump(20);
    CHECK(frames == 1);
    // Several requests coalesce into one Frame per iteration.
    win->requestFrame();
    win->requestFrame();
    app->pump(0);
    CHECK(frames == 2);

    uint32_t px = 0;
    CHECK(app->testHooks()->readPixel(*win, 10, 10, &px));
    CHECK(px == 0xffff0000);
    CHECK(app->testHooks()->readPixel(*win, 90, 10, &px));
    CHECK(px == 0xff0000ff);
}

void testFractionalScaleCanvas() {
    auto   app = headless();
    Canvas last;
    app->setEventHandler([&](const Event &e) {
        if (e.type == EventType::Frame) {
            last = e.window->beginPaint();
            e.window->endPaint({});
        }
    });
    auto win = app->createWindow({.size = {101, 33}});
    ti::setHeadlessScale(*win, 1.25);
    CHECK(pumpUntil(*app, [&] { return last.width != 0; }));
    CHECK(last.scale == 1.25);
    CHECK(last.width == 126); // round(101 * 1.25)
    CHECK(last.height == 41); // round(33 * 1.25)
    CHECK(win->size().w == 101);
}

void testKeyTextAndShortcuts() {
    auto               app = headless();
    std::vector<Event> ev;
    app->setEventHandler([&](const Event &e) { ev.push_back(e); });
    auto win = app->createWindow({});
    win->setTextInput({.enabled = true});
    auto *h = app->testHooks();

    h->injectKey(*win, Key::A, true);
    h->injectKey(*win, Key::A, true); // auto-repeat
    h->injectKey(*win, Key::A, false);
    h->injectKey(*win, Key::ControlLeft, true);
    h->injectKey(*win, Key::C, true);
    h->injectKey(*win, Key::C, false);
    h->injectKey(*win, Key::ControlLeft, false);

    std::string text;
    int         downs = 0, repeats = 0;
    bool        ctrlC = false;
    for (auto &e : ev) {
        if (e.type == EventType::TextInput)
            text += e.text;
        if (e.type == EventType::KeyDown) {
            ++downs;
            repeats += e.repeat;
            if (e.key == Key::C && (e.mods & ModCtrl))
                ctrlC = true;
        }
    }
    CHECK(text == "aa"); // the chord types nothing
    CHECK(downs == 4);
    CHECK(repeats == 1);
    CHECK(ctrlC);
    CHECK(ev.front().type == EventType::FocusIn);
}

void testPointerClicksAndHitTest() {
    auto               app = headless();
    std::vector<Event> ev;
    app->setEventHandler([&](const Event &e) { ev.push_back(e); });
    auto win = app->createWindow({.decorations = Decorations::Custom});
    win->setHitTest([](Point p) { return p.y < 30 ? HitArea::Caption : HitArea::Client; });
    auto *h = app->testHooks();

    h->injectPointerMove(*win, {50, 100});
    h->injectButton(*win, Button::Left, true);
    h->injectButton(*win, Button::Left, false);
    h->injectButton(*win, Button::Left, true);
    h->injectButton(*win, Button::Left, false);

    std::vector<int> clicks;
    for (auto &e : ev)
        if (e.type == EventType::PointerDown)
            clicks.push_back(e.clicks);
    CHECK((clicks == std::vector<int>{1, 2}));

    // A press on the caption turns into an OS move and never reaches the app.
    ev.clear();
    h->injectPointerMove(*win, {50, 10});
    h->injectButton(*win, Button::Left, true);
    bool sawDown = false;
    for (auto &e : ev)
        sawDown |= e.type == EventType::PointerDown;
    CHECK(!sawDown);
    CHECK(ti::headlessLastHit(*win) == HitArea::Caption);
}

void testClipboardIsAsyncAndTyped() {
    auto app = headless();
    app->setClipboardText("héllo ✓");
    std::optional<std::string> got;
    bool                       called = false;
    app->requestClipboard("text/plain;charset=utf-8", [&](auto v) {
        called = true;
        got    = v;
    });
    CHECK(!called); // never re-entrant
    CHECK(pumpUntil(*app, [&] { return called; }));
    CHECK(got && *got == "héllo ✓");

    called = false;
    app->requestClipboard("image/png", [&](auto v) {
        called = true;
        got    = v;
    });
    CHECK(pumpUntil(*app, [&] { return called; }));
    CHECK(!got);
}

void testKeyNames() {
    CHECK(std::string(keyName(Key::A)) == "A");
    CHECK(std::string(keyName(Key::KpEqual)) == "KpEqual");
    CHECK(std::string(keyName(Key::Count)) == "Unknown");
}

void testTextMimes() {
    using core::isTextMime;
    for (const char *m :
         {"text/plain",
          "text/plain;charset=utf-8",
          "text/plain;charset=UTF-8",
          "text/plain; charset=utf-8",
          "TEXT/PLAIN;CHARSET=Utf-8",
          "text/plain;charset=utf8",
          "text/plain;charset=\"utf-8\"",
          "UTF8_STRING",
          "STRING",
          "TEXT",
          core::kTextMime})
        CHECK(isTextMime(m));
    // Another charset is not text: nothing transcodes it.
    for (const char *m :
         {"text/plain;charset=iso-8859-1",
          "text/plain;charset=utf-16",
          "text/plain;format=flowed",
          "text/plainx",
          "text/html",
          "text/uri-list",
          "utf8_string",
          "string",
          "",
          "text/plain;charset=",
          "COMPOUND_TEXT"})
        CHECK(!isTextMime(m));
}

void testUriList() {
    using V = std::vector<std::string>;
    CHECK(core::parseUriList("") == V{});
    CHECK(core::parseUriList("file:///a\r\nfile:///b\r\n") == (V{"file:///a", "file:///b"}));
    CHECK(core::parseUriList("file:///a\nfile:///b") == (V{"file:///a", "file:///b"}));
    // Comments, blank lines, GTK's trailing NUL and stray trailing spaces.
    static const char kList[] = "# c\r\n\r\nfile:///a  \r\nfile:///b\r\n\0";
    CHECK(
        core::parseUriList(std::string_view(kList, sizeof kList - 1)) ==
        (V{"file:///a", "file:///b"})
    );
    CHECK(core::parseUriList("#only a comment\n") == V{});
}

void testPercentAndFileUris() {
    CHECK(core::percentEncode("a b/ü~-._") == "a%20b%2F%C3%BC~-._");
    CHECK(core::percentEncode("a b/c:d", "/:") == "a%20b/c:d");
    CHECK(core::percentDecode("a%20b%2f%C3%BC") == "a b/\xC3\xBC");
    // Malformed escapes stay literal.
    CHECK(core::percentDecode("100%") == "100%");
    CHECK(core::percentDecode("%2") == "%2");
    CHECK(core::percentDecode("%zz%4") == "%zz%4");
    CHECK(core::percentDecode("%41") == "A");
    CHECK(core::percentDecode("a+b") == "a+b"); // '+' is not a space here
    CHECK(core::fileUri("/tmp/a b/ü#1.txt") == "file:///tmp/a%20b/%C3%BC%231.txt");
    CHECK(core::pathFromFileUri("file:///tmp/a%20b/%C3%BC%231.txt") == "/tmp/a b/\xC3\xBC#1.txt");
    CHECK(core::pathFromFileUri("file://localhost/tmp/x") == "/tmp/x");
    CHECK(core::pathFromFileUri("file://otherhost/tmp/x").empty());
    CHECK(core::pathFromFileUri("http:///tmp/x").empty());
    CHECK(core::pathFromFileUri("file:///tmp/a%00b").empty());
    CHECK(core::pathFromFileUri("file://").empty());
    // Round trip of every byte but NUL.
    std::string all = "/";
    for (int c = 1; c < 256; ++c)
        all += char(c);
    CHECK(core::pathFromFileUri(core::fileUri(all)) == all);
}

void testClickCounter() {
    core::ClickCounter c;
    CHECK(c.press(1, 10, 10, 1000, 400, 4, 4) == 1);
    CHECK(c.press(1, 12, 13, 1300, 400, 4, 4) == 2);
    CHECK(c.press(1, 12, 13, 1700, 400, 4, 4) == 3); // 400 ms is still in
    CHECK(c.press(1, 12, 13, 2101, 400, 4, 4) == 1); // too late
    CHECK(c.press(3, 12, 13, 2200, 400, 4, 4) == 1); // another button
    CHECK(c.press(3, 17, 13, 2300, 400, 4, 4) == 1); // too far
    CHECK(c.clicks() == 1);
    c.reset();
    CHECK(c.press(3, 17, 13, 2350, 400, 4, 4) == 1); // reset: a fresh run
    // A 32-bit millisecond clock that wraps between the presses.
    core::ClickCounter w;
    CHECK(w.press(1, 0, 0, 0xFFFFFF00u, 400, 4, 4) == 1);
    CHECK(w.press(1, 0, 0, 0x00000010u, 400, 4, 4) == 2);
}

void testKeyFromAscii() {
    CHECK(core::keyFromAscii('a') == Key::A && core::keyFromAscii('Z') == Key::Z);
    CHECK(core::keyFromAscii('0') == Key::Num0 && core::keyFromAscii('9') == Key::Num9);
    const struct {
        char c;
        Key  k;
    } punct[] = {
        {'-', Key::Minus},
        {'=', Key::Equal},
        {'[', Key::BracketLeft},
        {']', Key::BracketRight},
        {'\\', Key::Backslash},
        {';', Key::Semicolon},
        {'\'', Key::Apostrophe},
        {'`', Key::Grave},
        {',', Key::Comma},
        {'.', Key::Period},
        {'/', Key::Slash}
    };
    for (const auto &p : punct) {
        CHECK(core::keyFromAscii(uint32_t(p.c)) == p.k);
        CHECK(core::keyFromPunctuation(uint32_t(p.c)) == p.k);
    }
    // Punctuation only: letters, digits, space and non-ASCII have no Key there.
    CHECK(core::keyFromPunctuation('a') == Key::Unknown);
    CHECK(core::keyFromPunctuation('1') == Key::Unknown);
    CHECK(core::keyFromAscii(' ') == Key::Unknown);
    CHECK(core::keyFromAscii(0xF6) == Key::Unknown); // ö
}

void testImageUtil() {
    CHECK(core::unpremultiply(0) == 0);
    CHECK(core::unpremultiply(0x00ffffffu) == 0); // alpha 0: nothing left
    CHECK(core::unpremultiply(0xff123456u) == 0xff123456u);
    CHECK(core::unpremultiply(0x80404040u) == 0x80808080u);
    CHECK(core::unpremultiply(0x80ff0000u) == 0x80ff0000u); // clamped at 255
    // Shrinking averages areas: a 2×2 checker to 1×1 is the mean.
    Image chk{2, 2, {0xffffffffu, 0xff000000u, 0xff000000u, 0xffffffffu}};
    Image one = core::scaleImage(chk, 1, 1);
    CHECK(one.width == 1 && one.pixels.size() == 1);
    CHECK(one.pixels[0] == 0xff808080u);
    // 3 → 2 columns: fractional coverage (1.5 source pixels each).
    Image row{3, 1, {0xff000000u, 0xff000000u, 0xffffffffu}};
    Image two = core::scaleImage(row, 2, 1);
    CHECK(two.pixels[0] == 0xff000000u);
    CHECK(two.pixels[1] == 0xffaaaaaau); // (0.5·0 + 1·255) / 1.5
    // Growing is bilinear: the ends keep their colour, the middle blends.
    Image grow = core::scaleImage(Image{2, 1, {0xff000000u, 0xffffffffu}}, 4, 1);
    CHECK(grow.pixels[0] == 0xff000000u && grow.pixels[3] == 0xffffffffu);
    CHECK((grow.pixels[1] & 0xff) > 0 && (grow.pixels[1] & 0xff) < 0x80);
    // Same size is a copy; empty input gives transparent pixels.
    CHECK(core::scaleImage(chk, 2, 2).pixels == chk.pixels);
    Image none = core::scaleImage(Image{}, 3, 2);
    CHECK(none.width == 3 && none.pixels.size() == 6 && none.pixels[5] == 0);
}

void testFrameInterval() {
    CHECK(core::frameIntervalMs(0) == 16); // unknown: 60 Hz
    CHECK(core::frameIntervalMs(60000) == 16);
    CHECK(core::frameIntervalMs(59940) == 16); // rounded down: never slower than the display
    CHECK(core::frameIntervalMs(75000) == 13);
    CHECK(core::frameIntervalMs(144000) == 6);
    CHECK(core::frameIntervalMs(1000000) == 2); // clamped
    CHECK(core::frameIntervalMs(5000) == 50);
}

void testPostWakesOncePerBatch() {
    // Only the post into an empty queue wakes; runPosted() takes the batch,
    // and the next post wakes again.
    core::LoopCore core;
    int            wakes = 0, ran = 0;
    core.wake = [&] { ++wakes; };
    core.post([&] { ++ran; });
    core.post([&] { ++ran; });
    core.post([&] { ++ran; });
    CHECK(wakes == 1);
    core.runPosted();
    CHECK(ran == 3);
    // A closure that posts from inside the batch wakes for the next one.
    core.post([&] { core.post([&] { ++ran; }); });
    CHECK(wakes == 2);
    core.runPosted();
    CHECK(wakes == 3);
    CHECK(core.hasPosted());
    core.runPosted();
    CHECK(ran == 4);
    core.wake = nullptr;
}

void testStrings() {
    CHECK(core::asciiLower("MsGa-Ö+1") == "msga-Ö+1");
    CHECK(core::asciiLower('Z') == 'z' && core::asciiLower('@') == '@');
    CHECK(core::trim("  a b\t") == "a b");
    CHECK(core::trim(" \t ").empty());
    CHECK(core::trim("\r x\r", " \r") == "x");
    CHECK(core::escapeMarkup("a<b>&\"c\"") == "a&lt;b&gt;&amp;\"c\"");
    CHECK(core::escapeMarkup("\"", true) == "&quot;");

    // The instance socket and pipe names hash with this exact basis; both
    // overloads agree.
    CHECK(core::fnv1a("") == 1469598103934665603ull);
    CHECK(core::fnv1a("a") == 0x44bd8ad473cd9906ull);
    CHECK(core::fnv1a("foobar", 6) == core::fnv1a(std::string_view("foobar")));

    CHECK(core::validScheme("msga") && core::validScheme("x-plat+1.2") && core::validScheme("c"));
    CHECK(!core::validScheme("") && !core::validScheme("1abc") && !core::validScheme("a b"));
    CHECK(!core::validScheme("ü"));

    std::vector<std::string> schemes;
    core::rememberScheme(schemes, "MSGA");
    core::rememberScheme(schemes, "msga");
    CHECK((schemes == std::vector<std::string>{"msga"}));
    CHECK(core::isSchemeUrl("MSGA://x", schemes) && core::isSchemeUrl("msga:", schemes));
    CHECK(!core::isSchemeUrl(":msga", schemes) && !core::isSchemeUrl("msga", schemes));
    CHECK(!core::isSchemeUrl("other://msga:", schemes));
    const std::vector<std::string> args = {"--flag", "Msga://open?x", "other://y", "msga:z"};
    CHECK((core::schemeUrls(args, schemes) == std::vector<std::string>{"Msga://open?x", "msga:z"}));
}

void testWire() {
    std::string out;
    core::putU32(out, 0x04030201u);
    CHECK(out == std::string("\x01\x02\x03\x04", 4));
    CHECK(core::getU32(out.data()) == 0x04030201u);
    out.clear();
    core::putString(out, "ab");
    core::putString(out, "");
    CHECK(out == std::string("\x02\0\0\0ab\0\0\0\0", 10));
    std::string_view in = out;
    std::string      s  = "x";
    CHECK(core::takeString(in, &s) && s == "ab");
    CHECK(core::takeString(in, &s) && s.empty() && in.empty());
    CHECK(!core::takeString(in, &s));
    // Truncated: the length promises more than is there; `in` is untouched.
    std::string_view cut = std::string_view(out).substr(0, 5);
    CHECK(!core::takeString(cut, &s) && cut.size() == 5);
    const std::string huge("\xff\xff\xff\xff", 4);
    std::string_view  h = huge;
    CHECK(!core::takeString(h, &s));
}

void testPreferredAction() {
    CHECK(core::preferredAction(ActCopy | ActMove | ActLink) == DropAction::Copy);
    CHECK(core::preferredAction(ActMove | ActLink) == DropAction::Move);
    CHECK(core::preferredAction(ActLink) == DropAction::Link);
    CHECK(core::preferredAction(0) == DropAction::None);
}

void testPickImage() {
    auto img = [](int size, double scale) {
        Image i{size, size, std::vector<uint32_t>(size_t(size) * size, 0xff000000u)};
        i.scale = scale;
        return i;
    };
    const std::vector<Image> set = {img(16, 1), img(32, 2), img(32, 1), img(64, 1)};
    // The smallest one big enough, else the largest.
    CHECK(core::pickImage(set, 20, 0, false) == &set[1]);
    CHECK(core::pickImage(set, 100, 0, false) == &set[3]);
    // Equal sizes: the closer scale wins.
    CHECK(core::pickImage(set, 32, 1.0, false) == &set[2]);
    CHECK(core::pickImage(set, 32, 2.0, false) == &set[1]);
    // onlyAtScale: images drawn for the scale first, whatever their size.
    CHECK(core::pickImage(set, 64, 2.0, true) == &set[1]);
    CHECK(core::pickImage(set, 64, 3.0, true) == &set[3]); // none at 3: all count
    // Empty and malformed ones are skipped.
    Image bad{8, 8, {}};
    CHECK(core::pickImage({bad}, 8, 1, false) == nullptr);
    CHECK(core::pickImage({}, 8, 1, false) == nullptr);
}

void testMonitorEquality() {
    Monitor a{.id = 1, .name = "DP-1", .bounds = {0, 0, 10, 10}, .workArea = {0, 0, 10, 9}};
    Monitor b = a;
    CHECK(a == b);
    b.workArea.h = 10;
    CHECK(!(a == b));
    b      = a;
    b.name = "DP-2";
    CHECK(!(a == b));
    CHECK((Rect{1, 2, 3, 4} == Rect{1, 2, 3, 4}) && !(Rect{1, 2, 3, 4} == Rect{1, 2, 3, 5}));
}

} // namespace

int main() {
    runCase("timers fire in due order, cancel works", testTimersOrderAndCancel);
    runCase(
        "repeating timer re-arms and can cancel itself", testRepeatingTimerRearmsAndSelfCancels
    );
    runCase(
        "timers added by callbacks wait; ties keep order",
        testTimersAddedByCallbacksWaitAndTiesKeepOrder
    );
    runCase("wait timeout clamps to next timer", testClampTimeout);
    runCase("shutdown drains closures that post as they go", testShutdownDrainsClosuresThatPost);
    runCase("post from 4 threads wakes a blocking pump", testPostFromThreadsWakesLoop);
    runCase("run() exits on quit() from a timer", testRunQuitFromTimer);
    runCase("self-reposting closure cannot starve the loop", testPostedWorkDoesNotStarve);
    runCase("frame requests coalesce; painted pixels read back", testFrameContract);
    runCase("fractional scale sizes the canvas in physical px", testFractionalScaleCanvas);
    runCase("keys: text, repeat, Ctrl chords type nothing", testKeyTextAndShortcuts);
    runCase("pointer: double-click count, caption press swallowed", testPointerClicksAndHitTest);
    runCase("clipboard: async, typed, never re-entrant", testClipboardIsAsyncAndTyped);
    runCase("key names", testKeyNames);
    runCase("transfer: text MIME names", testTextMimes);
    runCase("transfer: text/uri-list parsing", testUriList);
    runCase("transfer: percent and file:// URIs", testPercentAndFileUris);
    runCase("input: multi-click counter", testClickCounter);
    runCase("input: keys from ASCII", testKeyFromAscii);
    runCase("image: unpremultiply and scale", testImageUtil);
    runCase("pacing: frame interval from the refresh rate", testFrameInterval);
    runCase("loop: a post wakes only an empty queue", testPostWakesOncePerBatch);
    runCase("strings: case, trim, markup, URL schemes", testStrings);
    runCase("wire: u32 and length-prefixed strings", testWire);
    runCase("transfer: preferred drop action", testPreferredAction);
    runCase("image: picking the size for a slot", testPickImage);
    runCase("monitors compare field by field", testMonitorEquality);
    return plat_test::summary();
}
