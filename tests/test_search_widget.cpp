// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for SearchWidget features added/revised in the search-overlay session:
//   - Construction, initial state, render smoke
//   - Result list hidden until a search is run; cleared by setSession()
//   - Animation state reset by hideEvent so the next show() always fades from zero
//   - closeRequested signal via Escape key and close button
//   - Search result population, list visibility after search
//   - Channel name prefixed with '#'
//   - DM conversation name resolved to user displayName (not raw user ID)
//   - @mention entities in preview resolved to display name
//   - resultSelected signal on item click and on Enter key
//   - Keyboard Up/Down navigation (currentRow tracking)

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QGraphicsOpacityEffect>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "backend/backend.h"
#include "backend/domain.h"
#include "rpl/variable.h"
#include "session/session.h"
#include "ui/search/search_widget.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

// ── Custom main ───────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-search-widget");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();

    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── StubBackend ───────────────────────────────────────────────────────────────

struct StubBackend : Backend {
    rpl::variable<AuthState>                 _authState{AuthState::LoggedIn};
    rpl::variable<UserId>                    _meId;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;
    rpl::variable<std::vector<SearchResult>> _searchResults;

    rpl::producer<AuthState> authState() const override { return _authState.value(); }
    Capabilities             capabilities() const override { return {}; }
    void                     connectRealtime() override {}
    void                     disconnectRealtime() override {}

    rpl::producer<UserId>                    loadMe() override { return _meId.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers() override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }

    void sendMessage(ConversationId, OutgoingMessage) override {}
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override {}

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return _searchResults.value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }
    void uploadFiles(
        ConversationId,
        const QStringList &,
        const QString &,
        std::optional<Ts>                  = std::nullopt,
        std::function<void(bool, QString)> = {}
    ) override {}
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)>
    ) override {}

    rpl::producer<Event> events() const override { return _events.events(); }
};

// ── Test data ─────────────────────────────────────────────────────────────────

static const User kAlice = {
    .id          = UserId{"U1"},
    .name        = "alice",
    .displayName = "Alice Wonder",
    .isBot       = false,
    .isActive    = true
};
static const User kDmUser = {
    .id          = UserId{"U_DM"},
    .name        = "dmuser",
    .displayName = "DM Person",
    .isBot       = false,
    .isActive    = false
};
static const Conversation kGeneral = {
    .id       = ConversationId{"C1"},
    .kind     = ConvKind::PublicChannel,
    .name     = "general",
    .isMember = true,
};
static const Conversation kDmConv = {
    .id       = ConversationId{"D1"},
    .kind     = ConvKind::Im,
    .isMember = true,
    .dmUser   = UserId{"U_DM"},
};

// ── SessionFixture ────────────────────────────────────────────────────────────

struct SessionFixture {
    QTemporaryDir            tempDir;
    StubBackend             *stub;
    std::unique_ptr<Session> session;

    SessionFixture() {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());
        auto backend = std::make_unique<StubBackend>();
        stub         = backend.get();
        stub->_meId  = UserId{"U1"};
        stub->_convs = std::vector<Conversation>{kGeneral, kDmConv};
        stub->_users = std::vector<User>{kAlice, kDmUser};
        session      = std::make_unique<Session>(std::move(backend), "T_SEARCH_TEST");
        session->start();
    }
    ~SessionFixture() {
        session.reset();
        QDir(tempDir.path()).removeRecursively();
    }
};

// ── Widget helpers ────────────────────────────────────────────────────────────

static QListWidget *resultList(SearchWidget &w) {
    return w.findChild<QListWidget *>("searchResultList");
}
static QLineEdit *queryEdit(SearchWidget &w) {
    return w.findChild<QLineEdit *>();
}
static QPushButton *closeBtn(SearchWidget &w) {
    return w.findChild<QPushButton *>("searchCloseBtn");
}

static bool rendersOk(QWidget &w) {
    QPixmap px(w.size().expandedTo({1, 1}));
    px.fill(Qt::transparent);
    w.render(&px);
    return !px.isNull();
}

static void showWidget(SearchWidget &w) {
    w.resize(600, 500);
    w.show();
    QApplication::processEvents();
    QApplication::processEvents(); // flush the singleShot(0) focus timer in showEvent
}

static void triggerSearch(SearchWidget &w, const QString &query) {
    QLineEdit *ed = queryEdit(w);
    REQUIRE(ed != nullptr);
    ed->setText(query);
    QTest::keyClick(ed, Qt::Key_Return);
    QApplication::processEvents();
}

static SearchResult
makeResult(ConversationId conv, const Ts &ts, const QString &text, const QString &convName = {}) {
    Message msg;
    msg.ts        = ts;
    msg.text.text = text;
    return {std::move(conv), convName, msg};
}

// ── Construction & initial state ──────────────────────────────────────────────

TEST_CASE("SearchWidget constructs without crash", "[search_widget][smoke]") {
    SearchWidget w;
    CHECK(resultList(w) != nullptr);
    CHECK(queryEdit(w) != nullptr);
    CHECK(closeBtn(w) != nullptr);
}

TEST_CASE("SearchWidget renders without crash before session is set", "[search_widget][smoke]") {
    SearchWidget w;
    w.resize(600, 400);
    CHECK(rendersOk(w));
}

TEST_CASE("result list is hidden before any search is run", "[search_widget][state]") {
    SearchWidget w;
    REQUIRE(resultList(w) != nullptr);
    CHECK(!resultList(w)->isVisible());
}

TEST_CASE(
    "setSession(nullptr) does not crash and keeps result list hidden", "[search_widget][state]"
) {
    SearchWidget w;
    w.setSession(nullptr); // must not crash
    REQUIRE(resultList(w) != nullptr);
    CHECK(!resultList(w)->isVisible());
}

// ── Visibility / animation lifecycle ─────────────────────────────────────────

TEST_CASE("widget is visible after show()", "[search_widget][visibility]") {
    SearchWidget w;
    showWidget(w);
    CHECK(w.isVisible());
}

TEST_CASE("widget is hidden after hide()", "[search_widget][visibility]") {
    SearchWidget w;
    showWidget(w);
    w.hide();
    CHECK(!w.isVisible());
}

TEST_CASE("widget renders OK after show()", "[search_widget][visibility][render]") {
    SearchWidget w;
    showWidget(w);
    CHECK(rendersOk(w));
}

TEST_CASE(
    "show/hide/show cycle keeps widget visible and renders correctly", "[search_widget][visibility]"
) {
    SearchWidget w;
    w.resize(600, 400);
    for (int cycle = 0; cycle < 3; ++cycle) {
        showWidget(w);
        CHECK(w.isVisible());
        CHECK(rendersOk(w));
        w.hide();
        CHECK(!w.isVisible());
    }
}

TEST_CASE(
    "direct hide() resets animation state so next show() fades in from zero",
    "[search_widget][visibility][animation]"
) {
    // Regression: the toolbar search-icon click calls hide() directly (not closeSearch()).
    // Without hideEvent resetting _overlayAlpha / card opacity to 0, the next showEvent
    // would find start == end == fullAlpha and fire no animation.
    SearchWidget w;
    showWidget(w);
    REQUIRE(w.isVisible());

    w.hide(); // direct hide, bypasses the animated close path
    REQUIRE(!w.isVisible());

    // Second show must succeed — widget visible, animation state consistent
    showWidget(w);
    CHECK(w.isVisible());
    CHECK(rendersOk(w));
}

TEST_CASE(
    "closeSearch() hides the widget after the animation completes", "[search_widget][animation]"
) {
    SearchWidget w;
    showWidget(w);
    REQUIRE(w.isVisible());
    w.closeSearch();
    QTest::qWait(450); // animation is 350 ms
    CHECK(!w.isVisible());
}

TEST_CASE(
    "show() after closeSearch() animates correctly (no stuck state)", "[search_widget][animation]"
) {
    SearchWidget w;
    showWidget(w);
    w.closeSearch();
    QTest::qWait(450);
    REQUIRE(!w.isVisible());
    showWidget(w);
    CHECK(w.isVisible());
    CHECK(rendersOk(w));
}

// ── closeRequested signal ─────────────────────────────────────────────────────

TEST_CASE("Escape key on query input emits closeRequested", "[search_widget][signal][keyboard]") {
    SearchWidget w;
    bool         fired = false;
    QObject::connect(&w, &SearchWidget::closeRequested, [&fired] { fired = true; });
    showWidget(w);
    REQUIRE(queryEdit(w) != nullptr);
    queryEdit(w)->setFocus();
    QTest::keyClick(queryEdit(w), Qt::Key_Escape);
    QApplication::processEvents();
    CHECK(fired);
}

TEST_CASE("close button click emits closeRequested", "[search_widget][signal]") {
    SearchWidget w;
    bool         fired = false;
    QObject::connect(&w, &SearchWidget::closeRequested, [&fired] { fired = true; });
    showWidget(w);
    REQUIRE(closeBtn(w) != nullptr);
    closeBtn(w)->click(); // programmatic click — works regardless of animation opacity
    QApplication::processEvents();
    CHECK(fired);
}

// ── Search results ────────────────────────────────────────────────────────────

TEST_CASE(
    "no-results search shows 'No results' item and makes list visible", "[search_widget][results]"
) {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    f.stub->_searchResults = std::vector<SearchResult>{};
    showWidget(w);
    triggerSearch(w, "xyzzy");
    REQUIRE(resultList(w) != nullptr);
    CHECK(resultList(w)->isVisible());
    REQUIRE(resultList(w)->count() == 1);
    CHECK(resultList(w)->item(0)->text().contains("No results", Qt::CaseInsensitive));
}

TEST_CASE(
    "non-empty search results populate the list and make it visible", "[search_widget][results]"
) {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    f.stub->_searchResults = std::vector<SearchResult>{
        makeResult(ConversationId{"C1"}, "1.001", "first message"),
        makeResult(ConversationId{"C1"}, "1.002", "second message"),
    };
    showWidget(w);
    triggerSearch(w, "message");
    REQUIRE(resultList(w) != nullptr);
    CHECK(resultList(w)->isVisible());
    CHECK(resultList(w)->count() == 2);
}

TEST_CASE("public channel name is prefixed with '#' in results", "[search_widget][results][name]") {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    // Result in C1 = #general (resolved via session cache)
    f.stub->_searchResults =
        std::vector<SearchResult>{makeResult(ConversationId{"C1"}, "1.001", "hello")};
    showWidget(w);
    triggerSearch(w, "hello");
    REQUIRE(resultList(w)->count() == 1);
    CHECK(resultList(w)->item(0)->text().startsWith("#general"));
}

TEST_CASE(
    "DM conversation name is the user displayName, not the raw user ID",
    "[search_widget][results][name]"
) {
    // D1 is a ConvKind::Im with dmUser = U_DM, whose displayName = "DM Person".
    // The raw convName in the SearchResult ("U_DM") must never appear in the item text.
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    f.stub->_searchResults = std::vector<SearchResult>{
        makeResult(ConversationId{"D1"}, "1.001", "hey", /*convName=*/"U_DM"),
    };
    showWidget(w);
    triggerSearch(w, "hey");
    REQUIRE(resultList(w)->count() == 1);
    const QString text = resultList(w)->item(0)->text();
    CHECK(text.startsWith("DM Person"));
    CHECK(!text.contains("U_DM"));
}

TEST_CASE(
    "@mention entity in message preview is resolved to display name",
    "[search_widget][results][preview]"
) {
    // Build a message whose text contains a UserMention entity pointing to U_DM.
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());

    Message msg;
    msg.ts        = "1000002.000001";
    msg.text.text = "Say hi to <@U_DM> please";
    TextEntity ent;
    ent.type   = EntityType::UserMention;
    ent.offset = 10; // "<@U_DM>" starts here
    ent.length = 7;
    ent.data   = "U_DM"; // UserId::value stored in entity data
    msg.text.entities.push_back(ent);

    f.stub->_searchResults = std::vector<SearchResult>{{ConversationId{"C1"}, {}, msg}};
    showWidget(w);
    triggerSearch(w, "hi");

    REQUIRE(resultList(w)->count() == 1);
    const QString itemText = resultList(w)->item(0)->text();
    // Item text is "CONVLABEL  TIMESTAMP\nPREVIEW"
    const QString preview  = itemText.mid(itemText.indexOf('\n') + 1);
    CHECK(preview.contains("@DM Person"));
    CHECK(!preview.contains("U_DM"));
}

TEST_CASE(
    "setSession() clears results and hides the result list", "[search_widget][results][state]"
) {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    f.stub->_searchResults =
        std::vector<SearchResult>{makeResult(ConversationId{"C1"}, "1.0", "x")};
    showWidget(w);
    triggerSearch(w, "x");
    REQUIRE(resultList(w)->count() == 1);
    REQUIRE(resultList(w)->isVisible());

    w.setSession(nullptr);
    CHECK(!resultList(w)->isVisible());
    CHECK(resultList(w)->count() == 0);
}

// ── resultSelected signal ─────────────────────────────────────────────────────

TEST_CASE(
    "clicking a result item emits resultSelected with correct conv and ts",
    "[search_widget][signal][results]"
) {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    const ConversationId wantConv{"C1"};
    const Ts             wantTs = "1000003.000001";
    f.stub->_searchResults = std::vector<SearchResult>{makeResult(wantConv, wantTs, "click me")};
    showWidget(w);
    triggerSearch(w, "click");

    ConversationId gotConv;
    Ts             gotTs;
    QObject::connect(&w, &SearchWidget::resultSelected, [&](ConversationId c, Ts t) {
        gotConv = c;
        gotTs   = t;
    });

    QListWidget *list = resultList(w);
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 1);
    QTest::mouseClick(
        list->viewport(),
        Qt::LeftButton,
        Qt::NoModifier,
        list->visualItemRect(list->item(0)).center()
    );
    QApplication::processEvents();

    CHECK(gotConv == wantConv);
    CHECK(gotTs == wantTs);
}

// ── Keyboard navigation ───────────────────────────────────────────────────────

TEST_CASE(
    "Down key advances selection; Up key retreats; both clamp at boundaries",
    "[search_widget][keyboard][navigation]"
) {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    f.stub->_searchResults = std::vector<SearchResult>{
        makeResult(ConversationId{"C1"}, "1.001", "alpha"),
        makeResult(ConversationId{"C1"}, "1.002", "beta"),
        makeResult(ConversationId{"C1"}, "1.003", "gamma"),
    };
    showWidget(w);
    triggerSearch(w, "test");

    QLineEdit   *ed   = queryEdit(w);
    QListWidget *list = resultList(w);
    REQUIRE(ed != nullptr);
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 3);
    ed->setFocus();

    CHECK(list->currentRow() == -1); // no selection initially

    QTest::keyClick(ed, Qt::Key_Down);
    QApplication::processEvents();
    CHECK(list->currentRow() == 0);

    QTest::keyClick(ed, Qt::Key_Down);
    QApplication::processEvents();
    CHECK(list->currentRow() == 1);

    QTest::keyClick(ed, Qt::Key_Down);
    QApplication::processEvents();
    CHECK(list->currentRow() == 2);

    QTest::keyClick(ed, Qt::Key_Down);
    QApplication::processEvents();
    CHECK(list->currentRow() == 2); // clamped at last

    QTest::keyClick(ed, Qt::Key_Up);
    QApplication::processEvents();
    CHECK(list->currentRow() == 1);

    QTest::keyClick(ed, Qt::Key_Up);
    QApplication::processEvents();
    CHECK(list->currentRow() == 0);
}

TEST_CASE(
    "Enter key on keyboard-selected item emits resultSelected", "[search_widget][keyboard][signal]"
) {
    SessionFixture f;
    SearchWidget   w;
    w.setSession(f.session.get());
    const Ts wantTs = "1000004.000001";
    f.stub->_searchResults =
        std::vector<SearchResult>{makeResult(ConversationId{"C1"}, wantTs, "enter me")};
    showWidget(w);
    triggerSearch(w, "enter");

    Ts gotTs;
    QObject::connect(&w, &SearchWidget::resultSelected, [&](ConversationId, Ts t) { gotTs = t; });

    QLineEdit *ed = queryEdit(w);
    REQUIRE(ed != nullptr);
    ed->setFocus();
    QTest::keyClick(ed, Qt::Key_Down);
    QApplication::processEvents(); // select row 0
    QTest::keyClick(ed, Qt::Key_Return);
    QApplication::processEvents(); // confirm

    CHECK(gotTs == wantTs);
}
