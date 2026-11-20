#include "app/screens/messages/message_list_model.h"

#include "app/screens/common/message_rules.h"

#include <algorithm>

namespace screens {

using Kind = MessageList::ItemKind;

const model::Message *
messageListLookup(const model::Store &store, model::ConvRef conv, model::Ts root, model::Ts ts) {
    if (conv == model::kNoConv)
        return nullptr;
    if (root && ts != root)
        if (const std::vector<model::Message> *r = store.replies(conv, root)) {
            const auto it =
                std::lower_bound(r->begin(), r->end(), ts, [](const model::Message &m, model::Ts t) {
                    return m.ts < t;
                });
            if (it != r->end() && it->ts == ts)
                return &*it;
        }
    return store.findMessage(conv, ts);
}

float estimateItemHeight(const MessageList::Item &it, const model::Message *m) {
    switch (it.kind) {
    case Kind::Day:
        return 32;
    case Kind::Divider:
        return 36;
    case Kind::System:
        return 32;
    default:
        break;
    }
    float h = it.grouped ? 27 : 58;
    if (m) {
        for (const model::File &f : m->files())
            h += f.isImage() ? 260 : 66;
        h += float(m->attachments().size()) * 90;
        if (!m->reactions.empty())
            h += 30;
        if (m->replyCount)
            h += 36;
    }
    return h;
}

bool groupingHolds(
    const std::vector<MessageList::Item> &items,
    size_t                                 i,
    const model::Store                    &store,
    model::ConvRef                         conv,
    model::Ts                              root
) {
    const auto at = [&](size_t j) -> const model::Message * {
        return j < items.size() && (items[j].kind == Kind::Message || items[j].kind == Kind::System)
                   ? messageListLookup(store, conv, root, items[j].ts)
                   : nullptr;
    };
    const model::Message *m = at(i);
    if (!m || isSystem(*m) != (items[i].kind == Kind::System))
        return false;
    const model::Message *prev = i > 0 ? at(i - 1) : nullptr;
    if ((prev && groupable(*prev, *m)) != items[i].grouped)
        return false;
    const model::Message *next = at(i + 1);
    return !next || groupable(*m, *next) == items[i + 1].grouped;
}

namespace {

// Signed out, me == kNoUser would match every kNoUser-authored message.
bool mine(const model::Store &store, const model::Message &m) {
    return store.me != model::kNoUser && m.user == store.me;
}

bool meAdmin(const model::Store &store) {
    return store.me != model::kNoUser && store.user(store.me).admin;
}

} // namespace

bool canDeleteMessage(
    const model::Store &store, const model::Message &m, bool agentSession, bool backendAllows
) {
    return (agentSession || mine(store, m) || meAdmin(store)) && backendAllows;
}

bool canDeleteFile(const model::Store &store, const model::Message &m) {
    return mine(store, m) || meAdmin(store);
}

bool removesPreviewServerSide(
    const model::Store &store, const model::Message &m, bool removePreviewCapability
) {
    return !m.pending && removePreviewCapability && mine(store, m);
}

} // namespace screens
