// GIPHY GIF search (https://developers.giphy.com/docs/api): the
// composer's GIF picker for every workspace whose
// backend has no search of its own (Backend::gifSearchAvailable — only the
// demo's stand-in has one). GIF search is not a Slack feature: it runs here,
// over src/net, with the key from Settings → System (shell::Settings::giphyKey).
//
// One search at a time: a new query cancels the one in flight, because the
// picker re-queries as the user types and only the newest answer is wanted.
// Answers are cached per query for the life of the process: a free GIPHY key
// allows 100 calls an hour, which search-as-you-type would burn through in a
// sitting; backspacing to a query already fetched costs nothing.
#pragma once

#include "model/backend.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}
namespace net {
class Client;
}

namespace shell {

class GifSearch {
public:
    explicit GifSearch(plat::App &app);
    ~GifSearch(); // cancels: no callback runs afterwards

    struct Result {
        std::vector<model::Backend::Gif> gifs;
        std::string                      error; // user-facing; "" = the gifs are the answer
        // A failure a new key would fix (no key, or one GIPHY refused): the
        // picker offers the setup form again.
        bool                             keyRejected = false;
    };
    // An empty query fetches the trending set (what the picker shows before
    // anything is typed). `done` always runs later, on the UI thread.
    void search(std::string query, std::string key, std::function<void(Result)> done);
    void cancel();

    // GIPHY caps a beta key at 50 per call; 30 fills the panel without waste.
    static constexpr int kDefaultLimit = 30;

    // The request URL for a query, key applied (tests).
    static std::string requestUrl(std::string_view query, int limit, std::string_view key);
    // A search / trending body → GIFs; entries without a usable rendition
    // are skipped (tests).
    static std::vector<model::Backend::Gif> parseResponse(std::string_view body);
    // The message for a failed request, from the status alone (0: GIPHY was
    // never reached). Never the transport's own text: the URL carries the key.
    static std::string                      errorMessage(int httpStatus);

private:
    plat::App                   &_app;
    std::unique_ptr<net::Client> _client;
    uint64_t                     _inflight   = 0;
    uint32_t                     _generation = 0;
    std::shared_ptr<char>        _alive      = std::make_shared<char>(0);
};

} // namespace shell
