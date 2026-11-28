// Slack sign-in with app keys: the browser opens Slack's
// authorize page; Slack redirects to msga://oauth/callback?code=…&state=…,
// which the OS hands back to the running app (plat OpenUrls / a second
// launch's argv); the code (+ PKCE verifier) is exchanged for a user token.
#pragma once

#include "app/slack/credentials.h"
#include "net/net.h"

#include <functional>
#include <string>

namespace plat {
class App;
}

namespace slack {

inline constexpr const char *kOAuthRedirectUri = "msga://oauth/callback";

class OAuthFlow {
public:
    // done(creds, error): error "" on success; "cancelled" is not an error
    // the UI shows. Other errors are Slack's codes or a sentence.
    using Done = std::function<void(Credentials creds, std::string error)>;

    OAuthFlow(plat::App &app, net::Client &client, AppConfig config);
    ~OAuthFlow(); // no callback afterwards

    // Opens the browser. With no client id, done fails at once (later).
    void start(Done done);
    // A msga:// URL from the OS. True if it was this flow's callback.
    bool handleCallback(std::string_view url);

    // The user scopes msga asks for.
    static const char *userScopes();

private:
    void exchange(const std::string &code);
    void fail(std::string why);

    plat::App     &_app;
    net::Client   &_client;
    AppConfig      _config;
    std::string    _verifier, _state;
    Done           _done;
    net::RequestId _pending = 0;
};

} // namespace slack
