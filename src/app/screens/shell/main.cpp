// msga: the app. Opens the signed-in workspace (screens/shell/accounts.h),
// or the "Log in to workspace" page when there is none.
//
//   msga                  the saved workspace, or the logged-out page
//   msga msga://…         also hands an OAuth callback URL to the sign-in
//   --demo <dir>          the fake workspace in <dir>/fixture.json (only in
//                         builds configured with -DMSGA_DEMO=ON)
//   --demo-tour <file>    with --demo: msga's scripted walkthrough (demo/tour.json)
//   --claude-permission-hook <dir>
//                         Claude Code's permission hook for the sessions msga
//                         starts (app/claude/approvals.h): no window, no app
//
// Diagnostics (screenshots, measurements):
//   --theme light|dark   override the saved theme for this run
//   --open <conv id>     open this conversation instead of the fixture's start
//   --thread <text>      then open the thread whose root contains <text>
//   --browse-all         visit every conversation once, then print RSS
//   --soak <rounds>      the leak soak (demo/soak.h, scripts/soak.sh)
//   --exit-after <ms>    quit after that long
//
// Leak hunting (Debug and demo builds): SIGUSR1 prints the memory numbers
// (app/diag/mem_stats.h) and, in an ASan build, runs a LeakSanitizer pass;
// MSGA_MEMSTATS=<seconds> prints them that often.
#include "app/auth/workspaces.h"
#include "app/cache/workspace_cache.h"
#include "app/claude/approvals.h"
#include "app/crash/crash_handler.h"
#include "app/i18n/languages.h"
#include "app/identity.h"
#include "app/llm/service.h"
#include "app/media/audio_player.h"
#include "app/model/backend_proxy.h"
#include "app/model/null_backend.h"
#ifdef MSGA_SELF_UPDATE
#include "app/update/updater.h"
#endif
#include "base/file.h"
#include "base/i18n.h"
#include "base/time.h"
#include "base/log.h"
#include "base/str.h"
#include "gfx/icons_generated.h"
#include "net/net.h"
#include "screens/common/context.h"
#include "screens/common/remote_images.h"
#include "screens/shell/accounts.h"
#include "screens/shell/avatars.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/desktop_entry.h"
#include "screens/shell/settings.h"
#include "screens/shell/shell.h"
#ifdef MSGA_LEGACY_IMPORT
#include "app/legacy/legacy.h"
#endif

#ifdef MSGA_DEMO
#include "app/claude/backend.h"
#include "app/fake/fake_backend.h"
#include "screens/shell/demo/claude_demo.h"
#include "screens/shell/demo/soak.h"
#include "screens/shell/demo/tour.h"
#endif
#ifdef MSGA_DEV_DIAG
#include "app/diag/mem_stats.h"
#endif
#ifdef MSGA_HAVE_MESSAGES
#include "screens/messages/image_cache.h"
#endif
#if defined(MSGA_DEMO) && defined(MSGA_HAVE_MESSAGES)
#include "screens/messages/debug_scroll.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#if defined(MSGA_DEMO) && defined(__linux__)
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <ftw.h>
#include <unistd.h>
#endif
#if (defined(__SANITIZE_ADDRESS__) || defined(MSGA_DEV_DIAG)) && !defined(_WIN32)
#include <csignal>
#endif

namespace {

#if defined(__SANITIZE_ADDRESS__) && !defined(_WIN32)
// ASan builds (scripts/run-asan.sh): Ctrl+C / SIGTERM quit the app normally,
// so LeakSanitizer gets to print its report at exit.
volatile std::sig_atomic_t gQuitSignal = 0;
#endif
#if defined(MSGA_DEV_DIAG) && !defined(_WIN32)
// SIGUSR1: print the memory numbers (and run a LeakSanitizer pass) now.
volatile std::sig_atomic_t gStatsSignal = 0;
#endif

#ifdef MSGA_DEMO
// A demo run keeps nothing of the user's — its
// HOME and XDG dirs point at a fresh <tmp>/msga-demo-state-<pid>, so the
// settings, workspaces, caches and the credential store it reads and
// writes are throwaway ones. Before the platform starts: it reads them on first
// use. Per process so that demo runs can overlap (scripts/demo-video.sh in
// parallel); the dirs of earlier runs that are gone are removed here.
#ifdef __linux__
bool wipeDir(const std::string &dir) {
    return nftw(
               dir.c_str(),
               [](const char *path, const struct stat *, int, struct FTW *) {
                   return ::remove(path);
               },
               16,
               FTW_DEPTH | FTW_PHYS
           ) == 0;
}
#endif

bool isolateDemoState(std::string *error) {
#if !defined(__linux__)
    *error = "demo mode redirects HOME/XDG_* and is Linux-only for now";
    return false;
#else
    const char                *tmpEnv = std::getenv("TMPDIR");
    const std::string          tmp    = tmpEnv && *tmpEnv ? tmpEnv : "/tmp";
    constexpr std::string_view prefix = "msga-demo-state-";
    if (DIR *d = opendir(tmp.c_str())) {
        while (const dirent *e = readdir(d)) {
            const std::string_view name = e->d_name;
            if (!str::startsWith(name, prefix))
                continue;
            const int pid = std::atoi(std::string(name.substr(prefix.size())).c_str());
            if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH)
                wipeDir(file::join(tmp, name));
        }
        closedir(d);
    }
    const std::string state = file::join(tmp, str::concat({prefix, str::number(getpid())}));
    if (file::exists(state) && !wipeDir(state)) {
        *error = "cannot wipe " + state;
        return false;
    }
    for (const char *sub : {"config", "data", "cache", "state"})
        if (!file::makeDirs(file::join(state, sub))) {
            *error = "cannot create " + file::join(state, sub);
            return false;
        }
    setenv("HOME", state.c_str(), 1);
    setenv("XDG_CONFIG_HOME", file::join(state, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", file::join(state, "data").c_str(), 1);
    setenv("XDG_CACHE_HOME", file::join(state, "cache").c_str(), 1);
    setenv("XDG_STATE_HOME", file::join(state, "state").c_str(), 1);
    return true;
#endif
}

std::string demoInstanceKey() {
#ifdef __linux__
    return str::concat({identity::instanceKey(), "-demo-", str::number(getpid())});
#else
    return str::concat({identity::instanceKey(), "-demo"});
#endif
}
#endif

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && argv[1] == claude::kPermissionHookFlag)
        return claude::runPermissionHook(argv[2]);
#ifdef MSGA_DEMO
    std::string demo;
#else
    constexpr std::string_view demo; // no demo mode in this build: never set
#endif
    std::string                  theme;
    std::vector<std::string>     urls; // msga:// from the OS (an OAuth callback)
    int                          exitAfter = 0;
    int                          exitCode  = 0;
    // These diagnostics act on a workspace, which only the demo provides so
    // far. debugScroll (hidden): scroll torture test with frame verification.
    [[maybe_unused]] std::string openId, threadText;
    [[maybe_unused]] int         debugScroll = 0;
    [[maybe_unused]] int         soakRounds  = 0;
    [[maybe_unused]] bool        browseAll   = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a    = argv[i];
        auto              next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--theme")
            theme = next();
#ifdef MSGA_DEMO
        else if (a == "--demo")
            demo = next();
        else if (str::startsWith(a, "--demo="))
            demo = a.substr(7);
        else if (a == "--debug-scroll")
            debugScroll = std::atoi(next().c_str());
#endif
        else if (a == "--open")
            openId = next();
        else if (a == "--thread")
            threadText = next();
        else if (a == "--exit-after")
            exitAfter = std::atoi(next().c_str());
        else if (a == "--browse-all")
            browseAll = true;
        else if (a == "--soak")
            soakRounds = std::atoi(next().c_str());
        else if (str::startsWith(a, "msga://"))
            urls.push_back(a);
        // Anything else is ignored.
    }

    std::string err;
#ifdef MSGA_DEMO
    if (!demo.empty() && !isolateDemoState(&err)) {
        std::fprintf(stderr, "msga --demo: %s\n", err.c_str());
        return 2;
    }
#endif
    // Destroyed after everything below, the App included: sockets retired at
    // exit wait in its posted closures and timers, and go with its loop.
    struct NetCaches {
        ~NetCaches() { net::releaseCaches(); }
    } netCaches;
    auto app = ui::App::create(&err);
    if (!app) {
        std::fprintf(stderr, "msga: %s\n", err.c_str());
        return 1;
    }
    plat::App  &pa    = app->platform();
    // The app's ids (app/identity.h), which msga.desktop, the Start-menu
    // shortcut and the bundle carry: shells find the window's, the notifications'
    // and the launcher badge's entry by them (Linux: app_id "msga", WM_CLASS
    // "msga", "MSGA"; elsewhere com.nisdos.msga).
    const char *appId = identity::appId();
    // Windows: the "MSGA" Start-menu entry (with the AUMID), always
    // (not for a demo run).
    pa.setAppInfo({identity::kName, appId, {}, demo.empty()});
    // An earlier version still running takes this launch, as its own second
    // launches did; then a second launch of ours hands its arguments to the
    // running one (which raises its window on InstanceActivated) and exits.
    if (demo.empty() &&
        identity::handOffToEarlierVersion(urls.empty() ? std::string() : urls.front()))
        return 0;
    std::vector<std::string> args(argv + 1, argv + argc);
    // A demo run has its own channel, one per process: it neither hands off to
    // the user's msga nor takes its launches, and overlapping demo runs (the
    // socket is in $XDG_RUNTIME_DIR, which isolateDemoState leaves alone so
    // that audio still plays) don't hand off to each other.
#ifdef MSGA_DEMO
    const std::string instance =
        demo.empty() ? std::string(identity::instanceKey()) : demoInstanceKey();
#else
    const std::string instance(identity::instanceKey());
#endif
    if (!pa.claimSingleInstance(instance, args))
        return 0;
    // A crash now prints a stack trace (stderr + crash.log, the file earlier
    // versions wrote too) instead of a bare "Segmentation fault", then still core-dumps.
    crash::install(identity::crashLogPath(pa));
    // Dev builds only: the main-thread hang watchdog (crash_handler.h), its
    // heartbeat started below. AddressSanitizer makes everything ~5-10x
    // slower, so ASan builds get a roomier window and only genuine hangs fire.
#if defined(__SANITIZE_ADDRESS__)
    const bool watchdog = crash::startWatchdog(20000);
#else
    const bool watchdog = crash::startWatchdog(5000);
#endif

    const std::string settingsPath = shell::Settings::defaultPath(pa);
#ifdef MSGA_LEGACY_IMPORT
    // The upgrade from an earlier version: its settings, workspaces and caches.
    if (demo.empty())
        legacy::importOldData(pa, settingsPath, auth::WorkspaceStore::defaultPath(pa));
#endif
    shell::Settings settings = shell::Settings::load(settingsPath);
    // Before any UI text exists: strings are translated when a view is built,
    // so a changed language applies at the next start.
    app_i18n::registerLanguages();
    if (settings.language == "system" || !i18n::setLanguage(settings.language))
        i18n::setPreferredLanguage(pa.preferredLanguages());
    // Dates follow the setting at once (Shell::applySettings re-applies it);
    // Japanese writes the 24-hour clock by default.
    base::setDateLanguage(settings.language);
    if (!settings.use24hSaved)
        settings.use24h = std::strcmp(base::dateLanguage(), "ja") == 0;
    app->setThemeMode(
        theme == "dark"    ? ui::ThemeMode::Dark
        : theme == "light" ? ui::ThemeMode::Light
                           : settings.theme
    );
    app->setUserTextScale(settings.fontScale());
    settings.applyPalettes();

    // The demo workspace, or the signed-in one (Accounts swaps it in behind
    // the proxy the screens hold).
    model::Store        store;
    model::NullBackend  noWorkspace(store);
    model::BackendProxy backend(store, noWorkspace);
    net::Client         client(pa);
    // Pictures and the update's download on a worker pool of their own, so
    // a screenful of avatars never holds up the workspace's API calls.
    net::Client         transfers(pa);
#ifdef MSGA_SELF_UPDATE
    // The update check (msga.app's manifest; the shell drives it).
    update::Updater updater(pa, transfers, MSGA_VERSION);
#endif
    // Avatars, files, emoji and previews from URLs, cached on disk.
    screens::RemoteImages remote(pa, &transfers, screens::RemoteImages::defaultDir(pa));
    // The rest of the cache limit: the inline player's
    // audio and the opened HTML files are evicted with the pictures; the
    // workspaces' data and icons only count toward the limit.
    if (const std::string cache = identity::cacheDir(pa); !cache.empty())
        remote.coverDirs(
            {file::join(cache, "audio"), file::join(cache, "files")},
            {cache::WorkspaceCache::root(pa), file::join(cache, "workspace-icons")}
        );
#ifdef MSGA_DEMO
    // A Slack fixture runs on the fake backend; a Claude Code one
    // (claude-code.json) on the real Claude Code workspace, whose `claude` is
    // the fixture's stand-in (demo/claude_demo.h).
    std::optional<fake::FakeBackend> demoBackend;
    std::unique_ptr<claude::Backend> demoClaude;
    std::string                      demoStart;
    if (!demo.empty() && demo::isClaudeFixture(demo)) {
        demo::ClaudeDemo cd;
        if (!demo::prepareClaudeDemo(demo, pa, settings, &cd, &err)) {
            std::fprintf(stderr, "msga --demo: %s\n", err.c_str());
            return 2;
        }
        store.workspaceName = "Claude Code";
        demoClaude =
            std::make_unique<claude::Backend>(store, pa, claude::Credentials{cd.claudePath});
        backend.setTarget(*demoClaude);
        demoStart = cd.startSession;
    } else if (!demo.empty()) {
        demoBackend.emplace(store, pa);
        demoBackend->setFixture(demo, 0);
        backend.setTarget(*demoBackend);
    }
    const bool demoMode = demoBackend || demoClaude;
#else
    const bool demoMode = false;
#endif
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache  images(pa);
    screens::ImageCache &imageRef = images;
    images.setRemote(&remote);
#else
    // The messages screens (and their ImageCache) are not linked yet and
    // nothing dereferences this until they are.
    alignas(16) static char noCache[16];
    screens::ImageCache    &imageRef = *reinterpret_cast<screens::ImageCache *>(noCache);
#endif
    screens::Context ctx{*app, store, backend, imageRef, {}, {}, {}, {}, {}};
    ctx.remote = &remote;
    // The AI providers (Settings → AI assistance; the shell keeps them in step).
    llm::Service ai(pa);
    ctx.ai = &ai;
    // The inline audio player: one clip at a time, across conversations.
    media::AudioPlayer audio(pa);
    ctx.audio = &audio;
#ifdef MSGA_DEMO
    if (demoBackend) // the fixture's canned answers stand in for an AI server
        ai.setStandIn(
            llm::fromSettings("demo", "Lumen AI", "demo:", "", "lumen-1", ""),
            [&fb = *demoBackend](std::string_view request) {
                return std::string(fb.aiReply(request));
            }
        );
#endif

    plat::WindowDesc desc;
    desc.title   = "MSGA";
    desc.appId   = appId;
    desc.wmClass = "MSGA"; // WM_CLASS res_class: the application name
#ifdef __linux__
    // The logo for the X11 _NET_WM_ICON, at 16 to 256 px.
    for (int n : {16, 20, 24, 32, 48, 64, 128, 256}) {
        gfx::Bitmap  b(n, n);
        gfx::Painter p(b.view(), 1.f);
        gfx::drawIcon(p, gfx::Icon::Logo, {0, 0, float(n), float(n)}, 0xffffffffU);
        desc.icon.push_back(shell::toPlatImage(b));
    }
#endif
    desc.size        = {settings.width, settings.height};
    desc.minSize     = shell::Shell::kMinWindowSize; // fitToScreen lowers it on small screens
    desc.decorations = plat::Decorations::Custom;
    if (settings.hasPosition)
        desc.position = plat::Point{double(settings.x), double(settings.y)};
    ui::Window win(desc);
    if (settings.maximized)
        win.native().setMaximized(true);

    shell::Shell sh(ctx, win, settings, settingsPath);
    // A restart (an applied update, new Slack app keys) runs with the same
    // arguments, minus a one-off OAuth callback URL.
    for (const std::string &a : args)
        if (!str::startsWith(a, "msga://"))
            sh.restartArgs.push_back(a);
#ifdef MSGA_SELF_UPDATE
    if (!demoMode)
        sh.setUpdater(&updater);
#endif
#ifdef MSGA_DEMO
    const std::string           tourPath = demo::tourPathFromArgs(argc, argv);
    std::unique_ptr<demo::Tour> tour;
#endif
    win.onCloseRequested = [&] {
        if (!sh.hideToTray())
            sh.quit();
    };
    sh.onQuit = [&] { app->quit(); };

#ifdef MSGA_DEV_DIAG
    // The app's own holders of memory, next to the process numbers: a cache
    // that grows past its budget, or a Store that keeps growing, shows here
    // before it does in a profiler.
    auto appStats = [&]() -> std::string {
        const model::Store &st       = ctx.store;
        size_t              messages = 0;
        for (model::ConvRef c = 0; c < st.conversationCount(); ++c)
            messages += st.conversation(c).messages.size();
        char buf[256];
        std::snprintf(
            buf,
            sizeof buf,
            "images %zu KB in %zu, avatars %zu KB in %zu, convs %zu, messages %zu, users %zu",
#ifdef MSGA_HAVE_MESSAGES
            images.bytes() / 1024,
            images.entryCount(),
#else
            size_t(0),
            size_t(0),
#endif
            sh.avatars().bytes() / 1024,
            sh.avatars().size(),
            st.conversationCount(),
            messages,
            st.userCount()
        );
        return buf;
    };
#endif

    std::optional<shell::Accounts> accounts;
    app->onEvent = [&](const plat::Event &e) {
        // msga:// URLs (the OAuth callback): opened by the OS, or handed over
        // by a second launch — which also comes as InstanceActivated with the
        // same URL among its args, so only OpenUrls counts.
        if (accounts && e.type == plat::EventType::OpenUrls)
            for (const std::string &s : e.strings)
                if (str::startsWith(s, "msga://"))
                    accounts->handleUrl(s);
        if (accounts && e.type == plat::EventType::NetworkChanged)
            accounts->networkChanged(e.online);
        sh.handleAppEvent(e);
    };

    if (!demoMode) {
        // The OAuth redirect (msga://oauth/callback) comes back to us; on
        // Linux through the launcher entry (msga.desktop).
        shell::installDesktopEntry(pa);
        accounts.emplace(
            ctx,
            sh,
            win,
            settings,
            [&sh] { sh.saveSettingsSoon(); },
            backend,
            noWorkspace,
            client,
            auth::WorkspaceStore::defaultPath(pa)
        );
        shell::Accounts &acc           = *accounts;
        sh.onAddWorkspace              = [&acc](ui::PointF at) { acc.promptAdd(at); };
        sh.menus().hooks.signOut       = [&acc](const std::string &key) { acc.signOut(key); };
        sh.menus().hooks.muteWorkspace = [&acc](const std::string &key, bool on) {
            acc.setMuted(key, on);
        };
        sh.onSwitchWorkspace   = [&acc](const std::string &key) { acc.switchTo(key); };
        sh.onReorderWorkspaces = [&acc](const std::vector<std::string> &keys) {
            acc.reorder(keys);
        };
        sh.onImportSlackSession = [&acc] { acc.importSession(); };
        sh.onConvertToSession   = [&acc] { acc.convertToSession(); };
        sh.oauthSlackWorkspaces = [&acc] { return acc.oauthSlackWorkspaces(); };
        acc.start();
        for (const std::string &u : urls)
            acc.handleUrl(u);
    }
#ifdef MSGA_DEMO
    else {
        fake::FakeBackend *fb = demoBackend ? &*demoBackend : nullptr;
        model::Backend    &db = fb ? static_cast<model::Backend &>(*fb) : *demoClaude;
        if (demoClaude) { // as Accounts does for a workspace
            demoClaude->onError  = [&sh](const std::string &message) { sh.showError(message); };
            demoClaude->onUnsent = [&sh, &db](
                                       model::ConvRef                  conv,
                                       model::Ts                       thread,
                                       const std::string              &text,
                                       const std::vector<std::string> &files
                                   ) {
                sh.giveBackUnsent(sh.drafts().scope(), db.store(), conv, thread, text, files);
            };
        }
        const double t0 = app->nowMs();
        sh.setSignedIn(true); // the workspace opening: the first-load state until connected
        // t0 by value: connect answers after this block has ended.
        db.connect([&, fb, t0](bool ok, const std::string &why) {
            if (!ok) {
                std::fprintf(stderr, "msga: %s\n", why.c_str());
                app->quit();
                return;
            }
            model::ConvRef start =
                fb ? fb->fixture().startConversation : store.findConversation(demoStart);
            if (!openId.empty())
                if (model::ConvRef c = store.findConversation(openId); c != model::kNoConv)
                    start = c;
            sh.open(start);
            if (!threadText.empty())
                if (model::Ts root = fake::findTs(store, start, threadText))
                    sh.openThread(start, root);
            sh.setLive(true);
            std::fprintf(
                stderr, "msga: connected in %.0f ms, rss %ld KB\n", app->nowMs() - t0, diag::rssKb()
            );
            if (!tourPath.empty()) {
                demo::TourScript script;
                std::string      err;
                if (!demo::loadTour(tourPath, &script, &err)) {
                    std::fprintf(stderr, "msga --demo-tour: %s\n", err.c_str());
                    exitCode = 2;
                    app->quit();
                    return;
                }
                tour = std::make_unique<demo::Tour>(ctx, sh, win, fb, std::move(script));
                tour->start();
            }
#ifdef MSGA_HAVE_MESSAGES
            if (debugScroll > 0) {
                screens::DebugScrollHooks hooks{
                    [&](model::ConvRef c) { sh.open(c); },
                    [&](model::ConvRef c, model::Ts t) { sh.openThread(c, t); },
                    [&] { sh.closeThread(); },
                };
                app->addTimer(400, false, [&, hooks, debugScroll] {
                    screens::debugScrollTour(ctx, win, hooks, debugScroll, [&](int bad) {
                        exitCode = bad ? 3 : 0;
                        app->quit();
                    });
                });
            }
#endif
            if (browseAll) {
                // Every conversation in turn (a frame or two each), then back.
                auto step = std::make_shared<std::function<void(model::ConvRef)>>();
                *step     = [&, step, start](model::ConvRef c) {
                    if (c >= store.conversationCount()) {
                        sh.open(start);
                        // The function holds itself (step): let it go once
                        // it has returned, or the cycle leaks.
                        app->addTimer(0, false, [step] { *step = nullptr; });
                        app->addTimer(300, false, [] {
                            std::fprintf(
                                stderr,
                                "msga: after browsing all conversations, rss %ld KB\n",
                                diag::rssKb()
                            );
                        });
                        return;
                    }
                    sh.open(c);
                    app->addTimer(120, false, [step, c] { (*step)(c + 1); });
                };
                app->addTimer(500, false, [step] { (*step)(0); });
            }
            if (soakRounds > 0)
                app->addTimer(500, false, [&, start] {
                    demo::runSoak(ctx, sh, start, soakRounds, appStats, [&] { sh.quit(); });
                });
        });
    }
#endif
    if (exitAfter > 0)
        app->addTimer(exitAfter, false, [&] { sh.quit(); });
    // Pet the hang watchdog from the event loop: while the loop pumps, this
    // re-arms its deadline every second; if the loop wedges, it fires.
    if (watchdog)
        app->addTimer(1000, true, [] { crash::heartbeat(); });
#if defined(__SANITIZE_ADDRESS__) && !defined(_WIN32)
    std::signal(SIGINT, [](int) { gQuitSignal = 1; });
    std::signal(SIGTERM, [](int) { gQuitSignal = 1; });
    app->addTimer(250, true, [&] {
        if (gQuitSignal)
            sh.quit();
    });
#endif
#ifdef MSGA_DEV_DIAG
    auto printStats = [&](const char *why) {
        std::fprintf(
            stderr,
            "msga memstats (%s): %s; %s\n",
            why,
            diag::formatMem(diag::sampleMem()).c_str(),
            appStats().c_str()
        );
        std::fflush(stderr);
    };
#ifndef _WIN32
    std::signal(SIGUSR1, [](int) { gStatsSignal = 1; });
    app->addTimer(250, true, [&] {
        if (!gStatsSignal)
            return;
        gStatsSignal = 0;
        printStats("SIGUSR1");
        diag::checkLeaksNow();
    });
#endif
    if (const char *every = std::getenv("MSGA_MEMSTATS"); every && std::atoi(every) > 0)
        app->addTimer(std::atoi(every) * 1000, true, [&] { printStats("periodic"); });
#endif
    app->run();
    return exitCode;
}
