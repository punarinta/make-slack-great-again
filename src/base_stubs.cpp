// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include <cstdio>

namespace base::assertion {
void log(const char *message, const char *file, int line) {
    std::fprintf(stderr, "Assertion failed: %s (%s:%d)\n", message, file, line);
}
} // namespace base::assertion
