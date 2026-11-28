#include "freedesktop/sni_tray.h"

#include "core/image_util.h"

#include <algorithm>
#include <climits>

namespace plat::linux_services {

namespace {

constexpr const char *kWatcher     = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherPath = "/StatusNotifierWatcher";
constexpr const char *kItemIface   = "org.kde.StatusNotifierItem";
constexpr const char *kMenuIface   = "com.canonical.dbusmenu";
constexpr const char *kPropsIface  = "org.freedesktop.DBus.Properties";
constexpr const char *kIntroIface  = "org.freedesktop.DBus.Introspectable";
constexpr const char *kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
constexpr const char *kUnknownProp = "org.freedesktop.DBus.Error.UnknownProperty";

constexpr const char *kItemIntrospection =
    R"xml(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <method name="ProvideXdgActivationToken"><arg name="token" type="s" direction="in"/></method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
    <method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method>
    <signal name="PropertiesChanged"><arg type="s"/><arg type="a{sv}"/><arg type="as"/></signal>
  </interface>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect"><arg type="s" direction="out"/></method>
  </interface>
</node>
)xml";

constexpr const char *kMenuIntrospection =
    R"xml(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout"><arg type="i" name="parentId" direction="in"/><arg type="i" name="recursionDepth" direction="in"/><arg type="as" name="propertyNames" direction="in"/><arg type="u" name="revision" direction="out"/><arg type="(ia{sv}av)" name="layout" direction="out"/></method>
    <method name="GetGroupProperties"><arg type="ai" name="ids" direction="in"/><arg type="as" name="propertyNames" direction="in"/><arg type="a(ia{sv})" name="properties" direction="out"/></method>
    <method name="GetProperty"><arg type="i" name="id" direction="in"/><arg type="s" name="name" direction="in"/><arg type="v" name="value" direction="out"/></method>
    <method name="Event"><arg type="i" name="id" direction="in"/><arg type="s" name="eventId" direction="in"/><arg type="v" name="data" direction="in"/><arg type="u" name="timestamp" direction="in"/></method>
    <method name="EventGroup"><arg type="a(isvu)" name="events" direction="in"/><arg type="ai" name="idErrors" direction="out"/></method>
    <method name="AboutToShow"><arg type="i" name="id" direction="in"/><arg type="b" name="needUpdate" direction="out"/></method>
    <method name="AboutToShowGroup"><arg type="ai" name="ids" direction="in"/><arg type="ai" name="updatesNeeded" direction="out"/><arg type="ai" name="idErrors" direction="out"/></method>
    <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})" name="updatedProps"/><arg type="a(ias)" name="removedProps"/></signal>
    <signal name="LayoutUpdated"><arg type="u" name="revision"/><arg type="i" name="parent"/></signal>
    <signal name="ItemActivationRequested"><arg type="i" name="id"/><arg type="u" name="timestamp"/></signal>
  </interface>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
    <method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method>
  </interface>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect"><arg type="s" direction="out"/></method>
  </interface>
</node>
)xml";

constexpr const char *kItemProps[] = {
    "Category",
    "Id",
    "Title",
    "Status",
    "WindowId",
    "IconThemePath",
    "Menu",
    "ItemIsMenu",
    "IconName",
    "IconPixmap",
    "OverlayIconName",
    "OverlayIconPixmap",
    "AttentionIconName",
    "AttentionIconPixmap",
    "AttentionMovieName",
    "ToolTip",
};
constexpr const char *kMenuProps[] = {"Version", "TextDirection", "Status", "IconThemePath"};

bool selectable(const MenuItem &m) {
    return m.kind == MenuItem::Kind::Action || m.kind == MenuItem::Kind::Checkbox;
}

// dbusmenu labels use GTK mnemonics: "_" marks the access key and "__" is a
// literal underscore. plat labels have no mnemonics, so double every one.
std::string menuLabel(const std::string &s) {
    std::string out;
    for (char c : s) {
        out += c;
        if (c == '_')
            out += '_';
    }
    return out;
}

bool wanted(const std::vector<std::string> &props, std::string_view name) {
    return props.empty() || std::find(props.begin(), props.end(), name) != props.end();
}

void replyMessage(Bus &bus, DBusMessage *call, Fill fill) {
    if (dbus_message_get_no_reply(call))
        return;
    MessageRef r(dbus_message_new_method_return(call));
    MsgWriter  w(r.get());
    fill(w);
    bus.send(r);
}

} // namespace

SniTray::SniTray(BackendApp &app, const std::string &address)
    : _app(app), _bus(app, address), _id(app.appInfo().id), _title(app.appInfo().name) {
    _bus.addObject(kItemPath, [this](DBusMessage *m) { return handleItem(m); });
    _bus.addObject(kMenuPath, [this](DBusMessage *m) { return handleMenu(m); });
    _bus.onLost = [this] {
        _watcherOwner.clear();
        _registering = _registered = _hostRegistered = false;
        cancelRetry();
    };
    // (Re)register whenever a watcher appears; forget everything when it goes.
    _bus.watchName(kWatcher, [this](const std::string &owner) {
        if (owner == _watcherOwner && (_registering || _registered || _retryTimer))
            return;
        _watcherOwner = owner;
        _registered = _hostRegistered = false;
        cancelRetry();
        if (!owner.empty())
            registerWithWatcher();
    });
    // A watcher busy at login (kded5 in Plasma 5) can answer the registration
    // after our call has timed out, and still show the icon. Its signal for
    // our item says so either way.
    _bus.subscribe(
        "type='signal',interface='org.kde.StatusNotifierWatcher',"
        "member='StatusNotifierItemRegistered'",
        kWatcher,
        "StatusNotifierItemRegistered",
        [this](DBusMessage *m) {
            std::string key;
            MsgReader(m).str(&key);
            const std::string &me = _bus.uniqueName();
            if (!me.empty() && (key == me || key == me + kItemPath))
                registered();
        }
    );
    _bus.subscribe(
        "type='signal',interface='org.kde.StatusNotifierWatcher'",
        kWatcher,
        "StatusNotifierHostRegistered",
        [this](DBusMessage *) { _hostRegistered = true; }
    );
    _bus.subscribe(
        "type='signal',interface='org.kde.StatusNotifierWatcher'",
        kWatcher,
        "StatusNotifierHostUnregistered",
        [this](DBusMessage *) { queryHost(); }
    );
    rebuildMenu();
    _bus.start();
}

// Closing our connection unregisters us everywhere.
SniTray::~SniTray() {
    cancelRetry();
}

void SniTray::registerWithWatcher() {
    if (!_bus.ready())
        return;
    _registering = true;
    auto m       = methodCall(kWatcher, kWatcherPath, kWatcher, "RegisterStatusNotifierItem");
    // Our unique name: watchers then look for the item at /StatusNotifierItem,
    // the spec's default, and drop it when the connection closes.
    MsgWriter(m.get()).str(_bus.uniqueName());
    _bus.call(m, [this, owner = _watcherOwner](DBusMessage *reply, const char *, const char *) {
        if (owner != _watcherOwner || _registered)
            return; // a watcher since replaced, or its signal got here first
        _registering = false;
        if (reply)
            registered();
        else
            retryRegister();
    });
}

// A failed or timed-out registration is not the watcher's last word: one
// still starting up may only be slow. Ask again, less and less often; a
// watcher that already has us just says yes.
void SniTray::retryRegister() {
    _retryMs    = _retryMs ? std::min(_retryMs * 2, 60000) : 2000;
    _retryTimer = _app.addTimer(_retryMs, false, [this] {
        _retryTimer = 0;
        if (!_registered && !_registering && !_watcherOwner.empty())
            registerWithWatcher();
    });
}

void SniTray::cancelRetry() {
    if (_retryTimer)
        _app.cancelTimer(_retryTimer);
    _retryTimer = 0;
    _retryMs    = 0;
}

void SniTray::registered() {
    if (_registered)
        return;
    _registering = false;
    _registered  = true;
    cancelRetry();
    queryHost();
}

void SniTray::queryHost() {
    auto      m = methodCall(kWatcher, kWatcherPath, kPropsIface, "Get");
    MsgWriter w(m.get());
    w.str(kWatcher);
    w.str("IsStatusNotifierHostRegistered");
    _bus.call(m, [this](DBusMessage *reply, const char *, const char *) {
        bool      on = true; // a watcher without the property: assume its host shows us
        MsgReader r(reply);
        if (reply)
            r.value().boolean(&on);
        _hostRegistered = on;
    });
}

void SniTray::postEmit(Event e) {
    e.tray = this;
    linux_services::postEmit(_app, _alive, std::move(e));
}

// ── icon / tooltip ──────────────────────────────────────────────────────────

void SniTray::setIcon(const std::vector<Image> &sizes) {
    _icons.clear();
    for (const auto &img : sizes) {
        if (img.empty() || img.pixels.size() < size_t(img.width) * img.height)
            continue;
        Pixmap p{img.width, img.height, {}};
        p.argb.resize(size_t(img.width) * img.height * 4);
        uint8_t *o = p.argb.data();
        for (uint32_t px : img.pixels) {
            // SNI wants ARGB32 in network byte order with straight alpha.
            const uint32_t s = core::unpremultiply(px);
            for (int sh = 24; sh >= 0; sh -= 8)
                *o++ = uint8_t(s >> sh);
        }
        _icons.push_back(std::move(p));
    }
    itemChanged("NewIcon", "IconPixmap");
}

void SniTray::setTooltip(std::string_view utf8) {
    _tooltip = sanitizeUtf8(utf8);
    itemChanged("NewToolTip", "ToolTip");
}

void SniTray::itemChanged(const char *signal, const char *property) {
    if (!_bus.ready())
        return; // hosts read everything fresh when we register
    _bus.send(signalMessage(kItemPath, kItemIface, signal));
    // Newer hosts (Plasma 6) also follow PropertiesChanged.
    auto      m = signalMessage(kItemPath, kPropsIface, "PropertiesChanged");
    MsgWriter w(m.get());
    w.str(kItemIface);
    w.array("{sv}", [&](MsgWriter &a) {
        a.dictEntry([&](MsgWriter &e) {
            e.str(property);
            writeItemProperty(e, property);
        });
    });
    w.strings({});
    _bus.send(m);
}

void SniTray::writePixmaps(MsgWriter &w) const {
    w.array("(iiay)", [&](MsgWriter &a) {
        for (const auto &p : _icons)
            a.structure([&](MsgWriter &s) {
                s.i32(p.w);
                s.i32(p.h);
                s.bytes(p.argb.data(), p.argb.size());
            });
    });
}

// Writes the property as a variant; false if we have no such property.
bool SniTray::writeItemProperty(MsgWriter &w, std::string_view name) const {
    auto str = [&](std::string_view v) { w.variant("s", [&](MsgWriter &x) { x.str(v); }); };
    auto emptyPixmaps = [&] {
        w.variant("a(iiay)", [](MsgWriter &x) { x.array("(iiay)", [](MsgWriter &) {}); });
    };
    if (name == "Category")
        str("ApplicationStatus");
    else if (name == "Id")
        str(_id);
    else if (name == "Title")
        str(_title);
    else if (name == "Status")
        str("Active");
    else if (name == "WindowId")
        w.variant("i", [](MsgWriter &x) { x.i32(0); });
    else if (
        name == "IconThemePath" || name == "IconName" || name == "OverlayIconName" ||
        name == "AttentionIconName" || name == "AttentionMovieName"
    )
        str("");
    else if (name == "Menu")
        w.variant("o", [](MsgWriter &x) { x.objectPath(kMenuPath); });
    else if (name == "ItemIsMenu")
        w.variant("b", [](MsgWriter &x) { x.boolean(false); });
    else if (name == "IconPixmap")
        w.variant("a(iiay)", [&](MsgWriter &x) { writePixmaps(x); });
    else if (name == "OverlayIconPixmap" || name == "AttentionIconPixmap")
        emptyPixmaps();
    else if (name == "ToolTip")
        w.variant("(sa(iiay)ss)", [&](MsgWriter &x) {
            x.structure([&](MsgWriter &s) {
                s.str("");
                s.array("(iiay)", [](MsgWriter &) {});
                s.str(_tooltip);
                s.str("");
            });
        });
    else
        return false;
    return true;
}

bool SniTray::handleItem(DBusMessage *m) {
    if (dbus_message_is_method_call(m, kItemIface, "Activate") ||
        dbus_message_is_method_call(m, kItemIface, "SecondaryActivate")) {
        // SecondaryActivate is a middle click. plat has one "activate"
        // meaning (show the app), and hosts differ on which one a plain click
        // sends when ItemIsMenu is false, so both mean TrayActivated.
        _bus.replyEmpty(m);
        postEmit({.type = EventType::TrayActivated, .activationToken = takeToken()});
        return true;
    }
    if (dbus_message_is_method_call(m, kItemIface, "ProvideXdgActivationToken")) {
        // Plasma hands over an xdg-activation token right before Activate
        // (the only way a Wayland app may raise itself after a tray click).
        std::string token;
        MsgReader(m).str(&token);
        _token     = token;
        _tokenTime = std::chrono::steady_clock::now();
        _bus.replyEmpty(m);
        return true;
    }
    if (dbus_message_is_method_call(m, kItemIface, "ContextMenu") ||
        dbus_message_is_method_call(m, kItemIface, "Scroll")) {
        // The host draws the dbusmenu itself; nothing else to do.
        _bus.replyEmpty(m);
        return true;
    }
    if (dbus_message_is_method_call(m, kIntroIface, "Introspect")) {
        replyMessage(_bus, m, [](MsgWriter &w) { w.str(kItemIntrospection); });
        return true;
    }
    if (dbus_message_is_method_call(m, kPropsIface, "Get")) {
        MsgReader   r(m);
        std::string iface, name;
        r.str(&iface);
        r.str(&name);
        if (iface != kItemIface ||
            std::find(std::begin(kItemProps), std::end(kItemProps), name) == std::end(kItemProps)) {
            _bus.replyError(m, kUnknownProp, "no such property");
            return true;
        }
        replyMessage(_bus, m, [&](MsgWriter &w) { writeItemProperty(w, name); });
        return true;
    }
    if (dbus_message_is_method_call(m, kPropsIface, "GetAll")) {
        MsgReader   r(m);
        std::string iface;
        r.str(&iface);
        replyMessage(_bus, m, [&](MsgWriter &w) {
            w.array("{sv}", [&](MsgWriter &a) {
                if (iface != kItemIface)
                    return;
                for (const char *p : kItemProps)
                    a.dictEntry([&](MsgWriter &e) {
                        e.str(p);
                        writeItemProperty(e, p);
                    });
            });
        });
        return true;
    }
    if (dbus_message_is_method_call(m, kPropsIface, "Set")) {
        _bus.replyError(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
        return true;
    }
    return false;
}

// ── menu ────────────────────────────────────────────────────────────────────

void SniTray::setMenu(std::vector<MenuItem> items) {
    _menu = std::move(items);
    rebuildMenu();
    ++_revision;
    if (!_bus.ready())
        return;
    auto      m = signalMessage(kMenuPath, kMenuIface, "LayoutUpdated");
    MsgWriter w(m.get());
    w.u32(_revision);
    w.i32(0);
    _bus.send(m);
}

// dbusmenu ids are int32 with 0 reserved for the root. Selectable items keep
// their plat id when it fits and is unique (so a host's logs and ours agree);
// separators, submenus and anything clashing get a free id instead.
void SniTray::rebuildMenu() {
    _nodes.clear();
    _idOf.clear();
    std::set<int32_t>                                  used;
    std::function<void(const std::vector<MenuItem> &)> claim =
        [&](const std::vector<MenuItem> &items) {
            for (const auto &m : items) {
                if (selectable(m) && m.id >= 1 && m.id <= uint32_t(INT32_MAX) &&
                    used.insert(int32_t(m.id)).second)
                    _idOf[&m] = int32_t(m.id);
                claim(m.children);
            }
        };
    claim(_menu);
    int32_t                                                     next = 1;
    std::function<void(int32_t, const std::vector<MenuItem> &)> build =
        [&](int32_t parent, const std::vector<MenuItem> &items) {
            for (const auto &m : items) {
                int32_t id;
                if (auto it = _idOf.find(&m); it != _idOf.end()) {
                    id = it->second;
                } else {
                    while (used.count(next))
                        ++next;
                    id = next;
                    used.insert(id);
                    _idOf[&m] = id;
                }
                _nodes[parent].children.push_back(id);
                _nodes[id].item = &m;
                if (m.kind == MenuItem::Kind::Submenu)
                    build(id, m.children);
            }
        };
    _nodes[0] = {};
    build(0, _menu);
}

int32_t SniTray::menuIdFor(uint32_t id) const {
    std::function<int32_t(const std::vector<MenuItem> &)> find =
        [&](const std::vector<MenuItem> &items) -> int32_t {
        for (const auto &m : items) {
            if (m.kind == MenuItem::Kind::Submenu) {
                if (int32_t r = find(m.children); r >= 0)
                    return r;
            } else if (selectable(m) && m.id == id) {
                auto it = _idOf.find(&m);
                return it == _idOf.end() ? -1 : it->second;
            }
        }
        return -1;
    };
    return find(_menu);
}

void SniTray::menuClicked(int32_t id) {
    auto it = _nodes.find(id);
    if (it == _nodes.end() || !it->second.item)
        return;
    const MenuItem &m = *it->second.item;
    if (!selectable(m) || !m.enabled)
        return; // hosts normally grey these out; never trust that
    // Not in the contract for TrayMenuItem, but "Show window" from the menu
    // needs the token as much as a click does.
    postEmit({.type = EventType::TrayMenuItem, .id = m.id, .activationToken = takeToken()});
}

// A token only belongs to the click it came with; an old one would be
// rejected by the compositor anyway.
std::string SniTray::takeToken() {
    std::string t = std::move(_token);
    _token.clear();
    if (std::chrono::steady_clock::now() - _tokenTime > std::chrono::seconds(10))
        return {};
    return t;
}

// Which dbusmenu properties an item has a non-default value for; only those
// are sent, as libdbusmenu does.
static bool menuPropApplies(const MenuItem *m, std::string_view n) {
    if (!m) // root
        return n == "children-display";
    switch (m->kind) {
    case MenuItem::Kind::Separator:
        return n == "type" || (n == "enabled" && !m->enabled);
    case MenuItem::Kind::Checkbox:
        if (n == "toggle-type" || n == "toggle-state")
            return true;
        break;
    case MenuItem::Kind::Submenu:
        if (n == "children-display")
            return true;
        break;
    case MenuItem::Kind::Action:
        break;
    }
    return n == "label" || (n == "enabled" && !m->enabled);
}

bool SniTray::writeOneMenuProp(MsgWriter &w, int32_t id, std::string_view name) const {
    auto it = _nodes.find(id);
    if (it == _nodes.end() || !menuPropApplies(it->second.item, name))
        return false;
    const MenuItem *m = it->second.item;
    if (name == "children-display")
        w.variant("s", [](MsgWriter &x) { x.str("submenu"); });
    else if (name == "type")
        w.variant("s", [](MsgWriter &x) { x.str("separator"); });
    else if (name == "label")
        w.variant("s", [&](MsgWriter &x) { x.str(menuLabel(m->label)); });
    else if (name == "enabled")
        w.variant("b", [](MsgWriter &x) { x.boolean(false); });
    else if (name == "toggle-type")
        w.variant("s", [](MsgWriter &x) { x.str("checkmark"); });
    else if (name == "toggle-state")
        w.variant("i", [&](MsgWriter &x) { x.i32(m->checked ? 1 : 0); });
    return true;
}

void SniTray::writeMenuProps(
    MsgWriter &w, int32_t id, const std::vector<std::string> &props
) const {
    static const char *all[] = {
        "type", "label", "enabled", "toggle-type", "toggle-state", "children-display"
    };
    auto it = _nodes.find(id);
    w.array("{sv}", [&](MsgWriter &a) {
        if (it == _nodes.end())
            return;
        for (const char *p : all) {
            if (!wanted(props, p) || !menuPropApplies(it->second.item, p))
                continue;
            a.dictEntry([&](MsgWriter &e) {
                e.str(p);
                writeOneMenuProp(e, id, p);
            });
        }
    });
}

void SniTray::writeLayout(
    MsgWriter &w, int32_t id, int depth, const std::vector<std::string> &props
) const {
    w.structure([&](MsgWriter &s) {
        s.i32(id);
        writeMenuProps(s, id, props);
        s.array("v", [&](MsgWriter &a) {
            if (depth == 0)
                return;
            auto it = _nodes.find(id);
            if (it == _nodes.end())
                return;
            for (int32_t c : it->second.children)
                a.variant("(ia{sv}av)", [&](MsgWriter &v) {
                    writeLayout(v, c, depth < 0 ? -1 : depth - 1, props);
                });
        });
    });
}

bool SniTray::writeMenuProperty(MsgWriter &w, std::string_view name) const {
    if (name == "Version")
        w.variant("u", [](MsgWriter &x) { x.u32(3); });
    else if (name == "TextDirection")
        w.variant("s", [](MsgWriter &x) { x.str("ltr"); });
    else if (name == "Status")
        w.variant("s", [](MsgWriter &x) { x.str("normal"); });
    else if (name == "IconThemePath")
        w.variant("as", [](MsgWriter &x) { x.strings({}); });
    else
        return false;
    return true;
}

bool SniTray::handleMenu(DBusMessage *m) {
    if (dbus_message_is_method_call(m, kMenuIface, "GetLayout")) {
        MsgReader                r(m);
        int32_t                  parent = 0, depth = -1;
        std::vector<std::string> props;
        r.i32(&parent);
        r.i32(&depth);
        r.strings(&props);
        if (!_nodes.count(parent)) {
            _bus.replyError(m, kInvalidArgs, "no such menu item");
            return true;
        }
        replyMessage(_bus, m, [&](MsgWriter &w) {
            w.u32(_revision);
            writeLayout(w, parent, depth, props);
        });
        return true;
    }
    if (dbus_message_is_method_call(m, kMenuIface, "GetGroupProperties")) {
        MsgReader                r(m);
        std::vector<int32_t>     ids;
        std::vector<std::string> props;
        MsgReader                a = r.enter();
        for (int32_t id; a.i32(&id);)
            ids.push_back(id);
        r.strings(&props);
        if (ids.empty()) // libdbusmenu reads an empty list as "all items"
            for (auto &[id, n] : _nodes)
                ids.push_back(id);
        replyMessage(_bus, m, [&](MsgWriter &w) {
            w.array("(ia{sv})", [&](MsgWriter &arr) {
                for (int32_t id : ids) {
                    if (!_nodes.count(id))
                        continue;
                    arr.structure([&](MsgWriter &s) {
                        s.i32(id);
                        writeMenuProps(s, id, props);
                    });
                }
            });
        });
        return true;
    }
    if (dbus_message_is_method_call(m, kMenuIface, "GetProperty")) {
        MsgReader   r(m);
        int32_t     id = -1;
        std::string name;
        r.i32(&id);
        r.str(&name);
        MessageRef reply(dbus_message_new_method_return(m));
        MsgWriter  w(reply.get());
        if (!writeOneMenuProp(w, id, name))
            _bus.replyError(m, kInvalidArgs, "no such item or property");
        else if (!dbus_message_get_no_reply(m))
            _bus.send(reply);
        return true;
    }
    if (dbus_message_is_method_call(m, kMenuIface, "Event")) {
        MsgReader   r(m);
        int32_t     id = -1;
        std::string event;
        r.i32(&id);
        r.str(&event);
        _bus.replyEmpty(m);
        if (event == "clicked")
            menuClicked(id);
        return true;
    }
    if (dbus_message_is_method_call(m, kMenuIface, "EventGroup")) {
        MsgReader            r(m);
        MsgReader            events = r.enter();
        std::vector<int32_t> errors, clicked;
        while (events.type() == DBUS_TYPE_STRUCT) {
            MsgReader   e  = events.enter();
            int32_t     id = -1;
            std::string event;
            e.i32(&id);
            e.str(&event);
            if (!_nodes.count(id))
                errors.push_back(id);
            else if (event == "clicked")
                clicked.push_back(id);
        }
        replyMessage(_bus, m, [&](MsgWriter &w) {
            w.array("i", [&](MsgWriter &a) {
                for (int32_t id : errors)
                    a.i32(id);
            });
        });
        for (int32_t id : clicked)
            menuClicked(id);
        return true;
    }
    if (dbus_message_is_method_call(m, kMenuIface, "AboutToShow")) {
        // The layout is always current; nothing to rebuild lazily.
        replyMessage(_bus, m, [](MsgWriter &w) { w.boolean(false); });
        return true;
    }
    if (dbus_message_is_method_call(m, kMenuIface, "AboutToShowGroup")) {
        MsgReader            r(m);
        MsgReader            a = r.enter();
        std::vector<int32_t> errors;
        for (int32_t id; a.i32(&id);)
            if (!_nodes.count(id))
                errors.push_back(id);
        replyMessage(_bus, m, [&](MsgWriter &w) {
            w.array("i", [](MsgWriter &) {});
            w.array("i", [&](MsgWriter &x) {
                for (int32_t id : errors)
                    x.i32(id);
            });
        });
        return true;
    }
    if (dbus_message_is_method_call(m, kIntroIface, "Introspect")) {
        replyMessage(_bus, m, [](MsgWriter &w) { w.str(kMenuIntrospection); });
        return true;
    }
    if (dbus_message_is_method_call(m, kPropsIface, "Get")) {
        MsgReader   r(m);
        std::string iface, name;
        r.str(&iface);
        r.str(&name);
        MessageRef reply(dbus_message_new_method_return(m));
        MsgWriter  w(reply.get());
        if (iface != kMenuIface || !writeMenuProperty(w, name))
            _bus.replyError(m, kUnknownProp, "no such property");
        else if (!dbus_message_get_no_reply(m))
            _bus.send(reply);
        return true;
    }
    if (dbus_message_is_method_call(m, kPropsIface, "GetAll")) {
        MsgReader   r(m);
        std::string iface;
        r.str(&iface);
        replyMessage(_bus, m, [&](MsgWriter &w) {
            w.array("{sv}", [&](MsgWriter &a) {
                if (iface != kMenuIface)
                    return;
                for (const char *p : kMenuProps)
                    a.dictEntry([&](MsgWriter &e) {
                        e.str(p);
                        writeMenuProperty(e, p);
                    });
            });
        });
        return true;
    }
    return false;
}

} // namespace plat::linux_services
