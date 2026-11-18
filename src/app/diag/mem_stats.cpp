#include "app/diag/mem_stats.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
// In every ASan runtime, but GCC ships no sanitizer/allocator_interface.h.
extern "C" size_t __sanitizer_get_current_allocated_bytes();
#elif defined(MSGA_PORT)
// A port build asks the OS nothing here: the numbers stay unknown.
#elif defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#if defined(MSGA_PORT)
#elif defined(__linux__)
#include <dirent.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h> // FreeBSD's kinfo_proc
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo, in kernel32
#include <windows.h>
#include <psapi.h>
#endif

namespace diag {

#if defined(MSGA_PORT)

long rssKb() {
    return -1;
}

uint64_t privateBytes() {
    return 0;
}

MemStats sampleMem() {
    MemStats m;
#if defined(__SANITIZE_ADDRESS__)
    m.heapKb = long(__sanitizer_get_current_allocated_bytes() / 1024);
#endif
    return m;
}

#else

namespace {

#if defined(__linux__)
// A "Key:  123 kB" line of /proc/self/status (`key` with its colon); -1
// when absent.
long procStatus(const char *key) {
    long value = -1;
    if (FILE *f = std::fopen("/proc/self/status", "r")) {
        const size_t n = std::strlen(key);
        char         line[256];
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, key, n) == 0) {
                value = std::atol(line + n);
                break;
            }
        std::fclose(f);
    }
    return value;
}
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
// The kernel's record of this process (no procfs on the BSDs by default).
bool procInfo(kinfo_proc *kp) {
    size_t len = sizeof *kp;
#if defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(getpid())};
    return sysctl(mib, 4, kp, &len, nullptr, 0) == 0 && len == sizeof *kp;
#else
    int mib[6] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(getpid()), int(sizeof *kp), 1};
    return sysctl(mib, 6, kp, &len, nullptr, 0) == 0 && len == sizeof *kp;
#endif
}
#endif

} // namespace

long rssKb() {
#if defined(__linux__)
    return procStatus("VmRSS:");
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    kinfo_proc kp{};
    if (!procInfo(&kp))
        return -1;
#if defined(__FreeBSD__)
    const long pages = long(kp.ki_rssize);
#else
    const long pages = long(kp.p_vm_rssize);
#endif
    return pages * (sysconf(_SC_PAGESIZE) / 1024);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) ==
        KERN_SUCCESS)
        return long(info.resident_size / 1024);
    return -1;
#else
    return -1;
#endif
}

uint64_t privateBytes() {
#if defined(__linux__)
    if (FILE *f = std::fopen("/proc/self/smaps_rollup", "r")) {
        char               line[256];
        unsigned long long clean = 0, dirty = 0, kb = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "Private_Clean: %llu", &kb) == 1)
                clean = kb;
            else if (std::sscanf(line, "Private_Dirty: %llu", &kb) == 1)
                dirty = kb;
        }
        std::fclose(f);
        if (clean + dirty)
            return (clean + dirty) * 1024;
    }
    const long kb = rssKb(); // kernels < 4.14
    return kb > 0 ? uint64_t(kb) * 1024 : 0;
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    // No per-process private/shared split without walking every mapping
    // (FreeBSD's kinfo_getvmmap needs libutil): the resident size, which
    // also counts the shared libraries' pages.
    const long kb = rssKb();
    return kb > 0 ? uint64_t(kb) * 1024 : 0;
#elif defined(__APPLE__)
    task_vm_info_data_t    info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &n) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        return 0;
    // PagefileUsage is the same commit charge; Wine fills only that one.
    return pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage;
#else
    return 0;
#endif
}

MemStats sampleMem() {
    MemStats m;
#if defined(__SANITIZE_ADDRESS__)
    // ASan replaces malloc: mallinfo2 would describe its own arenas.
    m.heapKb = long(__sanitizer_get_current_allocated_bytes() / 1024);
#elif defined(__linux__) && defined(__GLIBC__)
    const struct mallinfo2 mi = mallinfo2();
    m.heapKb                  = long((mi.uordblks + mi.hblkhd) / 1024);
#elif defined(__APPLE__)
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st); // null: every zone
    m.heapKb = long(st.size_in_use / 1024);
#endif
    m.rssKb = rssKb();
#if defined(__linux__)
    m.threads = procStatus("Threads:");
    if (DIR *d = opendir("/proc/self/fd")) {
        long n = 0;
        while (const dirent *e = readdir(d))
            if (e->d_name[0] != '.')
                ++n;
        closedir(d);
        m.fds = n - 1; // the one opendir holds
    }
#elif defined(__FreeBSD__)
    if (kinfo_proc kp{}; procInfo(&kp))
        m.threads = long(kp.ki_numthreads);
#endif
    return m;
}

#endif // MSGA_PORT

std::string formatMem(const MemStats &m) {
    std::string out;
    auto        add = [&out](const char *fmt, long v) {
        if (v < 0)
            return;
        char buf[48];
        std::snprintf(buf, sizeof buf, fmt, v);
        if (!out.empty())
            out += ", ";
        out += buf;
    };
    add("heap %ld KB", m.heapKb);
    add("rss %ld KB", m.rssKb);
    add("fds %ld", m.fds);
    add("threads %ld", m.threads);
    return out;
}

bool checkLeaksNow() {
#if defined(__SANITIZE_ADDRESS__)
    return __lsan_do_recoverable_leak_check() != 0;
#else
    return false;
#endif
}

} // namespace diag
