#include "screens/common/message_text.h"

#include "app/mrkdwn/emoji.h"
#include "app/mrkdwn/link_labels.h"
#include "base/file.h"

#include <algorithm>
#include <vector>

namespace screens {

std::string userMentionText(const model::Store &store, model::UserRef u) {
    // The display name in either Names mode, as in Slack.
    if (u != model::kNoUser && !store.user(u).mentionLabel().empty())
        return "@" + std::string(store.user(u).mentionLabel());
    return {};
}

std::string emojiText(std::string unicode, uint8_t skinTone) {
    return skinTone && !unicode.empty() ? emoji::applySkinTone(unicode, skinTone) : unicode;
}

std::string entityText(const model::Store &store, const mrkdwn::Entity &e, uint8_t skinTone) {
    switch (e.kind) {
    case mrkdwn::Kind::User:
        return userMentionText(store, store.findUser(e.data));
    case mrkdwn::Kind::Channel:
        if (const model::ConvRef c = store.findConversation(e.data);
            c != model::kNoConv && !store.conversation(c).name.empty())
            return "#" + store.conversation(c).name;
        else if (const std::string *n = store.channelName(e.data); n && !n->empty())
            return "#" + *n;
        break;
    case mrkdwn::Kind::Usergroup:
        // A user group mention: the live handle (else name).
        if (const model::Store::Usergroup *g = store.findUsergroup(e.data))
            return "@" + (g->handle.empty() ? g->name : g->handle);
        break;
    case mrkdwn::Kind::Emoji:
        return emojiText(store.emojiFor(e.data).unicode, skinTone);
    default:
        break;
    }
    return {};
}

std::string plainText(const model::Store &store, std::string_view text, bool fullUrls) {
    const mrkdwn::Rich       r = mrkdwn::parse(text);
    std::vector<mrkdwn::Run> runs;
    mrkdwn::runs(r, 0, uint32_t(r.text.size()), runs);
    std::string out;
    out.reserve(r.text.size());
    int last = -1; // the entity just written whole
    for (const mrkdwn::Run &run : runs) {
        const std::string_view slice =
            std::string_view(r.text).substr(run.start, run.end - run.start);
        if (run.entity < 0) {
            out += slice;
            continue;
        }
        const mrkdwn::Entity &e = r.entities[size_t(run.entity)];
        const bool link = e.kind == mrkdwn::Kind::Link || e.kind == mrkdwn::Kind::MessageLink;
        const bool shortened =
            fullUrls && e.kind == mrkdwn::Kind::Link &&
            mrkdwn::isShortenedUrlLabel(std::string_view(r.text).substr(e.start, e.length), e.data);
        // A link's label is text like any other (a style change may split it
        // into runs); everything else, and a label swapped for its URL, is
        // written once, whole.
        if (link && !shortened) {
            out += slice;
            continue;
        }
        if (run.entity == last)
            continue;
        last = run.entity;
        if (shortened) {
            out += e.data;
            continue;
        }
        if (std::string to = entityText(store, e, run.skinTone); !to.empty()) {
            out += to;
            continue;
        }
        // Unresolved: its raw form (":name:", "@U0…"), with a merged
        // ":skin-tone-N:" after it.
        out += std::string_view(r.text).substr(e.start, std::max(e.end(), run.end) - e.start);
    }
    return out;
}

std::string firstLink(std::string_view text) {
    const mrkdwn::Rich r = mrkdwn::parse(text);
    for (const mrkdwn::Entity &e : r.entities) {
        if (e.kind == mrkdwn::Kind::Link && !e.data.empty())
            return e.data;
        // A link to another message is a link too: its entity holds the ref,
        // so rebuild the permalink the user sees.
        if (e.kind == mrkdwn::Kind::MessageLink)
            if (std::string url = mrkdwn::messagePermalink(mrkdwn::refFromToken(e.data));
                !url.empty())
                return url;
    }
    return {};
}

std::string fileUrl(const std::string &path) {
    return path.find("://") != std::string::npos ? path : file::toFileUrl(path);
}

} // namespace screens
