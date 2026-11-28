#include "base/log.h"

#include "base/thread.h"
#include "base/time.h"

#include <cstdarg>
#include <cstdio>

namespace base {

namespace {
LogLevel    g_level  = LogLevel::Info;
bool        g_stderr = true;
FILE       *g_file   = nullptr;
base::Mutex g_mutex; // network threads log too; keep lines whole
} // namespace

void setLogLevel(LogLevel l) {
    g_level = l;
}

LogLevel logLevel() {
    return g_level;
}

void setLogToStderr(bool on) {
    g_stderr = on;
}

bool setLogFile(const char *path) {
    base::Lock lock(g_mutex);
    if (g_file)
        std::fclose(g_file);
    g_file = nullptr;
    if (!path || !*path)
        return true;
    g_file = std::fopen(path, "a");
    return g_file != nullptr;
}

void logf(LogLevel l, const char *tag, const char *fmt, ...) {
    if (l < g_level)
        return;
    static const char kLetters[] = "DIWE";
    char              msg[1024];
    va_list           ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap); // long lines are cut, never allocated
    va_end(ap);

    const int64_t   us = nowMicros();
    const CivilTime t  = localTime(us / 1000000);
    char            line[1100];
    const int       n = std::snprintf(
        line,
        sizeof line,
        "%02d:%02d:%02d.%03d %c %s: %s\n",
        t.hour,
        t.minute,
        t.second,
        int(us / 1000 % 1000),
        kLetters[int(l) & 3],
        tag ? tag : "-",
        msg
    );
    const size_t len = n < 0 ? 0 : (size_t(n) < sizeof line ? size_t(n) : sizeof line - 1);

    base::Lock lock(g_mutex);
    if (g_stderr)
        std::fwrite(line, 1, len, stderr);
    if (g_file) {
        std::fwrite(line, 1, len, g_file);
        std::fflush(g_file); // the log is read after crashes
    }
}

} // namespace base
