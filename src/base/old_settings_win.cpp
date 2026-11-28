// old_settings.h on Windows: the store's registry tree,
// HKEY_CURRENT_USER\Software\msga\<app>. A key's '/' separates subkeys,
// its last part is the value name, and a '\' in a key part is stored as '/'
// (as earlier versions escaped it); "Default" and "." name the key's default
// value.
#include "base/old_settings.h"

#include "base/process.h"
#include "base/str.h"
#include "base/winstr.h"

#include <windows.h>

namespace oldsettings {

namespace {

using base::wide;

std::wstring root(std::string_view app) {
    const bool tests = base::testProcess();
    return wide(str::concat({tests ? "Software\\msga-tests\\" : "Software\\msga\\", app}));
}

// Store key → (subkey under the root, value name).
void split(std::string_view key, std::wstring *sub, std::wstring *name) {
    std::string k(key);
    for (char &c : k)
        c = c == '/' ? '\\' : c == '\\' ? '/' : c;
    const size_t slash = k.rfind('\\');
    *sub               = slash == std::string::npos ? std::wstring() : wide(k.substr(0, slash));
    std::string n      = slash == std::string::npos ? k : k.substr(slash + 1);
    if (n == "Default" || n == ".")
        n.clear();
    *name = wide(n);
}

std::string qtKey(const std::wstring &path) {
    std::string k = base::narrow(path);
    for (char &c : k)
        c = c == '\\' ? '/' : c == '/' ? '\\' : c;
    return k;
}

// Every value under `parent`\`name`; `rel` is that key's path below the
// root ("" for the root itself, else ending in '\').
void collect(HKEY parent, const std::wstring &name, const std::wstring &rel, Map *out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(parent, name.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    DWORD subkeys = 0, maxSubkey = 0, values = 0, maxName = 0, maxData = 0;
    RegQueryInfoKeyW(
        key,
        nullptr,
        nullptr,
        nullptr,
        &subkeys,
        &maxSubkey,
        nullptr,
        &values,
        &maxName,
        &maxData,
        nullptr,
        nullptr
    );
    std::wstring value(maxName + 2, L'\0');
    std::string  data(maxData + 4, '\0');
    for (DWORD i = 0; i < values; ++i) {
        DWORD len = DWORD(value.size()), size = DWORD(data.size()), type = 0;
        if (RegEnumValueW(
                key,
                i,
                value.data(),
                &len,
                nullptr,
                &type,
                reinterpret_cast<BYTE *>(data.data()),
                &size
            ) != ERROR_SUCCESS ||
            !len) // the default value: no store key has it
            continue;
        out->insert_or_assign(
            qtKey(rel + std::wstring(value.data(), len)),
            decodeRegistry(type, std::string_view(data.data(), size))
        );
    }
    std::wstring sub(maxSubkey + 2, L'\0');
    for (DWORD i = 0; i < subkeys; ++i) {
        DWORD len = DWORD(sub.size());
        if (RegEnumKeyExW(key, i, sub.data(), &len, nullptr, nullptr, nullptr, nullptr) ==
            ERROR_SUCCESS) {
            const std::wstring child(sub.data(), len);
            collect(key, child, rel + child + L"\\", out);
        }
    }
    RegCloseKey(key);
}

} // namespace

// The native store (old_settings.cpp picks it, or the tests' INI file).
namespace native {

Map load(std::string_view app) {
    Map out;
    collect(HKEY_CURRENT_USER, root(app), {}, &out);
    return out;
}

Value get(std::string_view key, std::string_view app) {
    std::wstring sub, name;
    split(key, &sub, &name);
    const std::wstring path = sub.empty() ? root(app) : root(app) + L"\\" + sub;
    HKEY               h    = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
        return {};
    DWORD type = 0, size = 0;
    Value v;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
        std::string data(size + 4, '\0');
        DWORD       got = DWORD(data.size());
        if (RegQueryValueExW(
                h, name.c_str(), nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &got
            ) == ERROR_SUCCESS)
            v = decodeRegistry(type, std::string_view(data.data(), got));
    }
    RegCloseKey(h);
    return v;
}

bool write(std::string_view key, std::string_view value, std::string_view app) {
    std::wstring sub, name;
    split(key, &sub, &name);
    const std::wstring path = sub.empty() ? root(app) : root(app) + L"\\" + sub;
    HKEY               h    = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr
        ) != ERROR_SUCCESS)
        return false;
    // A string value as stored: REG_SZ, NUL-terminated UTF-16.
    const std::wstring w  = wide(encodeString(value));
    const bool         ok = RegSetValueExW(
                                h,
                                name.c_str(),
                                0,
                                REG_SZ,
                                reinterpret_cast<const BYTE *>(w.c_str()),
                                DWORD((w.size() + 1) * sizeof(wchar_t))
                            ) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}

bool remove(std::string_view key, std::string_view app) {
    std::wstring sub, name;
    split(key, &sub, &name);
    const std::wstring path = sub.empty() ? root(app) : root(app) + L"\\" + sub;
    HKEY               h    = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS)
        return true; // nothing there
    const LONG r = RegDeleteValueW(h, name.c_str());
    RegCloseKey(h);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

} // namespace native

} // namespace oldsettings
