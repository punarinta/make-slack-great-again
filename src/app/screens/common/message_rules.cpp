#include "app/screens/common/message_rules.h"

#include "base/file.h"
#include "base/str.h"

namespace screens {

namespace {

const std::string &botName(const model::Message &m) {
    static const std::string none;
    return m.extra ? m.extra->botName : none;
}

// A bot's own name and picture stand for the author: a bot, or a message
// with no user at all (a huddle). A person posting through an app stays
// that person.
bool ownIdentity(const model::Store &st, const model::Message &m) {
    return m.user == model::kNoUser || isBot(st, m);
}

} // namespace

bool isSystem(const model::Message &m) {
    const std::string &s = m.subtype();
    return str::endsWith(s, "_join") || str::endsWith(s, "_leave") || s == "channel_topic" ||
           s == "channel_purpose" || s == "channel_name" || s == "pinned_item";
}

bool isBot(const model::Store &st, const model::Message &m) {
    return m.subtype() == "bot_message" || (m.user != model::kNoUser && st.user(m.user).bot);
}

bool groupable(const model::Message &prev, const model::Message &cur) {
    if (prev.user != cur.user || isSystem(prev) || isSystem(cur) || botName(prev) != botName(cur))
        return false;
    if (prev.isHuddle() || cur.isHuddle()) // each huddle its own row (its tile and name)
        return false;
    if (prev.replyCount > 0 || cur.replyCount > 0)
        return false;
    return cur.ts - prev.ts < kGroupMicros;
}

std::string_view authorName(const model::Store &st, const model::Message &m) {
    if (!botName(m).empty() && ownIdentity(st, m))
        return botName(m);
    return st.user(m.user).label();
}

const std::string &authorAvatar(const model::Store &st, const model::Message &m) {
    if (m.extra && !m.extra->botAvatar.empty() && ownIdentity(st, m))
        return m.extra->botAvatar;
    return st.user(m.user).avatar;
}

std::string_view pathExtension(std::string_view path) {
    return file::extension(path.substr(0, path.find('?')));
}

bool isGifPath(std::string_view path) {
    const std::string_view ext = pathExtension(path);
    // .webp too: GIPHY can serve a GIF as animated WebP
    // decodeAnimation sniffs the real bytes
    // a still WebP just decodes to one frame
    return str::iequals(ext, "gif") || str::iequals(ext, "webp");
}

} // namespace screens
