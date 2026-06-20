// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>
#include <QString>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace ProcessStats {

// Returns the process's PRIVATE memory in bytes, or 0 on failure.
// "Private" = resident pages owned by this process alone (not shared with other
// processes). Reported uniformly across platforms so the number means the same
// thing everywhere and lines up with each OS's own task manager:
//   Linux:   Private_Clean + Private_Dirty from /proc/self/smaps_rollup.
//   macOS:   phys_footprint — Activity Monitor's "Memory" column (private +
//            compressed-private, excludes shared/reusable).
//   Windows: the private working set — Task Manager's "Memory" column (not the
//            full WorkingSetSize, which also counts shared system-DLL pages).
inline std::int64_t rssBytes() {
#if defined(__linux__)
    std::FILE *f = std::fopen("/proc/self/smaps_rollup", "r");
    if (f) {
        // smaps_rollup is the kernel-precomputed sum over all mappings (fast).
        // Private memory = clean + dirty private pages.
        char      line[256];
        long long privClean = -1, privDirty = -1;
        while (std::fgets(line, sizeof(line), f)) {
            long long kb = 0;
            if (privClean < 0 && std::sscanf(line, "Private_Clean: %lld", &kb) == 1)
                privClean = kb;
            else if (privDirty < 0 && std::sscanf(line, "Private_Dirty: %lld", &kb) == 1)
                privDirty = kb;
            if (privClean >= 0 && privDirty >= 0)
                break;
        }
        std::fclose(f);
        if (privClean >= 0 || privDirty >= 0)
            return ((privClean < 0 ? 0 : privClean) + (privDirty < 0 ? 0 : privDirty)) * 1024LL;
    }
    // Fallback: kernels < 4.14 have no smaps_rollup. VmRSS isn't private-only,
    // but it's the best cheap approximation available there.
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
#elif defined(__APPLE__)
    // phys_footprint is Apple's private memory footprint (the value Activity
    // Monitor shows): private resident + compressed-private, no shared pages.
    task_vm_info_data_t    info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        return static_cast<std::int64_t>(info.phys_footprint);
    return 0;
#elif defined(_WIN32)
    // Task Manager shows the PRIVATE working set (resident pages not shared with
    // other processes), whereas PROCESS_MEMORY_COUNTERS::WorkingSetSize is the
    // FULL working set and includes shared system-DLL pages counted in full —
    // tens of MB higher. mingw's psapi.h has no PROCESS_MEMORY_COUNTERS_EX2
    // (PrivateWorkingSetSize), so enumerate the working set with QueryWorkingSet
    // and sum the non-shared pages to reproduce Task Manager's number.
    const HANDLE proc = GetCurrentProcess();
    SYSTEM_INFO  si{};
    GetSystemInfo(&si);
    const std::int64_t pageSize = si.dwPageSize ? si.dwPageSize : 4096;

    // Probe for the entry count: a too-small buffer fails with ERROR_BAD_LENGTH
    // but still fills NumberOfEntries.
    SIZE_T entries = 0;
    {
        PSAPI_WORKING_SET_INFORMATION probe{};
        QueryWorkingSet(proc, &probe, sizeof(probe));
        entries = probe.NumberOfEntries;
    }
    // The set can grow between the probe and the real read, so over-allocate and
    // retry a few times if it outgrows the buffer again.
    for (int attempt = 0; attempt < 4 && entries > 0; ++attempt) {
        const SIZE_T capacity = entries + 1024; // slack for growth
        const SIZE_T bytes    = sizeof(PSAPI_WORKING_SET_INFORMATION) +
                             (capacity - 1) * sizeof(PSAPI_WORKING_SET_BLOCK);
        std::vector<char> buf(bytes);
        auto             *info = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION *>(buf.data());
        if (!QueryWorkingSet(proc, info, static_cast<DWORD>(bytes))) {
            entries = info->NumberOfEntries; // outgrew capacity — retry larger
            continue;
        }
        std::int64_t privatePages = 0;
        for (SIZE_T i = 0; i < info->NumberOfEntries; ++i) {
            if (!info->WorkingSetInfo[i].Shared)
                ++privatePages;
        }
        return privatePages * pageSize;
    }
    // Enumeration kept failing — fall back to the full working set.
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(proc, &pmc, sizeof(pmc)))
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
