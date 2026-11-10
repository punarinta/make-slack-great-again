#!/usr/bin/env python3
"""Fake Linux desktop services for plat's selftest, on whatever session bus
DBUS_SESSION_BUS_ADDRESS names (plat-selftest-linux-services.sh starts a
private one; never point this at a real session).

Roles (any combination):
  --watcher        org.kde.StatusNotifierWatcher with a registered host; acts
                   like a host too: reads every item's properties and menu
                   the way Plasma/GNOME do, and logs what it saw.
  --notifications  org.freedesktop.Notifications plus the test interface
                   org.nisdos.PlatTest (Invoke = a user click, Get = what
                   the server received).
  --launcher       records com.canonical.Unity.LauncherEntry Update signals;
                   org.nisdos.PlatTest.BadgeCount(app_uri) answers the count.
  --portal         org.freedesktop.portal.Desktop: Settings (color-scheme plus
                   text scale, animations, caret blink, accent, contrast, all
                   non-default), NetworkMonitor and FileChooser (logs every
                   option it got).
                   org.nisdos.PlatTest.SetColorScheme(u) / SetTextScale(d)
                   change a setting and emit SettingChanged. --legacy-portal
                   drops ReadOne (v1 portals); --no-portal-network and
                   --no-file-chooser answer those interfaces UnknownMethod;
                   --file-chooser-fails answers every dialog with Response
                   code 2 (a portal whose backend failed);
                   --file-chooser-closed answers it 1.5 s later (GNOME's and
                   GTK's backends when their dialog is closed with Escape or
                   the window's close button). Each request also
                   logs a "portal-check:" line with the option types it got
                   (current_folder must be a NUL-terminated ay, current_name
                   an s, multiple/directory b).
  --system         on the bus in $DBUS_SYSTEM_BUS_ADDRESS (must be private):
                   org.freedesktop.NetworkManager (not with --no-nm) and
                   org.freedesktop.login1 (Manager with Inhibit and
                   PrepareForSleep).

Always: the control object org.nisdos.PlatTest at /org/nisdos/PlatTest,
which plat's TestHooks call: SetNextFileChooserResponse(as uris, [] =
cancel), TakeFileChooserResponse(as argv) for the fake zenity,
SetNetwork(s portal|nm, b), PrepareForSleep(b), ActivateTrayWithToken(s).

Prints "ready" on stdout once every name is owned.
"""
import argparse
import os
import sys

import dbus
import dbus.bus
import dbus.lowlevel
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

TEST_IFACE = "org.nisdos.PlatTest"
PROPS_IFACE = "org.freedesktop.DBus.Properties"


def log(*args):
    print("fake:", *args, file=sys.stderr, flush=True)


class Watcher(dbus.service.Object):
    IFACE = "org.kde.StatusNotifierWatcher"

    def __init__(self, bus, host):
        self._busname = dbus.service.BusName(self.IFACE, bus, do_not_queue=True)
        super().__init__(bus, "/StatusNotifierWatcher")
        self.bus = bus
        self.host = host
        self.items = {}  # registered key -> owning unique name
        bus.add_signal_receiver(
            self._owner_changed,
            "NameOwnerChanged",
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
        )

    @dbus.service.method(IFACE, in_signature="s", sender_keyword="sender")
    def RegisterStatusNotifierItem(self, service, sender=None):
        # KDE's rule: a path means "the sender at this path", anything else
        # is a bus name serving /StatusNotifierItem.
        if service.startswith("/"):
            key, name, path, owner = sender + service, sender, service, sender
        else:
            key, name, path = service, service, "/StatusNotifierItem"
            owner = service if service.startswith(":") else self.bus.get_name_owner(service)
        self.items[key] = owner
        log("watcher: registered", key)
        self.StatusNotifierItemRegistered(key)
        self._read_item(name, path, key)

    @dbus.service.method(IFACE, in_signature="s")
    def RegisterStatusNotifierHost(self, service):
        pass

    @dbus.service.signal(IFACE, signature="s")
    def StatusNotifierItemRegistered(self, key):
        pass

    @dbus.service.signal(IFACE, signature="s")
    def StatusNotifierItemUnregistered(self, key):
        pass

    @dbus.service.signal(IFACE, signature="")
    def StatusNotifierHostRegistered(self):
        pass

    def _props(self):
        return {
            "RegisteredStatusNotifierItems": dbus.Array(list(self.items), signature="s"),
            "IsStatusNotifierHostRegistered": dbus.Boolean(self.host),
            "ProtocolVersion": dbus.Int32(0),
        }

    @dbus.service.method(PROPS_IFACE, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        return self._props()[prop]

    @dbus.service.method(PROPS_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self._props()

    @dbus.service.method(TEST_IFACE, out_signature="as")
    def Items(self):
        return list(self.items)

    def _owner_changed(self, name, old, new):
        if new:
            return
        for key, owner in list(self.items.items()):
            if owner == old or key == name:
                del self.items[key]
                log("watcher: unregistered", key)
                self.StatusNotifierItemUnregistered(key)

    # Like a real host: fetch everything asynchronously right after the
    # registration, while the app's loop is busy doing other things.
    def _read_item(self, name, path, key):
        obj = self.bus.get_object(name, path, introspect=False)

        def got_props(props):
            icons = ["%dx%d" % (w, h) for (w, h, _data) in props.get("IconPixmap", [])]
            tip = props.get("ToolTip")
            log(
                "host: item %s id=%s title=%r status=%s icons=%s tooltip=%r menu=%s"
                % (
                    key,
                    props.get("Id"),
                    str(props.get("Title")),
                    props.get("Status"),
                    icons,
                    str(tip[2]) if tip else None,
                    props.get("Menu"),
                )
            )
            menu = props.get("Menu")
            if menu:
                m = self.bus.get_object(name, menu, introspect=False)
                m.GetLayout(
                    0,
                    -1,
                    dbus.Array([], signature="s"),
                    dbus_interface="com.canonical.dbusmenu",
                    reply_handler=lambda rev, layout: log(
                        "host: menu rev %d with %d top-level items" % (rev, len(layout[2]))
                    ),
                    error_handler=lambda e: log("host: GetLayout failed:", e),
                )

        obj.GetAll(
            "org.kde.StatusNotifierItem",
            dbus_interface=PROPS_IFACE,
            reply_handler=got_props,
            error_handler=lambda e: log("host: GetAll failed:", e),
        )


class Notifications(dbus.service.Object):
    IFACE = "org.freedesktop.Notifications"

    def __init__(self, bus):
        self._busname = dbus.service.BusName(self.IFACE, bus, do_not_queue=True)
        super().__init__(bus, "/org/freedesktop/Notifications")
        self.next_id = 1
        self.shown = {}  # id -> dict

    @dbus.service.method(IFACE, in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        nid = replaces if replaces in self.shown else self.next_id
        if nid == self.next_id:
            self.next_id += 1
        w = h = 0
        img = hints.get("image-data")
        if img is not None:
            iw, ih, stride, alpha, bits, channels, data = img
            if len(data) >= stride * (ih - 1) + iw * channels * bits // 8:
                w, h = int(iw), int(ih)
                log("notify: image %dx%d first pixel rgba=%s" % (w, h, bytes(data[:4]).hex()))
            else:
                log("notify: image-data too short")
        pairs = [(str(actions[i]), str(actions[i + 1])) for i in range(0, len(actions) - 1, 2)]
        self.shown[nid] = {
            "summary": str(summary),
            "body": str(body),
            "w": w,
            "h": h,
            "labels": [label for key, label in pairs if key != "default"],
            "closed": False,
        }
        log(
            "notify: #%d app=%r summary=%r desktop-entry=%s actions=%s"
            % (nid, str(app), str(summary), hints.get("desktop-entry"), pairs)
        )
        return nid

    @dbus.service.method(IFACE, in_signature="u")
    def CloseNotification(self, nid):
        n = self.shown.get(nid)
        log("close: #%d (%s)" % (nid, "open" if n and not n["closed"] else "gone"))
        if n and not n["closed"]:
            n["closed"] = True
            self.NotificationClosed(nid, 3)

    @dbus.service.method(IFACE, out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body", "body-markup", "persistence"]

    @dbus.service.method(IFACE, out_signature="ssss")
    def GetServerInformation(self):
        return ("plat-fake", "nisdos", "1.0", "1.2")

    @dbus.service.signal(IFACE, signature="uu")
    def NotificationClosed(self, nid, reason):
        pass

    @dbus.service.signal(IFACE, signature="us")
    def ActionInvoked(self, nid, key):
        pass

    @dbus.service.signal(IFACE, signature="us")
    def ActivationToken(self, nid, token):
        pass

    # A user click. Like Plasma's history, it works on closed ones too; like
    # most servers, a click closes a non-resident notification (reason 2).
    @dbus.service.method(TEST_IFACE, in_signature="us")
    def Invoke(self, nid, action):
        n = self.shown.get(nid)
        if n is None:
            raise dbus.exceptions.DBusException("no such notification", name=TEST_IFACE + ".Error")
        # Spec 1.2 servers hand over an xdg-activation token first.
        self.ActivationToken(nid, "fake-token-%d" % nid)
        self.ActionInvoked(nid, action)
        if not n["closed"]:
            n["closed"] = True
            self.NotificationClosed(nid, 2)

    @dbus.service.method(TEST_IFACE, in_signature="u", out_signature="ssiias")
    def Get(self, nid):
        n = self.shown.get(nid)
        if n is None:
            raise dbus.exceptions.DBusException("no such notification", name=TEST_IFACE + ".Error")
        return (n["summary"], n["body"], n["w"], n["h"], dbus.Array(n["labels"], signature="s"))


class Launcher(dbus.service.Object):
    NAME = "org.nisdos.PlatTest.Launcher"

    def __init__(self, bus):
        self._busname = dbus.service.BusName(self.NAME, bus, do_not_queue=True)
        super().__init__(bus, "/org/nisdos/PlatTest/Launcher")
        self.counts = {}
        bus.add_signal_receiver(
            self._update, "Update", "com.canonical.Unity.LauncherEntry", sender_keyword="sender"
        )

    def _update(self, uri, props, sender=None):
        visible = bool(props.get("count-visible", False))
        count = int(props.get("count", 0)) if visible else 0
        self.counts[str(uri)] = count
        log("launcher: %s count=%d (from %s)" % (uri, count, sender))

    @dbus.service.method(TEST_IFACE, in_signature="s", out_signature="i")
    def BadgeCount(self, uri):
        return self.counts.get(str(uri), -1)


class Control(dbus.service.Object):
    """org.nisdos.PlatTest at /org/nisdos/PlatTest: the test side of every
    fake that has no natural test hook. It makes the other fakes send their
    real signals, so plat's listeners run exactly as on a real desktop."""

    NAME = "org.nisdos.PlatTest"

    def __init__(self, bus):
        self._busname = dbus.service.BusName(self.NAME, bus, do_not_queue=True)
        super().__init__(bus, "/org/nisdos/PlatTest")
        self.next_response = None  # None = not set, [] = cancel, [uris…]
        self.waiting = []  # callables(uris) of dialogs waiting for an answer
        self.watcher = None
        self.portal = None
        self.nm = None
        self.login1 = None

    # ── file dialogs ─────────────────────────────────────────────────────────
    def dialog_opened(self, answer):
        self.waiting.append(answer)
        self._try_answer()

    def _try_answer(self):
        while self.next_response is not None and self.waiting:
            uris, self.next_response = self.next_response, None
            answer = self.waiting.pop(0)
            # After the method reply went out, as a real dialog would.
            GLib.timeout_add(20, lambda a=answer, u=uris: (a(u), False)[1])

    @dbus.service.method(TEST_IFACE, in_signature="as")
    def SetNextFileChooserResponse(self, uris):
        self.next_response = [str(u) for u in uris]
        log("control: next file chooser response", self.next_response or "(cancel)")
        self._try_answer()

    # The fake zenity/kdialog asks here; answered once a response is set.
    @dbus.service.method(
        TEST_IFACE, in_signature="as", out_signature="as", async_callbacks=("ok", "err")
    )
    def TakeFileChooserResponse(self, argv, ok, err):
        log("fallback dialog: argv", [str(a) for a in argv])
        self.dialog_opened(lambda uris: ok(dbus.Array(uris, signature="s")))

    # ── system events ────────────────────────────────────────────────────────
    @dbus.service.method(TEST_IFACE, in_signature="sb")
    def SetNetwork(self, source, online):
        if source == "portal" and self.portal and self.portal.network:
            self.portal.set_network(bool(online))
        elif source == "nm" and self.nm:
            self.nm.set_online(bool(online))
        else:
            raise dbus.exceptions.DBusException(
                "no fake network source %r" % str(source), name=TEST_IFACE + ".Error"
            )

    @dbus.service.method(TEST_IFACE, in_signature="b")
    def PrepareForSleep(self, sleeping):
        if not self.login1:
            raise dbus.exceptions.DBusException("no fake logind", name=TEST_IFACE + ".Error")
        self.login1.prepare_for_sleep(bool(sleeping))

    # What Plasma does on a tray click: ProvideXdgActivationToken, then Activate.
    @dbus.service.method(TEST_IFACE, in_signature="s")
    def ActivateTrayWithToken(self, token):
        if not self.watcher or not self.watcher.items:
            raise dbus.exceptions.DBusException("no tray item", name=TEST_IFACE + ".Error")
        for key in list(self.watcher.items):
            slash = key.find("/")
            name, path = (key[:slash], key[slash:]) if slash > 0 else (key, "/StatusNotifierItem")
            obj = self.watcher.bus.get_object(name, path, introspect=False)
            item = dbus.Interface(obj, "org.kde.StatusNotifierItem")
            # Async: the caller (a test hook) may block its loop until we answer.
            done = lambda *a: None
            failed = lambda e: log("control: tray call failed:", e)
            item.ProvideXdgActivationToken(token, reply_handler=done, error_handler=failed)
            item.Activate(0, 0, reply_handler=done, error_handler=failed)
            log("control: tray %s activated with token %r" % (key, str(token)))


class Portal(dbus.service.Object):
    """org.freedesktop.portal.Desktop: Settings, plus NetworkMonitor and
    FileChooser unless switched off."""

    IFACE = "org.freedesktop.portal.Settings"
    NETMON = "org.freedesktop.portal.NetworkMonitor"
    CHOOSER = "org.freedesktop.portal.FileChooser"

    def __init__(self, bus, control, scheme, legacy, network=True, chooser=True, chooser_fails=False, chooser_closed=False):
        self._busname = dbus.service.BusName("org.freedesktop.portal.Desktop", bus, do_not_queue=True)
        super().__init__(bus, "/org/freedesktop/portal/desktop")
        self.bus = bus
        self.control = control
        self.scheme = scheme
        self.legacy = legacy
        self.network = network
        self.chooser = chooser
        self.chooser_fails = chooser_fails
        self.chooser_closed = chooser_closed
        self.online = True
        # Non-default values, so the selftest's printout proves each parse.
        self.settings = {
            "org.gnome.desktop.interface": {
                "text-scaling-factor": dbus.Double(1.25),
                "enable-animations": dbus.Boolean(False),
                "cursor-blink": dbus.Boolean(True),
                "cursor-blink-time": dbus.Int32(1000),
            },
            "org.freedesktop.appearance": {
                "accent-color": dbus.Struct(
                    (dbus.Double(0.2), dbus.Double(0.4), dbus.Double(0.6)), signature="ddd"
                ),
                "contrast": dbus.UInt32(1),
            },
        }

    def _lookup(self, ns, key):
        if ns == "org.freedesktop.appearance" and key == "color-scheme":
            return dbus.UInt32(self.scheme)
        if ns in self.settings and key in self.settings[ns]:
            return self.settings[ns][key]
        raise dbus.exceptions.DBusException(
            "no such setting", name="org.freedesktop.portal.Error.NotFound"
        )

    @dbus.service.method(IFACE, in_signature="ss", out_signature="v")
    def ReadOne(self, ns, key):
        if self.legacy:
            raise dbus.exceptions.DBusException(
                "no ReadOne on a v1 portal", name="org.freedesktop.DBus.Error.UnknownMethod"
            )
        return self._lookup(ns, key)

    # v1 wraps the value in one more variant.
    @dbus.service.method(IFACE, in_signature="ss", out_signature="v")
    def Read(self, ns, key):
        return dbus.UInt32(self._lookup(ns, key), variant_level=1)

    @dbus.service.method(IFACE, in_signature="as", out_signature="a{sa{sv}}")
    def ReadAll(self, namespaces):
        out = {"org.freedesktop.appearance": {"color-scheme": dbus.UInt32(self.scheme)}}
        for ns, keys in self.settings.items():
            out.setdefault(ns, {}).update(keys)
        wanted = {str(n) for n in namespaces}
        if wanted:
            out = {ns: keys for ns, keys in out.items() if ns in wanted}
        return dbus.Dictionary(
            {ns: dbus.Dictionary(keys, signature="sv") for ns, keys in out.items()},
            signature="sa{sv}",
        )

    @dbus.service.signal(IFACE, signature="ssv")
    def SettingChanged(self, ns, key, value):
        pass

    @dbus.service.method(TEST_IFACE, in_signature="u")
    def SetColorScheme(self, scheme):
        self.scheme = int(scheme)
        log("portal: color-scheme ->", self.scheme)
        self.SettingChanged("org.freedesktop.appearance", "color-scheme", dbus.UInt32(self.scheme))

    @dbus.service.method(TEST_IFACE, in_signature="d")
    def SetTextScale(self, scale):
        v = dbus.Double(scale)
        self.settings["org.gnome.desktop.interface"]["text-scaling-factor"] = v
        log("portal: text-scaling-factor ->", float(scale))
        self.SettingChanged("org.gnome.desktop.interface", "text-scaling-factor", v)

    # ── NetworkMonitor ───────────────────────────────────────────────────────
    def _no(self, what):
        raise dbus.exceptions.DBusException(
            "fake portal has no " + what, name="org.freedesktop.DBus.Error.UnknownMethod"
        )

    @dbus.service.method(NETMON, out_signature="b")
    def GetAvailable(self):
        if not self.network:
            self._no("NetworkMonitor")
        return self.online

    @dbus.service.method(NETMON, out_signature="u")
    def GetConnectivity(self):
        if not self.network:
            self._no("NetworkMonitor")
        return dbus.UInt32(4 if self.online else 1)  # full / local only

    @dbus.service.signal(NETMON, signature="")
    def changed(self):
        pass

    def set_network(self, online):
        self.online = online
        log("portal: network ->", "online" if online else "offline")
        self.changed()

    # ── FileChooser ──────────────────────────────────────────────────────────
    def _request(self, method, sender, parent, title, options):
        if not self.chooser:
            self._no("FileChooser")
        token = str(options.get("handle_token", "fake%d" % id(options)))
        path = "/org/freedesktop/portal/desktop/request/%s/%s" % (
            sender[1:].replace(".", "_"),
            token,
        )
        raw = options.get("current_folder")
        folder = bytes(raw).rstrip(b"\0").decode("utf-8", "replace") if raw is not None else None
        # The spec: a byte string with exactly one terminating NUL.
        if raw is None:
            folder_enc = "none"
        elif isinstance(raw, dbus.Array) and raw.signature == "y":
            b = bytes(raw)
            folder_enc = "ay+nul" if b.endswith(b"\0") and not b.endswith(b"\0\0") and b.count(b"\0") == 1 else "ay-bad-nul"
        else:
            folder_enc = "not-ay:" + type(raw).__name__
        def btype(key):
            v = options.get(key)
            return "none" if v is None else ("b" if isinstance(v, dbus.Boolean) else "wrong:" + type(v).__name__)
        name = options.get("current_name")
        name_enc = "none" if name is None else ("s" if isinstance(name, dbus.String) else "wrong:" + type(name).__name__)
        log(
            "portal-check: %s multiple=%s directory=%s current_name=%s current_folder=%s"
            % (method, btype("multiple"), btype("directory"), name_enc, folder_enc)
        )
        filters = [
            (str(name), [(int(kind), str(pat)) for kind, pat in pats])
            for name, pats in options.get("filters", [])
        ]
        log(
            "portal: %s parent_window=%r title=%r multiple=%s directory=%s modal=%s "
            "current_name=%r current_folder=%r filters=%s"
            % (
                method,
                str(parent),
                str(title),
                bool(options.get("multiple", False)),
                bool(options.get("directory", False)),
                bool(options.get("modal", True)),
                str(options["current_name"]) if "current_name" in options else None,
                folder,
                filters,
            )
        )

        def answer(uris):
            msg = dbus.lowlevel.SignalMessage(path, "org.freedesktop.portal.Request", "Response")
            if self.chooser_fails or self.chooser_closed:
                msg.append(dbus.UInt32(2), dbus.Dictionary({}, signature="sv"), signature="ua{sv}")
                self.bus.send_message(msg)
                log("portal: %s response 2 (%s)" % (token, "closed" if self.chooser_closed else "backend failed"))
                return
            if uris:
                results = {"uris": dbus.Array(uris, signature="s")}
                msg.append(dbus.UInt32(0), dbus.Dictionary(results, signature="sv"), signature="ua{sv}")
            else:
                msg.append(dbus.UInt32(1), dbus.Dictionary({}, signature="sv"), signature="ua{sv}")
            self.bus.send_message(msg)
            log("portal: %s response %s" % (token, uris or "cancelled"))

        if self.chooser_fails:
            # Nothing to wait for: the backend "failed" at once.
            GLib.timeout_add(20, lambda: (answer([]), False)[1])
        elif self.chooser_closed:
            # A dialog was up and the user closed it.
            GLib.timeout_add(1500, lambda: (answer([]), False)[1])
        else:
            self.control.dialog_opened(answer)
        return dbus.ObjectPath(path)

    @dbus.service.method(CHOOSER, in_signature="ssa{sv}", out_signature="o", sender_keyword="sender")
    def OpenFile(self, parent, title, options, sender=None):
        return self._request("OpenFile", sender, parent, title, options)

    @dbus.service.method(CHOOSER, in_signature="ssa{sv}", out_signature="o", sender_keyword="sender")
    def SaveFile(self, parent, title, options, sender=None):
        return self._request("SaveFile", sender, parent, title, options)

# ── on the private "system" bus ─────────────────────────────────────────────


class NetworkManager(dbus.service.Object):
    IFACE = "org.freedesktop.NetworkManager"

    def __init__(self, bus):
        self._busname = dbus.service.BusName(self.IFACE, bus, do_not_queue=True)
        super().__init__(bus, "/org/freedesktop/NetworkManager")
        self.state = 70  # CONNECTED_GLOBAL

    def _props(self):
        return {
            "State": dbus.UInt32(self.state),
            "Connectivity": dbus.UInt32(4 if self.state >= 70 else 1),
        }

    @dbus.service.method(PROPS_IFACE, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        return self._props()[prop]

    @dbus.service.method(PROPS_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self._props()

    @dbus.service.signal(IFACE, signature="u")
    def StateChanged(self, state):
        pass

    @dbus.service.signal(PROPS_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    def set_online(self, online):
        self.state = 70 if online else 20  # CONNECTED_GLOBAL / DISCONNECTED
        log("nm: state ->", self.state)
        self.StateChanged(dbus.UInt32(self.state))
        self.PropertiesChanged(self.IFACE, self._props(), dbus.Array([], signature="s"))


class Login1(dbus.service.Object):
    IFACE = "org.freedesktop.login1.Manager"

    def __init__(self, bus):
        self._busname = dbus.service.BusName("org.freedesktop.login1", bus, do_not_queue=True)
        super().__init__(bus, "/org/freedesktop/login1")

    # The client holds the write end; closing it releases the inhibitor.
    @dbus.service.method(IFACE, in_signature="ssss", out_signature="h")
    def Inhibit(self, what, who, why, mode):
        r, w = os.pipe()
        log("logind: inhibitor taken what=%s who=%r mode=%s" % (what, str(who), mode))

        def released(fd, cond):
            os.close(fd)
            log("logind: inhibitor released by %r" % str(who))
            return False

        GLib.io_add_watch(r, GLib.IO_HUP | GLib.IO_ERR, released)
        fd = dbus.types.UnixFd(w)
        os.close(w)
        return fd

    @dbus.service.signal(IFACE, signature="b")
    def PrepareForSleep(self, sleeping):
        pass

    def prepare_for_sleep(self, sleeping):
        log("logind: PrepareForSleep", sleeping)
        self.PrepareForSleep(sleeping)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--watcher", action="store_true")
    p.add_argument("--no-host", action="store_true", help="watcher reports no registered host")
    p.add_argument("--notifications", action="store_true")
    p.add_argument("--launcher", action="store_true")
    p.add_argument("--portal", action="store_true")
    p.add_argument("--legacy-portal", action="store_true")
    p.add_argument("--no-portal-network", action="store_true")
    p.add_argument("--no-file-chooser", action="store_true")
    p.add_argument("--file-chooser-fails", action="store_true")
    p.add_argument("--file-chooser-closed", action="store_true")
    p.add_argument("--system", action="store_true")
    p.add_argument("--no-nm", action="store_true")
    p.add_argument("--color-scheme", type=int, default=1)
    args = p.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    control = Control(bus)
    keep = [control]
    if args.watcher:
        control.watcher = Watcher(bus, host=not args.no_host)
        keep.append(control.watcher)
    if args.notifications:
        keep.append(Notifications(bus))
    if args.launcher:
        keep.append(Launcher(bus))
    if args.portal or args.legacy_portal:
        control.portal = Portal(
            bus,
            control,
            args.color_scheme,
            args.legacy_portal,
            network=not args.no_portal_network,
            chooser=not args.no_file_chooser,
            chooser_fails=args.file_chooser_fails,
            chooser_closed=args.file_chooser_closed,
        )
        keep.append(control.portal)
    if args.system:
        address = os.environ.get("DBUS_SYSTEM_BUS_ADDRESS", "")
        if not address or "/run/dbus/" in address:
            sys.exit("refusing the real system bus: point DBUS_SYSTEM_BUS_ADDRESS at a private one")
        system = dbus.bus.BusConnection(address)
        if not args.no_nm:
            control.nm = NetworkManager(system)
            keep.append(control.nm)
        control.login1 = Login1(system)
        keep.append(control.login1)
    print("ready", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
