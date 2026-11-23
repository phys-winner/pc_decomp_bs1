#include "host.h"
#include <time.h>

static int hot_address(uint32_t va)
{
    return va == 0x10AC7810u || va == 0x10AC8BD0u || va == 0x10AC8C90u ||
        va == 0x10AC9850u || va == 0x10ABA0D0u || va == 0x10B65290u ||
        va == 0x10D7ACF0u || va == 0x109AF500u || va == 0x10D6E280u;
}

static uint64_t hot_ns(clockid_t clock)
{
    struct timespec value;
    return clock_gettime(clock, &value) ? 0 : (uint64_t)value.tv_sec * 1000000000ull + value.tv_nsec;
}

static __thread unsigned hot_depth, hot_calls, hot_reported;
static __thread uint32_t hot_entry_esp;
static __thread uint64_t hot_started_cpu, hot_started_wall, hot_started_wait, hot_elements, hot_next_report;

void void_bioshock_hot_enter(uint32_t va)
{
    if (!hot_address(va)) return;
    if (hot_depth++) return;
    ++hot_calls;
    hot_elements = g_ecx >= 0x10000u && void_vm_readable(g_ecx + 8, 4) ? MEM32(g_ecx + 8) : 0;
    hot_entry_esp = g_esp;
    hot_started_cpu = hot_ns(CLOCK_THREAD_CPUTIME_ID);
    hot_started_wall = hot_ns(CLOCK_MONOTONIC);
    hot_started_wait = void_lock_wait_ns();
}

void void_bioshock_hot_leave(uint32_t va)
{
    if (!hot_address(va) || !hot_depth || g_esp != hot_entry_esp || --hot_depth) return;
    uint64_t cpu = hot_ns(CLOCK_THREAD_CPUTIME_ID) - hot_started_cpu;
    uint64_t wall = hot_ns(CLOCK_MONOTONIC);
    uint64_t elapsed = wall - hot_started_wall;
    uint64_t wait = void_lock_wait_ns() - hot_started_wait;
    if (hot_reported++ < 32 || wall >= hot_next_report) {
        void_log("[hot] va=%08X call=%u elements=%llu cpu_ms=%.3f wall_ms=%.3f machine_wait_ms=%.3f",
            va, hot_calls, (unsigned long long)hot_elements, cpu / 1e6, elapsed / 1e6, wait / 1e6);
        hot_next_report = wall + 5000000000ull;
    }
}
