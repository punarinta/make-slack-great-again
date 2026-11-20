// MessageList's rules as pure functions, testable without a live
// MessageList/VirtualList. message_list.h is only here for Item/ItemKind;
// nothing touches ui:: state.
#pragma once

#include "app/model/store.h"
#include "app/screens/messages/message_list.h"

#include <cstddef>
#include <vector>

namespace screens {

// Thread replies in thread mode unless `ts` is the root (that one lives in
// the conversation's own messages). Null outside a conversation or if `ts`
// isn't loaded.
const model::Message *
messageListLookup(const model::Store &store, model::ConvRef conv, model::Ts root, model::Ts ts);

// Before the row is built. `m`: the item's message, null for Day/Divider.
float estimateItemHeight(const MessageList::Item &it, const model::Message *m);

// Item `i`'s grouped flag still matches screens::groupable(); while it does,
// an Update needn't rebuild the item list.
bool groupingHolds(
    const std::vector<MessageList::Item> &items,
    size_t                                 i,
    const model::Store                    &store,
    model::ConvRef                         conv,
    model::Ts                              root
);

// "Delete message" (chat.delete; Backend::remove in an agent session):
// - mine, or any as a workspace admin, or any at all in an agent session
// - and only if `backendAllows` (Backend::canDeleteMessage: not while an agent is working)
bool canDeleteMessage(
    const model::Store &store, const model::Message &m, bool agentSession, bool backendAllows
);

// "Delete file" (files.delete): owner or admin. No agent/backend gate:
// - agent sessions have no files.delete
// - Backend::canDeleteMessage is about the message, not the file
bool canDeleteFile(const model::Store &store, const model::Message &m);

// "Remove preview" also strips it for everyone (chat.deleteAttachment):
// only on my own sent message, where the backend has the call.
bool removesPreviewServerSide(
    const model::Store &store, const model::Message &m, bool removePreviewCapability
);

} // namespace screens
