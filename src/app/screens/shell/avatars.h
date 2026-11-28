// Small bitmaps the shell draws everywhere (avatars, the workspace icon,
// notification and tray pictures): decoded once per (path, size) and shared.
// Files are PNG / JPEG / GIF / WebP, or SVG (Claude Code's teammate tiles),
// which is rendered at the size asked for.
//
// Nothing is decoded on the UI thread: get() hands out an EMPTY bitmap — ui::
// Image paints its placeholder for it — and the ImageCache worker decodes the
// file (centre-cropped to a square, resized to px×px). When the picture lands
// that same bitmap is filled in place, so every view already holding it shows
// it without being told, and onLoaded runs (once per loop turn) for the shell
// to repaint the views showing those pictures. A path may be an http(s) URL
// (Slack's avatars): it is downloaded first (RemoteImages) unless on disk
// already. Callers that need pixels now (tray, notifications) use
// whenReady().
//
// The cache is bounded: least recently asked-for pictures go once their
// pixels pass the budget (views holding one keep it).
#pragma once

#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace screens {
class ImageCache;
class RemoteImages;
} // namespace screens

namespace model {
struct User;
}

namespace shell {

class Avatars {
public:
    using Picture = std::shared_ptr<const gfx::Bitmap>;

    explicit Avatars(screens::ImageCache &images, size_t budgetBytes = size_t(16) << 20);
    ~Avatars();
    Avatars(const Avatars &)            = delete;
    Avatars &operator=(const Avatars &) = delete;

    // The image at `path` scaled to fit px×px (aspect kept, centre-cropped to
    // a square); empty until decoded (or downloaded); null once the file
    // turned out missing or undecodable (remembered too), or for a URL with
    // no RemoteImages.
    Picture get(const std::string &path, int px);
    // The picture for `path` once it is there, null for none: at once when
    // it is decoded already, failed, or a URL still downloading (a
    // notification then goes without; the next one has it); otherwise as
    // soon as the worker has decoded it (on the UI thread, never inside
    // this call). Not called once this Avatars is gone.
    void    whenReady(const std::string &path, int px, std::function<void(Picture)> fn);

    size_t size() const { return _slots.size(); }
    size_t bytes() const { return _bytes; }
    size_t budget() const { return _budget; }
    void   setBudget(size_t b);
    // Decodes asked of the worker so far (tests).
    size_t decodes() const { return _decodes; }

    void setRemote(screens::RemoteImages *r) { _remote = r; }
    // Pictures filled their bitmaps (UI thread, coalesced): the ones that did.
    std::function<void(const std::vector<Picture> &landed)> onLoaded;

private:
    struct Slot {
        std::shared_ptr<gfx::Bitmap>              bmp; // null: failed
        std::vector<std::function<void(Picture)>> waiting;
        std::list<std::string>::iterator          lru; // valid while `ready`
        size_t                                    bytes       = 0;
        bool                                      ready       = false; // filled or failed
        bool                                      downloading = false;
    };
    void decode(const std::string &key, const std::string &file, int px);
    void landed(const std::string &key, const std::string &file, int px);
    void filled(const std::string &key, gfx::Bitmap b);
    void markReady(Slot &s, const std::string &key);
    void evict();
    void notifyLoaded(Picture landed);

    screens::ImageCache                  &_images;
    std::unordered_map<std::string, Slot> _slots;
    std::list<std::string>                _lru; // ready slots, most recently asked first
    screens::RemoteImages                *_remote = nullptr;
    std::shared_ptr<char>                 _alive;  // guards downloads and decodes
    std::string                           _probe;  // get()'s key buffer
    std::vector<Picture>                  _landed; // filled since onLoaded last ran
    size_t                                _bytes = 0, _budget, _decodes = 0;
    bool                                  _notifyQueued = false;
};

// premultiplied gfx::Bitmap → plat::Image (tray icons, notification pictures).
plat::Image toPlatImage(const gfx::Bitmap &b);
// A square picture masked into the rounded square
// avatars take everywhere (corner radius 22% of the side, anti-aliased), for
// an OS notification. Non-square input is centre-cropped first.
plat::Image roundedNotificationImage(const gfx::Bitmap &b);

// A square avatar (rounded or round) with a presence dot: a 6 px dot centred 3 px in from the
// bottom-right corner on a 10 px disc of `ring` (the row colour) — green when active, amber when
// away only for want of an official client (phantom), a DND bar, else a hollow ring (bright on a
// selected row).
class Avatar : public ui::Image {
public:
    enum class Presence : uint8_t { None, Active, Away, Phantom, Dnd };
    Avatar();
    ~Avatar() override;
    void            setPresence(Presence p, ui::C ring, bool selected = false);
    // The presence dot for a user: none for a bot or a backend
    // without presence, then DND, active, phantom (yellow: `phantom`, e.g. me
    // with no official client connected, or a peer that can't be reached,
    // User::unavailable), else away. Null u: none.
    static Presence presenceOf(const model::User *u, bool hasPresence, bool phantom = false);
    Presence        presence() const { return _presence; }
    // The initial-letter placeholder: the first letter of `name` over the
    // placeholder until the photo is there.
    void            setInitial(std::string_view name);
    void            paintOver(gfx::Painter &p) override;
    void            styleChanged() override;

private:
    std::string                   _initial;
    std::unique_ptr<text::Layout> _initialLayout;
    Presence                      _presence = Presence::None;
    ui::C                         _ring     = ui::C::Sidebar;
    bool                          _selected = false;
};

} // namespace shell
