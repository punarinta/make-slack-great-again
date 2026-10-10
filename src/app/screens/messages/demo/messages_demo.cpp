// messages_demo — MessageList + ThreadPanel on demo/fixture.json, without the
// shell: a channel header, the list, a bare composer, and the thread panel.
//
//   messages_demo [--conv NAME] [--thread] [--dark] [--send TEXT] [--hover X Y]
//                 [--context] [--viewer] [--exit-after MS]
#include "app/diag/mem_stats.h"
#include "app/fake/fake_backend.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"
#include "app/screens/messages/thread_panel.h"
#include "base/time.h"
#include "plat/testing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace screens;

namespace {

// A bare composer: a bordered TextEdit that sends on Enter.
std::unique_ptr<ui::View> composer(
    Context &ctx, std::function<ConvRef()> conv, std::function<Ts()> thread, const char *placeholder
) {
    auto box = std::make_unique<ui::View>();
    box->setBackground(ui::C::InputBg, 8);
    box->setBorder(ui::C::InputBorder);
    auto *edit = box->add<ui::TextEdit>();
    edit->setPlaceholder(placeholder);
    edit->setMaxLines(8);
    edit->style().padding(14, 10);
    edit->onSubmit = [&ctx, edit, conv, thread] {
        if (!edit->empty())
            ctx.backend.send(conv(), edit->text(), thread(), nullptr);
        edit->clear();
        return true;
    };
    return box;
}

} // namespace

int main(int argc, char **argv) {
    std::string convName = "design", sendText;
    bool        thread = false, dark = false, context = false, viewer = false, picker = false;
    int         exitAfter = 0;
    float       hoverX = -1, hoverY = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a    = argv[i];
        auto              next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--conv")
            convName = next();
        else if (a == "--thread")
            thread = true;
        else if (a == "--dark")
            dark = true;
        else if (a == "--context")
            context = true;
        else if (a == "--viewer")
            viewer = true;
        else if (a == "--picker")
            picker = true;
        else if (a == "--send")
            sendText = next();
        else if (a == "--exit-after")
            exitAfter = std::atoi(next());
        else if (a == "--hover") {
            hoverX = float(std::atof(next()));
            hoverY = float(std::atof(next()));
        }
    }
    std::string err;
    auto        app = ui::App::create(&err);
    if (!app) {
        std::fprintf(stderr, "messages_demo: %s\n", err.c_str());
        return 1;
    }
    if (dark)
        app->setThemeMode(ui::ThemeMode::Dark);
    model::Store      store;
    fake::FakeBackend backend(store, app->platform());
    backend.setFixture(MSGA_DEMO_DIR, 0);
    ImageCache images(app->platform());
    Context    ctx{*app, store, backend, images};

    plat::WindowDesc d;
    d.title = "messages demo";
    d.appId = "msga-messages-demo";
    d.size  = {1200, 800};
    ui::Window win(d);
    ctx.window   = &win; // lets popups (e.g. the transcript dialog) show
    auto *screen = win.root().add<ui::View>();
    screen->style().row();
    auto *main = screen->add<ui::View>();
    main->style().flex(1);
    main->setBackground(ui::C::Surface);
    auto *header = main->add<ui::View>();
    header->style().row().height(56).padding(20, 0).items(ui::Align::Center);
    auto *title = header->add<ui::Label>("", ui::Font::Title);
    main->add<ui::Separator>();
    auto *list = main->add<MessageList>(ctx);
    list->style().flex(1);
    auto *wrap = main->add<ui::View>();
    wrap->style().padding(20, 0, 20, 20);
    ConvRef current = model::kNoConv;
    wrap->adopt(composer(ctx, [&] { return current; }, [] { return Ts(0); }, "Message"));
    auto *sep   = screen->add<ui::Separator>(true);
    auto *panel = screen->add<ThreadPanel>(ctx);
    panel->style().width(420);
    panel->setComposer(composer(
        ctx, [&] { return panel->conversation(); }, [&] { return panel->root(); }, "Reply…"
    ));
    panel->setVisible(false);
    sep->setVisible(false);

    ctx.openConversation = [&](ConvRef c) {
        current = c;
        title->setText((store.conversation(c).isDirect() ? "" : "#") + store.displayName(c));
        list->showConversation(c);
    };
    ctx.openThread = [&](ConvRef c, Ts root) {
        panel->setVisible(true);
        sep->setVisible(true);
        panel->show(c, root);
    };
    ctx.closeThread = [&] {
        panel->setVisible(false);
        sep->setVisible(false);
    };
    ctx.openProfile = [&](UserRef u) { std::printf("profile: %s\n", store.user(u).id.c_str()); };
    ctx.openUrl     = [&](const std::string &u) { std::printf("open: %s\n", u.c_str()); };

    const double t0 = app->nowMs();
    backend.connect([&](bool ok, const std::string &e) {
        if (!ok) {
            std::fprintf(stderr, "fixture: %s\n", e.c_str());
            app->quit();
            return;
        }
        ConvRef c = store.findConversation("C0DESIGN");
        for (ConvRef i = 0; i < store.conversationCount(); ++i)
            if (store.conversation(i).name == convName)
                c = i;
        ctx.openConversation(c);
        if (thread)
            for (const model::Message &m : store.conversation(c).messages)
                if (m.replyCount > 0) {
                    ctx.openThread(c, m.ts);
                    break;
                }
        if (!sendText.empty())
            backend.send(c, sendText, 0, nullptr);
        // Once the first frames laid the list out.
        app->addTimer(150, false, [&, c] {
            if (context)
                list->openMenu(store.conversation(c).messages.back().ts, {600, 420});
            if (picker)
                list->openReactionPicker(
                    store.conversation(c).messages.back().ts, {640, 700, 0, 0}
                );
            if (viewer)
                for (const model::Message &m : store.conversation(c).messages)
                    for (const model::File &f : m.files())
                        if (f.isImage()) {
                            list->openImage(f.path, f.width, f.height);
                            return;
                        }
        });
    });
    for (int i = 0; i < 40; ++i)
        app->pump(5);
    std::fprintf(
        stderr,
        "first frames after %.0f ms (%d frames), images %zu entries %zu KB\n",
        app->nowMs() - t0,
        win.stats().frames,
        images.entryCount(),
        images.bytes() / 1024
    );
    if (hoverX >= 0)
        if (auto *h = app->platform().testHooks())
            h->injectPointerMove(win.native(), {hoverX, hoverY});
    if (exitAfter > 0)
        app->addTimer(exitAfter, false, [&] {
            std::fprintf(
                stderr,
                "rss %ld KB, images %zu KB in %zu entries\n",
                diag::rssKb(),
                images.bytes() / 1024,
                images.entryCount()
            );
            app->quit();
        });
    app->run();
    return 0;
}
