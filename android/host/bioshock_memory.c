#include "host.h"
#include <fcntl.h>
#include <unistd.h>

/* Current values on every call, read with one pread per /proc file from
 * descriptors opened once (the game polls this from its main loop; the old
 * fopen/fscanf/fgets path cost ~1 % of the main thread). Nothing is cached. */
static int memory_fd(int *fd, const char *path)
{
    int current = __atomic_load_n(fd, __ATOMIC_ACQUIRE);
    if (current >= 0) return current;
    int opened = open(path, O_RDONLY | O_CLOEXEC);
    if (opened < 0) return -1;
    int expected = -1;
    if (!__atomic_compare_exchange_n(fd, &expected, opened, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        close(opened);
        return expected;
    }
    return opened;
}

static void memory_sizes(uint64_t *virtual_size, uint64_t *resident, uint64_t *peak)
{
    static int statm_fd = -1, status_fd = -1;
    *virtual_size = *resident = *peak = 0;
    long page = sysconf(_SC_PAGESIZE);
    char text[4096];
    int fd = memory_fd(&statm_fd, "/proc/self/statm");
    ssize_t got = fd >= 0 ? pread(fd, text, sizeof text - 1, 0) : -1;
    unsigned long long pages = 0, rss = 0;
    if (got > 0) {
        text[got] = 0;
        if (sscanf(text, "%llu %llu", &pages, &rss) == 2) {
            *virtual_size = pages * (uint64_t)page;
            *resident = rss * (uint64_t)page;
        }
    }
    fd = memory_fd(&status_fd, "/proc/self/status");
    got = fd >= 0 ? pread(fd, text, sizeof text - 1, 0) : -1;
    if (got <= 0) return;
    text[got] = 0;
    const char *line = strstr(text, "\nVmHWM:");
    unsigned long long kib = 0;
    if (line && sscanf(line + 1, "VmHWM: %llu kB", &kib) == 1) *peak = kib * 1024u;
}

WINAPI_EXPORT("kernel32.dll", GetProcessWorkingSetSize, 3)
{
    if ((ARG(0) != UINT32_MAX && ARG(0) != 0) || !ARG(1) || !ARG(2) ||
        !void_vm_writable(ARG(1), 4) || !void_vm_writable(ARG(2), 4)) {
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    uint64_t virtual_size, resident, peak;
    memory_sizes(&virtual_size, &resident, &peak);
    MEM32(ARG(1)) = (uint32_t)resident;
    MEM32(ARG(2)) = (uint32_t)resident;
    return 1;
}

WINAPI_EXPORT("psapi.dll", GetProcessMemoryInfo, 3)
{
    uint32_t out = ARG(1), size = ARG(2);
    if ((ARG(0) != UINT32_MAX && ARG(0) != 0) || !out || size < 40 || size > 4096 ||
        !void_vm_writable(out, size)) {
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    uint64_t virtual_size, resident, peak;
    memory_sizes(&virtual_size, &resident, &peak);
    memset(GPTR(out), 0, size);
    MEM32(out) = size;
    MEM32(out + 8) = (uint32_t)peak;
    MEM32(out + 12) = (uint32_t)resident;
    MEM32(out + 32) = (uint32_t)virtual_size;
    MEM32(out + 36) = (uint32_t)virtual_size;
    return 1;
}
