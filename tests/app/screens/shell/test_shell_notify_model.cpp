#include "app/model/store.h"
#include "screens/shell/shell_notify_model.h"
#include "support/test.h"

using namespace model;
using namespace shell;

namespace {

ConvRef addChannel(Store &s, bool member = true) {
    Conversation c;
    c.id           = "C1";
    c.kind         = ConvKind::Channel;
    c.member       = member;
    c.huddleActive = true;
    return s.addConversation(std::move(c));
}

ConvRef addDm(Store &s, UserRef peer) {
    Conversation c;
    c.id           = "D1";
    c.kind         = ConvKind::Dm;
    c.dmUser       = peer;
    c.member       = true;
    c.huddleActive = true;
    return s.addConversation(std::move(c));
}

} // namespace

TEST("shell_notify_model: shouldNotifyHuddleStart -- Settings, workspace and backend gates") {
    Store       s;
    ConvRef     c  = addChannel(s);
    const auto &cv = s.conversation(c);

    CHECK(!shouldNotifyHuddleStart(cv, NotifyLevel::All, false, true, false, true, kNoUser));
    CHECK(!shouldNotifyHuddleStart(cv, NotifyLevel::All, true, false, false, true, kNoUser));
    CHECK(!shouldNotifyHuddleStart(cv, NotifyLevel::All, true, true, true, true, kNoUser));
    CHECK(!shouldNotifyHuddleStart(cv, NotifyLevel::All, true, true, false, false, kNoUser));
    CHECK(shouldNotifyHuddleStart(cv, NotifyLevel::All, true, true, false, true, kNoUser));
}

TEST("shell_notify_model: shouldNotifyHuddleStart -- not a member, muted, or Nothing") {
    Store   s;
    ConvRef left = addChannel(s, false);
    CHECK(!shouldNotifyHuddleStart(
        s.conversation(left), NotifyLevel::All, true, true, false, true, kNoUser
    ));

    ConvRef nothing = addChannel(s);
    CHECK(!shouldNotifyHuddleStart(
        s.conversation(nothing), NotifyLevel::Nothing, true, true, false, true, kNoUser
    ));
}

TEST("shell_notify_model: shouldNotifyHuddleStart -- never for a huddle I'm already in") {
    Store   s;
    ConvRef c = addChannel(s);
    User    meUser;
    meUser.id                            = "U1";
    UserRef me                           = s.addUser(meUser);
    s.conversation(c).huddleParticipants = {me};
    CHECK(
        !shouldNotifyHuddleStart(s.conversation(c), NotifyLevel::All, true, true, false, true, me)
    );
    // Someone else's huddle, I'm not in it: still notifies.
    User otherUser;
    otherUser.id  = "U2";
    UserRef other = s.addUser(otherUser);
    CHECK(
        shouldNotifyHuddleStart(s.conversation(c), NotifyLevel::All, true, true, false, true, other)
    );
}

TEST("shell_notify_model: shouldNotifyHuddleStart -- a channel only on \"All new posts\"") {
    Store   s;
    ConvRef chan = addChannel(s);
    CHECK(!shouldNotifyHuddleStart(
        s.conversation(chan), NotifyLevel::Mentions, true, true, false, true, kNoUser
    ));
    CHECK(shouldNotifyHuddleStart(
        s.conversation(chan), NotifyLevel::All, true, true, false, true, kNoUser
    ));

    // A DM notifies regardless of level, once past the filters above.
    UserRef peer = s.addUser({});
    ConvRef dm   = addDm(s, peer);
    CHECK(shouldNotifyHuddleStart(
        s.conversation(dm), NotifyLevel::Mentions, true, true, false, true, kNoUser
    ));
}
