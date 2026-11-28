// Pure functions of model::Store, no ui:: dependency — so Shell's
// notification rules are testable without a live Shell widget.
#pragma once

#include "model/store.h"

namespace shell {

// A huddle's start notifies once: Settings and the workspace allow it, the
// backend supports huddles, the conversation isn't muted or "Nothing", I'm
// not already a participant, and a channel only on "All new posts" (a DM
// always, once past those). Dedup (once per huddle, not per Meta) and
// whether the open conversation's own banner already says it are the
// caller's -- this is just whether a notification would ever be due.
bool shouldNotifyHuddleStart(
    const model::Conversation &c,
    model::NotifyLevel         level,
    bool                       notificationsEnabled,
    bool                       notifyHuddlesEnabled,
    bool                       workspaceMuted,
    bool                       huddlesCapability,
    model::UserRef             me
);

} // namespace shell
