#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#include "app/model/jobs.h"
#include "base/thread.h"
#include "plat/plat.h"

#include <algorithm>
#include <unordered_map>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace spell {

namespace {

// Past this many distinct words the cache starts over, so a long session of
// typing can't grow it without bound.
constexpr size_t kCacheLimit = 20000;
// Words new to the cache a main-thread backend checks per slice (one
// checkAll) before letting events through; the cache makes repeats free.
constexpr size_t kSliceWords = 200;

// Apostrophes: dictionaries spell "don't" with U+0027, keyboards and
// autocorrect often give U+2019.
std::string normalized(std::string_view word) {
    std::string w;
    w.reserve(word.size());
    for (size_t i = 0; i < word.size(); ++i) {
        if (word.substr(i, 3) == "\xE2\x80\x99") {
            w += '\'';
            i += 2;
        } else {
            w += word[i];
        }
    }
    return w;
}

// `work` then `then`, where the backend allows: on a worker (offThread), else
// both on the UI thread in a later turn — never inside the caller.
void workerOrPost(
    plat::App &app, bool offThread, std::function<void()> work, std::function<void()> then
) {
    if (offThread)
        return model::runInBackground(app, std::move(work), std::move(then));
    app.post([work = std::move(work), then = std::move(then)] {
        work();
        then();
    });
}

} // namespace

// The backend with what it learnt. Workers and the UI thread share it, so
// everything in it is used under `m`; a worker that outlives a configure()
// keeps its own (old) State alive and its answer is dropped by the caller.
struct Checker::State {
    base::Mutex                           m;
    std::unique_ptr<Backend>              backend;
    std::unordered_map<std::string, bool> misspelled; // word → answer cache
    std::vector<std::string>              ignored;
    // The last word's suggestions: a menu opened on it again asks no one.
    std::string                           suggestedFor;
    int                                   suggestedMax = 0;
    std::vector<std::string>              suggestions;

    // Under m, as the rest. A normalized word: 1 misspelled, 0 right, -1 not
    // known yet.
    int known(const std::string &w) const {
        if (std::find(ignored.begin(), ignored.end(), w) != ignored.end())
            return 0;
        const auto it = misspelled.find(w);
        return it == misspelled.end() ? -1 : it->second ? 1 : 0;
    }

    bool isMisspelled(std::string_view word) {
        std::string w = normalized(word);
        if (const int k = known(w); k >= 0)
            return k == 1;
        if (misspelled.size() >= kCacheLimit)
            misspelled.clear();
        const bool bad = !backend->check(w);
        misspelled.emplace(std::move(w), bad);
        return bad;
    }

    // The words from *next on, until kSliceWords of them were new to the
    // cache — those asked of the backend in one checkAll — misspelled ones
    // into *bad. *next moves past them.
    void checkSlice(
        std::string_view text, const std::vector<Span> &words, size_t *next, std::vector<Span> *bad
    ) {
        const size_t             from = *next;
        std::vector<std::string> norm, unknown;
        size_t                   i = from;
        for (; i < words.size() && unknown.size() < kSliceWords; ++i) {
            norm.push_back(normalized(text.substr(words[i].start, words[i].length)));
            const std::string &w = norm.back();
            if (known(w) < 0 && std::find(unknown.begin(), unknown.end(), w) == unknown.end())
                unknown.push_back(w);
        }
        if (!unknown.empty()) {
            std::vector<uint8_t> right;
            backend->checkAll(unknown, &right);
            if (misspelled.size() + unknown.size() > kCacheLimit)
                misspelled.clear();
            for (size_t j = 0; j < unknown.size(); ++j)
                misspelled.emplace(std::move(unknown[j]), j < right.size() && !right[j]);
        }
        for (size_t k = from; k < i; ++k) {
            const int b = known(norm[k - from]);
            if (b < 0 ? !backend->check(norm[k - from]) : b == 1)
                bad->push_back(words[k]);
        }
        *next = i;
    }
};

void Backend::checkAll(const std::vector<std::string> &words, std::vector<uint8_t> *right) {
    right->assign(words.size(), 0);
    for (size_t i = 0; i < words.size(); ++i)
        (*right)[i] = check(words[i]) ? 1 : 0;
}

Checker &Checker::instance() {
    static Checker *checker = new Checker; // lives as long as the app
    return *checker;
}

Checker::Checker() = default;

void Checker::configure(plat::App &app, bool enabled, const std::vector<std::string> &languages) {
    _app = &app;
    if (enabled == _enabled && languages == _languages)
        return;
    _enabled   = enabled;
    _languages = languages;
    apply();
}

void Checker::reset() {
    ++_generation;
    if (_state) {
        _state.reset();
        _backend = nullptr;
#if defined(__GLIBC__)
        // Hunspell holds a dictionary in millions of small blocks; hand the
        // pages back to the system instead of keeping them for reuse.
        malloc_trim(0);
#endif
    }
}

void Checker::apply() {
    const bool wasActive = active();
    reset();
    if (wasActive)
        changed(); // the old dictionaries are gone (while the new ones load)
    if (!_enabled || !_app)
        return;
    auto state               = std::make_shared<State>();
    state->backend           = createPlatformBackend(*_app);
    const auto     preferred = _app->preferredLanguages();
    const uint32_t gen       = _generation;
    auto           ok        = std::make_shared<bool>(false);
    // The languages (the system's when none were chosen) and the load itself
    // run where the backend allows: a worker for Hunspell's directory scan
    // and dictionary reads, the UI thread for the system checkers.
    auto           load      = [state, ok, codes = _languages, preferred]() mutable {
        if (codes.empty())
            codes = defaultLanguages(detail::availableLanguages(), preferred);
        *ok = !codes.empty() && state->backend->load(codes);
    };
    auto then = [this, state, ok, gen] {
        if (gen != _generation || !*ok)
            return; // superseded, or nothing loaded
        _state   = state;
        _backend = state->backend.get();
        changed();
    };
    workerOrPost(*_app, state->backend->loadsOffThread(), std::move(load), std::move(then));
}

void Checker::check(
    std::string text, std::vector<Span> excluded, std::function<void(std::vector<Span>)> done
) {
    if (!_state || !_app) {
        if (_app)
            _app->post([done = std::move(done)] { done({}); });
        return;
    }
    auto st    = _state;
    auto t     = std::make_shared<std::string>(std::move(text));
    auto words = std::make_shared<std::vector<Span>>();
    auto bad   = std::make_shared<std::vector<Span>>();
    if (_backend->threadSafe()) {
        model::runInBackground(
            *_app,
            [st, t, words, bad, excluded = std::move(excluded)] {
                *words = checkableWords(*t, excluded);
                base::Lock lock(st->m);
                for (const Span &w : *words)
                    if (st->isMisspelled(std::string_view(*t).substr(w.start, w.length)))
                        bad->push_back(w);
            },
            [bad, done = std::move(done)] { done(std::move(*bad)); }
        );
        return;
    }
    // A main-thread checker: the words are cut on a worker, then checked here
    // a slice at a time.
    plat::App *app = _app;
    model::runInBackground(
        *_app,
        [t, words, excluded = std::move(excluded)] { *words = checkableWords(*t, excluded); },
        [app, st, t, words, bad, done = std::move(done)]() mutable {
            auto next = std::make_shared<size_t>(0);
            auto step = std::make_shared<std::function<void()>>();
            *step     = [app, st, t, words, bad, next, step, done = std::move(done)] {
                {
                    base::Lock lock(st->m);
                    st->checkSlice(*t, *words, next.get(), bad.get());
                }
                if (*next < words->size()) {
                    app->addTimer(1, false, [step] { (*step)(); });
                    return;
                }
                auto cb     = std::move(done);
                auto result = std::move(*bad);
                auto self   = step;    // keeps the closure alive past the reset below
                *self       = nullptr; // breaks the step ↔ closure cycle
                cb(std::move(result));
            };
            (*step)();
        }
    );
}

void Checker::suggest(
    std::string word, int max, std::function<void(std::vector<std::string>)> done
) {
    if (!_state || !_app) {
        if (_app)
            _app->post([done = std::move(done)] { done({}); });
        return;
    }
    auto st  = _state;
    auto out = std::make_shared<std::vector<std::string>>();
    auto run = [st, out, w = normalized(word), max] {
        base::Lock lock(st->m);
        if (w != st->suggestedFor || max > st->suggestedMax) {
            st->suggestions  = st->backend->suggest(w, max);
            st->suggestedFor = w;
            st->suggestedMax = max;
        }
        out->assign(
            st->suggestions.begin(),
            st->suggestions.begin() + std::min(st->suggestions.size(), size_t(std::max(max, 0)))
        );
    };
    auto then = [out, done = std::move(done)] { done(std::move(*out)); };
    workerOrPost(*_app, _backend->threadSafe(), std::move(run), std::move(then));
}

void Checker::addToDictionary(const std::string &word) {
    if (!_state || !_app)
        return;
    auto st  = _state;
    auto run = [st, w = normalized(word)] {
        base::Lock lock(st->m);
        st->backend->addToDictionary(w); // Linux appends to msga's word list
        st->misspelled.clear();          // "msga" also makes "Msga" right
        st->suggestedFor.clear();
    };
    const uint32_t gen  = _generation;
    auto           then = [this, gen] {
        if (gen == _generation)
            changed();
    };
    // A main-thread checker adds it in a later turn too, like the others.
    workerOrPost(*_app, _backend->threadSafe(), std::move(run), std::move(then));
}

void Checker::ignore(const std::string &word) {
    if (_state) {
        base::Lock lock(_state->m);
        _state->ignored.push_back(normalized(word));
    }
    changed();
}

Checker::ObserverId Checker::observe(std::function<void()> fn) {
    return _observers.add([fn = std::move(fn)](const std::string &) { fn(); });
}

std::vector<std::string> Checker::defaultLanguages(
    const std::vector<Language> &available, const std::vector<std::string> &preferred
) {
    const auto norm = [](std::string c) {
        std::replace(c.begin(), c.end(), '-', '_');
        return c;
    };
    const std::string sys  = preferred.empty() ? std::string() : detail::localeCode(preferred[0]);
    const std::string lang = sys.substr(0, sys.find('_'));
    if (!sys.empty()) {
        for (const Language &l : available)
            if (norm(l.code) == sys)
                return {l.code};
        for (const Language &l : available) {
            const std::string c = norm(l.code);
            if (c.substr(0, c.find('_')) == lang)
                return {l.code};
        }
    }
    if (!available.empty())
        return {available.front().code};
    return {};
}

void Checker::setBackendForTesting(plat::App &app, std::unique_ptr<Backend> backend) {
    _app = &app;
    reset();
    if (backend) {
        _state          = std::make_shared<State>();
        _state->backend = std::move(backend);
        _backend        = _state->backend.get();
    }
    changed();
}

void listLanguages(plat::App &app, std::function<void(Available)> done) {
    auto out = std::make_shared<Available>();
    auto run = [out, preferred = app.preferredLanguages()] {
        out->languages = detail::availableLanguages();
        if (out->languages.empty())
            out->packageHint = detail::dictionaryPackageHint(preferred);
    };
    auto then = [out, done = std::move(done)] { done(std::move(*out)); };
    workerOrPost(app, detail::listsOffThread(), std::move(run), std::move(then));
}

} // namespace spell
