#include "screens/shell/shell_notify_model.h"

#include <algorithm>

namespace shell {

bool shouldNotifyHuddleStart(
    const model::Conversation &c,
    model::NotifyLevel         level,
    bool                       notificationsEnabled,
    bool                       notifyHuddlesEnabled,
    bool                       workspaceMuted,
    bool                       huddlesCapability,
    model::UserRef             me
) {
    if (!notificationsEnabled || !notifyHuddlesEnabled || workspaceMuted || !huddlesCapability)
        return false;
    if (!c.member || c.muted || level == model::NotifyLevel::Nothing)
        return false;
    const auto &ps = c.huddleParticipants;
    if (me != model::kNoUser && std::find(ps.begin(), ps.end(), me) != ps.end())
        return false;
    return c.isDirect() || level == model::NotifyLevel::All;
}

} // namespace shell
