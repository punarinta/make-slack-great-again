// StatusNotifierItem tray icon with a com.canonical.dbusmenu menu.
//
// Each tray owns its own bus connection. The SNI spec identifies an item by
// the bus name it registered with, and hosts only drop an item when that
// name disappears — there is no Unregister call. With one connection per
// tray, destroying the tray closes the connection and every watcher removes
// the icon; a shared connection would leave a ghost icon until the app exits
// (and could not serve two items at the spec's fixed /StatusNotifierItem).
#pragma once

#include "freedesktop/dbus_conn.h"

#include <chrono>
#include <map>
#include <memory>
#include <set>

namespace plat::linux_services {

class SniTray final : public Tray {
public:
    static constexpr const char *kItemPath = "/StatusNotifierItem";
    static constexpr const char *kMenuPath = "/MenuBar";

    SniTray(BackendApp &app, const std::string &address);
    ~SniTray() override;

    void setIcon(const std::vector<Image> &sizes) override;
    void setTooltip(std::string_view utf8) override;
    void setMenu(std::vector<MenuItem> items) override;
    bool isVisible() const override { return _registered && _hostRegistered; }

    // For the test hooks: where our objects live, and the dbusmenu id of the
    // first selectable plat item `id` (depth-first), or -1.
    const std::string &busName() const { return _bus.uniqueName(); }
    bool               busReady() const { return _bus.ready(); }
    int32_t            menuIdFor(uint32_t id) const;

private:
    struct Pixmap {
        int                  w = 0, h = 0;
        std::vector<uint8_t> argb; // network byte order, straight alpha
    };
    struct Node {
        const MenuItem      *item = nullptr; // null for the root (id 0)
        std::vector<int32_t> children;
    };

    void registerWithWatcher();
    void retryRegister();
    void cancelRetry();
    void registered();
    void queryHost();
    bool handleItem(DBusMessage *m);
    bool handleMenu(DBusMessage *m);
    bool writeItemProperty(MsgWriter &w, std::string_view name) const;
    bool writeMenuProperty(MsgWriter &w, std::string_view name) const;
    void writePixmaps(MsgWriter &w) const;
    void
    writeLayout(MsgWriter &w, int32_t id, int depth, const std::vector<std::string> &props) const;
    void writeMenuProps(MsgWriter &w, int32_t id, const std::vector<std::string> &props) const;
    bool writeOneMenuProp(MsgWriter &w, int32_t id, std::string_view name) const;
    void itemChanged(const char *signal, const char *property);
    void rebuildMenu();
    void menuClicked(int32_t id);
    void postEmit(Event e);
    std::string takeToken();

    BackendApp                           &_app;
    Bus                                   _bus;
    std::string                           _id, _title;
    std::vector<Pixmap>                   _icons;
    std::string                           _tooltip;
    std::vector<MenuItem>                 _menu;
    std::map<int32_t, Node>               _nodes;
    std::map<const MenuItem *, int32_t>   _idOf;
    uint32_t                              _revision = 1;
    std::string                           _watcherOwner;
    std::string                           _token; // ProvideXdgActivationToken
    std::chrono::steady_clock::time_point _tokenTime;
    bool                 _registering = false, _registered = false, _hostRegistered = false;
    TimerId              _retryTimer = 0;
    int                  _retryMs    = 0; // the next retry's delay; 0 = none failed yet
    std::shared_ptr<int> _alive      = std::make_shared<int>(0); // posted emits check it
};

} // namespace plat::linux_services
