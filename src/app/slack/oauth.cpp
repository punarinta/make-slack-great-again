#include "app/slack/oauth.h"

#include "app/slack/web_api.h"
#include "base/crypto.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

namespace slack {

namespace {

std::string randomToken(size_t bytes) {
    uint8_t buf[32];
    if (bytes > sizeof buf || !crypto::randomBytes(buf, bytes))
        return {};
    return crypto::base64url({reinterpret_cast<const char *>(buf), bytes});
}

} // namespace

const char *OAuthFlow::userScopes() {
    // Tokens issued before a scope was added lack it until the user signs in
    // again; those calls fail with missing_scope.
    return "channels:history,groups:history,im:history,mpim:history,channels:read,groups:read,"
           "im:read,mpim:read,users:read,team:read,emoji:read,reactions:read,files:read,"
           "users.profile:read,search:read,chat:write,reactions:write,files:write,stars:read,"
           "stars:write,channels:write,groups:write,mpim:write,im:write,users:write,"
           "users.profile:write,dnd:write,dnd:read,pins:write,canvases:read,canvases:write,"
           "usergroups:read,bookmarks:read";
}

OAuthFlow::OAuthFlow(plat::App &app, net::Client &client, AppConfig config)
    : _app(app), _client(client), _config(std::move(config)) {}

OAuthFlow::~OAuthFlow() {
    _done = nullptr;
    if (_pending)
        _client.cancel(_pending);
}

void OAuthFlow::fail(std::string why) {
    Done d = std::move(_done);
    if (d)
        d({}, std::move(why));
}

void OAuthFlow::start(Done done) {
    _done = std::move(done);
    if (_config.clientId.empty()) {
        // Keys can be pasted at runtime, so lead with that.
        _app.post([this] {
            fail(
                i18n::tr(
                    "No Slack app keys are set up yet.\n\nOpen Settings \xE2\x86\x92 System, "
                    "choose "
                    "\xE2\x80\x9CSlack app keys\xE2\x80\x9D, and paste your client ID, client "
                    "secret "
                    "and app token. Building msga yourself? Put them in credentials.cmake instead "
                    "and rebuild."
                )
            );
        });
        return;
    }
    // PKCE (RFC 7636): 32 random bytes → a 43-char verifier.
    _verifier = randomToken(32);
    _state    = randomToken(12);
    if (_verifier.empty() || _state.empty()) {
        _app.post([this] { fail("no_random"); });
        return;
    }
    const auto  digest    = crypto::sha256(_verifier);
    std::string challenge = crypto::base64url(crypto::bytes(digest));
    std::string url       = str::concat(
        {"https://slack.com/oauth/v2/authorize?",
         net::formEncode({
             {"client_id", _config.clientId},
             {"user_scope", userScopes()},
             {"redirect_uri", kOAuthRedirectUri},
             {"state", _state},
             {"code_challenge", challenge},
             {"code_challenge_method", "S256"},
         })}
    );
    _app.openUrl(url);
}

bool OAuthFlow::handleCallback(std::string_view url) {
    static constexpr std::string_view kPrefix = "msga://oauth/callback";
    // _pending: the code is already being exchanged; a second exchange
    // would race the first (and fail as invalid_code).
    if (!_done || _pending || !str::startsWith(url, kPrefix))
        return false;
    std::string_view query = url.substr(kPrefix.size());
    if (!query.empty() && query[0] == '/')
        query.remove_prefix(1);
    if (query.empty() || query[0] != '?')
        return false;
    if (const std::string err = net::queryValue(query, "error"); !err.empty()) {
        fail(err);
        return true;
    }
    if (net::queryValue(query, "state") != _state) {
        fail("state_mismatch");
        return true;
    }
    exchange(net::queryValue(query, "code"));
    return true;
}

void OAuthFlow::exchange(const std::string &code) {
    // No token yet: the client's own keys in the form.
    std::string form = net::formEncode({
        {"client_id", _config.clientId},
        {"client_secret", _config.clientSecret},
        {"code", code},
        {"redirect_uri", kOAuthRedirectUri},
        {"code_verifier", _verifier},
    });
    _pending         = apiCall(
        _client,
        {},
        "oauth.v2.access",
        std::move(form),
        [this](const json::Document &doc, const std::string &err) {
            _pending = 0;
            if (!err.empty()) {
                fail(err);
                return;
            }
            const json::Value o    = doc.root();
            const json::Value user = o["authed_user"];
            Credentials       c;
            c.token                 = std::string(user["access_token"].str());
            c.refreshToken          = std::string(user["refresh_token"].str());
            const int64_t expiresIn = user["expires_in"].integer();
            c.expiresAt             = expiresIn > 0 ? base::nowSecs() + expiresIn : 0;
            c.teamId                = std::string(o["team"]["id"].str());
            c.teamName              = std::string(o["team"]["name"].str());
            // The icon is best effort.
            _pending                = apiCall(
                _client,
                {c.token, {}},
                "team.info",
                {},
                [this, c](const json::Document &d, const std::string &err) mutable {
                    _pending = 0;
                    if (err.empty())
                        c.iconUrl = std::string(d.root()["team"]["icon"]["image_88"].str());
                    Done done = std::move(_done);
                    if (done)
                        done(std::move(c), {});
                }
            );
        }
    );
}

} // namespace slack
