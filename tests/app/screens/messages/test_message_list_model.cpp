#include "app/model/store.h"
#include "app/screens/messages/message_list_model.h"
#include "support/test.h"

using namespace model;
using namespace screens;
using Kind = MessageList::ItemKind;

namespace {

ConvRef addChannel(Store &s, const char *id) {
    Conversation c;
    c.id     = id;
    c.name   = id;
    c.kind   = ConvKind::Channel;
    c.member = true;
    return s.addConversation(std::move(c));
}

Message plainMessage(Ts ts, UserRef user = kNoUser) {
    Message m;
    m.ts   = ts;
    m.user = user;
    return m;
}

} // namespace

TEST("message_list_model: messageListLookup finds a top-level message by ts") {
    Store   s;
    ConvRef c = addChannel(s, "C1");
    s.addMessage(c, plainMessage(100));
    const Message *m = messageListLookup(s, c, 0, 100);
    REQUIRE(m);
    CHECK(m->ts == 100);
    CHECK_FALSE(messageListLookup(s, c, 0, 200));
}

TEST("message_list_model: messageListLookup reads thread replies in thread mode") {
    Store   s;
    ConvRef c = addChannel(s, "C1");
    s.addMessage(c, plainMessage(100)); // the root
    Message reply  = plainMessage(150);
    reply.threadTs = 100;
    s.addMessage(c, std::move(reply));
    // The root itself, in thread mode, comes from the conversation's own messages.
    const Message *root = messageListLookup(s, c, 100, 100);
    REQUIRE(root);
    CHECK(root->ts == 100);
    // A reply comes from Store::replies.
    const Message *r = messageListLookup(s, c, 100, 150);
    REQUIRE(r);
    CHECK(r->ts == 150);
}

TEST("message_list_model: messageListLookup is null outside a conversation") {
    Store s;
    CHECK_FALSE(messageListLookup(s, kNoConv, 0, 100));
}

TEST("message_list_model: estimateItemHeight uses the Day/Divider/System flat heights") {
    MessageList::Item day{0, 0, Kind::Day, false};
    CHECK(estimateItemHeight(day, nullptr) == 32);
    MessageList::Item divider{0, 0, Kind::Divider, false};
    CHECK(estimateItemHeight(divider, nullptr) == 36);
    MessageList::Item system{0, 0, Kind::System, false};
    CHECK(estimateItemHeight(system, nullptr) == 32);
}

TEST("message_list_model: estimateItemHeight's base is full vs. grouped") {
    MessageList::Item full{0, 0, Kind::Message, false};
    MessageList::Item grouped{0, 0, Kind::Message, true};
    CHECK(estimateItemHeight(full, nullptr) == 58);
    CHECK(estimateItemHeight(grouped, nullptr) == 27);
}

TEST("message_list_model: estimateItemHeight adds files, attachments, reactions and replies") {
    MessageList::Item it{0, 0, Kind::Message, false};
    Message           m = plainMessage(100);

    File img;
    img.mime  = "image/png";
    img.width = 10;
    m.extras().files.push_back(img);
    CHECK(estimateItemHeight(it, &m) == 58 + 260); // a full row + one image file

    File doc;
    doc.mime = "text/plain";
    m.extras().files.push_back(doc);
    CHECK(estimateItemHeight(it, &m) == 58 + 260 + 66); // + one non-image file

    m.extras().attachments.push_back({});
    CHECK(estimateItemHeight(it, &m) == 58 + 260 + 66 + 90); // + one attachment

    m.reactions.push_back({});
    CHECK(estimateItemHeight(it, &m) == 58 + 260 + 66 + 90 + 30); // + reactions row

    m.replyCount = 3;
    CHECK(estimateItemHeight(it, &m) == 58 + 260 + 66 + 90 + 30 + 36); // + reply-count row
}

TEST("message_list_model: groupingHolds matches neighbours by the author/time window") {
    Store   s;
    ConvRef c  = addChannel(s, "C1");
    UserRef u1 = s.internUser("U1");
    s.addMessage(c, plainMessage(100, u1));
    s.addMessage(c, plainMessage(200, u1)); // within kGroupMicros of 100

    std::vector<MessageList::Item> items = {
        {100, 0, Kind::Message, false},
        {200, 0, Kind::Message, true}, // grouped under 100
    };
    CHECK(groupingHolds(items, 0, s, c, 0));
    CHECK(groupingHolds(items, 1, s, c, 0));

    // Flip item 1's grouped flag without changing the Store: it no longer
    // matches what groupable() says, so the grouping no longer holds.
    items[1].grouped = false;
    CHECK_FALSE(groupingHolds(items, 1, s, c, 0));
}

TEST("message_list_model: groupingHolds is false for an item whose message isn't loaded") {
    Store                          s;
    ConvRef                        c     = addChannel(s, "C1");
    std::vector<MessageList::Item> items = {{999, 0, Kind::Message, false}};
    CHECK_FALSE(groupingHolds(items, 0, s, c, 0));
}

namespace {

// A store signed in as U1 (an admin or not), with U2 as somebody else.
struct People {
    Store   s;
    UserRef me, other;
    explicit People(bool admin = false) {
        User u;
        u.id    = "U1";
        u.admin = admin;
        me      = s.addUser(std::move(u));
        other   = s.internUser("U2");
        s.me    = me;
    }
};

} // namespace

TEST("message_list_model: canDeleteMessage — own, admin's, agent session's; backend gates all") {
    People        p;
    const Message own = plainMessage(100, p.me), theirs = plainMessage(200, p.other);
    CHECK(canDeleteMessage(p.s, own, false, true));
    CHECK_FALSE(canDeleteMessage(p.s, theirs, false, true));
    CHECK(canDeleteMessage(p.s, theirs, true, true)); // any, in an agent session
    // The backend saying "not now" wins over every role.
    CHECK_FALSE(canDeleteMessage(p.s, own, false, false));
    CHECK_FALSE(canDeleteMessage(p.s, theirs, true, false));

    People admin(true);
    CHECK(canDeleteMessage(admin.s, plainMessage(200, admin.other), false, true));
    CHECK_FALSE(canDeleteMessage(admin.s, plainMessage(200, admin.other), false, false));
}

TEST("message_list_model: canDeleteFile — the owner or an admin, agent/backend don't enter") {
    People p;
    CHECK(canDeleteFile(p.s, plainMessage(100, p.me)));
    CHECK_FALSE(canDeleteFile(p.s, plainMessage(200, p.other)));
    People admin(true);
    CHECK(canDeleteFile(admin.s, plainMessage(200, admin.other)));
}

TEST("message_list_model: nobody signed in owns nothing (kNoUser author == kNoUser me)") {
    Store         s; // me == kNoUser
    const Message anon = plainMessage(100, kNoUser);
    CHECK_FALSE(canDeleteMessage(s, anon, false, true));
    CHECK_FALSE(canDeleteFile(s, anon));
    CHECK_FALSE(removesPreviewServerSide(s, anon, true));
}

TEST("message_list_model: file and message delete agree on authorship outside agent sessions") {
    // Two separate API calls, but "mine"/"admin" must mean the same thing for
    // both: with no agent session and the backend allowing it, they match.
    for (bool admin : {false, true}) {
        People p(admin);
        for (UserRef author : {p.me, p.other, kNoUser}) {
            const Message m = plainMessage(100, author);
            CHECK(canDeleteFile(p.s, m) == canDeleteMessage(p.s, m, false, true));
        }
    }
    Store         s; // and signed out
    const Message anon = plainMessage(100, kNoUser);
    CHECK(canDeleteFile(s, anon) == canDeleteMessage(s, anon, false, true));
}

TEST("message_list_model: removesPreviewServerSide — own, sent, capability present") {
    People  p;
    Message own = plainMessage(100, p.me);
    CHECK(removesPreviewServerSide(p.s, own, true));
    CHECK_FALSE(removesPreviewServerSide(p.s, own, false)); // no chat.deleteAttachment
    CHECK_FALSE(removesPreviewServerSide(p.s, plainMessage(200, p.other), true));
    own.pending = true; // not on the server yet
    CHECK_FALSE(removesPreviewServerSide(p.s, own, true));
    // An admin still can't strip someone else's preview.
    People admin(true);
    CHECK_FALSE(removesPreviewServerSide(admin.s, plainMessage(200, admin.other), true));
}
