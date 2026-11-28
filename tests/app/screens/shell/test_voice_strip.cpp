#include "app/model/store.h"
#include "screens/shell/voice_strip.h"
#include "support/test.h"

using namespace model;
using namespace shell;

namespace {

ConvRef addChannel(Store &s, std::vector<UserRef> members = {}) {
    Conversation c;
    c.id      = "C1";
    c.name    = "general";
    c.kind    = ConvKind::Channel;
    c.member  = true;
    c.members = std::move(members);
    return s.addConversation(std::move(c));
}

ConvRef addDm(Store &s, UserRef peer) {
    Conversation c;
    c.id     = "D1";
    c.kind   = ConvKind::Dm;
    c.dmUser = peer;
    c.member = true;
    return s.addConversation(std::move(c));
}

Message post(Ts ts, UserRef u, std::string text, std::string subtype = "") {
    Message m;
    m.ts   = ts;
    m.user = u;
    m.text = std::move(text);
    if (!subtype.empty())
        m.extras().subtype = std::move(subtype);
    return m;
}

} // namespace

TEST("voice_strip: buildVoiceContext -- recent messages, oldest to newest, newest-first cap") {
    Store   s;
    ConvRef c  = addChannel(s);
    UserRef u1 = s.addUser({.id = "U1", .name = "mira"});

    auto &conv = s.conversation(c);
    conv.messages.push_back(post(1, u1, "first"));
    conv.messages.push_back(post(2, u1, "second"));
    conv.messages.push_back(post(3, u1, "third"));

    const llm::VoiceContext ctx = buildVoiceContext(s, c, 0, /*maxMessages=*/2);
    // Newest 2, kept oldest -> newest.
    REQUIRE(ctx.recentMessages.size() == 2);
    CHECK(ctx.recentMessages[0].find("second") != std::string::npos);
    CHECK(ctx.recentMessages[1].find("third") != std::string::npos);
}

TEST("voice_strip: buildVoiceContext -- skips system rows and pending sends") {
    Store   s;
    ConvRef c  = addChannel(s);
    UserRef u1 = s.addUser({.id = "U1", .name = "mira"});

    auto &conv = s.conversation(c);
    conv.messages.push_back(post(1, u1, "joined the channel", "channel_join"));
    Message pending = post(2, u1, "not sent yet");
    pending.pending = true;
    conv.messages.push_back(std::move(pending));
    conv.messages.push_back(post(3, u1, "real message"));

    const llm::VoiceContext ctx = buildVoiceContext(s, c, 0, 30);
    REQUIRE(ctx.recentMessages.size() == 1);
    CHECK(ctx.recentMessages[0].find("real message") != std::string::npos);
}

TEST("voice_strip: buildVoiceContext -- a DM names its peer, a channel names itself") {
    Store   s;
    UserRef peer = s.addUser({.id = "U1", .name = "mira"});
    ConvRef dm   = addDm(s, peer);
    CHECK(buildVoiceContext(s, dm, 0, 30).conversationName == s.user(peer).label());

    ConvRef chan = addChannel(s);
    CHECK(buildVoiceContext(s, chan, 0, 30).conversationName == "#general");
}

TEST("voice_strip: buildVoiceContext -- a group DM names its members, not itself") {
    Store        s;
    UserRef      me = s.me = s.addUser({.id = "ME", .name = "me"});
    UserRef      u1        = s.addUser({.id = "U1", .name = "mira"});
    UserRef      u2        = s.addUser({.id = "U2", .name = "jonas"});
    Conversation c;
    c.id          = "G1";
    c.kind        = ConvKind::Group;
    c.member      = true;
    c.members     = {me, u1, u2};
    ConvRef group = s.addConversation(std::move(c));

    const llm::VoiceContext ctx = buildVoiceContext(s, group, 0, 30);
    // Me excluded from the name; the others joined.
    CHECK(ctx.conversationName.find(s.user(u1).label()) != std::string::npos);
    CHECK(ctx.conversationName.find(s.user(u2).label()) != std::string::npos);
    CHECK(ctx.conversationName.find("me") == std::string::npos);
}

TEST("voice_strip: buildVoiceContext -- an unknown conversation is empty, not a crash") {
    Store                   s;
    const llm::VoiceContext ctx = buildVoiceContext(s, ConvRef(999), 0, 30);
    CHECK(ctx.recentMessages.empty());
    CHECK(ctx.conversationName.empty());
}
