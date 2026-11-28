#include "screens/shell/avatars.h"

#include "app/model/types.h"
#include "app/screens/common/avatar_initial.h"
#include "app/screens/common/remote_images.h"
#include "app/screens/messages/image_cache.h"
#include "base/str.h"

#include <algorithm>
#include <cmath>

namespace shell {

namespace {

// However small the pictures, the slots (failures too) stay this many.
constexpr size_t kMaxSlots = 4096;

} // namespace

Avatars::Avatars(screens::ImageCache &images, size_t budget)
    : _images(images), _alive(std::make_shared<char>(0)), _budget(budget) {}
Avatars::~Avatars() = default;

Avatars::Picture Avatars::get(const std::string &path, int px) {
    if (path.empty() || px <= 0)
        return nullptr;
    // The key built in a kept buffer: asking again (every row bind, every
    // refresh) allocates nothing.
    _probe.assign(path);
    _probe += '@';
    _probe += str::number(px);
    if (const auto it = _slots.find(_probe); it != _slots.end()) {
        Slot &s = it->second;
        if (s.ready && s.lru != _lru.begin())
            _lru.splice(_lru.begin(), _lru, s.lru);
        return s.bmp;
    }
    const std::string key  = _probe;
    std::string       file = path;
    if (screens::RemoteImages::isRemote(path)) {
        if (!_remote) { // no picture, and none later
            Slot &s = _slots[key];
            markReady(s, key);
            evict();
            return nullptr;
        }
        file = _remote->cachedPath(path);
    }
    // The blank the views hold until the picture fills it.
    Slot &s     = _slots[key];
    s.bmp       = std::make_shared<gfx::Bitmap>();
    Picture out = s.bmp;
    if (file.empty()) {
        s.downloading             = true;
        std::weak_ptr<char> alive = _alive;
        _remote->fetch(path, [this, alive, key, px](const std::string &f) {
            if (!alive.expired())
                landed(key, f, px);
        });
    } else {
        decode(key, file, px);
    }
    return out;
}

void Avatars::whenReady(const std::string &path, int px, std::function<void(Picture)> fn) {
    const Picture p = get(path, px);
    if (!p || !p->empty()) {
        fn(p);
        return;
    }
    const auto it = _slots.find(str::concat({path, "@", str::number(px)}));
    if (it == _slots.end() || it->second.downloading) {
        fn(nullptr);
        return;
    }
    it->second.waiting.push_back(std::move(fn));
}

void Avatars::decode(const std::string &key, const std::string &file, int px) {
    ++_decodes;
    std::weak_ptr<char> alive = _alive;
    // Small and waited for on screen: ahead of the message images.
    _images.decodeOnce(
        {file, px, px, screens::ImageCache::Shape::Square},
        [this, alive, key](gfx::Bitmap b) {
            if (!alive.expired())
                filled(key, std::move(b));
        },
        true
    );
}

void Avatars::landed(const std::string &key, const std::string &file, int px) {
    const auto it = _slots.find(key);
    if (it == _slots.end() || !it->second.downloading)
        return;
    if (file.empty()) {
        // Not remembered as a failure: a later get() asks again (RemoteImages
        // keeps that from becoming a storm). Views keep the blank.
        _slots.erase(it);
        return;
    }
    it->second.downloading = false;
    decode(key, file, px);
}

void Avatars::filled(const std::string &key, gfx::Bitmap b) {
    const auto it = _slots.find(key);
    if (it == _slots.end())
        return;
    Slot      &s  = it->second;
    const bool ok = !b.empty();
    if (ok) {
        s.bytes = size_t(b.width()) * size_t(b.height()) * 4;
        _bytes += s.bytes;
        *s.bmp = std::move(b); // in place: every view holding it shows it
    }
    // A file that can't be decoded: views keep the blank, and later asks
    // get null (remembered).
    const Picture pic     = ok ? Picture(s.bmp) : nullptr;
    auto          waiting = std::move(s.waiting);
    if (!ok)
        s.bmp.reset();
    markReady(s, key);
    evict(); // may drop `s`
    for (auto &fn : waiting)
        fn(pic);
    if (ok)
        notifyLoaded(pic);
}

void Avatars::markReady(Slot &s, const std::string &key) {
    s.ready = true;
    s.lru   = _lru.insert(_lru.begin(), key);
}

void Avatars::setBudget(size_t b) {
    _budget = b;
    evict();
}

void Avatars::evict() {
    // The least recently asked for first; views holding one keep it.
    while ((_bytes > _budget || _slots.size() > kMaxSlots) && !_lru.empty()) {
        const auto it = _slots.find(_lru.back());
        _lru.pop_back();
        if (it == _slots.end())
            continue;
        _bytes -= it->second.bytes;
        _slots.erase(it);
    }
}

void Avatars::notifyLoaded(Picture landed) {
    if (!onLoaded)
        return;
    _landed.push_back(std::move(landed)); // held until told: never a stale pointer
    if (_notifyQueued)
        return;
    // One repaint for a burst of avatars landing together.
    _notifyQueued             = true;
    std::weak_ptr<char> alive = _alive;
    ui::app()->platform().post([this, alive] {
        if (alive.expired())
            return;
        _notifyQueued                 = false;
        const std::vector<Picture> to = std::move(_landed);
        _landed.clear();
        if (onLoaded)
            onLoaded(to);
    });
}

plat::Image toPlatImage(const gfx::Bitmap &b) {
    plat::Image img;
    img.width  = b.width();
    img.height = b.height();
    img.pixels.assign(b.pixels(), b.pixels() + size_t(b.width()) * b.height());
    return img;
}

plat::Image roundedNotificationImage(const gfx::Bitmap &b) {
    const int side = std::min(b.width(), b.height());
    if (side <= 0)
        return {};
    gfx::Bitmap sq = gfx::coverResize(b.view(), side, side); // a crop, no resize
    gfx::maskRoundedRect(sq, float(side) * 0.22f);
    return toPlatImage(sq);
}

Avatar::Presence Avatar::presenceOf(const model::User *u, bool hasPresence, bool phantom) {
    return !u || u->bot || !hasPresence ? Presence::None
           : u->dnd                     ? Presence::Dnd
           : u->active                  ? Presence::Active
           : phantom || u->unavailable  ? Presence::Phantom
                                        : Presence::Away;
}

Avatar::Avatar() {
    setPaintOutset(3); // the dot's ring pokes past the corner
    setPlaceholder(ui::C::SidebarTextMuted);
}

void Avatar::setPresence(Presence p, ui::C ring, bool selected) {
    if (p == _presence && ring == _ring && selected == _selected)
        return;
    _presence = p;
    _ring     = ring;
    _selected = selected;
    update();
}

Avatar::~Avatar() = default;

void Avatar::setInitial(std::string_view name) {
    std::string i = screens::avatarInitial(name);
    if (i == _initial)
        return;
    _initial = std::move(i);
    _initialLayout.reset();
    update();
}

void Avatar::styleChanged() {
    _initialLayout.reset();
    Image::styleChanged();
}

void Avatar::paintOver(gfx::Painter &p) {
    if (!_initial.empty() && (!bitmap() || bitmap()->empty()))
        screens::paintInitial(p, *this, bounds(), 0, 0, _initial, _initialLayout);
    if (_presence == Presence::None)
        return;
    const ui::PointF c{width() - 3, height() - 3};
    if (_ring != ui::C::None)
        p.fillCircle(c, 5, ui::color(_ring));
    switch (_presence) {
    case Presence::Active:
        p.fillCircle(c, 3, ui::color(ui::C::Online));
        break;
    case Presence::Phantom:
        p.fillCircle(c, 3, ui::color(ui::C::PresencePhantom));
        break;
    case Presence::Dnd:
        p.fillCircle(c, 3, ui::color(ui::C::FormDivider));
        p.drawLine({c.x - 2, c.y}, {c.x + 2, c.y}, 1.5f, ui::color(ui::C::PresenceAway));
        break;
    default:
        p.strokeCircle(
            c, 3, 1, ui::color(_selected ? ui::C::SidebarSelectedText : ui::C::SidebarTextMuted)
        );
    }
}

} // namespace shell
