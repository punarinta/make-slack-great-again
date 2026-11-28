// Logging: printf-style, to stderr and optionally a file. Messages below the
// level are dropped before formatting, so debug logging in hot paths costs a
// compare. No streams (no <iostream>, see the size rules).
#pragma once

#ifdef __MINGW32__
#include <cstdio> // __MINGW_PRINTF_FORMAT: the dialect vsnprintf really is
#endif

namespace base {

enum class LogLevel : unsigned char { Debug, Info, Warn, Error, Off };

void     setLogLevel(LogLevel l);
LogLevel logLevel();
// Also append every line to this file (created if missing); empty path stops.
// Returns false when the file cannot be opened.
bool     setLogFile(const char *path);
// Silences stderr (tests that exercise error paths).
void     setLogToStderr(bool on);

#if defined(__MINGW32__)
// g++ on mingw uses mingw's C99 stdio (%zu, %lld), not msvcrt's printf dialect.
#define BASE_PRINTF(f, a) __attribute__((format(__MINGW_PRINTF_FORMAT, f, a)))
#elif defined(__GNUC__)
#define BASE_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define BASE_PRINTF(f, a)
#endif

void logf(LogLevel l, const char *tag, const char *fmt, ...) BASE_PRINTF(3, 4);

} // namespace base

// The level check is inline so disabled lines never evaluate their arguments.
#define LOG_AT(level, tag, ...)                                                                    \
    do {                                                                                           \
        if (level >= base::logLevel())                                                             \
            base::logf(level, tag, __VA_ARGS__);                                                   \
    } while (0)
#define LOG_DEBUG(tag, ...) LOG_AT(base::LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...) LOG_AT(base::LogLevel::Info, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) LOG_AT(base::LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) LOG_AT(base::LogLevel::Error, tag, __VA_ARGS__)
