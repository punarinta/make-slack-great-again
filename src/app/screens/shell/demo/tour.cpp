#include "screens/shell/demo/tour.h"

#include "app/fake/fake_backend.h"
#include "base/time.h"
#include "base/utf8.h"
#include "plat/testing.h"
#include "screens/common/context.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/header.h"
#include "screens/shell/header_jobs.h"
#include "screens/shell/message_search.h"
#include "screens/shell/shell.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

using namespace ui;
using model::ConvRef;
using model::kNoConv;
using model::Ts;
using K = demo::TourStep::Kind;

namespace demo {

namespace {

// Depth-first, visible views only; popups (menus, dialogs, pickers) first,
// topmost first, then the screen.
View *find(View *v, const std::function<bool(View *)> &match) {
    if (!v || !v->visible())
        return nullptr;
    if (match(v))
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (View *f = find(v->child(i), match))
            return f;
    return nullptr;
}

// The text fields under `v`, in order: the outermost view of each (a field
// and the editor inside it are one).
void textFields(View *v, std::vector<View *> &out) {
    if (!v || !v->visible())
        return;
    if (v->role() == Role::TextInput) {
        out.push_back(v);
        return;
    }
    for (size_t i = 0; i < v->childCount(); ++i)
        textFields(v->child(i), out);
}

plat::Key keyNamed(const std::string &name) {
    if (name == "Return")
        return plat::Key::Enter;
    if (name == "Tab")
        return plat::Key::Tab;
    if (name == "Escape")
        return plat::Key::Escape;
    if (name == "Down")
        return plat::Key::Down;
    if (name == "Up")
        return plat::Key::Up;
    return plat::Key::Backspace;
}

} // namespace

Tour::Tour(
    screens::Context  &ctx,
    shell::Shell      &sh,
    ui::Window        &win,
    fake::FakeBackend *backend,
    TourScript         script
)
    : _ctx(ctx), _sh(sh), _win(win), _backend(backend), _script(std::move(script)) {}

Tour::~Tour() = default;

void Tour::start() {
    if (!_ctx.app.platform().testHooks()) {
        // The tour moves the real pointer: nothing to drive without it.
        std::fprintf(stderr, "tour: this platform has no input injection (X11 needs XTest)\n");
        finish();
        return;
    }
    _win.native().setSize({_script.width, _script.height});
    _pos      = {float(_script.width) / 2, float(_script.height) / 2};
    // The first conversation on screen, then a moment for the window to settle.
    auto wait = std::make_shared<std::function<void()>>();
    *wait     = [this, wait] {
        if (_sh.current() == kNoConv) {
            after(100, *wait);
            return;
        }
        after(1500, [this] {
            // Epoch ms: scripts/demo-video.sh trims the capture to these stamps.
            std::fprintf(stderr, "tour: start %" PRId64 "\n", base::nowMicros() / 1000);
            std::fflush(stderr);
            runNext();
        });
    };
    (*wait)();
}

void Tour::runNext() {
    if (_done)
        return;
    if (_index >= _script.steps.size()) {
        finish();
        return;
    }
    _jumped = false;
    run(_script.steps[_index++], [this] { after(_script.pauseMs, [this] { runNext(); }); });
}

void Tour::finish() {
    if (std::exchange(_done, true))
        return;
    std::fprintf(stderr, "tour: end %" PRId64 "\n", base::nowMicros() / 1000);
    std::fflush(stderr);
    _sh.quit();
}

// ── Primitives ──────────────────────────────────────────────────────────────

void Tour::after(int ms, Done done) {
    std::weak_ptr<char> alive = _alive;
    _ctx.app.addTimer(std::max(ms, 0), false, [alive, done = std::move(done)] {
        if (!alive.expired())
            done();
    });
}

void Tour::moveTo(PointF to, Done done) {
    // An eased glide, like a hand on a mouse (about 280 ms).
    constexpr int kSteps = 14;
    const PointF  from   = _pos;
    auto          i      = std::make_shared<int>(0);
    auto          step   = std::make_shared<std::function<void()>>();
    *step                = [this, from, to, i, step, done] {
        const float t = float(++*i) / kSteps;
        const float e = t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2;
        _pos          = {from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e};
        // The real pointer for the screen capture (XTest motion does not
        // reach every X server's clients, Xvfb's among them), and the same
        // motion handed to the window, so hovering works either way.
        if (auto *h = _ctx.app.platform().testHooks())
            h->injectPointerMove(_win.native(), {_pos.x, _pos.y});
        send(plat::EventType::PointerMove);
        if (*i < kSteps)
            after(20, *step);
        else
            after(60, done);
    };
    (*step)();
}

void Tour::send(plat::EventType type, plat::Button b, plat::Key k) {
    plat::Event e;
    e.type   = type;
    e.window = &_win.native();
    e.pos    = {_pos.x, _pos.y};
    e.button = b;
    e.clicks = type == plat::EventType::PointerDown ? 1 : 0;
    e.key    = k;
    _win.handle(e);
}

void Tour::click(PointF at, Done done, plat::Button b) {
    moveTo(at, [this, b, done] {
        send(plat::EventType::PointerDown, b);
        after(70, [this, b, done] {
            send(plat::EventType::PointerUp, b);
            after(120, done);
        });
    });
}

void Tour::typeText(std::string text, double cps, Done done) {
    if (!_win.focusView())
        _sh.composer().edit().focus();
    const int gap  = int(1000 / std::max(1.0, cps));
    auto      pos  = std::make_shared<size_t>(0);
    auto      step = std::make_shared<std::function<void()>>();
    *step          = [this, text = std::move(text), gap, pos, step, done] {
        if (*pos >= text.size()) {
            done();
            return;
        }
        const size_t from = *pos;
        utf8::decode(text, *pos);
        plat::Event e;
        e.type   = plat::EventType::TextInput;
        e.window = &_win.native();
        e.text   = text.substr(from, *pos - from);
        _win.handle(e);
        after(gap, *step);
    };
    (*step)();
}

void Tour::pressKey(plat::Key k, Done done) {
    send(plat::EventType::KeyDown, plat::Button::Left, k);
    after(40, [this, k, done] {
        send(plat::EventType::KeyUp, plat::Button::Left, k);
        after(80, done);
    });
}

void Tour::park(Done done) {
    // Over the messages, off any row's toolbar: the middle of the area right
    // of the sidebar, a third down.
    const RectF s = _sh.sidebar().windowRect();
    const SizeF w = _win.size();
    moveTo({s.x + s.w + (w.w - s.x - s.w) / 2, w.h / 3}, done);
}

View *Tour::findText(std::string_view fragment, View *in) const {
    const auto match = [fragment](View *v) {
        return v->role() == Role::Text && v->accessibleName().find(fragment) != std::string::npos;
    };
    if (in)
        return find(in, match);
    if (Popup *p = _win.topPopup())
        if (View *v = find(p, match))
            return v;
    return find(&_win.root(), match);
}

View *Tour::findName(std::string_view name, View *in) const {
    // Icon buttons go by their tooltip.
    const auto match = [name](View *v) {
        return v->accessibleName() == name || v->tooltip() == name;
    };
    if (in)
        return find(in, match);
    if (Popup *p = _win.topPopup())
        if (View *v = find(p, match))
            return v;
    return find(&_win.root(), match);
}

PointF Tour::centerOf(const View *v) const {
    const RectF r = v->windowRect();
    return {r.x + r.w / 2, r.y + r.h / 2};
}

ConvRef Tour::conv(const std::string &id) const {
    const model::Store &st = _ctx.store();
    if (const ConvRef c = st.findConversation(id); c != kNoConv)
        return c;
    // A Claude Code session started during the tour has an id nobody knows
    // beforehand: it goes by its name instead.
    for (ConvRef c = 0; c < st.conversationCount(); ++c)
        if (st.conversation(c).member && utf8::containsFolded(st.displayName(c), id))
            return c;
    return kNoConv;
}

View *Tour::messageView(const TourStep &step, Done retry, Ts *ts, bool *retrying) {
    *retrying       = false;
    const ConvRef c = _sh.current();
    *ts             = c == kNoConv ? 0 : fake::findTs(_ctx.store, c, step.arg);
    if (!*ts) {
        std::fprintf(stderr, "tour: no message contains \"%s\"\n", step.arg.c_str());
        return nullptr;
    }
    // A short prefix of the fragment: rendered text may differ (emoji,
    // mentions) further in.
    const std::string key    = step.arg.substr(0, std::min<size_t>(step.arg.size(), 24));
    // Only where it can be pointed at: between the tabs and the composer
    // (a row scrolled out of the list's viewport keeps its view a while). The
    // newest message sits right above the composer, its centre ~25 px up.
    const float       top    = _sh.tabs().windowRect().y + _sh.tabs().windowRect().h + 20;
    const float       bottom = _sh.composer().windowRect().y - 10;
    // Play: the audio card itself, named by its file (a voice clip has no text).
    const bool        card   = step.kind == K::Play;
    if (View *v = find(&_win.root(), [&](View *x) {
            if ((!card && x->role() != Role::Text) ||
                x->accessibleName().find(key) == std::string::npos)
                return false;
            const float y = centerOf(x).y;
            return y > top && y < bottom;
        }))
        return v;
    if (!_jumped) {
        _jumped = true;
        _sh.jumpToMessage(c, *ts, 0); // a smooth jump, then the step again
        after(900, std::move(retry));
        *retrying = true;
        return nullptr;
    }
    std::fprintf(stderr, "tour: \"%s\" is not on screen\n", step.arg.c_str());
    return nullptr;
}

// ── Steps ───────────────────────────────────────────────────────────────────

void Tour::run(const TourStep &step, Done done) {
    model::Store &st = _ctx.store;
    switch (step.kind) {
    case K::Wait:
        after(step.ms, done);
        return;

    case K::Open: {
        const ConvRef c = conv(step.arg);
        if (c == kNoConv) {
            std::fprintf(stderr, "tour: no conversation %s\n", step.arg.c_str());
            done();
            return;
        }
        // Its sidebar row; one that isn't listed opens directly.
        if (View *row = findText(st.displayName(c), &_sh.sidebar())) {
            click(centerOf(row), [this, c, done] {
                if (_sh.current() != c)
                    _sh.open(c);
                done();
            });
            return;
        }
        _sh.open(c);
        done();
        return;
    }

    case K::Scroll:
        // Wheel notches (a third of 100 px each line, three lines a notch).
        park([this, px = step.num, done] {
            const int  n    = std::max(1, int(std::lround(std::fabs(px) / 100)));
            const auto left = std::make_shared<int>(n);
            auto       tick = std::make_shared<std::function<void()>>();
            *tick           = [this, px, left, tick, done] {
                if ((*left)-- <= 0) {
                    after(300, done);
                    return;
                }
                plat::Event e;
                e.type   = plat::EventType::Scroll;
                e.window = &_win.native();
                e.pos    = {_pos.x, _pos.y};
                e.dy     = px < 0 ? -1 : 1; // a notch, +y = down
                _win.handle(e);
                after(70, *tick);
            };
            (*tick)();
        });
        return;

    case K::Hover:
    case K::Thread:
    case K::React:
    case K::Play:
    case K::OpenImage:
    case K::MessageMenu:
    case K::MoveToThread: {
        Ts    ts       = 0;
        bool  retrying = false;
        View *v        = messageView(step, [this, step, done] { run(step, done); }, &ts, &retrying);
        if (!v) {
            if (!retrying)
                done(); // logged; the tour goes on
            return;
        }
        const ConvRef c   = _sh.current();
        const PointF  pos = centerOf(v);
        switch (step.kind) {
        case K::Hover:
            moveTo(pos, done);
            return;
        case K::Thread:
            moveTo(pos, [this, c, ts, done] {
                _sh.openThread(c, ts);
                // Its reply box takes the keys, so what is typed next goes to
                // the thread.
                after(400, [this, done] {
                    std::vector<View *> fields;
                    textFields(_sh.threadPanel(), fields);
                    if (!fields.empty())
                        fields.back()->focus();
                    done();
                });
            });
            return;
        case K::React:
            moveTo(pos, [this, c, ts, emoji = step.arg2, done] {
                after(350, [this, c, ts, emoji, done] {
                    _ctx.backend.react(c, ts, emoji, true);
                    done();
                });
            });
            return;
        case K::Play: { // the card's round play button
            View *play = findName("Play", v);
            if (!play) {
                std::fprintf(stderr, "tour: no audio card on \"%s\"\n", step.arg.c_str());
                moveTo(pos, done);
                return;
            }
            click(centerOf(play), done);
            return;
        }
        case K::OpenImage: {
            // The first picture in the message's row.
            View *row = v;
            for (int up = 0; up < 6 && row->parent(); ++up)
                row = row->parent();
            View *img = find(row, [](View *x) { return x->role() == Role::Image; });
            if (!img) {
                moveTo(pos, done);
                return;
            }
            click(centerOf(img), done);
            return;
        }
        default: // the menu behind the hover toolbar's "…"
            moveTo(pos, [this, step, done] {
                after(250, [this, step, done] {
                    View *more = findName("More actions", &_win.root());
                    if (!more) {
                        std::fprintf(stderr, "tour: no message toolbar\n");
                        done();
                        return;
                    }
                    click(centerOf(more), [this, step, done] {
                        if (step.kind != K::MoveToThread) {
                            done();
                            return;
                        }
                        // The genuine path: "Move to thread…" → filter → Move.
                        TourStep pick;
                        pick.kind = K::MenuPick;
                        pick.arg  = "Move to thread";
                        after(450, [this, pick, target = step.arg2, done] {
                            run(pick, [this, target, done] {
                                after(700, [this, target, done] {
                                    typeText(target, 16, [this, done] {
                                        after(700, [this, done] {
                                            View *b = findName("Move");
                                            if (b)
                                                click(centerOf(b), done);
                                            else
                                                done();
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
            return;
        }
    }

    case K::CloseThread:
        _sh.closeThread();
        done();
        return;

    case K::Type:
        typeText(step.arg, step.num, done);
        return;

    case K::Key:
        pressKey(keyNamed(step.arg), done);
        return;

    case K::Send:
        pressKey(plat::Key::Enter, done);
        return;

    case K::Search:
        _sh.openSearch();
        after(300, [this, query = step.arg, done] {
            // Onto the results, where the eye goes next.
            moveTo(centerOf(_sh.messageSearch()), [this, query, done] {
                typeText(query, 18, [this, done] {
                    after(250, [this, done] { pressKey(plat::Key::Enter, done); });
                });
            });
        });
        return;

    case K::CloseSearch:
        _sh.messageSearch()->close();
        done();
        return;

    case K::QuickSwitch:
        park([this, text = step.arg, done] {
            _sh.showQuickSwitcher();
            after(300, [this, text, done] {
                typeText(text, 16, [this, done] {
                    after(250, [this, done] { pressKey(plat::Key::Enter, done); });
                });
            });
        });
        return;

    case K::Theme:
        park([this, dark = step.arg == "dark", done] {
            _ctx.app.setThemeMode(dark ? ThemeMode::Dark : ThemeMode::Light);
            done();
        });
        return;

    case K::Settings:
        park([this, pages = step.list, each = step.ms, done] {
            static constexpr const char *kPages[] = {
                "appearance", "notifications", "ai", "storage", "system", "about"
            };
            _sh.openSettings();
            auto idx  = std::make_shared<size_t>(0);
            auto next = std::make_shared<std::function<void()>>();
            *next     = [this, pages, each, idx, next, done] {
                settings::SettingsDialog *d = _sh.settingsDialog();
                if (!d || *idx >= pages.size()) {
                    if (d)
                        d->close();
                    done();
                    return;
                }
                const std::string &name = pages[(*idx)++];
                int                page = 0;
                for (int i = 0; i < 6; ++i)
                    if (name == kPages[i])
                        page = i;
                // The section list's row for the page.
                const RectF  list = d->sections().windowRect();
                const float  rowH = 36;
                const PointF at{list.x + list.w / 2, list.y + 8 + rowH * (float(page) + 0.5f)};
                moveTo(at, [this, page, each, next] {
                    if (settings::SettingsDialog *dd = _sh.settingsDialog())
                        dd->showPage(settings::SettingsDialog::Page(page));
                    after(each, *next);
                });
            };
            after(500, *next);
        });
        return;

    case K::ChannelMenu: {
        const ConvRef c   = conv(step.arg);
        View         *row = c == kNoConv ? nullptr : findText(st.displayName(c), &_sh.sidebar());
        if (!row) {
            done();
            return;
        }
        click(centerOf(row), done, plat::Button::Right);
        return;
    }

    case K::MenuHover:
    case K::MenuPick: {
        Popup *p = _win.topPopup();
        if (!p || p->role() != Role::Menu) {
            std::fprintf(stderr, "tour: no menu open for \"%s\"\n", step.arg.c_str());
            done();
            return;
        }
        const RectF r = static_cast<Menu *>(p)->itemRect(step.arg);
        if (r.w <= 0) {
            std::fprintf(stderr, "tour: no menu item \"%s\"\n", step.arg.c_str());
            done();
            return;
        }
        const RectF  m = p->windowRect();
        const PointF at{m.x + r.x + std::min(r.w / 2, 90.f), m.y + r.y + r.h / 2};
        if (step.kind == K::MenuHover)
            moveTo(at, done);
        else
            click(at, done);
        return;
    }

    case K::CloseMenu:
    case K::CloseDialog:
    case K::CloseImage:
        pressKey(plat::Key::Escape, done);
        return;

    case K::DialogButton: {
        View *b = _win.topPopup() ? findName(step.arg, _win.topPopup()) : nullptr;
        if (!b) {
            done();
            return;
        }
        click(centerOf(b), done);
        return;
    }

    case K::Gif: {
        View *btn = findName("Search GIFs", &_sh.composer());
        if (!btn) {
            done();
            return;
        }
        click(centerOf(btn), [this, query = step.arg, done] {
            after(500, [this, query, done] {
                typeText(query, 14, [this, done] {
                    // Debounced query → the stand-in server → previews decode.
                    after(1600, [this, done] {
                        Popup *p = _win.topPopup();
                        View  *cell =
                            p ? find(p, [](View *x) { return x->role() == Role::Image; }) : nullptr;
                        if (!cell) {
                            done();
                            return;
                        }
                        click(centerOf(cell), done);
                    });
                });
            });
        });
        return;
    }

    case K::OpenThreads:
    case K::OpenSaved: {
        const bool threads = step.kind == K::OpenThreads;
        View      *entry   = findText(threads ? "Threads" : "Saved messages", &_sh.sidebar());
        if (!entry) {
            if (threads)
                _sh.showPage(shell::Shell::Page::Threads);
            else
                _sh.showPage(shell::Shell::Page::Saved);
            done();
            return;
        }
        click(centerOf(entry), done);
        return;
    }

    case K::Canvas:
    case K::MessagesTab: {
        View *tab = _sh.tabs().tab(step.kind == K::Canvas ? 1 : 0);
        if (!tab || !tab->visible()) {
            done();
            return;
        }
        click(centerOf(tab), done);
        return;
    }

    case K::Post: {
        const ConvRef        c = conv(step.conv);
        const model::UserRef u = st.findUser(step.user);
        if (!_backend || c == kNoConv || u == model::kNoUser) {
            std::fprintf(stderr, "tour: cannot post to %s here\n", step.conv.c_str());
            done();
            return;
        }
        const Ts root = step.arg2.empty() ? 0 : fake::findTs(st, c, step.arg2);
        _backend->postAs(c, u, step.arg, root);
        done();
        return;
    }

    case K::Click:
    case K::Point: {
        View *in = nullptr;
        if (step.arg2 == "sidebar")
            in = &_sh.sidebar();
        else if (step.arg2 == "dialog")
            in = _win.topPopup();
        View *v = findName(step.arg, in);
        if (!v)
            v = findText(step.arg, in);
        if (!v) {
            std::fprintf(stderr, "tour: nothing named \"%s\"\n", step.arg.c_str());
            done();
            return;
        }
        if (step.kind == K::Point)
            moveTo(centerOf(v), done);
        else
            click(centerOf(v), done);
        return;
    }

    case K::Field: {
        std::vector<View *> fields;
        textFields(_win.topPopup(), fields);
        const size_t n = size_t(std::max(0.0, step.num));
        if (n >= fields.size()) {
            std::fprintf(stderr, "tour: no text field %zu in a dialog\n", n);
            done();
            return;
        }
        click(centerOf(fields[n]), done);
        return;
    }

    case K::Jobs: {
        View *v = _sh.header().jobs();
        if (!v->visible()) {
            std::fprintf(stderr, "tour: no background jobs in the header\n");
            done();
            return;
        }
        click(centerOf(v), done);
        return;
    }

    case K::Quit:
        finish();
        return;
    }
    done();
}

} // namespace demo
