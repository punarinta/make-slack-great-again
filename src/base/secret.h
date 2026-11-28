// Where msga keeps credentials (workspace tokens, cookies, API keys): the
// very entries earlier versions used, so an upgrade finds them and a rollback
// still works.
//
//   macOS     Keychain generic-password items (Security.framework), account
//             = key: msga's own (service "com.nisdos.msga"), copied once
//             from earlier versions' ("app.msga.msga", still written for a
//             rollback; secret_mac.mm). If the keychain refuses a write
//             (locked, prompt denied), the value goes to earlier versions'
//             plaintext fallback, its settings (old_settings.h), where a read
//             finds it and moves it into the keychain later, as earlier
//             versions did.
//   elsewhere the settings store of earlier versions (msga.conf / the
//             registry), their fallback: no OS keychain backend there.
//
//   tests     (base::testProcess(), marked by the test harness) and demo
//             runs (base::isolatedStorage()): the settings store on every
//             OS — a temporary INI file there — never a keychain: such a
//             process that reaches the keychain aborts.
//
// Keys are opaque, caller-owned ("workspace/slack:T0123/auth") and the old
// app's own. Values are UTF-8. Calls block (a keychain may prompt); make them
// at load/save time, not per frame.
#pragma once

#include <string>
#include <string_view>

namespace secret {

bool        available();
// Inserts or replaces; an empty value removes. False when nothing could
// store it: the caller then keeps it in its own file.
bool        write(std::string_view key, std::string_view value);
// "" when absent; *found (optional) tells absent from empty.
std::string read(std::string_view key, bool *found = nullptr);
void        remove(std::string_view key);

} // namespace secret
