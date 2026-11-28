// Demo mode's own checks (MSGA_DEMO builds with tests only): the tour script
// parser, and the repository's demo/tour.json and demo/fixture.json, which
// only the README recording uses.
#include "app/fake/fixture.h"
#include "app/model/store.h"
#include "base/file.h"
#include "base/time.h"
#include "screens/shell/demo/tour_script.h"
#include "support/test.h"

#include <string>

TEST("demo tour: the script parses the tour verbs; mistakes are named") {
    demo::TourScript s;
    std::string      err;
    REQUIRE(
        demo::parseTour(
            R"({"window": [1280, 800], "pause": 350, "steps": [
            {"wait": 600}, {"open": "C0DESIGN"}, {"scroll": -520},
            {"type": "On it", "cps": 26}, {"send": true},
            {"moveToThread": ["exports are in", "Can someone export"]},
            {"settings": {"pages": ["appearance", "ai"], "each": 1000}},
            {"post": {"conv": "C0DESIGN", "user": "U0YUKI", "thread": "export", "text": "Got it"}},
            {"key": "Return"}, {"theme": "dark"}, {"quit": true}]})",
            &s,
            &err
        )
    );
    CHECK(s.width == 1280);
    CHECK(s.pauseMs == 350);
    REQUIRE(s.steps.size() == 11);
    using K = demo::TourStep::Kind;
    CHECK(s.steps[0].kind == K::Wait);
    CHECK(s.steps[0].ms == 600);
    CHECK(s.steps[2].num == -520);
    CHECK(s.steps[3].kind == K::Type);
    CHECK(s.steps[3].num == 26);
    CHECK_STR(s.steps[5].arg2, "Can someone export");
    REQUIRE(s.steps[6].list.size() == 2);
    CHECK(s.steps[6].ms == 1000);
    CHECK_STR(s.steps[7].user, "U0YUKI");
    CHECK_STR(s.steps[7].arg2, "export");
    CHECK(s.steps[10].kind == K::Quit);

    // Pointing at views by name, and a dialog's text fields by place.
    REQUIRE(
        demo::parseTour(
            R"({"steps": [{"point": "Team", "in": "sidebar"},
            {"click": "Add teammate", "in": "sidebar"}, {"click": "Change folder"},
            {"field": 2}]})",
            &s,
            &err
        )
    );
    REQUIRE(s.steps.size() == 4);
    CHECK(s.steps[0].kind == K::Point);
    CHECK_STR(s.steps[0].arg2, "sidebar");
    CHECK(s.steps[1].kind == K::Click);
    CHECK_STR(s.steps[1].arg, "Add teammate");
    CHECK(s.steps[2].arg2.empty());
    CHECK(s.steps[3].kind == K::Field);
    CHECK(s.steps[3].num == 2);
    CHECK_FALSE(demo::parseTour(R"({"steps": [{"click": "x", "in": "menu"}]})", &s, &err));

    CHECK_FALSE(demo::parseTour(R"({"steps": [{"dance": 1}]})", &s, &err));
    CHECK_STR(err, "tour: step 1: unknown verb \"dance\"");
    CHECK_FALSE(demo::parseTour(R"({"steps": [{"theme": "blue"}]})", &s, &err));
    CHECK_FALSE(demo::parseTour(R"({"steps": [{"key": "F5"}]})", &s, &err));
    CHECK_FALSE(demo::parseTour(R"({"window": [640, 480], "steps": [{"quit": 1}]})", &s, &err));
    CHECK_FALSE(demo::parseTour(R"({"steps": []})", &s, &err));
    // The repository's own tours: the Slack workspace's, the Claude Code one's.
    REQUIRE(demo::loadTour(std::string(MSGA_DEMO_DIR) + "/tour.json", &s, &err));
    CHECK(s.steps.size() > 50);
    REQUIRE(demo::loadTour(std::string(MSGA_DEMO_DIR) + "/claude/tour.json", &s, &err));
    CHECK(s.steps.size() > 30);

    char  a0[] = "msga", a1[] = "--demo-tour=demo/tour.json", a2[] = "--demo-tour", a3[] = "t.json";
    char *v1[] = {a0, a1};
    char *v2[] = {a0, a2, a3};
    CHECK_STR(demo::tourPathFromArgs(2, v1), "demo/tour.json");
    CHECK_STR(demo::tourPathFromArgs(3, v2), "t.json");
}

TEST("demo fixture: demo/fixture.json loads and every asset it names exists") {
    model::Store  s;
    fake::Fixture fx;
    std::string   err;
    REQUIRE(fake::loadFixture(MSGA_DEMO_DIR, s, &fx, &err, base::fromLocal(2026, 9, 21, 16, 0)));
    CHECK(err.empty());
    CHECK(s.userCount() > 0);
    CHECK(s.conversationCount() > 0);
    for (model::UserRef u = 0; u < s.userCount(); ++u)
        if (!s.user(u).avatar.empty())
            CHECK(file::exists(s.user(u).avatar));
    for (model::ConvRef c = 0; c < s.conversationCount(); ++c)
        for (const auto &m : s.conversation(c).messages)
            for (const auto &f : m.files())
                CHECK(file::exists(f.path));
    for (const auto &cv : fx.canvases)
        CHECK(file::exists(cv.htmlPath));
    CHECK(file::isDir(fx.gifDir));
}

BASE_TEST_MAIN()
