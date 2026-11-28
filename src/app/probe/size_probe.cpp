// Links every public entry point of msga_base and msga_app_core, reading its
// inputs from argv so nothing folds away, so the linker map measures what the
// two libraries really cost in a stripped, gc-sectioned binary. Not a test:
// it is built, never run by ctest.
#include "app/fake/fake_backend.h"
#include "app/mrkdwn/emoji.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/time.h"
#include "base/utf8.h"

#include <cstdio>

int main(int argc, char **argv) {
    const char *arg = argc > 1 ? argv[1] : "";
    std::string text;
    file::readAll(arg, &text);
    json::Document d;
    d.parse(text, nullptr);
    std::string out = json::write(d.root(), argc > 2);
    file::writeAtomic(arg, out);
    LOG_INFO("probe", "%s %d", utf8::foldCase(arg).c_str(), int(utf8::countCodePoints(arg)));
    out += base::dayLabel(base::nowSecs(), base::nowSecs()) + base::lastReplyLabel(1, 2) +
           base::dateTimeLabel(1, 2) + base::formatDateTime(3) + i18n::trn("%n a", "%n b", argc);

    const auto               rich = mrkdwn::parse(arg);
    std::vector<mrkdwn::Run> runs;
    for (const auto &b : mrkdwn::blocks(rich))
        mrkdwn::runs(rich, b.start, b.end, runs);
    out += mrkdwn::resolveTokens(arg).text + emoji::toUnicode(arg) + emoji::expandShortcodes(arg);

    auto app = plat::App::create(nullptr);
    if (app) {
        model::Store      store;
        fake::FakeBackend be(store, *app);
        be.setFixture(arg, 0);
        be.connect([&](bool, const std::string &) {
            be.send(0, "hi", 0, nullptr);
            be.react(0, 1, "x", true);
            be.edit(0, 1, "y");
            be.remove(0, 1);
            be.markRead(0, 1);
            be.setStarred(0, true);
            be.userTyping(0, 0);
            be.loadHistory(0, 0, nullptr);
            be.loadThread(0, 0, nullptr);
            be.search("q", [](std::vector<model::Backend::SearchHit>) {});
            std::printf("%s %zu\n", store.emojiFor(arg).unicode.c_str(), store.indexOf(0, 1));
            app->quit();
        });
        app->run();
    }
    std::printf("%zu %zu\n", out.size(), runs.size());
    return 0;
}
