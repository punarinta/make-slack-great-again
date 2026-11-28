// secret.h over the port contract (MSGA_PORT builds): the port's keychain
// when it has one (PORT_CAP_SECRETS) and the value fits its size limit
// (PEOS keeps 1–256 bytes per secret); otherwise, and always for tests and
// demo runs, the settings store, as on Linux (secret_settings.cpp).
#include "base/old_settings.h"
#include "base/process.h"
#include "base/secret.h"

#include "port/port.h"

#include <string>

namespace secret {

namespace {

// The keychain, unless this process must never touch one.
bool keychain(size_t valueBytes = 0) {
    if (base::isolatedStorage() || !(port_caps() & PORT_CAP_SECRETS))
        return false;
    port_info info{};
    info.size = sizeof info;
    port_init(&info); // already done at startup: answers the same info again
    return info.secret_max == 0 || valueBytes <= info.secret_max;
}

} // namespace

bool available() {
    return true;
}

bool write(std::string_view key, std::string_view value) {
    const std::string k(key);
    if (value.empty()) {
        remove(key);
        return true;
    }
    if (keychain(value.size()) &&
        port_secret_write(k.c_str(), value.data(), value.size()) == PORT_OK) {
        oldsettings::remove(key); // an older, longer value parked there
        return true;
    }
    if (keychain())
        port_secret_remove(k.c_str()); // too long now: the settings store has it
    return oldsettings::write(key, value);
}

std::string read(std::string_view key, bool *found) {
    if (keychain()) {
        const std::string k(key);
        std::string       out(256, '\0');
        size_t            len = 0;
        int               rc  = port_secret_read(k.c_str(), out.data(), out.size(), &len);
        if (rc == PORT_E_RANGE) {
            out.resize(len);
            rc = port_secret_read(k.c_str(), out.data(), out.size(), &len);
        }
        if (rc == PORT_OK) {
            out.resize(len);
            if (found)
                *found = true;
            return out;
        }
    }
    const oldsettings::Value v = oldsettings::get(key);
    if (found)
        *found = v.kind != oldsettings::Value::Kind::None;
    return v.text();
}

void remove(std::string_view key) {
    if (keychain())
        port_secret_remove(std::string(key).c_str());
    oldsettings::remove(key);
}

} // namespace secret
