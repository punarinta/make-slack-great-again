// The channel dialogs:
//
//   Find a channel   a search field ("Search
//                    for channels" / "Search for people"), "Create channel",
//                    the close button; the Channels and People tabs. Channel
//                    rows: hash or lock, the name, "N members · topic",
//                    "Joined"; people: the picture, the name, "@handle".
//   Create a channel two steps: the name
//                    ("# e.g. plan-budget", Next), then Public / Private
//                    (Back, Create).
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "screens/shell/session_dialogs.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

// "Find a channel" on its Channels (tab 0) or People (tab 1) tab. channel(c)
// / person(u) after a row was chosen, create() after "Create channel" (the
// dialog is closed in each case). people(users) starts the group DM picked in
// "New group DM", the People tab's other button; null leaves that button out.
ui::Popup *showFindChannel(
    screens::Context                                        &ctx,
    ui::Window                                              &w,
    Avatars                                                 &avatars,
    int                                                      tab,
    std::function<void(model::ConvRef)>                      channel,
    std::function<void(model::UserRef)>                      person,
    std::function<void()>                                    create,
    std::function<void(const std::vector<model::UserRef> &)> people = {}
);

// "Create a channel": done(name, isPrivate) after Create; the name is
// normalised: trimmed, lower-cased, spaces as dashes.
ui::Popup *showCreateChannel(
    ui::Window                                             &w,
    const std::string                                      &workspaceName,
    std::function<void(const std::string &name, bool priv)> done
);

// The rows, ordered by lower-cased name: every channel the Store
// knows (joined or not), and everyone but the deactivated and the Slack
// Connect strangers.
std::vector<BrowseList::Item> channelItems(const model::Store &store);
std::vector<BrowseList::Item> peopleItems(const model::Store &store);

// CreateChannelDialog::channelName().
std::string channelName(std::string_view typed);

} // namespace shell
