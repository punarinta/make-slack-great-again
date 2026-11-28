// The huddle banner: a 34 px green strip over the messages while the open
// conversation has a live huddle — the headphones, "A huddle is happening"
// and a white "Join" (its tooltip above it: "Opens the huddle in Slack for
// web"). The shell shows it (Conversation::huddleActive, where the service
// has huddles) and opens the join link.
#pragma once

#include "ui/ui.h"

#include <functional>

namespace shell {

class HuddleBanner final : public ui::View {
public:
    HuddleBanner();
    std::function<void()> onJoin;
};

} // namespace shell
