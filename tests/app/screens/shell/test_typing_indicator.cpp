#include "app/model/store.h"
#include "screens/shell/typing_indicator.h"
#include "support/test.h"

using namespace model;
using namespace shell;

namespace {

ConvRef addChannel(Store &s) {
    Conversation c;
    c.id     = "C1";
    c.kind   = ConvKind::Channel;
    c.member = true;
    return s.addConversation(std::move(c));
}

} // namespace

TEST("typing_indicator: typingStateOf -- nobody typing is not thinking either") {
    Store                    s;
    ConvRef                  c     = addChannel(s);
    const TypingState        state = typingStateOf(s, c, 0);
    CHECK(state.who.empty());
    CHECK_FALSE(state.thinking);
}

TEST("typing_indicator: typingStateOf -- a person typing isn't thinking") {
    Store   s;
    ConvRef c = addChannel(s);
    UserRef u = s.addUser({.id = "U1"});
    s.setTyping(c, u, 0, true, 0); // sinceMs 0: plain typing, not an agent
    const TypingState state = typingStateOf(s, c, 0);
    CHECK(state.who.size() == 1);
    CHECK_FALSE(state.thinking);
}

TEST("typing_indicator: typingStateOf -- an agent at work is thinking, since its sinceMs") {
    Store   s;
    ConvRef c = addChannel(s);
    UserRef u = s.addUser({.id = "U1"});
    s.setTyping(c, u, 0, true, 12345);
    const TypingState state = typingStateOf(s, c, 0);
    CHECK(state.who.size() == 1);
    CHECK(state.thinking);
    CHECK(state.since == 12345);
}

TEST("typing_indicator: typingStateOf -- a mix of person and agent isn't \"thinking\"") {
    Store   s;
    ConvRef c  = addChannel(s);
    UserRef u1 = s.addUser({.id = "U1"});
    UserRef u2 = s.addUser({.id = "U2"});
    s.setTyping(c, u1, 0, true, 0);     // plain typing
    s.setTyping(c, u2, 0, true, 999);   // an agent
    const TypingState state = typingStateOf(s, c, 0);
    CHECK(state.who.size() == 2);
    CHECK_FALSE(state.thinking); // one of them isn't an agent -> not all thinking
}

TEST("typing_indicator: typingStateOf -- only this thread's typers count") {
    Store   s;
    ConvRef c  = addChannel(s);
    UserRef u1 = s.addUser({.id = "U1"});
    UserRef u2 = s.addUser({.id = "U2"});
    s.setTyping(c, u1, 0, true, 0);   // top-level
    s.setTyping(c, u2, 7, true, 0);   // a reply thread
    CHECK(typingStateOf(s, c, 0).who.size() == 1);
    CHECK(typingStateOf(s, c, 7).who.size() == 1);
    CHECK(typingStateOf(s, c, 99).who.empty());
}
