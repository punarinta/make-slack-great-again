// Desktop services that are the same under Wayland and X11 because they live
// on the session D-Bus, not the display server: the StatusNotifierItem tray
// (+ com.canonical.dbusmenu), org.freedesktop.Notifications, the launcher
// badge (com.canonical.Unity.LauncherEntry) and the colour scheme from
// xdg-desktop-portal. Both Linux backends own one and forward to it; it only
// uses the public App loop API (watchFd/addTimer/post), so it is backend-free.
#pragma once

#include "core/backends.h"
#include "linux/instance.h"
#ifdef PLAT_TEST_HOOKS
#include "plat/testing.h"
#endif

#include <optional>

namespace plat::linux_services {

class Services {
public:
    // Never null: without a session bus everything reports "unavailable".
    static std::unique_ptr<Services> create(BackendApp &app);
    virtual ~Services() = default;

    virtual std::unique_ptr<Tray> createTray()                               = 0;
    virtual bool                  notificationsAvailable() const             = 0;
    virtual bool                  notify(uint64_t id, const Notification &n) = 0;
    virtual void                  setBadgeCount(int count)                   = 0;
    // nullopt until/unless the portal answered; the backend keeps its default.
    virtual std::optional<bool>   darkMode() const                           = 0;
    // Called when the portal reports a colour-scheme change; the backend
    // emits ThemeChanged to its windows.
    std::function<void()>         onThemeChanged;

#ifdef PLAT_TEST_HOOKS
    // TestHooks halves that belong here (the backend forwards).
    virtual bool trayActivate(Tray &t)                                          = 0;
    virtual bool trayMenuSelect(Tray &t, uint32_t id)                           = 0;
    virtual bool trayProbe(Tray &t, TestHooks::TrayProbe *out)                  = 0;
    virtual bool notificationInvoke(uint64_t id, std::string_view action)       = 0;
    virtual bool notificationProbe(uint64_t id, TestHooks::NotificationProbe *) = 0;
    virtual int  badgeCount()                                                   = 0;
#endif

    // ── Round 3 (defaults = unavailable, until implemented) ─────────────────
    // xdg-desktop-portal FileChooser; parentHandle is "x11:<hex xid>" or
    // "wayland:<xdg-foreign handle>" or "" (see ServicesApp::parentHandle).
    // tools: may fall back to zenity/kdialog (showFileDialog) or not
    // (showFileDialogEx, see file_dialog.h).
    virtual void showFileDialog(
        const FileDialogDesc                 &d,
        std::string                           parentHandle,
        bool                                  tools,
        std::function<void(FileDialogResult)> cb
    ) {
        _host.post([cb = std::move(cb)] { cb({.status = FileDialogResult::Status::Unavailable}); });
    }
    virtual std::optional<bool> networkOnline() const { return std::nullopt; }
    // Portal org.gnome.desktop.interface / org.freedesktop.appearance values.
    virtual SystemSettings      systemSettings() const { return {}; }
#ifdef PLAT_TEST_HOOKS
    // Emits NetworkChanged / Suspending / Resumed
    // on the app itself (NetworkManager, logind on the system bus, …).
    virtual bool fileDialogRespond(std::vector<std::string> paths) { return false; }
    virtual bool simulateSystemEvent(EventType t, bool online) { return false; }
#endif

protected:
    explicit Services(BackendApp &app) : _host(app) {}
    BackendApp &_host;
};

// Base for the Wayland and X11 apps: implements the service half of App (and
// of TestHooks in PLAT_TEST_HOOKS builds) by forwarding to one lazily created
// Services.
class ServicesApp : public BackendApp
#ifdef PLAT_TEST_HOOKS
    ,
                    public TestHooks
#endif
{
public:
    std::unique_ptr<Tray> createTray() override { return services().createTray(); }
    bool notificationsAvailable() const override { return services().notificationsAvailable(); }
    bool notify(uint64_t id, const Notification &n) override { return services().notify(id, n); }
    void setBadgeCount(int n) override { services().setBadgeCount(n); }
    bool darkMode() const override { return services().darkMode().value_or(false); }

#ifdef PLAT_TEST_HOOKS
    bool trayActivate(Tray &t) override { return services().trayActivate(t); }
    bool trayMenuSelect(Tray &t, uint32_t id) override { return services().trayMenuSelect(t, id); }
    bool trayProbe(Tray &t, TrayProbe *out) override { return services().trayProbe(t, out); }
    bool notificationInvoke(uint64_t id, std::string_view a) override {
        return services().notificationInvoke(id, a);
    }
    bool notificationProbe(uint64_t id, NotificationProbe *out) override {
        return services().notificationProbe(id, out);
    }
    int badgeCount() override { return services().badgeCount(); }
#endif

    void showFileDialog(
        const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
    ) override {
        services().showFileDialog(d, parentHandle(d.parent), true, [cb = std::move(cb)](auto r) {
            cb(std::move(r.paths));
        });
    }
    void
    showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb) override {
        services().showFileDialog(d, parentHandle(d.parent), false, std::move(cb));
    }
    std::optional<bool> networkOnline() const override { return services().networkOnline(); }
    SystemSettings      systemSettings() const override { return services().systemSettings(); }
#ifdef PLAT_TEST_HOOKS
    bool fileDialogRespond(std::vector<std::string> p) override {
        return services().fileDialogRespond(std::move(p));
    }
    bool simulateSystemEvent(EventType t, bool online) override {
        return services().simulateSystemEvent(t, online);
    }
#endif

    bool claimSingleInstance(std::string_view key, const std::vector<std::string> &args) override {
        return linux_instance::claim(*this, key, args, &_instance);
    }
    bool registerUrlScheme(std::string_view scheme) override {
        return linux_instance::registerUrlScheme(*this, scheme);
    }
    std::string standardDir(StandardDir d) const override { return linux_instance::standardDir(d); }

    // Called by the backend factories once the loop is up: system events
    // (network, sleep, lock) and the portal settings must flow even in an app
    // that never touches a tray or notification.
    void startServices() { services(); }

protected:
    // Send ThemeChanged to every window (backend-specific window list).
    virtual void        emitThemeChanged() = 0;
    // The portal's parent-window string for w ("" = no parent): X11
    // "x11:<hex xid>", Wayland "wayland:<xdg_foreign exported handle>".
    virtual std::string parentHandle(Window *w) { return {}; }
    // Backends destroy it before tearing down their loop.
    void                resetServices() {
        _services.reset();
        _instance.reset();
    }

    Services &services() const {
        if (!_services) {
            auto *self                = const_cast<ServicesApp *>(this);
            _services                 = Services::create(*self);
            _services->onThemeChanged = [self] { self->emitThemeChanged(); };
        }
        return *_services;
    }

private:
    mutable std::unique_ptr<Services>       _services;
    std::unique_ptr<linux_instance::Server> _instance;
};

} // namespace plat::linux_services
