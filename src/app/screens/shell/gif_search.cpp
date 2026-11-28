#include "screens/shell/gif_search.h"

#include "app/screens/common/message_rules.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/str.h"
#include "base/utf8.h"
#include "net/net.h"
#include "plat/plat.h"

#include <cstdlib>
#include <utility>

using i18n::tr;

namespace shell {

namespace {

constexpr char kBase[]   = "https://api.giphy.com/v1/gifs/";
// G through PG-13: GIPHY's explicit tier is left out while the results still
// look like a normal GIF picker's.
constexpr char kRating[] = "pg-13";

// Answers by case-folded, trimmed query; the oldest go past kCacheMax.
struct CacheEntry {
    std::string                      query;
    std::vector<model::Backend::Gif> gifs;
};
constexpr size_t         kCacheMax = 64;
std::vector<CacheEntry> &cache() {
    static std::vector<CacheEntry> c;
    return c;
}

std::string cacheKey(std::string_view query) {
    return utf8::foldCase(str::trim(query));
}

// Literally a .gif URL
// narrower than screens::isGifPath, which also matches .webp
// prefers a rendition any GIF-only client can decode
bool isLiterallyGif(std::string_view url) {
    return str::iequals(screens::pathExtension(url), "gif");
}

// One rendition of a GIPHY images object. Sizes come as strings ("200").
bool rendition(json::Value images, const char *name, std::string *url, int *w, int *h) {
    const json::Value r = images[name];
    if (r["url"].str().empty())
        return false;
    *url = std::string(r["url"].str());
    if (w && h) {
        const int rw = std::atoi(std::string(r["width"].str()).c_str());
        const int rh = std::atoi(std::string(r["height"].str()).c_str());
        if (rw > 0 && rh > 0)
            *w = rw, *h = rh;
    }
    return true;
}

} // namespace

GifSearch::GifSearch(plat::App &app) : _app(app), _client(std::make_unique<net::Client>(app)) {}

GifSearch::~GifSearch() = default; // the Client cancels what is in flight

std::string GifSearch::requestUrl(std::string_view query, int limit, std::string_view key) {
    const std::string_view q        = str::trim(query);
    const bool             trending = q.empty();
    std::string            url      = str::concat({kBase, trending ? "trending" : "search"});
    url += "?api_key=";
    url += net::percentEncode(key);
    if (!trending) {
        // GIPHY caps the search term at 50 characters and answers 400 past it.
        url += "&q=";
        url += net::percentEncode(q.substr(0, utf8::prefixBytes(q, 50)));
    }
    url += "&limit=";
    url += str::number(int64_t(std::min(50, std::max(1, limit))));
    url += "&rating=";
    url += kRating;
    // The messaging rendition set rather than all ~20: the picker reads a
    // preview and one send-size rendition only.
    url += "&bundle=messaging_non_clips";
    return url;
}

std::vector<model::Backend::Gif> GifSearch::parseResponse(std::string_view body) {
    std::vector<model::Backend::Gif> out;
    json::Document                   d;
    if (!d.parse(std::string(body), nullptr))
        return out;
    for (const json::Value hit : d.root()["data"]) {
        const json::Value   images = hit["images"];
        model::Backend::Gif g;
        // Preview: the fixed-width renditions are 200 px wide, about a picker
        // column; coverage varies per GIF, lightest first.
        if (!rendition(images, "fixed_width_downsampled", &g.preview, &g.width, &g.height) &&
            !rendition(images, "fixed_width_small", &g.preview, &g.width, &g.height) &&
            !rendition(images, "fixed_width", &g.preview, &g.width, &g.height) &&
            !rendition(images, "preview_gif", &g.preview, &g.width, &g.height))
            continue;
        // Posted rendition -- prefer whichever GIPHY serves as a literal
        // .gif, tried in order:
        // - fixed_width: the 200px size
        // - downsized: ≤2MB fallback
        // - downsized_medium: ≤5MB fallback
        // - original: no url field, only mp4/webp
        //
        // Why check: fixed_width can come back as WebP under
        // bundle=messaging_non_clips (confirmed live)
        // trusting the field name would post a picture a GIF-only client can't decode
        for (const char *key : {"fixed_width", "downsized", "downsized_medium"}) {
            std::string url;
            if (rendition(images, key, &url, nullptr, nullptr) && isLiterallyGif(url)) {
                g.url = std::move(url);
                break;
            }
        }
        if (g.url.empty())
            g.url = g.preview;
        g.title = std::string(hit["alt_text"].str());
        if (g.title.empty())
            g.title = std::string(hit["title"].str());
        out.push_back(std::move(g));
    }
    return out;
}

std::string GifSearch::errorMessage(int httpStatus) {
    if (httpStatus == 429)
        return tr("GIPHY rate limit reached. Try again shortly.");
    if (httpStatus > 0)
        return i18n::arg(tr("GIPHY request failed (HTTP %1)."), str::number(int64_t(httpStatus)));
    return tr("Could not reach GIPHY \xE2\x80\x94 check your connection.");
}

void GifSearch::cancel() {
    if (_inflight)
        _client->cancel(std::exchange(_inflight, 0));
}

void GifSearch::search(std::string query, std::string key, std::function<void(Result)> done) {
    cancel();
    ++_generation; // a cached or keyless answer still to be posted is superseded too
    const std::string ck    = cacheKey(query);
    // Answered later like a fetch, never inside this call.
    const auto        later = [this](Result r, std::function<void(Result)> cb) {
        std::weak_ptr<char> alive = _alive;
        _app.post([this, alive, gen = _generation, r = std::move(r), cb = std::move(cb)]() mutable {
            if (!alive.expired() && gen == _generation)
                cb(std::move(r));
        });
    };
    for (const CacheEntry &e : cache())
        if (e.query == ck) {
            Result r;
            r.gifs = e.gifs;
            return later(std::move(r), std::move(done));
        }
    key = std::string(str::trim(key));
    if (key.empty()) {
        Result r;
        r.error       = tr("No GIPHY API key configured.");
        r.keyRejected = true;
        return later(std::move(r), std::move(done));
    }
    net::Request req;
    req.url       = requestUrl(query, kDefaultLimit, key);
    req.timeoutMs = 15000;
    _inflight =
        _client->send(std::move(req), [this, ck, done = std::move(done)](net::Response resp) {
            _inflight = 0;
            Result r;
            if (resp.status == 401 || resp.status == 403) {
                r.error       = tr("GIPHY rejected this API key.");
                r.keyRejected = true;
            } else if (!resp.ok()) {
                // Never resp.error: transport details may carry the URL, and the
                // URL carries the key.
                r.error = errorMessage(resp.status);
            } else {
                r.gifs  = parseResponse(resp.body);
                auto &c = cache();
                if (c.size() >= kCacheMax)
                    c.erase(c.begin());
                c.push_back({ck, r.gifs});
            }
            done(std::move(r));
        });
}

} // namespace shell
