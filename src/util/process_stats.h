// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <cstdint>
#include <cstdio>
#include <QString>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace ProcessStats {

// Returns PSS (Proportional Set Size) in bytes, or 0 on failure.
// PSS divides shared pages (Qt libs etc.) proportionally among all processes
// that map them, giving a fair "this process's share" rather than the inflated
// VmRSS which counts shared pages in full for every process.
// Linux: /proc/self/smaps_rollup (kernel pre-computes the rollup, fast).
// macOS: phys_footprint (same semantics — private + proportional shared).
// Windows: WorkingSetSize (closest available equivalent).
inline std::int64_t rssBytes() {
#if defined(__linux__)
    std::FILE *f = std::fopen("/proc/self/smaps_rollup", "r");
    if (!f) {
        // Fallback: kernels < 4.14 don't have smaps_rollup; read VmRSS instead.
        f = std::fopen("/proc/self/status", "r");
        if (!f)
            return 0;
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            long long kb = 0;
            if (std::sscanf(line, "VmRSS: %lld", &kb) == 1) {
                std::fclose(f);
                return kb * 1024LL;
            }
        }
        std::fclose(f);
        return 0;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        long long kb = 0;
        if (std::sscanf(line, "Pss: %lld", &kb) == 1) {
            std::fclose(f);
            return kb * 1024LL;
        }
    }
    std::fclose(f);
    return 0;
#elif defined(__APPLE__)
    task_vm_info_data_t    info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        return static_cast<std::int64_t>(info.phys_footprint);
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<std::int64_t>(pmc.WorkingSetSize);
    return 0;
#else
    return 0;
#endif
}

// Format bytes as "12.3 MB" or "1.2 GB".
inline QString formatRss(std::int64_t bytes) {
    if (bytes <= 0)
        return QStringLiteral("—");
    if (bytes >= 1'073'741'824LL)
        return QString::number(bytes / 1'073'741'824.0, 'f', 1) + QStringLiteral(" GB");
    return QString::number(bytes / 1'048'576.0, 'f', 1) + QStringLiteral(" MB");
}

} // namespace ProcessStats
