// Placeholder until the D-Bus implementation lands: everything unavailable.
#include "linux/services.h"

namespace plat::linux_services {
namespace {

class StubServices final : public Services {
public:
    explicit StubServices(BackendApp &app) : Services(app) {}
    std::unique_ptr<Tray> createTray() override { return nullptr; }
    bool                  notificationsAvailable() const override { return false; }
    bool                  notify(uint64_t, const Notification &) override { return false; }
    void                  setBadgeCount(int) override {}
    std::optional<bool>   darkMode() const override { return std::nullopt; }
#ifdef PLAT_TEST_HOOKS
    bool trayActivate(Tray &) override { return false; }
    bool trayMenuSelect(Tray &, uint32_t) override { return false; }
    bool trayProbe(Tray &, TestHooks::TrayProbe *) override { return false; }
    bool notificationInvoke(uint64_t, std::string_view) override { return false; }
    bool notificationProbe(uint64_t, TestHooks::NotificationProbe *) override { return false; }
    int  badgeCount() override { return -1; }
#endif
};

} // namespace

std::unique_ptr<Services> Services::create(BackendApp &app) {
    return std::make_unique<StubServices>(app);
}

} // namespace plat::linux_services
