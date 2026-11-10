#include "app/model/null_backend.h"
#include "app/model/store.h"
#include "app/model/store_slot.h"
#include "support/test.h"

#include <utility>

using namespace model;

namespace {

Message msg(Ts ts, UserRef u, std::string text = "hi", Ts thread = 0) {
    Message m;
    m.ts       = ts;
    m.user     = u;
    m.text     = std::move(text);
    m.threadTs = thread;
    return m;
}

std::vector<Message> page(std::initializer_list<Ts> tss, UserRef u) {
    std::vector<Message> v;
    for (Ts t : tss)
        v.push_back(msg(t, u));
    return v;
}

struct Recorder {
    std::vector<Change> log;
    Store::Observer     fn() {
        return [this](const Change &c) { log.push_back(c); };
    }
};

struct Fixture {
    Store   s;
    UserRef me, mira;
    ConvRef c;
    Fixture() {
        User a;
        a.id   = "U1";
        a.name = "me";
        me     = s.addUser(a);
        s.me   = me;
        User b;
        b.id   = "U2";
        b.name = "mira";
        mira   = s.addUser(b);
        Conversation conv;
        conv.id   = "C1";
        conv.name = "general";
        c         = s.addConversation(std::move(conv));
    }
};

} // namespace

TEST("store: a composer draft is kept per conversation and thread") {
    Fixture        f;
    const uint64_t rev = f.s.localRevision();
    f.s.setDraft(f.c, 0, "half a thought", {"/tmp/a"});
    REQUIRE(f.s.draft(f.c, 0));
    CHECK_STR(f.s.draft(f.c, 0)->html, "half a thought");
    CHECK(f.s.draft(f.c, 0)->files == std::vector<std::string>{"/tmp/a"});
    CHECK(f.s.localRevision() > rev);
    CHECK_FALSE(f.s.draft(f.c, 1)); // another thread, another draft
    const uint64_t same = f.s.localRevision();
    f.s.setDraft(f.c, 0, "half a thought", {"/tmp/a"});
    CHECK(f.s.localRevision() == same); // the same draft again: nothing moved
    f.s.setDraft(f.c, 0, {}, {});
    CHECK_FALSE(f.s.draft(f.c, 0));
}

TEST("store: a whole sections list stays whole; telling it alone is a change") {
    Store s;
    int   metas = 0;
    s.observe(Store::kAnyConv, [&](const Change &ch) { metas += ch.kind == ChangeKind::Meta; });
    const Store::Section work = {"S1", "Work", "", {}};
    CHECK_FALSE(s.sectionsComplete());
    s.setSections({work}); // made here before the list came
    CHECK_FALSE(s.sectionsComplete());
    CHECK(metas == 1);
    const uint64_t rev = s.sectionsRevision();
    s.setSections({work}, true); // the same list, now known whole
    CHECK(s.sectionsComplete());
    CHECK(s.sectionsRevision() > rev);
    CHECK(metas == 2);
    s.setSections({work}, true); // nothing new
    CHECK(metas == 2);
    s.setSections({}); // an edit of the whole list keeps it whole
    CHECK(s.sectionsComplete());
    CHECK(s.sections().empty());
    s.clear();
    CHECK_FALSE(s.sectionsComplete());
}

TEST("store: ts strings round-trip as micros") {
    CHECK(parseTs("1758445200.123456") == 1758445200123456);
    CHECK(parseTs("1758445200.1") == 1758445200100000);
    CHECK(parseTs("1758445200") == 1758445200000000);
    CHECK(parseTs("x") == 0);
    CHECK(parseTs("1.1234567") == 0);
    CHECK_STR(formatTs(1758445200000007), "1758445200.000007");
}

TEST("store: users are interned and merged in place") {
    Store   s;
    UserRef ghost = s.internUser("U9");
    CHECK(s.user(ghost).placeholder);
    CHECK(s.internUser("U9") == ghost);
    User u;
    u.id          = "U9";
    u.name        = "nine";
    u.displayName = "Nine";
    CHECK(s.addUser(u) == ghost); // same ref: messages keep pointing at it
    CHECK_FALSE(s.user(ghost).placeholder);
    CHECK(s.user(ghost).label() == "Nine");
    CHECK(s.findUser("nope") == kNoUser);
    CHECK(s.user(kNoUser).id.empty());
}

TEST("store: re-adding an unchanged conversation (a roster reload) emits nothing") {
    Store s;
    for (const char *id : {"C1", "C2", "D1"}) {
        Conversation c;
        c.id   = id;
        c.name = id;
        s.addConversation(std::move(c));
    }
    {
        std::vector<Message> page(1);
        page[0].ts   = 5;
        page[0].text = "kept";
        s.addPage(0, std::move(page));
    }
    int meta = 0, roster = 0;
    s.observe(Store::kAnyConv, [&](const Change &ch) {
        meta += ch.kind == ChangeKind::Meta;
        roster += ch.kind == ChangeKind::Roster;
    });
    // The reload: the same metadata again, no messages.
    for (ConvRef r = 0; r < 3; ++r) {
        const Conversation &cur = s.conversation(r);
        Conversation        c;
        c.id     = cur.id;
        c.name   = cur.name;
        c.latest = cur.latest;
        s.addConversation(std::move(c));
    }
    CHECK(meta == 0);
    CHECK(roster == 0);
    CHECK(s.conversation(0).messages.size() == 1); // the loaded messages stay
    // A real change is still news, once.
    Conversation c;
    c.id    = "C2";
    c.name  = "C2";
    c.topic = "new topic";
    s.addConversation(std::move(c));
    CHECK(meta == 1);
    CHECK(s.conversation(1).topic == "new topic");
}

TEST("store: user revisions tell profile changes from presence flips") {
    Store s;
    User  a, b;
    a.id             = "UA";
    a.name           = "a";
    b.id             = "UB";
    b.name           = "b";
    const UserRef ra = s.addUser(a);
    const UserRef rb = s.addUser(b);
    s.usersChanged();
    const uint64_t p0 = s.profileRevision(), q0 = s.presenceRevision(), t0 = s.textRevision();
    CHECK(s.userRevision(ra) == p0 && s.userRevision(rb) == p0);
    // A presence flip: the presence revision moves, the profile one doesn't.
    s.user(ra).active = true;
    s.usersChanged();
    CHECK(s.profileRevision() == p0);
    CHECK(s.presenceRevision() != q0);
    // Nothing changed: nothing moves.
    const uint64_t q1 = s.presenceRevision();
    s.usersChanged();
    CHECK(s.profileRevision() == p0 && s.presenceRevision() == q1);
    // A name: only that user's revision moves.
    s.user(rb).displayName = "Bee";
    s.usersChanged();
    CHECK(s.profileRevision() > p0);
    CHECK(s.userRevision(rb) == s.profileRevision());
    CHECK(s.userRevision(ra) == p0);
    // A new user counts as changed.
    const UserRef rc = s.internUser("UC");
    s.usersChanged();
    CHECK(s.userRevision(rc) == s.profileRevision());
    // Emoji and channel names are text, not users.
    s.setCustomEmoji("party", "https://e/p.png");
    s.setChannelName("C9", "nine");
    CHECK(s.textRevision() != t0);
}

TEST("store: pages prepend, append and insert with the right change kinds") {
    Fixture  f;
    Recorder r;
    f.s.observe(f.c, r.fn());
    f.s.addPage(f.c, page({300, 100, 200}, f.mira)); // any order in, sorted
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].kind == ChangeKind::Append);
    CHECK(r.log[0].count == 3);
    const auto &msgs = f.s.conversation(f.c).messages;
    CHECK((msgs[0].ts == 100 && msgs[1].ts == 200 && msgs[2].ts == 300));

    r.log.clear();
    std::vector<Message> next = page({50, 10, 250, 200, 400}, f.mira);
    next[3].text              = "edited"; // 200 changed
    f.s.addPage(f.c, std::move(next));
    // 200 existed (Update), 10/50 older (Prepend 2), 250 middle (Insert), 400 newer (Append 1)
    REQUIRE(r.log.size() == 4);
    CHECK((r.log[0].kind == ChangeKind::Update && r.log[0].ts == 200));
    CHECK((r.log[1].kind == ChangeKind::Prepend && r.log[1].count == 2));
    CHECK((r.log[2].kind == ChangeKind::Insert && r.log[2].ts == 250));
    CHECK((r.log[3].kind == ChangeKind::Append && r.log[3].count == 1));
    CHECK(msgs.size() == 7);
    CHECK(f.s.indexOf(f.c, 250) == 4);
    CHECK(f.s.indexOf(f.c, 999) == SIZE_MAX);
    CHECK(f.s.conversation(f.c).latest == 400);
    for (size_t i = 1; i < msgs.size(); ++i)
        CHECK(msgs[i - 1].ts < msgs[i].ts);
}

TEST("store: a refetched page emits Update only for messages that changed") {
    Fixture f;
    auto    full = [&] {
        std::vector<Message> v = page({100, 200, 300}, f.mira);
        v[1].reactions.push_back(Reaction{"tada", 1, {f.me}});
        v[2].extras().files.push_back(File{});
        v[2].extras().files.back().id = "F1";
        return v;
    };
    f.s.addPage(f.c, full());
    Recorder r;
    f.s.observe(f.c, r.fn());
    // The 5 s poll's head page, unchanged: nothing at all.
    f.s.addPage(f.c, full());
    CHECK(r.log.empty());
    // No extras and empty extras are the same message.
    std::vector<Message> same = page({100}, f.mira);
    same[0].extras();
    f.s.addPage(f.c, std::move(same));
    CHECK(r.log.empty());
    // A reaction and a file name moved: exactly those two.
    std::vector<Message> moved      = full();
    moved[1].reactions[0].count     = 2;
    moved[2].extras().files[0].name = "a.png";
    f.s.addPage(f.c, std::move(moved));
    REQUIRE(r.log.size() == 2);
    CHECK((r.log[0].kind == ChangeKind::Update && r.log[0].ts == 200));
    CHECK((r.log[1].kind == ChangeKind::Update && r.log[1].ts == 300));
    CHECK(f.s.findMessage(f.c, 300)->files()[0].name == "a.png");
    // A reminder still fires its own Update (and moves the local revision the
    // workspace cache saves meta.json on).
    r.log.clear();
    const uint64_t local = f.s.localRevision();
    f.s.setReminderAt(f.c, 200, 1234);
    REQUIRE(r.log.size() == 1);
    CHECK((r.log[0].kind == ChangeKind::Update && r.log[0].ts == 200));
    CHECK(f.s.localRevision() != local);
    CHECK(f.s.reminderAt(f.c, 200) == 1234);
}

TEST("store: addConversations merges a whole roster with one Roster emit") {
    Store s;
    int   roster = 0, meta = 0;
    s.observe(Store::kAnyConv, [&](const Change &ch) {
        roster += ch.kind == ChangeKind::Roster;
        meta += ch.kind == ChangeKind::Meta;
    });
    std::vector<Conversation> all;
    for (const char *id : {"C1", "C2", "C3", "D1"}) {
        Conversation c;
        c.id   = id;
        c.name = id;
        all.push_back(std::move(c));
    }
    s.addConversations(std::move(all));
    CHECK(roster == 1);
    CHECK(meta == 0);
    CHECK(s.conversationCount() == 4);
    CHECK(s.findConversation("D1") == 3);
    // A reload with one renamed and one new: one Roster, one Meta.
    std::vector<Conversation> again;
    for (const char *id : {"C1", "C2", "C3", "D1", "C4"}) {
        Conversation c;
        c.id   = id;
        c.name = std::string(id) == "C2" ? "renamed" : id;
        again.push_back(std::move(c));
    }
    s.addConversations(std::move(again));
    CHECK(roster == 2);
    CHECK(meta == 1);
    CHECK(s.conversation(s.findConversation("C2")).name == "renamed");
    // Nothing new, nothing changed: silence.
    std::vector<Conversation> none;
    Conversation              c;
    c.id   = "C1";
    c.name = "C1";
    none.push_back(std::move(c));
    s.addConversations(std::move(none));
    CHECK(roster == 2 && meta == 1);
}

TEST("store: usersChanged(u) re-hashes only that user") {
    Store s;
    User  a, b;
    a.id             = "UA";
    b.id             = "UB";
    const UserRef ra = s.addUser(a);
    const UserRef rb = s.addUser(b);
    s.usersChanged();
    const uint64_t p0     = s.profileRevision();
    // Changed in place and named: that user's revision moves.
    s.user(ra).statusText = "lunch";
    s.usersChanged(ra);
    CHECK(s.profileRevision() > p0);
    CHECK(s.userRevision(ra) == s.profileRevision());
    CHECK(s.userRevision(rb) == p0);
    // Only the named user is looked at: another's in-place change waits for
    // a full usersChanged().
    const uint64_t p1     = s.profileRevision();
    s.user(rb).statusText = "away";
    s.usersChanged(ra);
    CHECK(s.profileRevision() == p1);
    s.usersChanged();
    CHECK(s.userRevision(rb) == s.profileRevision());
    // addUser counts as touched (the merge of a fetched profile).
    User b2       = s.user(rb);
    b2.title      = "CTO";
    const auto p2 = s.profileRevision();
    s.usersChanged(s.addUser(b2));
    CHECK(s.profileRevision() > p2);
    CHECK(s.userRevision(rb) == s.profileRevision());
    CHECK(s.userRevision(ra) == p1);
}

TEST("store: textChangesSince lists changed channels, emoji and groups") {
    Store                          s;
    const uint64_t                 t0 = s.textRevision();
    std::vector<Store::TextChange> out;
    CHECK(s.textChangesSince(t0, &out));
    CHECK(out.empty());
    s.setChannelName("C9", "nine");
    s.setCustomEmoji("party", "https://e/p.png");
    REQUIRE(s.textChangesSince(t0, &out));
    REQUIRE(out.size() == 2);
    CHECK((out[0].kind == Store::TextChange::Kind::Channel && out[0].id == "C9"));
    CHECK((out[1].kind == Store::TextChange::Kind::Emoji && out[1].id == "party"));
    // Since the channel only: the emoji.
    const uint64_t t1 = s.textRevision();
    s.replaceCustomEmoji({{"party", "https://e/p2.png"}, {"yay", "alias:party"}});
    out.clear();
    REQUIRE(s.textChangesSince(t1, &out));
    REQUIRE(out.size() == 2); // party changed, yay new (any order)
    // Groups: added, then one edited.
    const uint64_t t2 = s.textRevision();
    s.setUsergroups({{"S1", "devs", "Devs", {"UA"}}, {"S2", "ops", "Ops", {}}});
    s.setUsergroups({{"S1", "devs", "Devs", {"UA"}}, {"S2", "ops", "Ops!", {}}});
    out.clear();
    REQUIRE(s.textChangesSince(t2, &out));
    REQUIRE(out.size() == 3);
    CHECK(out[2].id == "S2");
    // clear() has no ids: unlisted.
    const uint64_t t3 = s.textRevision();
    s.clear();
    out.clear();
    CHECK_FALSE(s.textChangesSince(t3, &out));
    CHECK(s.textChangesSince(s.textRevision(), &out));
}

TEST("store: saved items stay in their order as they change") {
    Store s;
    s.setSavedItem(0, 10, true, 0, 100);   // bookmark, older
    s.setSavedItem(0, 20, true, 0, 200);   // bookmark, newer
    s.setSavedItem(0, 30, true, 5000, 50); // reminder
    s.setSavedItem(0, 40, true, 4000, 60); // sooner reminder
    const std::vector<Store::SavedItem> &v = s.savedList();
    REQUIRE(v.size() == 4);
    CHECK((v[0].ts == 40 && v[1].ts == 30 && v[2].ts == 20 && v[3].ts == 10));
    s.setSavedItem(0, 10, true, 1000); // becomes the soonest reminder
    CHECK(s.savedList()[0].ts == 10);
    CHECK(s.savedList()[3].ts == 20);
}

TEST("store: live messages count unread and mentions, markRead recounts") {
    Fixture  f;
    Recorder r;
    f.s.observe(f.c, r.fn());
    f.s.addMessage(f.c, msg(100, f.mira, "hello"));
    f.s.addMessage(f.c, msg(200, f.mira, "hey <@U1> look"));
    f.s.addMessage(f.c, msg(300, f.me, "mine"));
    f.s.addMessage(f.c, msg(400, f.mira, "<!here> standup"));
    const auto &c = f.s.conversation(f.c);
    CHECK(c.unread == 3);
    CHECK(c.mentions == 2);
    CHECK(r.log[0].kind == ChangeKind::Append);
    CHECK(r.log[1].kind == ChangeKind::Meta);
    f.s.markRead(f.c, 200);
    CHECK(c.unread == 1);
    CHECK(c.mentions == 1);
    f.s.markRead(f.c, 400);
    CHECK(c.unread == 0);
    CHECK(c.mentions == 0);
    // Out-of-order live message: Insert, not Append.
    r.log.clear();
    f.s.addMessage(f.c, msg(250, f.mira));
    CHECK((r.log[0].kind == ChangeKind::Insert && r.log[0].ts == 250));
}

TEST("store: mentionsMe matches Slack's rule") {
    Fixture f;
    CHECK(f.s.mentionsMe("<@U1>"));
    CHECK(f.s.mentionsMe("hi <@U1|me>"));
    CHECK_FALSE(f.s.mentionsMe("<@U12>")); // a longer id is someone else
    CHECK(f.s.mentionsMe("<!channel>"));
    CHECK(f.s.mentionsMe("<!everyone>"));
    CHECK_FALSE(f.s.mentionsMe("<!subteam^S1|@eng>"));
    f.s.myGroups.push_back("S1");
    CHECK(f.s.mentionsMe("<!subteam^S1|@eng>"));
    CHECK(f.s.mentionsMe("<!subteam^S1>"));
    CHECK_FALSE(f.s.mentionsMe("plain"));
}

TEST("store: thread replies update their root") {
    Fixture f;
    f.s.addPage(f.c, page({100}, f.mira));
    Recorder r;
    f.s.observe(f.c, r.fn());
    f.s.addMessage(f.c, msg(150, f.me, "reply", 100));
    const Message *root = f.s.findMessage(f.c, 100);
    REQUIRE(root);
    CHECK(root->replyCount == 1);
    CHECK(root->latestReply == 150);
    CHECK(root->replyUsers.size() == 1);
    REQUIRE(r.log.size() == 2);
    CHECK((r.log[0].kind == ChangeKind::Append && r.log[0].thread == 100 && r.log[0].count == 1));
    CHECK((r.log[1].kind == ChangeKind::Update && r.log[1].thread == 0 && r.log[1].ts == 100));
    // Replies are not unread top-level messages.
    CHECK(f.s.conversation(f.c).unread == 0);
    REQUIRE(f.s.replies(f.c, 100));
    CHECK(f.s.replies(f.c, 100)->size() == 1);
    CHECK(f.s.findMessage(f.c, 150)->isReply());
    CHECK(f.s.indexOf(f.c, 150, 100) == 0);

    r.log.clear();
    CHECK(f.s.removeMessage(f.c, 150));
    CHECK(f.s.findMessage(f.c, 100)->replyCount == 0);
    CHECK((r.log[0].kind == ChangeKind::Remove && r.log[0].thread == 100 && r.log[0].ts == 150));
    CHECK((r.log[1].kind == ChangeKind::Update && r.log[1].ts == 100));
}

TEST("store: reactions, updates and removal notify with the ts") {
    Fixture f;
    f.s.addPage(f.c, page({100, 200}, f.mira));
    Recorder r;
    f.s.observe(f.c, r.fn());
    CHECK(f.s.setReaction(f.c, 100, "tada", f.me, true));
    CHECK(f.s.setReaction(f.c, 100, "tada", f.mira, true));
    CHECK(f.s.setReaction(f.c, 100, "tada", f.me, true)); // twice: no double count
    const Message *m = f.s.findMessage(f.c, 100);
    REQUIRE(m->reactions.size() == 1);
    CHECK(m->reactions[0].count == 2);
    CHECK(f.s.reactedByMe(m->reactions[0]));
    CHECK(f.s.setReaction(f.c, 100, "tada", f.me, false));
    CHECK_FALSE(f.s.reactedByMe(m->reactions[0]));
    CHECK(f.s.setReaction(f.c, 100, "tada", f.mira, false));
    CHECK(m->reactions.empty());
    for (const auto &ch : r.log)
        CHECK((ch.kind == ChangeKind::Update && ch.ts == 100));
    CHECK_FALSE(f.s.setReaction(f.c, 999, "x", f.me, true));

    r.log.clear();
    CHECK(f.s.removeMessage(f.c, 100));
    CHECK((r.log[0].kind == ChangeKind::Remove && r.log[0].ts == 100));
    CHECK(f.s.conversation(f.c).messages.size() == 1);
}

TEST("store: observers filter by conversation and survive self-removal") {
    Fixture      f;
    Conversation other;
    other.id               = "C2";
    const ConvRef     c2   = f.s.addConversation(std::move(other));
    int               mine = 0, any = 0, second = 0;
    Store::ObserverId self = 0;
    f.s.observe(f.c, [&](const Change &ch) {
        if (ch.kind != ChangeKind::Roster)
            ++mine;
    });
    f.s.observe(Store::kAnyConv, [&](const Change &) { ++any; });
    self = f.s.observe(c2, [&](const Change &) {
        ++second;
        f.s.unobserve(self); // removing itself mid-dispatch is safe
        f.s.observe(c2, [&](const Change &) { second += 100; }); // joins after this dispatch
    });
    f.s.addMessage(c2, msg(100, f.mira)); // Append + Meta
    CHECK(mine == 0);
    CHECK(second == 101); // the self-remover once, then the new observer on Meta
    CHECK(any == 2);
    f.s.addMessage(f.c, msg(100, f.mira));
    CHECK(mine == 2);
    CHECK(second == 101);
}

TEST("store: typing indicators end with the author's message") {
    Fixture  f;
    Recorder r;
    f.s.observe(f.c, r.fn());
    f.s.setTyping(f.c, f.mira, 0, true);
    f.s.setTyping(f.c, f.mira, 0, true); // refresh: no event
    CHECK(f.s.typing(f.c).size() == 1);
    CHECK(r.log.size() == 1);
    f.s.setTyping(f.c, f.me, 0, true); // never show myself
    CHECK(f.s.typing(f.c).size() == 1);
    f.s.addMessage(f.c, msg(100, f.mira));
    CHECK(f.s.typing(f.c).empty());
    CHECK(r.log.back().kind == ChangeKind::Typing);
}

TEST("store: emoji resolution: skin tones, Unicode, raw glyphs, then custom aliases") {
    Store s;
    s.setCustomEmoji("partyparrot", "/img/parrot.gif");
    s.setCustomEmoji("myparrot", "alias:partyparrot");
    s.setCustomEmoji("thumbs", "alias:+1");
    s.setCustomEmoji("loopy", "alias:loopy");
    CHECK_STR(s.emojiFor("rocket").unicode, "🚀");
    CHECK_STR(s.emojiFor("+1::skin-tone-2").unicode, "👍🏻");
    CHECK_STR(s.emojiFor("partyparrot").image, "/img/parrot.gif");
    CHECK_STR(s.emojiFor("myparrot").image, "/img/parrot.gif");
    CHECK_STR(s.emojiFor("thumbs").unicode, "👍");
    CHECK_STR(s.emojiFor("👍").unicode, "👍"); // Teams sends the glyph
    CHECK_STR(
        s.emojiFor("myparrot::skin-tone-3").image, "/img/parrot.gif"
    ); // custom base ignores tone
    CHECK_FALSE(s.emojiFor("loopy").resolved());
    CHECK_STR(
        s.emojiFor("parrot").unicode, "🦜"
    ); // standard names win over custom ones // bounded alias walk
    CHECK_FALSE(s.emojiFor("unknown").resolved());
}

TEST("store: display names for DMs and groups") {
    Fixture      f;
    Conversation dm;
    dm.id           = "D1";
    dm.kind         = ConvKind::Dm;
    dm.dmUser       = f.mira;
    const ConvRef d = f.s.addConversation(std::move(dm));
    CHECK_STR(f.s.displayName(d), "mira");
    f.s.user(f.mira).displayName = "Mira Okafor";
    CHECK_STR(f.s.displayName(d), "Mira Okafor");
    Conversation g;
    g.id      = "G1";
    g.kind    = ConvKind::Group;
    g.members = {f.mira, f.me};
    CHECK_STR(f.s.displayName(f.s.addConversation(std::move(g))), "Mira Okafor");
    CHECK_STR(f.s.displayName(f.c), "general");
}

TEST("store: names resolve per mode, falling back to the other name, then the handle") {
    const auto shown = [](const char *real, const char *profile, bool realNames) {
        User u;
        u.id          = "U1";
        u.name        = "meg";
        u.displayName = "kept";
        u.realName    = real;
        u.profileName = profile;
        u.resolveName(realNames);
        return u.displayName;
    };
    CHECK_STR(shown("Meg Ryan", "Meg", true), "Meg Ryan");
    CHECK_STR(shown("Meg Ryan", "Meg", false), "Meg");
    CHECK_STR(shown("Meg Ryan", "", false), "Meg Ryan"); // no display name: the full name
    CHECK_STR(shown("", "Meg", true), "Meg");            // no full name: the display name
    CHECK_STR(shown("", "", true), "kept");              // neither: the backend's own
    CHECK_STR(shown("", "", false), "kept");
    // A mention is the display name in both modes.
    User u;
    u.name        = "meg";
    u.realName    = "Meg Ryan";
    u.profileName = "Meg";
    u.resolveName(true);
    CHECK_STR(u.label(), "Meg Ryan");
    CHECK_STR(u.mentionLabel(), "Meg");
    u.profileName.clear();
    u.resolveName(true);
    CHECK_STR(u.mentionLabel(), "Meg Ryan");
    User bare; // an agent session or a bot: only displayName
    bare.name        = "claude";
    bare.displayName = "Claude";
    CHECK_STR(bare.mentionLabel(), "Claude");
}

TEST("store: setRealNames re-resolves every user and repaints once") {
    Store s;
    User  a, b, agent;
    a.id = "UA", a.name = "meg", a.realName = "Meg Ryan", a.profileName = "Meg";
    b.id = "UB", b.name = "tom", b.realName = "Tom Hanks"; // no display name
    agent.id = "UC", agent.name = "claude", agent.displayName = "Claude";
    const UserRef ra = s.addUser(a), rb = s.addUser(b), rc = s.addUser(agent);
    Conversation  dm;
    dm.id           = "D1";
    dm.kind         = ConvKind::Dm;
    dm.dmUser       = ra;
    const ConvRef d = s.addConversation(std::move(dm));
    s.usersChanged();
    CHECK(s.realNames());
    CHECK_STR(s.displayName(d), "Meg Ryan");
    Recorder r;
    s.observe(Store::kAnyConv, r.fn());
    const uint64_t p0 = s.profileRevision(), m0 = s.metaRevision();
    s.setRealNames(false);
    CHECK_STR(s.user(ra).displayName, "Meg");
    CHECK_STR(s.user(rb).displayName, "Tom Hanks");
    CHECK_STR(s.user(rc).displayName, "Claude");
    CHECK_STR(s.displayName(d), "Meg");
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].kind == ChangeKind::Users);
    CHECK(s.profileRevision() > p0 && s.metaRevision() > m0);
    CHECK(s.userRevision(ra) == s.profileRevision());
    CHECK(s.userRevision(rc) < s.profileRevision()); // unchanged
    // The same mode again: nothing. A user added now resolves in it.
    s.setRealNames(false);
    CHECK(r.log.size() == 1);
    User late;
    late.id = "UD", late.name = "rita", late.realName = "Rita Wilson", late.profileName = "Rita";
    CHECK_STR(s.user(s.addUser(late)).displayName, "Rita");
    s.setRealNames(true);
    CHECK_STR(s.user(s.findUser("UD")).displayName, "Rita Wilson");
    CHECK_STR(s.user(ra).displayName, "Meg Ryan");
}

TEST("store: mark unread moves the read cursor back and recounts") {
    Fixture f;
    f.s.addPage(f.c, page({100, 200, 300}, f.mira));
    f.s.markRead(f.c, 300);
    CHECK(f.s.conversation(f.c).unread == 0);
    Recorder r;
    f.s.observe(f.c, r.fn());
    f.s.markUnread(f.c, 200); // "Mark unread" on the middle message
    CHECK(f.s.conversation(f.c).lastRead == 199);
    CHECK(f.s.conversation(f.c).unread == 2);
    REQUIRE(!r.log.empty());
    CHECK(r.log.back().kind == ChangeKind::Meta);
    f.s.markRead(f.c, 300);
    CHECK(f.s.conversation(f.c).unread == 0);
}

TEST("store: links follow the workspace URL; replies name their thread") {
    Fixture f;
    f.s.workspaceId = "T1";
    CHECK_STR(f.s.workspaceLink(), "https://app.slack.com/client/T1/");
    CHECK_STR(f.s.conversationLink(f.c), "https://slack.com/archives/C1");
    CHECK_STR(
        f.s.permalink(f.c, 1758445200123456), "https://slack.com/archives/C1/p1758445200123456"
    );
    f.s.workspaceUrl = "https://acme.slack.com";
    CHECK_STR(f.s.workspaceLink(), "https://acme.slack.com/");
    CHECK_STR(
        f.s.permalink(f.c, 1758445200123456, 1758445100000001),
        "https://acme.slack.com/archives/C1/p1758445200123456?thread_ts=1758445100.000001&cid=C1"
    );
    f.s.clear();
    CHECK(f.s.workspaceUrl.empty() && !f.s.workspaceMuted);
}

TEST("store: a closed DM reopens with its next message") {
    Fixture      f;
    Conversation d;
    d.id             = "D1";
    d.kind           = ConvKind::Dm;
    d.dmUser         = f.mira;
    d.member         = false;
    const ConvRef dm = f.s.addConversation(std::move(d));
    f.s.addMessage(dm, msg(500, f.mira));
    CHECK(f.s.conversation(dm).member);
}

// ── StoreSlot: the screens' Store, retargeted on a workspace switch ─────────

TEST("store slot: observers move to the new Store and hear Roster then Users") {
    Store a, b;
    a.addConversation({.id = "C1", .name = "one"});
    b.addConversation({.id = "C2", .name = "two"});
    StoreSlot               slot(a);
    Recorder                views, pinned;
    const Store::ObserverId id = slot.observe(Store::kAnyConv, views.fn());
    b.observe(Store::kAnyConv, pinned.fn()); // b's own (a background workspace's)
    CHECK(&slot() == &a);
    a.updateConversation(0, [](Conversation &) {});
    REQUIRE(views.log.size() == 1);
    CHECK(views.log[0].kind == ChangeKind::Meta);

    views.log.clear();
    slot.setTarget(b);
    CHECK(&slot() == &b);
    REQUIRE(views.log.size() == 2);
    CHECK(views.log[0].kind == ChangeKind::Roster);
    CHECK(views.log[1].kind == ChangeKind::Users);
    CHECK(pinned.log.empty()); // a switch is news for the views only

    // a's changes no longer reach the views; b's do, and b's own observer too.
    views.log.clear();
    a.updateConversation(0, [](Conversation &) {});
    CHECK(views.log.empty());
    b.updateConversation(0, [](Conversation &) {});
    CHECK(views.log.size() == 1);
    CHECK(pinned.log.size() == 1);

    // The slot's id unobserves wherever the observer lives now.
    slot.unobserve(id);
    views.log.clear();
    b.updateConversation(0, [](Conversation &) {});
    CHECK(views.log.empty());
    slot.setTarget(a); // nobody left to tell
    CHECK(views.log.empty());
}

TEST("store: a ref the Store lacks reads as an empty conversation") {
    Store s;
    CHECK(s.conversation(kNoConv).messages.empty());
    CHECK(s.conversation(3).id.empty()); // a ref kept from another Store
    s.conversation(3).name = "lost";     // lands in scratch, not past the end
    CHECK(s.conversation(3).name.empty());
    CHECK(std::as_const(s).conversation(3).threads.empty());
    CHECK(s.conversationCount() == 0);
}

TEST("store slot: an observer may unobserve itself while hearing the switch") {
    Store a, b;
    b.addConversation({.id = "C1"});
    StoreSlot         slot(a);
    int               heard = 0;
    Store::ObserverId id    = 0;
    id                      = slot.observe(Store::kAnyConv, [&](const Change &) {
        ++heard;
        slot.unobserve(id);
    });
    slot.setTarget(b);
    CHECK(heard == 1); // Roster, then gone before Users
    b.updateConversation(0, [](Conversation &) {});
    CHECK(heard == 1);
}

TEST("backend: calls a service lacks default to nothing") {
    Store       s;
    NullBackend b{s};
    ConvRef     dm = 7;
    b.openDm(1, [&](ConvRef c) { dm = c; });
    CHECK(dm == kNoConv);
    int hits = -1;
    b.search("x", [&](std::vector<Backend::SearchHit> h) { hits = int(h.size()); });
    CHECK(hits == 0);
    b.edit(0, 1, "x"); // no-ops
    b.setPinned(0, 1, true);
    b.userTyping(0, 0);
    CHECK(s.conversationCount() == 0);
}
