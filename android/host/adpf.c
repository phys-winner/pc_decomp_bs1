/* ADPF hint session (APerformanceHint) of the frame's work: the game thread reports how long each frame's
 * work took against a share of the frame limit's period (adpf_target_percent, 75 %), so the scheduler raises
 * clocks and placement as far as the frame needs them with headroom for its spikes.
 *
 * BioShock (2026-11-21): the native Direct3D 10 renderer calls ut3_adpf_work_end() when the game hands a
 * frame to IDXGISwapChain::Present and ut3_adpf_work_begin() when Present returns (after the frame limiter
 * and the wait for frames in flight), so the interval is the game's own work, its waits for the single guest
 * machine included.  The session covers every guest thread (they serialize on the machine): on the Pixel 10
 * (Tensor G5) without a session the game thread ran on the A725 cores near 2 GHz and the X4 idled at
 * 883 MHz, because no thread was ever busy enough for the utilization-based governor (and the kernel kept
 * the guest's affinity at cpus 2-7 instead of the prime core).  The launcher's option "Performance hints
 * (ADPF)" (PORT_OPTION_ADPF=1, off by default since 25 November 2026) turns it on; BIOSHOCK_ADPF=0 (QA
 * perf-flags.env) keeps it off even then.  No guest state,
 * thread affinity or pacing is changed. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#ifdef UT3_ADPF_TEST
#include "test_platform.h"
#else
#include <stdatomic.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

extern void void_log(const char *, ...);
extern void void_ui_hud_frame_begin(void) __attribute__((weak));
extern void void_ui_camera_frame_begin(void) __attribute__((weak));
/* threads.c: the guest threads alive now; render10: the frame limit the target follows. */
extern int void_guest_thread_ids(int32_t *out, int max) __attribute__((weak));
extern float r10_frame_limit(void) __attribute__((weak));
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
/* Exact API33 C signatures from NDK27 performance_hint.h; dynamic lookup keeps
 * the existing API30 minimum. No API35/GPU/auto-timing dependency is required. */
static struct {
    APerformanceHintManager *(*manager)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    void (*close)(APerformanceHintSession *);
    int (*update_target)(APerformanceHintSession *, int64_t);                    /* API 33, optional here */
    int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);       /* API 34, optional */
    APerformanceHintManager *instance;
    int ready;
} adpf_api;
#define ADPF_MAX_THREADS 32
#ifndef ADPF_TARGET_PERCENT
#define ADPF_TARGET_PERCENT 75   /* the test tablet A/B of 22 November 2026: 100 % 57.0/56.3 FPS, 75 % 60.0/59.9 FPS */
#endif
static pthread_once_t adpf_once = PTHREAD_ONCE_INIT;
static pthread_key_t adpf_key;
/* Low bit blocks work; upper bits cancel intervals across every lifecycle event.
 * Start blocked until Android publishes the first ready lifecycle snapshot. */
static _Atomic uint64_t adpf_epoch = 1;
static int64_t adpf_target_ns = INT64_C(16666666);

/* Set before starting the game thread and creating its hint session. */
void ut3_adpf_set_target(float fps)
{
    if (fps >= 1 && fps <= 1000) adpf_target_ns = (int64_t)(1000000000.0 / (double)fps);
}
typedef struct {
    APerformanceHintSession *session;
    int32_t tid;
    int failed, armed;
    uint64_t epoch;
    int64_t start;
    int64_t target;                       /* the session's current target */
    int32_t tids[ADPF_MAX_THREADS];       /* the session's threads: this one first, then the guest threads */
    size_t tid_count;
    unsigned frames, thread_failures;
} adpf_thread;
static _Thread_local adpf_thread adpf_tls;

static void adpf_destroy(void *opaque)
{
    adpf_thread *thread = opaque;
    if (thread->session) adpf_api.close(thread->session);
    memset(thread, 0, sizeof *thread);
}

static void adpf_load(void)
{
    /* The launcher's switch (port.json option "adpf", off by default since 25 November 2026, the owner's
     * decision), read once per process: only an explicit 1/true opens a session; unset or anything else is off. */
    const char *option = getenv("PORT_OPTION_ADPF");
    if (!option || (strcmp(option, "1") && strcmp(option, "true"))) {
        void_log("[adpf] off (launcher option %s)", option ? option : "unset");
        return;
    }
    const char *off = getenv("BIOSHOCK_ADPF");
    if (off && off[0] == '0') { void_log("[adpf] off (BIOSHOCK_ADPF=0)"); return; }
    /* Keep the system DSO loaded while TLS destructors may still use its API. */
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { void_log("[adpf] unavailable: libandroid.so"); return; }
    *(void **)&adpf_api.manager = dlsym(library, "APerformanceHint_getManager");
    *(void **)&adpf_api.create = dlsym(library, "APerformanceHint_createSession");
    *(void **)&adpf_api.report = dlsym(library, "APerformanceHint_reportActualWorkDuration");
    *(void **)&adpf_api.close = dlsym(library, "APerformanceHint_closeSession");
    if (!adpf_api.manager || !adpf_api.create || !adpf_api.report || !adpf_api.close) {
        void_log("[adpf] unavailable: native API33 entrypoints"); return;
    }
    *(void **)&adpf_api.update_target = dlsym(library, "APerformanceHint_updateTargetWorkDuration");
    *(void **)&adpf_api.set_threads = dlsym(library, "APerformanceHint_setThreads");
    adpf_api.instance = adpf_api.manager();
    if (!adpf_api.instance) { void_log("[adpf] unavailable: manager"); return; }
    int error = pthread_key_create(&adpf_key, adpf_destroy);
    if (error) { void_log("[adpf] TLS key failed %d", error); return; }
    adpf_api.ready = 1;
}

static int64_t adpf_now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) || time.tv_sec < 0 || time.tv_nsec < 0 ||
        time.tv_nsec >= 1000000000 || time.tv_sec > (INT64_MAX - time.tv_nsec) / INT64_C(1000000000)) return 0;
    return (int64_t)time.tv_sec * INT64_C(1000000000) + time.tv_nsec;
}

/* UI thread: a pure atomic cancellation, never an API close/report. `blocked`
 * is the combined (paused || Surface missing) snapshot from android_main. */
void ut3_adpf_cancel(int blocked)
{
    uint64_t before = atomic_load_explicit(&adpf_epoch, memory_order_acquire), after;
    do {
        after = ((before & ~UINT64_C(1)) + 2u) | (blocked ? 1u : 0u);
    } while (!atomic_compare_exchange_weak_explicit(&adpf_epoch, &before, after,
                                                   memory_order_acq_rel, memory_order_acquire));
}

/* This thread first, then the guest threads alive now (threads.c), without repeats. */
static size_t adpf_thread_list(int32_t self, int32_t *out)
{
    size_t n = 0;
    out[n++] = self;
    if (!void_guest_thread_ids) return n;
    int32_t guest[ADPF_MAX_THREADS];
    int count = void_guest_thread_ids(guest, ADPF_MAX_THREADS);
    for (int i = 0; i < count && n < ADPF_MAX_THREADS; ++i) {
        size_t k = 0;
        while (k < n && out[k] != guest[i]) ++k;
        if (k == n) out[n++] = guest[i];
    }
    return n;
}

/* The share of the frame period the session asks the frame's work to fit in (BIOSHOCK_ADPF_TARGET, percent).
 * A target of the whole period lets the power HAL lower clocks until the work fills it: on the Lenovo TB373FU
 * (Dimensity 8300) the game's core ran at 1.6-2.2 GHz instead of 2.9-3.2 GHz without a session, and every
 * spike missed the 60 Hz refresh (58.2 vs 60.0 FPS standing, 17 vs 1 frames over 33 ms walking).  At 75 % the
 * session keeps a quarter of the period as headroom: 60.0 FPS standing and 59.9 walking with one frame over 33 ms,
 * as without a session, while the game's core averaged 2.5 instead of 3.0 GHz standing (work/ha22-perf-20261122). */
static int adpf_target_percent(void)
{
    static int percent;
    if (!percent) {
        const char *text = getenv("BIOSHOCK_ADPF_TARGET");
        int value = text ? atoi(text) : 0;
        percent = value >= 30 && value <= 100 ? value : ADPF_TARGET_PERCENT;
    }
    return percent;
}

/* The period of the frame limit the game runs at now (render10), else the start's, times the target share. */
static int64_t adpf_target(void)
{
    float fps = r10_frame_limit ? r10_frame_limit() : 0.0f;
    int64_t period = fps >= 1 && fps <= 1000 ? (int64_t)(1000000000.0 / (double)fps) : adpf_target_ns;
    return period * adpf_target_percent() / 100;
}

/* Every 128 frames: the session follows new guest threads and a changed frame limit. */
static void adpf_refresh(adpf_thread *thread)
{
    if (++thread->frames & 127u) return;
    int64_t target = adpf_target();
    if (target != thread->target && adpf_api.update_target && !adpf_api.update_target(thread->session, target)) {
        thread->target = target;
        void_log("[adpf] target %lld ns", (long long)target);
    }
    if (!adpf_api.set_threads || thread->thread_failures >= 4) return;
    int32_t tids[ADPF_MAX_THREADS];
    size_t n = adpf_thread_list(thread->tid, tids);
    if (n == thread->tid_count && !memcmp(tids, thread->tids, n * sizeof tids[0])) return;
    int error = adpf_api.set_threads(thread->session, tids, n);
    if (error) {
        if (++thread->thread_failures <= 2) void_log("[adpf] setThreads (%zu threads) failed %d", n, error);
        return;
    }
    memcpy(thread->tids, tids, n * sizeof tids[0]);
    thread->tid_count = n;
    void_log("[adpf] session threads %zu", n);
}

/* Exact begin: original 00407E9D, after the tick limiter returned, including
 * its no-wait path. Never substitute Present-return or generic Sleep-return.
 * BioShock: the return of IDXGISwapChain::Present (render10/r10_dxgi.c). */
void ut3_adpf_work_begin(void)
{
    int saved_errno = errno;
    if (void_ui_hud_frame_begin) void_ui_hud_frame_begin();
    if (void_ui_camera_frame_begin) void_ui_camera_frame_begin();
    adpf_thread *thread = &adpf_tls;
    thread->armed = 0;
    uint64_t epoch = atomic_load_explicit(&adpf_epoch, memory_order_acquire);
    if ((epoch & 1u) || thread->failed) goto done;
    pthread_once(&adpf_once, adpf_load);
    if (!adpf_api.ready) goto done;
    if (!thread->session) {
        thread->tid = (int32_t)syscall(SYS_gettid);
        if (thread->tid <= 0) { thread->failed = 1; goto done; }
        thread->target = adpf_target();
        thread->tid_count = adpf_thread_list(thread->tid, thread->tids);
        thread->session = adpf_api.create(adpf_api.instance, thread->tids, thread->tid_count, thread->target);
        if (!thread->session && thread->tid_count > 1) {   /* a guest thread the system refused: this one alone */
            thread->tid_count = 1;
            thread->session = adpf_api.create(adpf_api.instance, thread->tids, 1, thread->target);
        }
        if (!thread->session) {
            thread->failed = 1;
            void_log("[adpf] session unavailable for tid %d", thread->tid);
            goto done;
        }
        int error = pthread_setspecific(adpf_key, thread);
        if (error) {
            adpf_api.close(thread->session); thread->session = NULL; thread->failed = 1;
            void_log("[adpf] TLS registration failed %d", error); goto done;
        }
        void_log("[adpf] session tid %d target %lld ns, %zu threads", thread->tid, (long long)thread->target,
                 thread->tid_count);
        /* The one-time lookup/session setup is not a steady workload sample. */
        goto done;
    }
    adpf_refresh(thread);
    thread->start = adpf_now();
    if (thread->start > 0 && atomic_load_explicit(&adpf_epoch, memory_order_acquire) == epoch) {
        thread->epoch = epoch; thread->armed = 1;
    }
done:
    errno = saved_errno;
}

/* End of producer work: incoming guest Present, BEFORE the opaque native
 * Present/host limiter. This does not claim GPU or native-submission duration. */
void ut3_adpf_work_end(void)
{
    int saved_errno = errno;
    adpf_thread *thread = &adpf_tls;
    if (!thread->armed) goto done;
    thread->armed = 0; /* Multiple viewport Presents do not duplicate a sample. */
    int64_t end = adpf_now();
    uint64_t epoch = atomic_load_explicit(&adpf_epoch, memory_order_acquire);
    if (epoch != thread->epoch || (epoch & 1u) || end <= thread->start) goto done;
    int error = adpf_api.report(thread->session, end - thread->start);
    if (error) {
        /* Owner-only close; UI cancellation never races this API operation.
         * Disable after communication/argument failure rather than churn per frame. */
        adpf_api.close(thread->session); thread->session = NULL; thread->failed = 1;
        pthread_setspecific(adpf_key, NULL);
        void_log("[adpf] report failed tid %d status %d", thread->tid, error);
    }
done:
    errno = saved_errno;
}

/* Owner-only normal shutdown; pthread TLS destruction is the fallback. */
void ut3_adpf_thread_close(void)
{
    int saved_errno = errno;
    if (adpf_tls.session) {
        pthread_setspecific(adpf_key, NULL);
        adpf_destroy(&adpf_tls);
    }
    errno = saved_errno;
}
