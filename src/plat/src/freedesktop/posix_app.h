// What the Wayland and X11 apps share beyond the D-Bus services: a
// posix::PosixLoop behind post / timers / fd watches, the double-click time,
// openUrl through xdg-open, and testHooks() (each backend implements the
// TestHooks injectors itself).
#pragma once

#include "freedesktop/services.h"
#include "posix/posix_loop.h"

namespace plat::linux_services {

class PosixServicesApp : public ServicesApp {
public:
    void    quit() override { _loop.quit(); }
    void    post(std::function<void()> fn) override { _loop.core.post(std::move(fn)); }
    TimerId addTimer(int ms, bool repeat, std::function<void()> fn) override {
        return _loop.core.addTimer(ms, repeat, std::move(fn));
    }
    void     cancelTimer(TimerId id) override { _loop.core.cancelTimer(id); }
    uint64_t watchFd(int fd, uint32_t ev, std::function<void(uint32_t)> fn) override {
        return _loop.watch(fd, ev, std::move(fn));
    }
    void unwatchFd(uint64_t id) override { _loop.unwatch(id); }
    int  doubleClickMs() const override { return 400; }
    bool openUrl(std::string_view url) override;
#ifdef PLAT_TEST_HOOKS
    // Always this: the service hooks (tray, notifications) and readbacks work
    // everywhere; input injectors that need a protocol or extension say so.
    TestHooks *testHooks() override { return this; }
#endif

protected:
    posix::PosixLoop _loop;
};

} // namespace plat::linux_services
