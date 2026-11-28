// ui::Image: a shrink made once per size, and animations that only advance
// while they are painted.
#include "harness.h"

#include <memory>
#include <vector>

using namespace uitest;

namespace {

std::shared_ptr<gfx::Bitmap> checker(int w, int h, uint32_t a, uint32_t b) {
    auto bmp = std::make_shared<gfx::Bitmap>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            bmp->pixels()[y * w + x] = ((x / 3 + y / 3) & 1) ? a : b;
    return bmp;
}

} // namespace

TEST("image: a shrunk bitmap is resized once, not on every paint") {
    Win   w(200, 150);
    auto *img = w.root().add<ui::Image>();
    img->style().size(40, 30).alignSelf(ui::Align::Start);
    img->setBitmap(checker(160, 120, 0xffff0000u, 0xff0000ffu));
    w.frame();
    CHECK(img->shrinkCount() == 1);
    for (int i = 0; i < 5; ++i) {
        img->update();
        w.frame();
    }
    CHECK(img->shrinkCount() == 1);
    // A new size shrinks again, once.
    img->style().size(50, 40);
    img->invalidateLayout();
    w.frame();
    CHECK(img->shrinkCount() == 2);
    img->update();
    w.frame();
    CHECK(img->shrinkCount() == 2);
    // A bitmap filled in place (an avatar that arrived) is shrunk anew.
    auto filled = std::make_shared<gfx::Bitmap>();
    img->setBitmap(filled);
    w.frame();
    const int before = img->shrinkCount();
    *filled          = *checker(100, 100, 0xff00ff00u, 0xff000000u);
    img->update();
    w.frame();
    CHECK(img->shrinkCount() == before + 1);
}

TEST("image: an animation stops while hidden and goes on when shown") {
    Win   w(200, 150);
    auto *img = w.root().add<ui::Image>();
    img->style().size(20, 20).alignSelf(ui::Align::Start);
    auto frames = std::make_shared<ui::Image::Frames>();
    for (uint32_t c : {0xffff0000u, 0xff00ff00u, 0xff0000ffu})
        frames->push_back({*checker(20, 20, c, c), 20});
    img->setFrames(frames);
    w.frame();
    CHECK(img->frameScheduled());
    CHECK(w.until([&] { return img->frameIndex() == 2; }));
    // Hidden: the frame due runs once more, then nothing is scheduled.
    img->setVisible(false);
    w.until([&] { return !img->frameScheduled(); });
    CHECK_FALSE(img->frameScheduled());
    const int stopped = img->frameIndex();
    for (int i = 0; i < 20; ++i)
        app().pump(5);
    CHECK(img->frameIndex() == stopped);
    CHECK_FALSE(img->frameScheduled());
    // Shown again: it goes on.
    img->setVisible(true);
    w.frame();
    CHECK(img->frameScheduled());
    CHECK(w.until([&] { return img->frameIndex() != stopped; }));
}
