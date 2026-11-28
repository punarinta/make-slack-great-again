// Adding a Slack workspace: sign in through a
// driven browser (the headline path), import from the local Slack app, or
// paste the `d` cookie + the workspace address; the xoxc token is derived.
// "Use app keys (OAuth) instead" hands over to the OAuth flow.
#pragma once

#include "app/slack/credentials.h"
#include "screens/common/context.h"

#include <functional>
#include <vector>

namespace net {
class Client;
}

namespace shell {

struct SessionImportHooks {
    // Verified workspaces (one or more), with session credentials.
    std::function<void(std::vector<slack::Credentials>)> imported;
    std::function<void()>                                useAppKeys; // the dialog closes first
};

// A message box: a title, the text, OK.
ui::Popup *showMessage(ui::Window &w, std::string title, std::string text);

ui::Popup *showSessionImportDialog(
    screens::Context &ctx, ui::Window &w, net::Client &client, SessionImportHooks hooks
);

} // namespace shell
