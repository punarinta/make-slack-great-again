#include "harness.h"

#include <cmath>

using namespace uitest;

namespace {

// Rows are plain boxes whose height comes from a vector the test mutates.
struct Rows : ui::VirtualList::Adapter {
    std::vector<float>    h;
    std::vector<uint64_t> keys; // Adapter::key per row (empty: none)
    int                   created = 0, binds = 0, reuses = 0;
    explicit Rows(int n, float height = 30) : h(size_t(n), height) {}
    int      count() const override { return int(h.size()); }
    uint64_t key(int i) const override { return keys.empty() ? 0 : keys[size_t(i)]; }
    bool     reuse(ui::View &, int) override {
        ++reuses;
        return true;
    }
    std::unique_ptr<ui::View> create(int) override {
        ++created;
        return std::make_unique<Box>();
    }
    void bind(ui::View &row, int i) override {
        ++binds;
        static_cast<Box &>(row).content = {10, h[size_t(i)]};
        row.invalidateLayout();
    }
};

struct ListWin : Win {
    Rows             rows;
    ui::VirtualList *list;
    explicit ListWin(int n, bool stick = true) : Win(300, 300), rows(n) {
        list = w->root().add<ui::VirtualList>(&rows);
        list->setBackground(ui::C::Surface);
        list->setStickToBottom(stick);
        frame();
    }
    float yOf(int i) const {
        ui::View *v = list->viewFor(i);
        return v ? v->frame().y : NAN;
    }
};

} // namespace

TEST("vlist: opens at the bottom and measures only what it shows") {
    ListWin l(10000);
    CHECK(l.list->pinned());
    CHECK(near(l.yOf(9999) + 30, 300)); // last row's bottom at the viewport bottom
    CHECK(l.list->measuredCount() < 30);
    CHECK(l.list->liveCount() < 30);
    CHECK(l.rows.created < 30);
}

TEST("vlist: pinned bottom stays pinned when rows are appended or grow") {
    ListWin l(1000);
    l.rows.h.push_back(30);
    l.rows.h.push_back(60);
    l.list->itemsInserted(1000, 2);
    l.frame();
    CHECK(l.list->pinned());
    CHECK(near(l.yOf(1001) + 60, 300));
    CHECK(near(l.yOf(1000) + 30, l.yOf(1001)));
    // The newest row grows (an image loaded): still pinned to the bottom.
    l.rows.h[1001] = 120;
    l.list->itemsChanged(1001, 1);
    l.frame();
    CHECK(near(l.yOf(1001) + 120, 300));
}

TEST("vlist: prepending history while scrolled keeps the viewport on the same row") {
    ListWin l(1000);
    l.list->scrollToItem(500, ui::VirtualList::ItemAlign::Start, false);
    l.frame();
    l.list->scrollBy(10); // 10 px into row 500
    l.frame();
    CHECK(!l.list->pinned());
    CHECK(near(l.yOf(500), -10));
    const float y501 = l.yOf(501);
    l.rows.h.insert(l.rows.h.begin(), 50, 45.f);
    l.list->itemsInserted(0, 50);
    l.frame();
    CHECK(near(l.yOf(550), -10)); // same content, same place
    CHECK(near(l.yOf(551), y501));
    CHECK(l.list->anchor().index == 550);
}

TEST("vlist: a row above the viewport changing height does not move the viewport") {
    ListWin l(1000);
    l.list->scrollToItem(500, ui::VirtualList::ItemAlign::Start, false);
    l.frame();
    l.list->scrollBy(10);
    l.frame();
    REQUIRE(l.list->viewFor(498) != nullptr); // live in the overscan above
    l.rows.h[498] = 100;
    l.list->itemsChanged(498, 1);
    l.rows.h[100] = 500; // far above, never measured
    l.list->itemsChanged(100, 1);
    l.frame();
    CHECK(near(l.yOf(500), -10));
    CHECK(near(l.yOf(499), -40));
    CHECK(near(l.yOf(498), -140));
    // A row inside the viewport growing pushes the rows below it, not above.
    l.rows.h[502] = 90;
    l.list->itemsChanged(502, 1);
    l.frame();
    CHECK(near(l.yOf(500), -10) && near(l.yOf(503), 20 + 30 + 90));
}

TEST("vlist: removing rows keeps the list consistent") {
    ListWin l(100);
    l.list->scrollToItem(50, ui::VirtualList::ItemAlign::Start, false);
    l.frame();
    l.rows.h.erase(l.rows.h.begin() + 40, l.rows.h.begin() + 45);
    l.list->itemsRemoved(40, 5);
    l.frame();
    CHECK(l.list->anchor().index == 45 && near(l.yOf(45), 0));
    l.rows.h.erase(l.rows.h.begin() + 45, l.rows.h.begin() + 47); // the anchor itself
    l.list->itemsRemoved(45, 2);
    l.frame();
    CHECK(l.list->anchor().index == 45 && near(l.yOf(45), 0));
}

TEST("vlist: scrolling to the end re-pins; scrolling up unpins") {
    ListWin l(200);
    CHECK(l.list->pinned());
    l.list->scrollBy(-100);
    l.frame();
    CHECK(!l.list->pinned());
    CHECK(near(l.yOf(199) + 30, 400));
    l.list->scrollBy(1000);
    l.frame();
    CHECK(l.list->pinned());
    l.list->scrollBy(-1e7f); // to the very top
    l.frame();
    CHECK(near(l.yOf(0), 0) && l.list->anchor().index == 0);
}

TEST("vlist: animated jump to a far item ends exactly aligned") {
    ListWin l(10000);
    l.list->scrollToItem(2000, ui::VirtualList::ItemAlign::Center, true);
    bool ok = l.until([&] {
        ui::View *v = l.list->viewFor(2000);
        return v && near(v->frame().y, std::floor((300 - 30) / 2.f)) && l.list->liveCount() > 0 &&
               l.list->anchor().index <= 2000;
    });
    CHECK(ok);
    CHECK(l.list->measuredCount() < 200); // it did not measure the 8000 rows between
}

TEST("vlist: rows are recycled while scrolling far") {
    ListWin l(10000);
    for (int i = 0; i < 200; ++i) {
        l.list->scrollBy(-97);
        l.frame(1);
    }
    CHECK(l.rows.created < 40);
    CHECK(l.list->liveCount() < 30);
    CHECK(l.rows.binds > 100);
}

TEST("vlist: kept rows come back unbound; changes, removal and reset drop them") {
    ListWin l(1000, false);
    for (int i = 0; i < 1000; ++i)
        l.rows.keys.push_back(uint64_t(i + 1));
    l.list->setKeep(20);
    l.list->scrollToItem(500, ui::VirtualList::ItemAlign::Start, false);
    l.frame();
    const int shown = l.list->liveCount();
    // Down a viewport and back: the rows of 500… return as they were.
    l.list->scrollBy(400);
    l.frame();
    const int binds = l.rows.binds, reuses = l.rows.reuses;
    l.list->scrollBy(-400);
    l.frame();
    REQUIRE(l.list->viewFor(500) != nullptr);
    CHECK(l.rows.binds == binds);
    CHECK(l.rows.reuses - reuses >= 10);
    CHECK(near(l.yOf(500), 0));

    // A changed row scrolled out is bound again when it comes back.
    l.list->scrollBy(400);
    l.frame();
    REQUIRE(l.list->viewFor(501) == nullptr);
    l.rows.h[501] = 50;
    l.list->itemsChanged(501, 1);
    int b = l.rows.binds;
    l.list->scrollBy(-400);
    l.frame();
    CHECK(l.rows.binds == b + 1);
    CHECK(near(l.yOf(502) - l.yOf(501), 50));

    // A removed item's row is not handed to a new item under its key.
    l.list->scrollBy(400);
    l.frame();
    l.rows.h.erase(l.rows.h.begin() + 505);
    l.rows.keys.erase(l.rows.keys.begin() + 505);
    l.list->itemsRemoved(505, 1);
    l.rows.h.insert(l.rows.h.begin() + 505, 30.f);
    l.rows.keys.insert(l.rows.keys.begin() + 505, uint64_t(506));
    l.list->itemsInserted(505, 1);
    b = l.rows.binds;
    l.list->scrollBy(-400);
    l.frame();
    CHECK(l.rows.binds == b + 1);

    // After a reset nothing kept is trusted.
    b = l.rows.binds;
    l.list->reset();
    l.list->scrollToItem(500, ui::VirtualList::ItemAlign::Start, false);
    l.frame();
    CHECK(l.rows.binds >= b + shown - 1);
}

TEST("vlist: kept rows stay bounded while scrolling far") {
    ListWin l(10000);
    for (int i = 0; i < 10000; ++i)
        l.rows.keys.push_back(uint64_t(i + 1));
    l.list->setKeep(16);
    for (int i = 0; i < 200; ++i) {
        l.list->scrollBy(-97);
        l.frame(1);
    }
    CHECK(l.rows.binds > 100);
    CHECK(l.list->liveCount() < 30);
    CHECK(l.list->childCount() <= size_t(l.list->liveCount()) + 16 + 8 + 2);
    CHECK(l.rows.created < 30 + 16 + 8);
}

TEST("vlist: variable heights with estimates keep a sane scrollbar extent") {
    struct Mixed : Rows {
        using Rows::Rows;
        float estimateHeight(int) const override { return 40; }
    } rows(1000);
    for (size_t i = 0; i < rows.h.size(); ++i)
        rows.h[i] = 20 + float(i % 7) * 10;
    Win   w(300, 300);
    auto *list = w.root().add<ui::VirtualList>(&rows);
    list->setStickToBottom(true);
    w.frame();
    CHECK(list->contentExtent() > 30000 && list->contentExtent() < 60000);
    CHECK(list->atEnd());
}

TEST("scroll: wheel notches glide, touchpad flings after lift, blits the canvas") {
    ListWin l(10000, false);
    l.list->scrollToItem(5000, ui::VirtualList::ItemAlign::Start, false);
    l.frame();
    l.move(150, 150);
    const int a0 = l.list->anchor().index;
    hooks().injectScroll(l.native(), 0, 1); // one notch down
    CHECK(l.until([&] { return l.list->anchor().index > a0; }));
    const float before = l.list->scrollOffset();
    hooks().injectPhasedScroll(l.native(), 0, 0, plat::ScrollPhase::Begin);
    for (int i = 0; i < 5; ++i) {
        hooks().injectPhasedScroll(l.native(), 0, 20, plat::ScrollPhase::Update);
        app().pump(1);
    }
    const float atLift = l.list->scrollOffset();
    CHECK(atLift >= before + 99);
    hooks().injectPhasedScroll(l.native(), 0, 0, plat::ScrollPhase::End);
#ifndef __APPLE__
    CHECK(l.until([&] { return l.list->scrollOffset() > atLift + 20; }));
#endif
    // A single small scroll step repaints a strip, not the viewport.
    l.list->stopScrolling();
    l.frame();
    l.list->scrollBy(7);
    l.frame();
    CHECK(l.damageArea() > 0);
    CHECK(l.damageArea() < 300 * 300 / 4);
}

TEST("scroll: ScrollView clamps, ensureVisible, scrollbar drag") {
    Win                w(300, 200);
    auto              *sv  = w.root().add<ui::ScrollView>();
    auto              *col = sv->content();
    std::vector<Box *> boxes;
    for (int i = 0; i < 20; ++i)
        boxes.push_back(col->add<Box>(10, 50));
    w.frame();
    CHECK(sv->contentExtent() == 1000);
    sv->scrollBy(5000);
    w.frame();
    CHECK(sv->scrollOffset() == 800);
    sv->ensureVisible(boxes[2]);
    w.frame();
    CHECK(sv->scrollOffset() == 100);
    // Drag the thumb to the bottom of the track.
    w.move(296, 2 + (196 - 40) * 100.f / 800 + 5); // on the thumb
    w.press();
    w.move(296, 400);
    w.release();
    CHECK(sv->scrollOffset() == 800);
}

TEST("scroll: a thin thumb grabs only on itself and keeps its width") {
    Win                w(300, 200);
    auto              *sv  = w.root().add<ui::ScrollView>();
    auto              *col = sv->content();
    std::vector<Box *> boxes;
    for (int i = 0; i < 20; ++i)
        boxes.push_back(col->add<Box>(300, 50));
    sv->setThinThumb(ui::C::SidebarScrollbar);
    w.frame();
    sv->scrollBy(100);
    w.frame();
    CHECK(sv->scrollOffset() == 100);
    // Full-height track, 40 px thumb at (200 - 40) * 100 / 800 = 20.
    CHECK(sv->hitTest({296, 40}) == sv);
    CHECK(sv->cursorAt({296, 40}) == uint8_t(plat::Cursor::ResizeV));
    // Beside the thumb the row underneath gets the press: no paging.
    CHECK(sv->hitTest({296, 150}) == boxes[5]);
    w.move(296, 150);
    w.press();
    w.release();
    w.frame();
    CHECK(sv->scrollOffset() == 100);
    // Dragging the thumb still scrolls.
    w.move(296, 40);
    w.press();
    w.move(296, 400);
    w.release();
    CHECK(sv->scrollOffset() == 800);
}

// Appending (new messages) extends the offset index in place instead of
// rebuilding it: the extent matches a full rebuild (setGap forces one),
// with measured, estimated and unknown rows mixed.
TEST("vlist: appended rows extend the offset index exactly") {
    struct Mixed : Rows {
        using Rows::Rows;
        float estimateHeight(int i) const override { return i % 3 == 0 ? 40 : 0; }
    } rows(300);
    for (size_t i = 0; i < rows.h.size(); ++i)
        rows.h[i] = 20 + float(i % 5) * 10;
    Win   w(300, 300);
    auto *list = w.root().add<ui::VirtualList>(&rows);
    w.frame();
    list->scrollToItem(150, ui::VirtualList::ItemAlign::Start, false);
    w.frame();
    for (int batch = 0; batch < 5; ++batch) {
        const int n = int(rows.h.size()), k = 1 + batch * 7;
        for (int i = 0; i < k; ++i)
            rows.h.push_back(20 + float((n + i) % 5) * 10);
        list->itemsInserted(n, k);
        const float ext = list->contentExtent();
        list->setGap(0); // the same gap: only rebuilds the index
        CHECK(list->contentExtent() == ext);
        list->scrollBy(-200); // measures a few more rows on the way
        w.frame();
    }
}

// Spares are kept per kind (8 each), whatever order the kinds come back in.
TEST("vlist: spare rows stay bounded per kind") {
    struct Kinds : Rows {
        using Rows::Rows;
        int kind(int i) const override { return i % 3; }
    } rows(3000);
    Win   w(300, 300);
    auto *list = w.root().add<ui::VirtualList>(&rows);
    w.frame();
    for (int i = 0; i < 150; ++i) {
        list->scrollBy(-97);
        w.frame(1);
    }
    CHECK(list->liveCount() < 30);
    CHECK(list->childCount() <= size_t(list->liveCount()) + 3 * 8);
    CHECK(rows.created < 30 + 3 * 8);
}
