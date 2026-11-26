/* Long-frame attribution: host work that can stall a frame (file system,
 * shader compilation, texture conversion) adds its time here; a presented
 * frame much longer than usual logs what happened during it as "[hitch]". */
#include "host.h"
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>

static const char *const perf_names[VOID_PERF_COUNT] = {
    "open", "map", "find", "read", "compile", "link", "cached-program", "texture",
};

static _Atomic uint64_t perf_ns[VOID_PERF_COUNT], perf_calls[VOID_PERF_COUNT], perf_bytes[VOID_PERF_COUNT];

uint64_t void_perf_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t void_perf_calls(int kind)
{
    return kind >= 0 && kind < VOID_PERF_COUNT ? atomic_load_explicit(&perf_calls[kind], memory_order_relaxed) : 0;
}

void void_perf_add(int kind, uint64_t ns, uint64_t bytes)
{
    if (kind < 0 || kind >= VOID_PERF_COUNT) return;
    atomic_fetch_add_explicit(&perf_ns[kind], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&perf_calls[kind], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&perf_bytes[kind], bytes, memory_order_relaxed);
}

/* Called after every Present on the render thread. */
void void_perf_frame_end(uint32_t frame)
{
    static uint64_t last_ns, logged;
    static uint64_t base_ns[VOID_PERF_COUNT], base_calls[VOID_PERF_COUNT], base_bytes[VOID_PERF_COUNT];
    uint64_t now = void_perf_now_ns();
    uint64_t wall = last_ns ? now - last_ns : 0;
    last_ns = now;
    uint64_t ns[VOID_PERF_COUNT], calls[VOID_PERF_COUNT], bytes[VOID_PERF_COUNT];
    for (int i = 0; i < VOID_PERF_COUNT; ++i) {
        ns[i] = atomic_load_explicit(&perf_ns[i], memory_order_relaxed) - base_ns[i];
        calls[i] = atomic_load_explicit(&perf_calls[i], memory_order_relaxed) - base_calls[i];
        bytes[i] = atomic_load_explicit(&perf_bytes[i], memory_order_relaxed) - base_bytes[i];
        base_ns[i] += ns[i];
        base_calls[i] += calls[i];
        base_bytes[i] += bytes[i];
    }
    /* QA travel sweeps also need the peak after a fix puts every link below the 70 ms hitch threshold.
     * Reuse the frame counters and emit at most one line per 60 frames, outside pipeline creation. */
    static int qa_travel = -1;
    static uint64_t peak_ns, peak_wall, peak_calls;
    static uint32_t peak_frame;
    if (qa_travel < 0) qa_travel = getenv("BIOSHOCK_QA_CONSOLE") != NULL;
    if (qa_travel) {
        if (ns[VOID_PERF_PROGRAM_LINK] > peak_ns) {
            peak_ns = ns[VOID_PERF_PROGRAM_LINK];
            peak_calls = calls[VOID_PERF_PROGRAM_LINK];
            peak_wall = wall;
            peak_frame = frame;
        }
        if (frame % 60 == 0 && peak_ns) {
            void_log("[link-peak] frame %u: %.1f ms; link %llu/%.3f ms;", peak_frame, peak_wall / 1e6,
                     (unsigned long long)peak_calls, peak_ns / 1e6);
            peak_ns = 0;
        }
    }
    /* A budget of 400 lines that refills by 100 a minute: loading screens alone used to spend a session's
     * whole cap of 400 before the first level was played, so later gameplay hitches went unattributed. */
    static uint64_t refill_ns;
    if (!refill_ns) refill_ns = now;
    if (now - refill_ns >= 60000000000ull) {
        logged = logged > 100 ? logged - 100 : 0;
        refill_ns = now;
    }
    if (wall < 70000000ull || logged >= 400) return;
    ++logged;
    char line[1024];
    int used = snprintf(line, sizeof line, "[hitch] frame %u: %.1f ms;", frame, (double)wall / 1e6);
    for (int i = 0; i < VOID_PERF_COUNT && used < (int)sizeof line - 64; ++i) {
        if (!calls[i]) continue;
        used += snprintf(line + used, sizeof line - (size_t)used, " %s %llu/%.1f ms", perf_names[i],
                         (unsigned long long)calls[i], (double)ns[i] / 1e6);
        if (bytes[i]) used += snprintf(line + used, sizeof line - (size_t)used, "/%.1f MiB", (double)bytes[i] / 1048576.0);
        used += snprintf(line + used, sizeof line - (size_t)used, ";");
    }
    void_log("%s", line);
}
