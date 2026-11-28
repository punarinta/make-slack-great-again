#include "app/screens/messages/thread_export.h"

#include "app/model/jobs.h"
#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "app/screens/messages/rich.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

using i18n::arg;
using i18n::tr;

namespace screens {

namespace {

// Messages resolved per UI-loop turn (names, mentions, emoji need the Store,
// which is the UI thread's), so a thread of thousands never stalls a frame.
constexpr size_t kSlice = 200;

struct Raw { // what a transcript entry needs, copied out of the Store
    Ts                       ts = 0;
    std::string              who, text;
    bool                     edited = false;
    std::vector<std::string> files;
};

struct Export {
    Context                  &ctx;
    ConvRef                   conv;
    Ts                        root;
    std::string               convId, title, path;
    std::function<void(bool)> done;
    int                       job      = 0;
    model::Store::ObserverId  observer = 0;
    bool                      over     = false; // failed, or handed to the worker
    std::vector<Raw>          raw;
    std::vector<std::string>  entries; // resolved transcript blocks, oldest first
    size_t                    next = 0;

    Export(Context &c, ConvRef cv, Ts r) : ctx(c), conv(cv), root(r) {}
    // The backend dropped its callback (its workspace closed, or the app is
    // quitting): the job must not outlive its task. The Store may be gone
    // already, so its observer is left to drop itself (see exportThread).
    ~Export() {
        if (!over)
            model::jobs().end(job);
    }

    // Still the conversation the export started in (a workspace switch
    // clears the Store; ConvRefs are reused by the next workspace).
    bool valid() const {
        return conv < ctx.store().conversationCount() &&
               ctx.store().conversation(conv).id == convId;
    }
    void stop() {
        over = true;
        if (observer)
            ctx.store.unobserve(observer);
        observer = 0;
        model::jobs().end(job);
        job = 0;
    }
    void fail(const std::string &why) {
        if (over)
            return;
        LOG_WARN("messages", "thread export failed: %s", why.c_str());
        stop();
        if (done)
            ctx.app.platform().post([d = std::move(done)] { d(false); });
    }
};

void collect(Export &x) {
    const Store                        &st = x.ctx.store;
    std::vector<const model::Message *> msgs;
    if (const model::Message *r = st.findMessage(x.conv, x.root))
        msgs.push_back(r);
    if (const auto *rs = st.replies(x.conv, x.root))
        for (const auto &m : *rs)
            if (!m.pending)
                msgs.push_back(&m);
    // Bulletproof against pagination quirks: the root, repeated on every
    // page, is deduplicated, and all is re-sorted by time.
    std::stable_sort(msgs.begin(), msgs.end(), [](auto *a, auto *b) { return a->ts < b->ts; });
    msgs.erase(
        std::unique(msgs.begin(), msgs.end(), [](auto *a, auto *b) { return a->ts == b->ts; }),
        msgs.end()
    );
    x.raw.reserve(msgs.size());
    for (const model::Message *m : msgs) {
        Raw r;
        r.ts     = m->ts;
        r.who    = authorName(st, *m);
        r.text   = m->text;
        r.edited = m->edited;
        for (const model::File &f : m->files())
            r.files.push_back(f.name);
        x.raw.push_back(std::move(r));
    }
}

std::string entry(const Context &ctx, const Raw &m, int64_t now) {
    const int64_t s   = model::tsSecs(m.ts);
    std::string   out = str::concat(
        {m.who,
         " \xE2\x80\x94 ",
         base::formatDate(s, now),
         " ",
         base::formatTime(s),
         m.edited ? " " : "",
         m.edited ? tr("(edited)") : "",
         "\n"}
    );
    const std::string text(str::trim(plainText(ctx.store(), m.text)));
    if (!text.empty())
        out += text + "\n";
    for (const std::string &f : m.files)
        out += arg(tr("[file: %1]"), f.empty() ? tr("untitled") : f) + "\n";
    out += "\n";
    return out;
}

void write(std::shared_ptr<Export> x) {
    // From here the worker owns the transcript; the Store is not read again.
    if (x->observer)
        x->ctx.store.unobserve(x->observer);
    x->observer = 0;
    x->over     = true; // ended below, on the UI thread (not by ~Export on a worker)
    auto out    = std::make_shared<std::string>();
    if (!x->title.empty())
        *out += arg(tr("Thread in %1"), x->title) + "\n";
    *out += arg(tr("Messages: %1"), str::number(int64_t(x->entries.size()))) + "\n\n";
    auto entries = std::make_shared<std::vector<std::string>>(std::move(x->entries));
    auto ok      = std::make_shared<bool>(false);
    model::runInBackground(
        x->ctx.app.platform(),
        [out, entries, ok, path = x->path] {
            size_t n = out->size();
            for (const std::string &e : *entries)
                n += e.size();
            out->reserve(n);
            for (const std::string &e : *entries)
                *out += e;
            *ok = file::writeAtomic(path, *out);
        },
        [x, ok] {
            model::jobs().end(x->job);
            x->job = 0;
            if (!*ok)
                LOG_WARN("messages", "thread export failed: cannot write %s", x->path.c_str());
            if (x->done)
                x->done(*ok);
        }
    );
}

// One slice of names and text, then the next on a later loop turn.
void resolve(std::shared_ptr<Export> x) {
    if (x->over)
        return;
    if (!x->valid()) {
        x->fail("the workspace changed");
        return;
    }
    const int64_t now = base::nowSecs();
    const size_t  end = std::min(x->raw.size(), x->next + kSlice);
    for (; x->next < end; ++x->next)
        x->entries.push_back(entry(x->ctx, x->raw[x->next], now));
    if (x->next < x->raw.size()) {
        x->ctx.app.platform().post([x] { resolve(x); });
        return;
    }
    x->raw.clear();
    write(std::move(x));
}

} // namespace

std::string threadExportTitle(const Store &st, ConvRef conv) {
    if (conv >= st.conversationCount())
        return {};
    // A DM or group DM by its people (or a renamed session's name), which
    // the link chips' place label leaves out; a channel as they name it.
    const model::Conversation &c = st.conversation(conv);
    return c.isDirect() ? st.displayName(conv) : placeLabel(st, c.id);
}

void exportThread(
    Context                  &ctx,
    ConvRef                   conv,
    Ts                        root,
    std::string               title,
    std::string               path,
    std::function<void(bool)> done
) {
    if (conv >= ctx.store().conversationCount() || !root || path.empty()) {
        if (done)
            ctx.app.platform().post([done = std::move(done)] { done(false); });
        return;
    }
    auto x                   = std::make_shared<Export>(ctx, conv, root);
    x->convId                = ctx.store().conversation(conv).id;
    x->title                 = std::move(title);
    x->path                  = std::move(path);
    x->done                  = std::move(done);
    x->job                   = model::jobs().begin(tr("Downloading thread…"));
    // A workspace switch or sign-out clears the Store: the replies won't
    // come (or come for nobody), so the export ends there instead of
    // leaving its job spinning. Weak: the Store doesn't keep it alive.
    std::weak_ptr<Export> w  = x;
    auto                  id = std::make_shared<model::Store::ObserverId>(0);
    model::StoreSlot     &st = ctx.store; // a switch moves the observer (and says Roster)
    *id = x->observer = st.observe(Store::kAnyConv, [w, id, &st](const model::Change &ch) {
        auto s = w.lock();
        if (!s) { // ended without unobserving (~Export)
            st.unobserve(*id);
            return;
        }
        if (ch.kind == model::ChangeKind::Roster && !s->valid())
            s->fail("the workspace changed");
    });
    ctx.backend().loadThread(conv, root, [x](bool ok, const std::string &err) {
        if (x->over)
            return;
        if (!ok) {
            x->fail(err.empty() ? std::string("thread page fetch failed") : err);
            return;
        }
        if (!x->valid()) {
            x->fail("the workspace changed");
            return;
        }
        collect(*x);
        resolve(x);
    });
}

} // namespace screens
