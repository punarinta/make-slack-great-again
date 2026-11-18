#include "support/test.h"

#include "base/file.h"
#include "base/host.h"
#include "base/process.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace base::test {

namespace {
Case       *g_first = nullptr, *g_last = nullptr;
Suite      *g_suites     = nullptr;
int         g_failures   = 0;
bool        g_caseFailed = false;
// The suite the command line named; "" = every suite.
std::string g_suite;
} // namespace

int add(Case *c) {
    // Keep file order: tests read top to bottom like the source.
    if (g_last)
        g_last->next = c;
    else
        g_first = c;
    g_last = c;
    return 0;
}

int addSuite(Suite *s) {
    s->next  = g_suites;
    g_suites = s;
    return 0;
}

bool fail(const char *file, int line, const char *expr) {
    std::fprintf(stderr, "  %s:%d: FAILED: %s\n", file, line, expr);
    ++g_failures;
    g_caseFailed = true;
    return false;
}

bool failEq(const char *file, int line, const char *expr, std::string_view a, std::string_view b) {
    fail(file, line, expr);
    std::fprintf(
        stderr,
        "    got:      \"%.*s\"\n    expected: \"%.*s\"\n",
        int(a.size()),
        a.data(),
        int(b.size()),
        b.data()
    );
    return false;
}

// ── Portable helpers ────────────────────────────────────────────────────────

#ifdef _WIN32
namespace {
// UTF-8 → UTF-16 as it is (environment values).
std::wstring wideText(std::string_view s) {
    std::wstring w;
    if (s.empty())
        return w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    w.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// A path for the Win32 calls: '\\' separators.
std::wstring wide(std::string_view s) {
    std::wstring w = wideText(s);
    for (auto &c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

std::string narrow(std::wstring_view w) {
    std::string s;
    if (w.empty())
        return s;
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    s.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    for (auto &c : s)
        if (c == '\\')
            c = '/';
    return s;
}

void removeTreeW(const std::wstring &path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
    // A junction or a directory symlink is removed as the link, never entered.
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE           h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (std::wcscmp(fd.cFileName, L".") && std::wcscmp(fd.cFileName, L".."))
                    removeTreeW(path + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
        RemoveDirectoryW(path.c_str());
    else
        DeleteFileW(path.c_str());
}
} // namespace

static std::string newTempDir(const char *prefix, const std::string &parent) {
    std::wstring base;
    if (parent.empty()) {
        wchar_t     buf[MAX_PATH + 2];
        const DWORD n = GetTempPathW(MAX_PATH + 2, buf);
        if (!n || n > MAX_PATH + 1)
            return {};
        base.assign(buf, n);
    } else {
        base = wide(parent);
        for (size_t i = base.find(L'\\', 3); i != std::wstring::npos; i = base.find(L'\\', i + 1))
            CreateDirectoryW(base.substr(0, i).c_str(), nullptr);
        CreateDirectoryW(base.c_str(), nullptr);
    }
    while (!base.empty() && base.back() == L'\\')
        base.pop_back();
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned          seed     = unsigned(GetTickCount64()) ^ unsigned(GetCurrentProcessId() << 16);
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = prefix;
        for (int i = 0; i < 6; ++i) {
            seed = seed * 1103515245u + 12345u;
            name += kChars[(seed >> 16) % 36];
        }
        const std::wstring dir = base + L"\\" + wide(name);
        if (CreateDirectoryW(dir.c_str(), nullptr))
            return narrow(dir);
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
    }
    return {};
}

void removeTree(const std::string &path) {
    if (!path.empty())
        removeTreeW(wide(path));
}

void setEnv(const char *name, const std::string &value) {
    // _wputenv_s with an empty value unsets; tests never set "" anyway.
    _wputenv_s(wideText(name).c_str(), wideText(value).c_str());
}

void unsetEnv(const char *name) {
    _wputenv_s(wideText(name).c_str(), L"");
}

bool setModifiedTime(const std::string &path, int64_t unixSeconds) {
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, // directories too
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    // FILETIME: 100 ns ticks since 1601.
    const uint64_t t = uint64_t(unixSeconds + 11644473600ll) * 10000000ull;
    FILETIME       ft{DWORD(t), DWORD(t >> 32)};
    const bool     ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok;
}
std::string realPath(const std::string &path) {
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return {};
    wchar_t     buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= 4096)
        return {};
    std::wstring_view w(buf, n);
    if (w.substr(0, 8) == L"\\\\?\\UNC\\")
        return "//" + narrow(w.substr(8));
    if (w.substr(0, 4) == L"\\\\?\\")
        w.remove_prefix(4);
    return narrow(w);
}

void killProcess(int64_t pid) {
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}
#else
static std::string newTempDir(const char *prefix, const std::string &parent) {
    if (!parent.empty())
        for (size_t i = parent.find('/', 1);; i = parent.find('/', i + 1)) {
            mkdir(parent.substr(0, i).c_str(), 0700);
            if (i == std::string::npos)
                break;
        }
    std::string tmpl = (parent.empty() ? std::string("/tmp") : parent) + "/" + prefix + "XXXXXX";
    return mkdtemp(tmpl.data()) ? tmpl : std::string();
}

void removeTree(const std::string &path) {
    if (path.empty())
        return;
    nftw(
        path.c_str(),
        [](const char *p, const struct stat *, int, struct FTW *) { return ::remove(p); },
        16,
        FTW_DEPTH | FTW_PHYS
    );
}

void setEnv(const char *name, const std::string &value) {
    ::setenv(name, value.c_str(), 1);
}

void unsetEnv(const char *name) {
    ::unsetenv(name);
}

bool setModifiedTime(const std::string &path, int64_t unixSeconds) {
    struct timeval tv[2] = {{time_t(unixSeconds), 0}, {time_t(unixSeconds), 0}};
    return ::utimes(path.c_str(), tv) == 0;
}

std::string realPath(const std::string &path) {
    char *p = ::realpath(path.c_str(), nullptr);
    if (!p)
        return {};
    std::string out = p;
    std::free(p);
    return out;
}

void killProcess(int64_t pid) {
    ::kill(pid_t(pid), SIGKILL);
}
#endif

std::string fileUri(const std::string &path) {
    return (path.empty() || path[0] == '/' ? "file://" : "file:///") + path;
}

// Every test's scratch directory goes when the process does.
std::string makeTempDir(const char *prefix, const std::string &parent) {
    static std::vector<std::string> made;
    std::string                     dir = newTempDir(prefix, parent);
    if (!dir.empty()) {
        if (made.empty())
            std::atexit([] {
                for (const std::string &d : made)
                    removeTree(d);
            });
        made.push_back(dir);
    }
    return dir;
}

void restoreEnv(const char *name, const std::string &old) {
    if (old.empty())
        unsetEnv(name);
    else
        setEnv(name, old);
}

namespace {

void makeDir(const std::string &path) {
#ifdef _WIN32
    CreateDirectoryW(wide(path).c_str(), nullptr);
#else
    mkdir(path.c_str(), 0700);
#endif
}

// Code that must never run in a test (the keychain, secret.h) knows a test
// process by this (base::testProcess), from static-init time on, so before
// runAll too; the variable passes it on to the processes tests start.
[[maybe_unused]] const bool g_marked = [] {
    base::markTestProcess();
    setEnv("MSGA_TEST_ISOLATED", "1");
    return true;
}();

// Tests must never reach the user's session, however they are started (ctest
// sets all this too; a direct run of a test binary does not): the headless
// backend only, D-Bus pointed at nothing, and a throwaway HOME unless ctest
// already gave one.
void isolate() {
    // The keychain and the settings of earlier versions (base/secret.h,
    // base/old_settings.h): a temporary file. Audio (plat/audio.h) looks for
    // its helper programs only here: nowhere, so nothing reaches the
    // speakers or the microphone. Audio tests point it at fake helpers of
    // their own.
    setEnv("MSGA_TEST_ISOLATED", "1");
    setEnv("PLAT_AUDIO_HELPERS", "/nonexistent/msga-test-audio");
    setEnv("PLAT_BACKEND", "headless");
#ifndef _WIN32
    setEnv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/msga-test-bus");
    setEnv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/msga-test-sysbus");
    unsetEnv("DISPLAY");
    unsetEnv("WAYLAND_DISPLAY");
#endif
    const char *home = std::getenv("HOME");
    std::string h    = home && std::strstr(home, "test-home") ? home : "";
    if (h.empty()) {
        h = newTempDir("msga-test-home-", {});
        if (h.empty())
            return;
        setEnv("HOME", h);
        static std::string g_home;
        g_home = h;
        std::atexit([] { removeTree(g_home); });
        const char *const xdg[][2] = {
            {"XDG_CONFIG_HOME", "/.config"},
            {"XDG_DATA_HOME", "/.local/share"},
            {"XDG_CACHE_HOME", "/.cache"},
            {"XDG_STATE_HOME", "/.local/state"},
            {"XDG_RUNTIME_DIR", "/run"},
        };
        for (const auto &v : xdg)
            setEnv(v[0], h + v[1]);
        makeDir(h + "/run");
    }
#ifdef _WIN32
    // The profile folders msga reads from the environment (identity, Claude
    // Code's files, the font cache): under the test home as well. The
    // headless backend's standard folders already are (it follows HOME).
    setEnv("USERPROFILE", h);
    makeDir(h + "/AppData");
    makeDir(h + "/AppData/Roaming");
    makeDir(h + "/AppData/Local");
    setEnv("APPDATA", h + "/AppData/Roaming");
    setEnv("LOCALAPPDATA", h + "/AppData/Local");
#endif
}
} // namespace

namespace {

// A file's suite: its name without the directories and the last extension
// (".../test_shell.cpp" -> "test_shell").
std::string_view suiteOf(std::string_view file) {
    const size_t slash = file.find_last_of("/\\");
    if (slash != std::string_view::npos)
        file.remove_prefix(slash + 1);
    return file.substr(0, file.rfind('.'));
}

// A case's group: its name before the first ':' ("image: decodes ..." ->
// "image"); the whole name when it has none.
std::string_view groupOf(std::string_view name) {
    return name.substr(0, name.find(':'));
}

bool isSuite(std::string_view name) {
    for (Case *c = g_first; c; c = c->next)
        if (suiteOf(c->file) == name)
            return true;
    for (Suite *s = g_suites; s; s = s->next)
        if (suiteOf(s->file) == name)
            return true;
    return false;
}

bool inSuite(const Case *c) {
    return g_suite.empty() || suiteOf(c->file) == g_suite;
}

bool selected(const Case *c, const char *filter) {
    return inSuite(c) &&
           (!filter || groupOf(c->name) == filter || std::strcmp(c->name, filter) == 0);
}

// The suites (and, with `cases`, every case) as the usage and --list show them.
void listSuites(FILE *out, bool cases) {
    std::string last;
    for (Case *c = g_first; c; c = c->next) {
        const std::string_view suite = suiteOf(c->file);
        if (suite != last) {
            last = suite;
            std::fprintf(out, "%s\n", last.c_str());
        } else if (!cases) {
            continue;
        }
        if (cases)
            std::fprintf(out, "  %s\n", c->name);
    }
}

void usage(const char *argv0) {
    std::fprintf(
        stderr,
        "usage: %s [suite] [group | case] | --list\n"
        "A suite is a test file (test_shell.cpp -> test_shell); a group is the cases\n"
        "whose names start \"<group>:\". Suites:\n",
        argv0
    );
    listSuites(stderr, false);
}

// Two files of one executable with the same name would be one suite.
bool suitesUnique() {
    for (Case *a = g_first; a; a = a->next)
        for (Case *b = a->next; b; b = b->next)
            if (std::strcmp(a->file, b->file) != 0 && suiteOf(a->file) == suiteOf(b->file)) {
                std::fprintf(
                    stderr,
                    "two suites named %s: %s, %s\n",
                    std::string(suiteOf(a->file)).c_str(),
                    a->file,
                    b->file
                );
                return false;
            }
    return true;
}

} // namespace

int runAll(int argc, char **argv) {
    isolate();
    const char *argv0 = argc > 0 ? argv[0] : "test";
    if (!suitesUnique())
        return 2;
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {
        listSuites(stdout, true);
        return 0;
    }
    // The suite's name off the command line: what follows is the suite's own.
    std::vector<char *> args{const_cast<char *>(argv0)};
    if (argc > 1 && isSuite(argv[1])) {
        g_suite = argv[1];
        args.insert(args.end(), argv + 2, argv + argc);
    } else if (argc > 1) {
        args.insert(args.end(), argv + 1, argv + argc);
    }
    args.push_back(nullptr);
    const int suiteArgc = int(args.size()) - 1;

    SuiteMain own = nullptr;
    if (!g_suite.empty()) {
        for (Suite *s = g_suites; s; s = s->next)
            if (suiteOf(s->file) == g_suite)
                own = s->main;
    } else if (g_suites) {
        // A suite's process setup must not leak into other suites' cases:
        // with no suite named, only an executable of that one suite runs it.
        for (Case *c = g_first; c; c = c->next)
            if (g_suites->next || suiteOf(c->file) != suiteOf(g_suites->file)) {
                if (argc > 1)
                    std::fprintf(stderr, "%s: no suite named '%s'\n", argv0, argv[1]);
                std::fprintf(
                    stderr,
                    "%s: name a suite: %s has its own process setup\n",
                    argv0,
                    std::string(suiteOf(g_suites->file)).c_str()
                );
                usage(argv0);
                return 2;
            }
        own = g_suites->main;
    }
    return own ? own(suiteArgc, args.data()) : runSuite(suiteArgc, args.data());
}

int runSuite(int argc, char **argv) {
    const char *argv0  = argc > 0 && argv[0] ? argv[0] : "test";
    const char *filter = argc > 1 ? argv[1] : nullptr;
    if (argc > 2) {
        usage(argv0);
        return 2;
    }
    int ran = 0, failed = 0;
    for (Case *c = g_first; c; c = c->next) {
        if (!selected(c, filter))
            continue;
        g_caseFailed = false;
        c->fn();
        ++ran;
        if (g_caseFailed) {
            ++failed;
            std::fprintf(stderr, "FAIL %s\n", c->name);
        }
    }
    if (!ran && filter) {
        std::fprintf(
            stderr,
            "%s: no suite, case group or case named '%s'%s%s\n",
            argv0,
            filter,
            g_suite.empty() ? "" : " in ",
            g_suite.c_str()
        );
        usage(argv0);
        return 2;
    }
    std::printf("%d cases, %d failed, %d failed checks\n", ran, failed, g_failures);
    return failed ? 1 : 0;
}

#ifndef _WIN32
namespace {

// flatpak-spawn's options as msga passes them; anything else fails the
// command, as the real one would. Each call's options go to the log, one a
// line, then a "--" line. The command runs in place of the script (exec), so
// it is the pid the caller sees — as the host pid is, there.
constexpr const char *kFakeSpawn = R"sh(#!/bin/sh
log=$1; shift
dir=
while [ $# -gt 0 ]; do
  case "$1" in
    --host|--watch-bus|--forward-fd=*) ;;
    --directory=*) dir=${1#--directory=} ;;
    --env=*) export "${1#--env=}" ;;
    --unset-env=*) unset "${1#--unset-env=}" ;;
    --*) echo "fake flatpak-spawn: unknown option $1" >&2; exit 125 ;;
    *) break ;;
  esac
  printf '%s\n' "$1" >> "$log"
  shift
done
printf -- '--\n' >> "$log"
if [ -n "$dir" ]; then cd "$dir" || exit 126; fi
exec "$@"
)sh";

} // namespace

FakeSandbox::FakeSandbox() {
    const std::string dir    = makeTempDir("fake-sandbox-");
    const std::string script = dir + "/flatpak-spawn-impl";
    _log                     = dir + "/spawns.log";
    file::writeAtomic(script, kFakeSpawn, 0755, false);
    // host::wrap passes options only, so the log's path rides in a wrapper.
    const std::string front = dir + "/flatpak-spawn";
    file::writeAtomic(
        front, "#!/bin/sh\nexec '" + script + "' '" + _log + "' \"$@\"\n", 0755, false
    );
    file::writeAtomic(_log, "", 0644, false);
    host::setSpawnerForTest(front);
}

FakeSandbox::~FakeSandbox() {
    host::setSpawnerForTest({});
}

int FakeSandbox::spawns() const {
    std::string data;
    file::readAll(_log, &data);
    int n = 0;
    for (size_t at = 0; (at = data.find("--\n", at)) != std::string::npos; at += 3)
        if (at == 0 || data[at - 1] == '\n')
            ++n;
    return n;
}

std::string FakeSandbox::lastOptions() const {
    std::string data;
    file::readAll(_log, &data);
    if (data.size() < 3)
        return {};
    data.resize(data.size() - 3); // the last "--\n"
    const size_t prev = data.rfind("\n--\n");
    return prev == std::string::npos ? data : data.substr(prev + 4);
}
#endif

} // namespace base::test
