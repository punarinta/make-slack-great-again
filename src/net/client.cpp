// net::Client: a small pool of worker threads running blocking transport
// exchanges, plus what every OS stack is told not to do itself — following
// redirects and carrying cookies along them (see net.h).
#include "base/str.h"
#include "base/thread.h"
#include "net/net.h"
#include "net/transport.h"
#include "plat/plat.h"

#include <algorithm>
#include <atomic>

namespace net {

namespace {

constexpr int kMaxWorkers = 4;

// A worker with nothing to do for this long ends; the next burst starts
// new ones. Slack's polling keeps one busy, an idle app has none.
std::atomic<int> g_workerIdleMs{30000};

using str::iequals;

// "team.slack.com" → "slack.com": the scope a caller's own Cookie header
// keeps across redirects (the Slack boot chain stays on *.slack.com).
std::string_view siteOf(std::string_view host) {
    const size_t last = host.rfind('.');
    if (last == std::string_view::npos || last == 0)
        return host;
    const size_t prev = host.rfind('.', last - 1);
    return prev == std::string_view::npos ? host : host.substr(prev + 1);
}

bool domainMatches(std::string_view host, std::string_view domain, bool hostOnly) {
    if (hostOnly)
        return host == domain;
    return host == domain || (host.size() > domain.size() && str::endsWith(host, domain) &&
                              host[host.size() - domain.size() - 1] == '.');
}

// The cookies of one redirect chain.
struct Jar {
    struct Cookie {
        std::string name, value, domain;
        bool        hostOnly = false;
    };
    std::vector<Cookie> cookies;

    void set(std::string name, std::string value, std::string domain, bool hostOnly) {
        for (auto &c : cookies)
            if (c.name == name && c.domain == domain) {
                c.value    = std::move(value);
                c.hostOnly = hostOnly;
                return;
            }
        cookies.push_back({std::move(name), std::move(value), std::move(domain), hostOnly});
    }
    void erase(std::string_view name, std::string_view domain) {
        for (size_t i = 0; i < cookies.size(); ++i)
            if (cookies[i].name == name && cookies[i].domain == domain) {
                cookies.erase(cookies.begin() + i);
                return;
            }
    }
    // A caller's "Cookie: a=1; b=2", scoped to the request's site.
    void seed(std::string_view header, std::string_view host) {
        while (!header.empty()) {
            const size_t     semi = header.find(';');
            std::string_view pair = str::trim(header.substr(0, semi));
            header = semi == std::string_view::npos ? std::string_view() : header.substr(semi + 1);
            const size_t eq = pair.find('=');
            if (eq == std::string_view::npos || eq == 0)
                continue;
            set(std::string(pair.substr(0, eq)),
                std::string(pair.substr(eq + 1)),
                std::string(siteOf(host)),
                false);
        }
    }
    // One Set-Cookie header from `host`'s response.
    void take(std::string_view header, std::string_view host) {
        const size_t     semi = header.find(';');
        std::string_view pair = str::trim(header.substr(0, semi));
        const size_t     eq   = pair.find('=');
        if (eq == std::string_view::npos || eq == 0)
            return;
        std::string      domain(host);
        bool             hostOnly = true, expired = false;
        std::string_view attrs =
            semi == std::string_view::npos ? std::string_view() : header.substr(semi + 1);
        while (!attrs.empty()) {
            const size_t     s = attrs.find(';');
            std::string_view a = str::trim(attrs.substr(0, s));
            attrs = s == std::string_view::npos ? std::string_view() : attrs.substr(s + 1);
            const size_t     e = a.find('=');
            std::string_view k = a.substr(0, e);
            std::string_view v = e == std::string_view::npos ? std::string_view() : a.substr(e + 1);
            if (iequals(k, "domain") && !v.empty()) {
                if (v[0] == '.')
                    v.remove_prefix(1);
                const std::string d = str::asciiLower(v);
                if (domainMatches(host, d, false)) { // never for a foreign domain
                    domain   = d;
                    hostOnly = false;
                }
            } else if (iequals(k, "max-age")) {
                expired = !v.empty() && (v[0] == '-' || v == "0");
            }
        }
        const std::string name(pair.substr(0, eq));
        if (expired)
            erase(name, domain);
        else
            set(name, std::string(pair.substr(eq + 1)), domain, hostOnly);
    }
    std::string headerFor(std::string_view host) const {
        std::string out;
        for (const auto &c : cookies) {
            if (!domainMatches(host, c.domain, c.hostOnly))
                continue;
            if (!out.empty())
                out += "; ";
            out += c.name;
            out += '=';
            out += c.value;
        }
        return out;
    }
};

struct Job {
    RequestId                       id = 0;
    Request                         req;
    std::shared_ptr<detail::Cancel> cancel;
    bool                            progress = false; // the caller set onProgress
};

// Every worker may park: a burst's threads wait for the next one.
base::WorkerPool::Options poolOptions() {
    return {
        .maxWorkers = kMaxWorkers,
        .maxParked  = kMaxWorkers,
        .idleMs     = g_workerIdleMs.load(),
        .stackBytes = detail::kThreadStack
    };
}

} // namespace

struct Client::Impl : std::enable_shared_from_this<Client::Impl> {
    explicit Impl(plat::App &a) : app(a), pool(poolOptions()) {}

    // A request the caller is still waiting for (UI thread only).
    struct Pending {
        RequestId                             id = 0;
        std::function<void(Response)>         done;
        std::shared_ptr<detail::Cancel>       flag;
        std::function<void(int64_t, int64_t)> progress; // null: the caller set none
    };

    plat::App           &app;
    // UI thread only: a handful in flight, so a plain vector.
    RequestId            nextId = 1;
    std::vector<Pending> pending;

    // Shared with the workers. The queue is ours, not the pool's, so that
    // cancel() can take a request out: a worker takes the oldest per task.
    base::Mutex      mutex;
    std::vector<Job> queue;
    bool             stopping = false;
    base::WorkerPool pool;

    Pending *find(RequestId id);
    void     forget(Pending *p);
    void     runNext();
    void     run(Job &job);
    void     deliver(RequestId id, Response resp);
    void     report(RequestId id, int64_t received, int64_t total);
};

namespace detail {

void setWorkerIdleMs(int ms) {
    g_workerIdleMs.store(ms);
}

bool parseContentLength(std::string_view v, int64_t *out) {
    v = str::trim(v);
    if (v.empty())
        return false;
    int64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9' || n > (int64_t(1) << 50))
            return false;
        n = n * 10 + (c - '0');
    }
    *out = n;
    return true;
}

int64_t contentLength(const std::vector<Header> &headers) {
    int64_t n = 0;
    return parseContentLength(headerValue(headers, "content-length"), &n) ? n : 0;
}

std::string_view headerValue(const std::vector<Header> &headers, std::string_view name) {
    for (const auto &h : headers)
        if (iequals(h.name, name))
            return h.value;
    return {};
}

bool parseHeaderLines(std::string_view text, std::vector<Header> *out) {
    while (!text.empty()) {
        const size_t     eol  = text.find("\r\n");
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 2);
        if (line.empty())
            break;
        if (line[0] == ' ' || line[0] == '\t') { // obsolete line folding
            if (out->empty())
                return false;
            out->back().value += ' ';
            out->back().value += str::trim(line);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return false;
        out->push_back(
            {std::string(line.substr(0, colon)), std::string(str::trim(line.substr(colon + 1)))}
        );
    }
    return true;
}

} // namespace detail

std::string_view Response::header(std::string_view name) const {
    return detail::headerValue(headers, name);
}

Client::Impl::Pending *Client::Impl::find(RequestId id) {
    for (auto &p : pending)
        if (p.id == id)
            return &p;
    return nullptr;
}

void Client::Impl::forget(Pending *p) {
    if (p != &pending.back())
        *p = std::move(pending.back());
    pending.pop_back();
}

void Client::Impl::runNext() {
    Job job;
    {
        base::Lock lock(mutex);
        if (stopping || queue.empty())
            return; // cancelled meanwhile: its task has nothing to do
        job = std::move(queue.front());
        queue.erase(queue.begin());
    }
    run(job);
}

void Client::Impl::run(Job &job) {
    if (job.cancel->isSet())
        return; // cancelled while it waited in the queue: never sent
    Response resp;
    Url      url;
    if (!url.parse(job.req.url) || url.scheme == "ws" || url.scheme == "wss") {
        resp.error = "url";
        resp.url   = job.req.url;
        if (job.req.handler)
            job.req.handler->finished(resp);
        deliver(job.id, std::move(resp));
        return;
    }
    Request         &req = job.req;
    Jar              jar;
    // Posted about once a percent (every 256 KiB when the size is unknown):
    // a hundred UI-thread hops for a whole download, whatever the transport.
    detail::Progress progress;
    if (job.progress)
        progress = [this, id = job.id, posted = int64_t(-1)](int64_t got, int64_t total) mutable {
            const int64_t step = total > 0 ? std::max<int64_t>(total / 100, 16 * 1024) : 256 * 1024;
            if (posted >= 0 && got - posted < step && got != total)
                return;
            if (got == posted)
                return;
            posted = got;
            report(id, got, total);
        };
    for (size_t i = 0; i < req.headers.size();)
        if (iequals(req.headers[i].name, "cookie")) {
            jar.seed(req.headers[i].value, url.host);
            req.headers.erase(req.headers.begin() + i);
        } else {
            ++i;
        }
    bool agent = false;
    for (const auto &h : req.headers)
        agent = agent || iequals(h.name, "user-agent");
    if (!agent)
        req.headers.push_back({"User-Agent", detail::kUserAgent});
    const std::string firstHost = url.host;
    // Every hop sends `req` itself, its headers adjusted in place: the body
    // (an upload can be tens of megabytes) is never copied.
    for (int hop = 0;; ++hop) {
        if (url.host != firstHost) { // credentials never leak to another host, nor come back
            for (size_t i = 0; i < req.headers.size();) {
                if (iequals(req.headers[i].name, "authorization"))
                    req.headers.erase(req.headers.begin() + i);
                else
                    ++i;
            }
        }
        if (job.cancel->isSet())
            return; // no next hop for a request cancelled meanwhile
        const std::string cookie = jar.headerFor(url.host);
        if (!cookie.empty())
            req.headers.push_back({"Cookie", cookie});
        resp = Response();
        detail::perform(url, req, resp, *job.cancel, progress);
        if (!cookie.empty())
            req.headers.pop_back();
        resp.url = url.str();
        if (!resp.error.empty() || resp.status < 300 || resp.status >= 400 || resp.status == 304)
            break;
        const std::string_view location = resp.header("location");
        if (location.empty() || req.maxRedirects <= 0)
            break;
        if (hop >= req.maxRedirects) {
            resp.status = 0;
            resp.error  = "too_many_redirects";
            break;
        }
        for (const auto &h : resp.headers)
            if (iequals(h.name, "set-cookie"))
                jar.take(h.value, url.host);
        Url next;
        if (!next.parse(url.resolve(location)) || next.scheme == "ws" || next.scheme == "wss")
            break; // hand the 3xx back as is
        // What browsers do: 303 always, and 301/302 after a POST, become a GET.
        if (resp.status == 303 ||
            ((resp.status == 301 || resp.status == 302) && req.method == "POST")) {
            req.method = "GET";
            req.body.clear();
            for (size_t i = 0; i < req.headers.size();)
                if (iequals(req.headers[i].name, "content-type"))
                    req.headers.erase(req.headers.begin() + i);
                else
                    ++i;
        }
        url = std::move(next);
    }
    if (req.handler)
        req.handler->finished(resp);
    deliver(job.id, std::move(resp));
}

void Client::Impl::report(RequestId id, int64_t received, int64_t total) {
    std::weak_ptr<Impl> weak = weak_from_this();
    {
        base::Lock lock(mutex);
        if (stopping)
            return;
    }
    // Posted before deliver()'s, so it runs first: never after done.
    app.post([weak, id, received, total] {
        auto self = weak.lock();
        if (!self)
            return;
        Pending *p = self->find(id);
        if (!p || !p->progress)
            return;            // cancelled or answered
        auto fn = p->progress; // it may cancel itself
        fn(received, total);
    });
}

void Client::Impl::deliver(RequestId id, Response resp) {
    std::weak_ptr<Impl> weak = weak_from_this();
    {
        base::Lock lock(mutex);
        if (stopping)
            return; // the destructor is waiting for us; nothing to deliver to
    }
    app.post([weak, id, resp = std::move(resp)]() mutable {
        auto self = weak.lock();
        if (!self)
            return;
        Pending *p = self->find(id);
        if (!p)
            return; // cancelled
        auto fn = std::move(p->done);
        self->forget(p);
        if (fn)
            fn(std::move(resp));
    });
}

// How many Clients are alive, so the last one out can drop the shared
// keep-alive pool (its sockets would otherwise sit open until the process
// ends — and a leak check would flag them).
static std::atomic<int> g_liveClients{0};

void releaseCaches() {
    detail::releaseCaches();
}

Client::Client(plat::App &app) : _impl(std::make_shared<Impl>(app)) {
    g_liveClients.fetch_add(1, std::memory_order_relaxed);
}

Client::~Client() {
    {
        base::Lock lock(_impl->mutex);
        _impl->stopping = true;
        for (auto &p : _impl->pending)
            p.flag->set();
        _impl->queue.clear();
    }
    _impl->pool.stop(); // joins the workers
    _impl->pending.clear();
    if (g_liveClients.fetch_sub(1, std::memory_order_acq_rel) == 1)
        detail::closeIdleConnections();
}

RequestId Client::send(Request req, std::function<void(Response)> done) {
    Impl           &d        = *_impl;
    const RequestId id       = d.nextId++;
    auto            flag     = std::make_shared<detail::Cancel>();
    // The callbacks stay on the UI thread; the worker only knows whether
    // there is a progress one.
    const bool      progress = bool(req.onProgress);
    d.pending.push_back({id, std::move(done), flag, std::move(req.onProgress)});
    req.onProgress = nullptr;
    {
        base::Lock lock(d.mutex);
        d.queue.push_back({id, std::move(req), flag, progress});
    }
    Impl *raw = &d; // the destructor joins every worker before Impl goes
    d.pool.post([raw] { raw->runNext(); });
    return id;
}

void Client::cancel(RequestId id) {
    Impl          &d = *_impl;
    Impl::Pending *p = d.find(id);
    if (!p)
        return;
    p->flag->set();
    // Destroyed after the bookkeeping: a callback's captures may cancel more.
    Impl::Pending gone = std::move(*p);
    d.forget(p);
    // Still queued: it goes now (run() would skip it anyway).
    base::Lock lock(d.mutex);
    for (auto q = d.queue.begin(); q != d.queue.end(); ++q)
        if (q->id == id) {
            d.queue.erase(q);
            break;
        }
}

} // namespace net
