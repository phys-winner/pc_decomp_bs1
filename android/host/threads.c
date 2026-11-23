/* Guest threads, the machine lock, handles and Win32 synchronization.
 *
 * One host pthread per guest thread. Only the thread holding the machine runs
 * lifted code or touches the global register file; a host call that blocks
 * releases the machine around the wait (void_blocking_begin/end), and lifted
 * loops hand it over periodically (recomp_yield_hook), as native32 did on
 * Windows. The lock is a FIFO ticket lock so a yield really passes the
 * machine to a waiting thread instead of re-acquiring it immediately. */
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <sched.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TLS_SLOTS 1088
#define STATIC_TLS_MODULES 64
#define GUEST_STACK_DEFAULT (1u << 20)
#define HOST_STACK_SIZE (32u << 20)

struct void_thread {
    void_regs regs;
    int depth;
    uint32_t id;
    pid_t native_tid;
    uint32_t tib, peb;
    uint32_t stack_lo, stack_hi;
    uint32_t last_error;
    uint32_t tls[TLS_SLOTS];
    uint32_t static_tls;
    void_object *object;
    uint32_t start, param;
    uint32_t exit_code;
    uint32_t affinity_mask, virtual_cpu;
    int suspended;
    int guest_owned;                 /* created by CreateThread */
    int first_wait;
    uint32_t trace_index;
    uint32_t trace[256];
    jmp_buf exit_jb;
    int exit_armed;
    pthread_t pt;
    struct void_thread *next_thread;   /* every guest thread, for TLS slot resets */
    /* Private wake-ups: a machine release, an object signal or a critical-section
     * release wakes only the thread it concerns (see wake_policy below). */
    pthread_cond_t machine_cond, object_cond, cs_cond_self;
};

static struct void_thread *all_threads;
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t cpu_model_once = PTHREAD_ONCE_INIT;
static uint32_t guest_cpu_count, guest_system_mask, guest_process_mask;

static void cpu_model_init(void)
{
    long count = sysconf(_SC_NPROCESSORS_CONF);
    if (count < 1) count = 1;
    if (count > 32) count = 32;
    guest_cpu_count = (uint32_t)count;
    guest_system_mask = (uint32_t)((1ull << count) - 1);
    guest_process_mask = guest_system_mask;
}

/* Guest virtual CPU eligibility is separate from pin_guest_thread's Android
 * strong-core placement. Virtual CPUs are multiplexed by the machine lock. */
uint32_t void_guest_cpu_count(void) { pthread_once(&cpu_model_once, cpu_model_init); return guest_cpu_count; }
uint32_t void_guest_cpu_system_mask(void) { pthread_once(&cpu_model_once, cpu_model_init); return guest_system_mask; }

static void select_virtual_cpu(void_thread *thread)
{
    if (!(thread->affinity_mask & (1u << thread->virtual_cpu)))
        thread->virtual_cpu = (uint32_t)__builtin_ctz(thread->affinity_mask);
}

static __thread void_thread *t_self;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mcond = PTHREAD_COND_INITIALIZER;
static uint64_t ticket_next, ticket_serving;

/* Wake policy. Every guest thread that is not running sleeps in exactly one of
 * three places: waiting for its machine ticket, in a Win32 object wait, or for
 * a critical section. Historically each of them was one shared condition that
 * every release/signal broadcast, so a single SetEvent or machine hand-over
 * woke every sleeping guest thread just to let it re-check and sleep again:
 * on the RMX3851 that was ~20k context switches/s on the guest core and the
 * machine holder spent 73% of its time preempted (work/perf_optimization_295.md).
 * Now a waiter registers with its own condition and only the thread whose
 * ticket, object or critical section changed is woken. The predicates, FIFO
 * ticket order and every Win32 result are unchanged; BIOSHOCK_LEGACY_WAKE=1
 * restores the shared broadcasts for A/B measurements. */
static int wake_legacy = -1;
static int legacy_wake(void)
{
    int value = __atomic_load_n(&wake_legacy, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *env = getenv("BIOSHOCK_LEGACY_WAKE");
        value = env && *env == '1';
        __atomic_store_n(&wake_legacy, value, __ATOMIC_RELAXED);
    }
    return value;
}

static int wait_release_first_flag = -1;
static int wait_release_first(void)
{
    int value = __atomic_load_n(&wait_release_first_flag, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *env = getenv("BIOSHOCK_WAIT_RELEASE_FIRST");
        value = env && *env == '1';
        __atomic_store_n(&wait_release_first_flag, value, __ATOMIC_RELAXED);
    }
    return value;
}

typedef struct machine_waiter {
    struct machine_waiter *next;
    uint64_t ticket;
    pthread_cond_t *cond;
} machine_waiter;
static machine_waiter *machine_waiters;   /* guarded by mlock */

/* mlock held: hand the machine to whichever thread holds ticket_serving. */
static void machine_wake_turn_locked(void)
{
    if (legacy_wake()) { pthread_cond_broadcast(&mcond); return; }
    for (machine_waiter *waiter = machine_waiters; waiter; waiter = waiter->next) {
        if (waiter->ticket == ticket_serving) { pthread_cond_signal(waiter->cond); return; }
    }
}

/* FMOD feeder threads (entry 10E4B2C6) waiting for the machine; see void_machine_offer_audio. */
static int audio_waiting;

/* The audio producer (audio_native.c: FMOD's output thread) renders while a
 * low-latency AAudio callback waits for PCM, so its waits for the machine are
 * urgent (transferred from The Sims 2, wiki note 25). The holder offers the
 * machine at its next loop back-edge instead of up to RECOMP_YIELD_EVERY
 * back-edges later: the waiter sets the shared back-edge counter to the
 * threshold, and repeats it every URGENT_POKE_NS until served (the store can
 * race with the holder's own increment, and each thread ahead in the queue
 * takes its turn). The producer keeps the machine for URGENT_SLICE_NS itself
 * while others wait. A wait longer than URGENT_REPORT_NS is logged with the
 * holder's guest function and its last indirect call (an import or method that
 * keeps the machine has no back-edge to poke). BIOSHOCK_AUDIO_URGENT=0 turns
 * this off. */
#define URGENT_POKE_NS 250000L
#define URGENT_REPORT_NS 20000000ull
#define URGENT_REPORTS 60
#define URGENT_SLICE_NS 2000000ull
static __thread int t_urgent;
static __thread uint64_t t_held_since;
static unsigned urgent_reports;
static uint32_t machine_holder;
static int urgent_flag = -1;
static int urgent_enabled(void)
{
    int value = __atomic_load_n(&urgent_flag, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *env = getenv("BIOSHOCK_AUDIO_URGENT");
        value = !(env && *env == '0');
        __atomic_store_n(&urgent_flag, value, __ATOMIC_RELAXED);
    }
    return value;
}
void void_machine_urgent(int urgent) { t_urgent = urgent && urgent_enabled(); }
static uint64_t now_ns(void);

/* Loans. A host call that waits for the GPU (a fence, the next swap-chain
 * image, a buffer swap) keeps the machine: the renderer's state is half
 * updated and several guest threads call Direct3D. It lends the machine to the
 * urgent producer alone (void_machine_lend_begin/end): the producer, waiting
 * anywhere in the queue, takes the offer and leaves its ticket parked (the
 * queue skips a parked ticket when it reaches it, or the producer takes it
 * back when it waits again before that); its release ends the loan, and the
 * lender takes the machine back when its wait is over. The producer never
 * touches the lender's GL context. A borrower that would yield or block (a
 * critical section the lender holds, say) returns the loan first. A returned
 * loan is offered again for as long as the lender waits. */
enum { LOAN_NONE, LOAN_OFFERED, LOAN_TAKEN };
static int loan_state;                   /* mlock */
static uint64_t parked_ticket;           /* the borrower's ticket + 1; mlock */
static uint32_t loan_lender, machine_loans;
static __thread int t_borrowing, t_lending;

/* mlock held: sleep until `ticket` is served. Spurious wake-ups re-check. */
static int machine_wait_turn_locked(uint64_t ticket)
{
    int borrowed = 0;
    pthread_cond_t local = PTHREAD_COND_INITIALIZER;
    machine_waiter self = { machine_waiters, ticket,
                            legacy_wake() ? &mcond : t_self ? &t_self->machine_cond : &local };
    machine_waiters = &self;
    int audio = t_self && t_self->start == 0x10E4B2C6u && ticket != ticket_serving;
    if (audio) __atomic_add_fetch(&audio_waiting, 1, __ATOMIC_RELAXED);
    if (!t_urgent) {
        while (ticket != ticket_serving) pthread_cond_wait(self.cond, &mlock);
    } else if (ticket != ticket_serving) {
        uint64_t start = now_ns();
        uint32_t ahead = (uint32_t)(ticket - ticket_serving), held_by = 0, held_in = 0, held_call = 0, held_from = 0;
        int sampled = 0;
        while (ticket != ticket_serving) {
            if (loan_state == LOAN_OFFERED) {
                loan_state = LOAN_TAKEN;
                parked_ticket = ticket + 1u;
                ++machine_loans;
                borrowed = 1;
                break;
            }
            __atomic_store_n(&g_backedges, RECOMP_YIELD_EVERY, __ATOMIC_RELAXED);
            if (!sampled && now_ns() - start >= URGENT_REPORT_NS) {
                sampled = 1;
                uint32_t last = (__atomic_load_n(&g_icall_trace_idx, __ATOMIC_RELAXED) - 1u) & (ICALL_TRACE_SIZE - 1u);
                held_by = machine_holder;
                held_in = __atomic_load_n(&g_cur_func, __ATOMIC_RELAXED);
                held_call = __atomic_load_n(&g_icall_trace[last], __ATOMIC_RELAXED);
                held_from = __atomic_load_n(&g_icall_from[last], __ATOMIC_RELAXED);
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += URGENT_POKE_NS;
            if (until.tv_nsec >= 1000000000L) { until.tv_nsec -= 1000000000L; ++until.tv_sec; }
            pthread_cond_timedwait(self.cond, &mlock, &until);
        }
        if (sampled && urgent_reports < URGENT_REPORTS) {
            ++urgent_reports;
            const char *name = void_thunk_name(held_call);
            void_log("[machine] urgent wait %.1f ms behind %u thread(s): thread %u held the machine in %08X; "
                     "last indirect call %08X (%s) from %08X; %u loans so far%s", (now_ns() - start) / 1e6, ahead,
                     held_by, held_in, held_call, name ? name : "guest", held_from, machine_loans,
                     borrowed ? "; ended on a loan" : "");
        }
    }
    if (audio) __atomic_sub_fetch(&audio_waiting, 1, __ATOMIC_RELAXED);
    for (machine_waiter **link = &machine_waiters; *link; link = &(*link)->next)
        if (*link == &self) { *link = self.next; break; }
    pthread_cond_destroy(&local);
    return borrowed;
}

/* mlock held: the next ticket's turn; a parked ticket is passed over. */
static void serve_next_locked(void)
{
    ++ticket_serving;
    if (parked_ticket && ticket_serving == parked_ticket - 1u) { parked_ticket = 0; ++ticket_serving; }
    machine_wake_turn_locked();
}
static uint32_t next_thread_id = 0x104;
static uint32_t peb_va;
static uint8_t tls_used[TLS_SLOTS];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

/* PE static TLS uses fs:2C; TlsAlloc uses the independent TEB+E10 slots. */
typedef struct {
    uint32_t base, raw, raw_size, total_size, alignment, callbacks;
    int attached;
} static_tls_module;
static static_tls_module static_tls_modules[STATIC_TLS_MODULES];
static uint32_t static_tls_count;
/* shortcut: mapped modules and thread storage stay resident, matching this host; add reclamation with module unloading. */

static void tls_thread_slot(void_thread *t, uint32_t index)
{
    const static_tls_module *m = &static_tls_modules[index];
    uint32_t allocation = void_galloc(m->total_size + m->alignment - 1);
    if (!allocation) void_fatal("static TLS allocation failed");
    uint32_t block = (allocation + m->alignment - 1) & ~(m->alignment - 1);
    memset(GPTR(block), 0, m->total_size);
    if (m->raw_size) memcpy(GPTR(block), GPTR(m->raw), m->raw_size);
    MEM32(t->static_tls + 4 * index) = block;
}

static void tls_thread_init(void_thread *t)
{
    t->static_tls = void_galloc(4 * STATIC_TLS_MODULES);
    if (!t->static_tls) void_fatal("static TLS vector allocation failed");
    MEM32(t->tib + 0x2C) = t->static_tls;
    for (uint32_t i = 0; i < static_tls_count; ++i) tls_thread_slot(t, i);
}

int void_tls_register(uint32_t base, uint32_t directory)
{
    if (!directory) return 1;
    if (!void_vm_readable(directory, 24)) return 0;
    uint32_t raw = MEM32(directory), end = MEM32(directory + 4);
    uint32_t index = MEM32(directory + 8), callbacks = MEM32(directory + 12);
    uint32_t zero_fill = MEM32(directory + 16), align_code = (MEM32(directory + 20) >> 20) & 15;
    uint64_t total = (uint64_t)end - raw + zero_fill;
    if (end < raw || total > UINT32_MAX - 8191u || align_code > 14 ||
        (end != raw && !void_vm_readable(raw, end - raw)) || !void_vm_writable(index, 4)) return 0;
    if (callbacks) {
        uint32_t n;
        for (n = 0; n < 1024; ++n) {
            uint64_t slot = (uint64_t)callbacks + 4 * n;
            if (slot + 4 > 0x100000000ull || !void_vm_readable((uint32_t)slot, 4)) return 0;
            if (!MEM32((uint32_t)slot)) break;
        }
        if (n == 1024) return 0;
    }
    pthread_mutex_lock(&threads_lock);
    for (uint32_t i = 0; i < static_tls_count; ++i) {
        if (static_tls_modules[i].base == base) { MEM32(index) = i; pthread_mutex_unlock(&threads_lock); return 1; }
    }
    if (static_tls_count == STATIC_TLS_MODULES) { pthread_mutex_unlock(&threads_lock); return 0; }
    uint32_t slot = static_tls_count++;
    static_tls_modules[slot] = (static_tls_module){ base, raw, end - raw,
        total ? (uint32_t)total : 1, align_code ? 1u << (align_code - 1) : 16, callbacks, 0 };
    MEM32(index) = slot;
    for (void_thread *t = all_threads; t; t = t->next_thread) tls_thread_slot(t, slot);
    pthread_mutex_unlock(&threads_lock);
    return 1;
}

void void_tls_callbacks(uint32_t base, uint32_t reason)
{
    for (uint32_t i = 0; i < static_tls_count; ++i) {
        static_tls_module *m = &static_tls_modules[i];
        if (m->base != base) continue;
        if (reason == 1) m->attached = 1;
        else if (reason == 0) m->attached = 0;
        uint32_t args[3] = { base, reason, 0 };
        if (m->callbacks) {
            for (uint32_t n = 0; n < 1024; ++n) {
                uint64_t slot = (uint64_t)m->callbacks + 4 * n;
                if (slot + 4 > 0x100000000ull || !void_vm_readable((uint32_t)slot, 4))
                    void_fatal("invalid static TLS callback list for %08X", base);
                uint32_t callback = MEM32((uint32_t)slot);
                if (!callback) return;
                void_call_guest(callback, 3, args, 1);
            }
            void_fatal("unterminated static TLS callback list for %08X", base);
        }
        return;
    }
}

static void tls_thread_callbacks(uint32_t reason)
{
    for (uint32_t n = 0; n < static_tls_count; ++n) {
        uint32_t i = reason == 3 ? static_tls_count - 1 - n : n;
        if (static_tls_modules[i].attached) void_tls_callbacks(static_tls_modules[i].base, reason);
    }
}

/* ---------------------------------------------------------- machine lock */
/* Time this thread spent waiting for the machine (frame statistics). */
static __thread uint64_t t_lock_wait_ns;
static __thread int t_machine_owned;
uint64_t void_lock_wait_ns(void) { return t_lock_wait_ns; }
int void_machine_is_owned(void) { return t_self && t_self->depth > 0 && t_machine_owned; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin_guest_thread(void);
extern void d3d_gl_thread_acquire(void) __attribute__((weak));
extern void d3d_gl_thread_release(void) __attribute__((weak));

/* QA diagnostic, BIOSHOCK_MACHINE_STATS=1: per-thread machine ownership over
 * 5-second windows (holding time, acquisitions, waits, yields) and a check
 * that a yield point is reached only by the holder. Off: one load per call. */
static int machine_stats = -1;
static __thread uint64_t t_hold_start;
typedef struct { uint32_t id, start; pid_t tid; uint64_t hold_ns, wait_ns, acquires, yields, max_hold_ns, max_wait_ns; } machine_stat;
static machine_stat machine_stat_rows[64];
static unsigned machine_stat_used;
static uint64_t machine_stat_window, machine_foreign_yields;
static int machine_stats_on(void)
{
    int value = __atomic_load_n(&machine_stats, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *env = getenv("BIOSHOCK_MACHINE_STATS");
        value = env && *env == '1';
        __atomic_store_n(&machine_stats, value, __ATOMIC_RELAXED);
    }
    return value;
}
/* mlock held. */
static void machine_stat_add(uint64_t hold, uint64_t wait, int acquire, int yield)
{
    uint32_t id = t_self ? t_self->id : 0;
    unsigned i = 0;
    while (i < machine_stat_used && machine_stat_rows[i].id != id) ++i;
    if (i == machine_stat_used) {
        if (i == 64) return;
        machine_stat_rows[machine_stat_used++] = (machine_stat){ id, t_self ? t_self->start : 0, (pid_t)gettid(), 0, 0, 0, 0, 0, 0 };
    }
    machine_stat_rows[i].hold_ns += hold;
    machine_stat_rows[i].wait_ns += wait;
    machine_stat_rows[i].acquires += (uint64_t)acquire;
    machine_stat_rows[i].yields += (uint64_t)yield;
    if (hold > machine_stat_rows[i].max_hold_ns) machine_stat_rows[i].max_hold_ns = hold;
    if (wait > machine_stat_rows[i].max_wait_ns) machine_stat_rows[i].max_wait_ns = wait;
    uint64_t now = now_ns();
    if (!machine_stat_window) machine_stat_window = now;
    if (now - machine_stat_window < 5000000000ull) return;
    double seconds = (double)(now - machine_stat_window) / 1e9;
    for (unsigned k = 0; k < machine_stat_used; ++k) {
        machine_stat *row = &machine_stat_rows[k];
        if (row->acquires || row->hold_ns)
            void_log("[machine-stats] id=%u start=%08X tid=%d hold=%.1f%% wait=%.1f%% acquires=%.0f/s yields=%.0f/s max_hold=%.1fms max_wait=%.1fms",
                row->id, row->start, row->tid, 100.0 * (double)row->hold_ns / 1e9 / seconds, 100.0 * (double)row->wait_ns / 1e9 / seconds,
                (double)row->acquires / seconds, (double)row->yields / seconds,
                (double)row->max_hold_ns / 1e6, (double)row->max_wait_ns / 1e6);
        row->hold_ns = row->wait_ns = row->acquires = row->yields = row->max_hold_ns = row->max_wait_ns = 0;
    }
    void_log("[machine-stats] window=%.2fs foreign_yields=%llu", seconds, (unsigned long long)machine_foreign_yields);
    machine_foreign_yields = 0;
    machine_stat_window = now;
}

/* QA (BIOSHOCK_HOLD_SAMPLER=1): a machine hold longer than 40 ms while other
 * guest threads wait is logged every 10 ms (at most 12 lines per hold) with
 * the holder's current lifted function, to find work that never reaches a
 * yield point. Off: one relaxed load per acquisition. */
static int hold_sampler_flag = -1;
static uint64_t machine_hold_since;
static int hold_sampler_on(void)
{
    int value = __atomic_load_n(&hold_sampler_flag, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *env = getenv("BIOSHOCK_HOLD_SAMPLER");
        value = env && *env == '1';
        __atomic_store_n(&hold_sampler_flag, value, __ATOMIC_RELAXED);
    }
    return value;
}
static int machine_contended(void);
static void *hold_sampler(void *unused)
{
    (void)unused;
    uint64_t current = 0;
    unsigned lines = 0;
    for (;;) {
        struct timespec delay = { 0, 10000000 };
        nanosleep(&delay, NULL);
        uint64_t since = __atomic_load_n(&machine_hold_since, __ATOMIC_ACQUIRE);
        uint32_t holder = __atomic_load_n(&machine_holder, __ATOMIC_RELAXED);
        if (!since || !holder || !machine_contended()) continue;
        uint64_t held = now_ns() - since;
        if (held < 40000000ull) continue;
        if (since != current) { current = since; lines = 0; }
        if (lines++ < 12)
            void_log("[hold] holder=%u held=%.0fms func=%08X waiting=%llu", holder, (double)held / 1e6,
                __atomic_load_n(&g_cur_func, __ATOMIC_RELAXED),
                (unsigned long long)(__atomic_load_n(&ticket_next, __ATOMIC_RELAXED) - __atomic_load_n(&ticket_serving, __ATOMIC_RELAXED) - 1));
    }
    return NULL;
}
static void hold_sampler_launch(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, hold_sampler, NULL)) pthread_detach(thread);
}
static void hold_mark(int held)
{
    if (!hold_sampler_on()) return;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, hold_sampler_launch);
    __atomic_store_n(&machine_hold_since, held ? now_ns() : 0, __ATOMIC_RELEASE);
}

static void machine_acquire(void)
{
    pthread_mutex_lock(&mlock);
    uint64_t ticket;
    if (t_urgent && parked_ticket) { ticket = parked_ticket - 1u; parked_ticket = 0; }   /* still ahead of serving */
    else ticket = ticket_next++;
    uint64_t waited = 0;
    int borrowed = 0;
    if (ticket != ticket_serving) {
        uint64_t start = now_ns();
        borrowed = machine_wait_turn_locked(ticket);
        uint64_t elapsed = now_ns() - start;
        waited = elapsed;
        t_lock_wait_ns += elapsed;
#ifdef RECOMP_TRACE
        if (elapsed > 200000000u)
            void_log("[machine-wait] tid=%u caller=%08X ticket=%llu serving=%llu holder=%u wait_ms=%llu",
                void_thread_id(), g_cur_func, (unsigned long long)ticket, (unsigned long long)ticket_serving,
                machine_holder, (unsigned long long)(elapsed / 1000000u));
#endif
    }
    machine_holder = void_thread_id();
    t_held_since = now_ns();
    hold_mark(1);
    if (machine_stats_on()) { machine_stat_add(0, waited, 1, 0); t_hold_start = now_ns(); }
    pthread_mutex_unlock(&mlock);
    t_machine_owned = 1;
    if (borrowed) { t_borrowing = 1; return; }   /* the lender's thread placement and GL context stay */
    /* Android may reset affinity when the app changes lifecycle state. */
    pin_guest_thread();
    if (d3d_gl_thread_acquire) d3d_gl_thread_acquire();
}

static void machine_release(void)
{
    if (t_borrowing) {
        t_borrowing = 0;
        t_machine_owned = 0;
        pthread_mutex_lock(&mlock);
        machine_holder = loan_lender;
        loan_state = LOAN_OFFERED;              /* the lender still waits: the next render may borrow again */
        pthread_cond_broadcast(&mcond);
        pthread_mutex_unlock(&mlock);
        return;
    }
    if (d3d_gl_thread_release) d3d_gl_thread_release();
#ifdef RECOMP_TRACE
    if (t_self) {
        t_self->trace_index = g_enter_idx;
        for (uint32_t i = 0; i < 256; ++i)
            t_self->trace[i] = g_enter_trace[(g_enter_idx - 256 + i) & 1023];
    }
#endif
    t_machine_owned = 0;
    pthread_mutex_lock(&mlock);
    if (machine_stats_on()) machine_stat_add(now_ns() - t_hold_start, 0, 0, 0);
    machine_holder = 0;
    serve_next_locked();
    pthread_mutex_unlock(&mlock);
}

#ifdef RECOMP_TRACE
#define DEBUG_SAMPLE_COUNT 256
typedef struct __attribute__((aligned(8))) { uint32_t tid, func; } debug_sample;
static debug_sample debug_samples[DEBUG_SAMPLE_COUNT];
static uint32_t debug_sample_index;

static void *debug_sample_worker(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec delay = { 0, 20000000 };
        nanosleep(&delay, NULL);
        uint32_t slot = __atomic_fetch_add(&debug_sample_index, 1, __ATOMIC_RELAXED) & (DEBUG_SAMPLE_COUNT - 1);
        debug_sample sample = { __atomic_load_n(&machine_holder, __ATOMIC_RELAXED), __atomic_load_n(&g_cur_func, __ATOMIC_RELAXED) };
        __atomic_store(&debug_samples[slot], &sample, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void debug_sample_launch(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, debug_sample_worker, NULL)) pthread_detach(thread);
}

static void debug_sample_dump(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, debug_sample_launch);
    uint32_t end = __atomic_load_n(&debug_sample_index, __ATOMIC_ACQUIRE);
    uint32_t begin = end > DEBUG_SAMPLE_COUNT ? end - DEBUG_SAMPLE_COUNT : 0;
    uint32_t total = 0;
    struct { uint32_t tid, func, count; } top[6] = { { 0, 0, 0 } };
    for (uint32_t i = begin; i < end; ++i) {
        debug_sample sample;
        __atomic_load(&debug_samples[i & (DEBUG_SAMPLE_COUNT - 1)], &sample, __ATOMIC_ACQUIRE);
        if (!sample.tid) continue;
        ++total;
        uint32_t previous;
        for (previous = begin; previous < i; ++previous) {
            debug_sample old;
            __atomic_load(&debug_samples[previous & (DEBUG_SAMPLE_COUNT - 1)], &old, __ATOMIC_ACQUIRE);
            if (old.tid == sample.tid && old.func == sample.func) break;
        }
        if (previous != i) continue;
        uint32_t count = 1;
        for (uint32_t j = i + 1; j < end; ++j) {
            debug_sample next;
            __atomic_load(&debug_samples[j & (DEBUG_SAMPLE_COUNT - 1)], &next, __ATOMIC_ACQUIRE);
            if (next.tid == sample.tid && next.func == sample.func) ++count;
        }
        for (unsigned rank = 0; rank < 6; ++rank) {
            if (count <= top[rank].count) continue;
            for (unsigned move = 5; move > rank; --move) top[move] = top[move - 1];
            top[rank] = (typeof(top[0])) { sample.tid, sample.func, count };
            break;
        }
    }
    for (unsigned rank = 0; rank < 6 && top[rank].count; ++rank)
        void_log("[thread-hot] tid=%u guest=%08X samples=%u/%u window_ms=%u",
            top[rank].tid, top[rank].func, top[rank].count, total, (end - begin) * 20);
}
#endif

void void_debug_thread_snapshot(void)
{
#ifdef RECOMP_CALL_DIAG
    pthread_mutex_lock(&threads_lock);
    void_log("[thread-snapshot] begin");
    for (void_thread *t = all_threads; t; t = t->next_thread) {
        const char *current_name = void_thunk_name(t->regs.cur);
        uint32_t guest = machine_holder == t->id ? g_cur_func : 0;
        void_log("[thread-state] id=%u host_tid=%d start=%08X param=%08X depth=%d suspended=%d cur=%08X%s%s guest=%08X esp=%08X ebp=%08X trace_index=%u",
            t->id, t->native_tid, t->start, t->param, t->depth,
            t->suspended, t->regs.cur, current_name ? ":" : "", current_name ? current_name : "", guest,
            t->regs.esp, t->regs.ebp, t->trace_index);
        char line[1024];
        int used = snprintf(line, sizeof line, "[thread-trace] id=%u", t->id);
        for (uint32_t i = 192; i < 256 && used < (int)sizeof line - 12; ++i)
            used += snprintf(line + used, sizeof line - (size_t)used, " %08X", t->trace[i]);
        void_log("%s", line);
        uint32_t frame = t->regs.ebp;
        for (unsigned depth = 0; depth < 24 && frame >= t->stack_lo && frame + 8 <= t->stack_hi && !(frame & 3); ++depth) {
            uint32_t next = MEM32(frame), ret = MEM32(frame + 4);
            const char *name = void_thunk_name(ret);
            void_log("[thread-frame] id=%u depth=%u ebp=%08X next=%08X return=%08X%s%s",
                t->id, depth, frame, next, ret, name ? ":" : "", name ? name : "");
            if (next <= frame || next >= t->stack_hi) break;
            frame = next;
        }
    }
    void_log("[thread-snapshot] end");
    pthread_mutex_unlock(&threads_lock);
#ifdef RECOMP_TRACE
    debug_sample_dump();
#endif
#endif
}

static int machine_contended(void)
{
    /* Racy read is fine: a missed waiter is picked up at the next edge. */
    return __atomic_load_n(&ticket_next, __ATOMIC_RELAXED) - __atomic_load_n(&ticket_serving, __ATOMIC_RELAXED) > 1;
}

static uint32_t make_peb(void)
{
    if (peb_va) return peb_va;
    peb_va = void_galloc(0x1000);
    MEM8(peb_va + 2) = 0;                          /* BeingDebugged */
    MEM32(peb_va + 8) = void_module_exe_base();    /* ImageBaseAddress */
    MEM32(peb_va + 0x18) = void_process_heap();    /* ProcessHeap */
    MEM32(peb_va + 0x64) = void_guest_cpu_count();  /* NumberOfProcessors */
    MEM32(peb_va + 0xA4) = 5;                      /* OSMajorVersion */
    MEM32(peb_va + 0xA8) = 1;                      /* OSMinorVersion */
    MEM16(peb_va + 0xAC) = 2600;                   /* OSBuildNumber */
    MEM32(peb_va + 0xB0) = 2;                      /* OSPlatformId: VER_PLATFORM_WIN32_NT */
    return peb_va;
}

static void_thread *thread_new(uint32_t guest_stack)
{
    void_thread *t = (void_thread *)calloc(1, sizeof *t);
    if (!t) void_fatal("thread allocation failed");
    if (pthread_cond_init(&t->machine_cond, NULL) || pthread_cond_init(&t->object_cond, NULL) ||
        pthread_cond_init(&t->cs_cond_self, NULL))
        void_fatal("thread wake condition initialization failed");
    if (!guest_stack) guest_stack = GUEST_STACK_DEFAULT;
    guest_stack = (guest_stack + 0xFFFFu) & ~0xFFFFu;
    uint32_t lo = void_vm_alloc(0, guest_stack, 0x3000u, 0x04u);
    uint32_t tib = void_vm_alloc(0, 0x1000, 0x3000u, 0x04u);
    if (!lo || !tib) void_fatal("guest stack/TIB allocation failed");
    pthread_mutex_lock(&mlock);
    t->id = next_thread_id;
    next_thread_id += 4;
    pthread_mutex_unlock(&mlock);
    t->stack_lo = lo;
    t->stack_hi = lo + guest_stack;
    t->tib = tib;
    MEM32(tib + 0x00) = 0xFFFFFFFFu;        /* ExceptionList: end of chain */
    MEM32(tib + 0x04) = t->stack_hi;        /* StackBase */
    MEM32(tib + 0x08) = t->stack_lo;        /* StackLimit */
    MEM32(tib + 0x18) = tib;                /* Self */
    MEM32(tib + 0x20) = 0x100;              /* ClientId.UniqueProcess */
    MEM32(tib + 0x24) = t->id;              /* ClientId.UniqueThread */
    MEM32(tib + 0x30) = make_peb();         /* ProcessEnvironmentBlock */
    /* The bridge-style headroom native32 kept above the first frame. */
    t->regs.esp = t->stack_hi - 0x100;
    t->regs.fs = tib;
    t->regs.fpu_cw = 0x027F;
    t->regs.fp_round_mask = t->regs.fp_store_mask = ~0ull;   /* fp_cw_masks(0x027F) */
    t->regs.fpu_cmp = 1;
    t->regs.mxcsr = 0x1F80;
    pthread_mutex_lock(&threads_lock);
    pthread_once(&cpu_model_once, cpu_model_init);
    t->affinity_mask = guest_process_mask;
    select_virtual_cpu(t);
    tls_thread_init(t);
    t->next_thread = all_threads;
    all_threads = t;
    pthread_mutex_unlock(&threads_lock);
    return t;
}

/* TlsAlloc and TlsFree zero the slot in every thread, as Windows does: a
 * module that frees its index and one that allocates it next must not see a
 * stale value (the MSVC CRT keeps a function pointer in its slot). */
static void tls_clear_everywhere(uint32_t index)
{
    pthread_mutex_lock(&threads_lock);
    for (void_thread *t = all_threads; t; t = t->next_thread) {
        t->tls[index] = 0;
        if (index < 64) MEM32(t->tib + 0xE10 + 4 * index) = 0;
    }
    pthread_mutex_unlock(&threads_lock);
}

void_thread *void_thread_self(void)
{
    if (!t_self) {
        /* A host thread entering the guest for the first time (the launcher
         * thread, a timer or audio callback) becomes a guest thread. */
        t_self = thread_new(GUEST_STACK_DEFAULT);
        t_self->object = void_object_new(VOID_OBJ_THREAD, 0);
        t_self->object->data = t_self;
        t_self->pt = pthread_self();
        t_self->native_tid = gettid();
        pin_guest_thread();
    }
    return t_self;
}

/* The host thread ids of the guest threads alive now (the ADPF hint session of adpf.c covers every thread
 * that can hold the machine); returns how many were written. */
int void_guest_thread_ids(int32_t *out, int max)
{
    int n = 0;
    pid_t self = getpid();
    pthread_mutex_lock(&threads_lock);
    for (void_thread *t = all_threads; t && n < max; t = t->next_thread)
        if (t->native_tid > 0 && syscall(SYS_tgkill, self, t->native_tid, 0) == 0) out[n++] = (int32_t)t->native_tid;
    pthread_mutex_unlock(&threads_lock);
    return n;
}

static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static void suspend_checkpoint(void);

void void_machine_enter(void)
{
    void_thread *t = void_thread_self();
    if (t->depth++ == 0) {
        machine_acquire();
        select_virtual_cpu(t);
        void_regs_load(&t->regs);
    }
    suspend_checkpoint();
}

uint32_t void_guest_cpu_id(void)
{
    void_thread *thread = void_thread_self();
    select_virtual_cpu(thread);
    return thread->virtual_cpu;
}

void void_machine_leave(void)
{
    void_thread *t = t_self;
    if (!t || t->depth <= 0) void_fatal("machine leave without a claim");
    if (--t->depth == 0) {
        void_regs_save(&t->regs);
        machine_release();
    }
}

void void_blocking_begin(void)
{
    void_thread *t = t_self;
    if (!t || t->depth <= 0) void_fatal("blocking call outside the guest machine");
    if (t->start == 0x10E4B2C6u && !t->first_wait) {
        pthread_mutex_lock(&wait_lock);
        t->first_wait = 1;
        pthread_cond_broadcast(&wait_cond);
        pthread_mutex_unlock(&wait_lock);
    }
    void_regs_save(&t->regs);
    machine_release();
}

void void_blocking_end(void)
{
    void_thread *t = t_self;
    machine_acquire();
    void_regs_load(&t->regs);
}

static void suspend_checkpoint(void)
{
    void_thread *t = t_self;
    if (!t || !t->suspended) return;
    int owned = t_machine_owned;
    if (owned) {
        void_regs_save(&t->regs);
        machine_release();
    }
    pthread_mutex_lock(&wait_lock);
    while (t->suspended) pthread_cond_wait(&wait_cond, &wait_lock);
    pthread_mutex_unlock(&wait_lock);
    if (owned) {
        machine_acquire();
        void_regs_load(&t->regs);
    }
}

static void yield_hook(void)
{
    void void_bioshock_poll_main_navigation(void);
    suspend_checkpoint();
    if (t_self && t_self->id == 260) void_bioshock_poll_main_navigation();
    if (__builtin_expect(machine_stats_on(), 0) && t_self &&
        __atomic_load_n(&machine_holder, __ATOMIC_RELAXED) != t_self->id)
        __atomic_add_fetch(&machine_foreign_yields, 1, __ATOMIC_RELAXED);
    if (!machine_contended()) return;
    if (t_urgent && now_ns() - t_held_since < URGENT_SLICE_NS) return;
    void_thread *t = t_self;
    if (!t || t->depth <= 0) void_fatal("yield outside the guest machine");
    if (t_borrowing) {
        /* A loan is not a turn of the queue: return it, then wait for the parked ticket. */
        void_regs_save(&t->regs);
        machine_release();
        machine_acquire();
        void_regs_load(&t->regs);
        return;
    }
    void_regs_save(&t->regs);
    pthread_mutex_lock(&mlock);
    /* Reserve our return before another thread can run. Releasing first lets
     * the next owner mistake a preempted yielding thread for no contender. */
    uint64_t ticket = ticket_next++;
    uint64_t start = now_ns();
    int stats = machine_stats_on();
    if (stats) machine_stat_add(start - t_hold_start, 0, 0, 1);
    if (d3d_gl_thread_release) d3d_gl_thread_release();
    t_machine_owned = 0;
    machine_holder = 0;
    serve_next_locked();
    int borrowed = machine_wait_turn_locked(ticket);
    if (stats) { uint64_t resumed = now_ns(); machine_stat_add(0, resumed - start, 1, 0); t_hold_start = resumed; }
    machine_holder = t->id;
    t_held_since = now_ns();
    hold_mark(1);
    t_machine_owned = 1;
    t_lock_wait_ns += now_ns() - start;
    pthread_mutex_unlock(&mlock);
    if (borrowed) { t_borrowing = 1; void_regs_load(&t->regs); return; }
    pin_guest_thread();
    if (d3d_gl_thread_acquire) d3d_gl_thread_acquire();
    void_regs_load(&t->regs);
}

/* RECOMP_SPIN_WAIT back edges: the awaited guest thread needs the machine.
 * Hand it over when a thread waits for it; otherwise let a runnable thread on
 * this core run -- a woken worker shares the prime core and must first reach
 * its ticket, which it cannot do while this thread spins there. */
static void spin_hook(void)
{
    if (machine_contended()) yield_hook();
    else sched_yield();
}

/* A native call boundary that may block (DXVK can wait for the GPU while the
 * machine is held) hands the machine over first when an FMOD feeder waits:
 * its 93 ms output ring replays old PCM behind long main-thread holds. Other
 * waiters keep the loop/blocking hand-over, so ordinary calls cost one load. */
void void_machine_offer_audio(void)
{
    if (!__atomic_load_n(&audio_waiting, __ATOMIC_RELAXED)) return;
    void_thread *t = t_self;
    if (!t || t->depth <= 0 || !t_machine_owned) return;
    yield_hook();
}

/* See "Loans" above. Call around a native wait that touches no guest state. */
void void_machine_lend_begin(void)
{
    void_thread *t = t_self;
    if (!urgent_enabled() || !t || t->depth <= 0 || !t_machine_owned || t_lending || t_borrowing) return;
    void_regs_save(&t->regs);
    pthread_mutex_lock(&mlock);
    loan_state = LOAN_OFFERED;
    loan_lender = t->id;
    pthread_mutex_unlock(&mlock);
    t_lending = 1;
}

void void_machine_lend_end(void)
{
    if (!t_lending) return;
    t_lending = 0;
    void_thread *t = t_self;
    pthread_mutex_lock(&mlock);
    while (loan_state == LOAN_TAKEN) pthread_cond_wait(&mcond, &mlock);
    loan_state = LOAN_NONE;
    machine_holder = t->id;
    pthread_mutex_unlock(&mlock);
    void_regs_load(&t->regs);
}

uint32_t void_thread_id(void) { return void_thread_self()->id; }
uint32_t void_get_last_error(void) { return void_thread_self()->last_error; }
void void_set_last_error(uint32_t e) { void_thread_self()->last_error = e; MEM32(void_thread_self()->tib + 0x34) = e; }

/* ---------------------------------------------------- guest call helpers */
uint32_t void_call_guest(uint32_t va, int nargs, const uint32_t *args, int stdcall_pops)
{
    recomp_func_t fn = void_resolve(va);
    uint32_t entry_esp = g_esp, caller = g_cur_func;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
    for (int i = nargs; i > 0; --i) PUSH32(g_esp, args[i - 1]);
    PUSH32(g_esp, RECOMP_RETADDR);
    fn();
    uint32_t expected = stdcall_pops ? entry_esp : entry_esp - 4u * (uint32_t)nargs;
    /* A thread root has no guest caller; old FMOD workers return with plain RET. */
    int thread_cdecl = stdcall_pops == 2 && nargs == 1 && g_esp == entry_esp - 4u;
    if (g_esp != expected && !thread_cdecl) {
        void_log("[void] guest call %08X: ESP %08X, expected %08X (args %d, %s)", va, g_esp, expected, nargs,
            stdcall_pops ? "stdcall" : "cdecl");
        void_lift_trap(va, "guest callback stack imbalance");
    }
    g_esp = entry_esp;
    g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
    g_cur_func = caller;
    return g_eax;
}

uint32_t void_call_guest_this(uint32_t va, uint32_t this_ptr, int nargs, const uint32_t *args)
{
    g_ecx = this_ptr;
    return void_call_guest(va, nargs, args, 1);
}

/* --------------------------------------------------------------- handles */
#define MAX_HANDLES 65536
static void_object *handles[MAX_HANDLES];
static uint32_t handle_access[MAX_HANDLES]; /* Named event opens may restrict rights. */
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t handle_hint;

void_object *void_object_new(int kind, size_t extra)
{
    void_object *o = (void_object *)calloc(1, sizeof *o + extra);
    if (!o) void_fatal("object allocation failed");
    o->kind = kind;
    o->refs = 1;
    if (extra) o->data = (void *)(o + 1);
    return o;
}

void void_object_release(void_object *o)
{
    if (!o) return;
    pthread_mutex_lock(&handle_lock);
    int last = --o->refs == 0;
    pthread_mutex_unlock(&handle_lock);
    if (last) {
        if (o->destroy) o->destroy(o);
        free(o);
    }
}

uint32_t void_handle_new(void_object *o)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t n = 0; n < MAX_HANDLES; ++n) {
        uint32_t i = (handle_hint + n) % MAX_HANDLES;
        if (!i) continue;
        if (!handles[i]) {
            handles[i] = o;
            handle_access[i] = UINT32_MAX;
            handle_hint = i + 1;
            pthread_mutex_unlock(&handle_lock);
            return 0x1000u + 4u * i;
        }
    }
    pthread_mutex_unlock(&handle_lock);
    void_fatal("handle table full");
}

static uint32_t handle_index(uint32_t h)
{
    if (h < 0x1000u || (h & 3u)) return 0;
    uint32_t i = (h - 0x1000u) / 4u;
    return i < MAX_HANDLES ? i : 0;
}

void_object *void_handle_get(uint32_t h, int kind)
{
    if (h == 0xFFFFFFFEu) {           /* GetCurrentThread pseudo-handle */
        void_object *o = void_thread_self()->object;
        return (kind == VOID_OBJ_NONE || kind == VOID_OBJ_THREAD) ? o : NULL;
    }
    uint32_t i = handle_index(h);
    pthread_mutex_lock(&handle_lock);
    void_object *o = i ? handles[i] : NULL;
    pthread_mutex_unlock(&handle_lock);
    if (o && kind != VOID_OBJ_NONE && o->kind != kind) return NULL;
    return o;
}

int void_handle_close(uint32_t h)
{
    uint32_t i = handle_index(h);
    pthread_mutex_lock(&handle_lock);
    void_object *o = i ? handles[i] : NULL;
    if (o) handles[i] = NULL;
    pthread_mutex_unlock(&handle_lock);
    if (!o) return 0;
    void_object_release(o);
    return 1;
}

uint32_t void_handle_dup(uint32_t h)
{
    void_object *o = void_handle_get(h, VOID_OBJ_NONE);
    if (!o) return 0;
    pthread_mutex_lock(&handle_lock);
    ++o->refs;
    pthread_mutex_unlock(&handle_lock);
    uint32_t duplicate = void_handle_new(o);
    pthread_mutex_lock(&handle_lock);
    handle_access[handle_index(duplicate)] = handle_access[handle_index(h)];
    pthread_mutex_unlock(&handle_lock);
    return duplicate;
}

static int event_handle_allows(uint32_t handle, uint32_t access)
{
    uint32_t index = handle_index(handle);
    pthread_mutex_lock(&handle_lock);
    int allowed = index && handles[index] && (handle_access[index] & access) == access;
    pthread_mutex_unlock(&handle_lock);
    return allowed;
}

/* ---------------------------------------------------------------- waiting */
/* Threads blocked in wait_core, each with the objects it waits for. wait_cond
 * itself remains for the few direct predicate waits (thread start/resume,
 * FMOD's first metered wait) and for the legacy A/B policy. */
typedef struct object_waiter {
    struct object_waiter *next;
    pthread_cond_t *cond;
    void_object *const *objects;
    int count;
    int extra;                 /* also wakes for message-queue changes */
} object_waiter;
static object_waiter *object_waiters;   /* guarded by wait_lock */

/* wait_lock held: `o` changed state; wake the threads that wait for it. */
static void wake_object_waiters_locked(const void_object *o)
{
    if (legacy_wake()) { pthread_cond_broadcast(&wait_cond); return; }
    for (object_waiter *waiter = object_waiters; waiter; waiter = waiter->next) {
        for (int i = 0; i < waiter->count; ++i) {
            if (waiter->objects[i] == o) { pthread_cond_signal(waiter->cond); break; }
        }
    }
}

/* wait_lock held: a direct predicate changed (thread start/exit/resume). */
static void wake_predicate_waiters_locked(void)
{
    pthread_cond_broadcast(&wait_cond);
}

static int object_ready(void_object *o, uint32_t tid)
{
    switch (o->kind) {
    case VOID_OBJ_EVENT: case VOID_OBJ_THREAD: case VOID_OBJ_PROCESS: case VOID_OBJ_TIMER:
        return o->signaled;
    case VOID_OBJ_SEMAPHORE:
        return o->count > 0;
    case VOID_OBJ_MUTEX:
        return o->owner == 0 || o->owner == tid;
    default:
        return 1;                     /* files and others: always signaled */
    }
}

static void object_consume(void_object *o, uint32_t tid)
{
    switch (o->kind) {
    case VOID_OBJ_EVENT: case VOID_OBJ_TIMER:
        if (!o->manual_reset) {
            o->signaled = 0;
            if (o->pulse_claimed < o->pulse_generation) ++o->pulse_claimed;
        }
        break;
    case VOID_OBJ_SEMAPHORE:
        --o->count;
        break;
    case VOID_OBJ_MUTEX:
        o->owner = tid;
        ++o->recursion;
        break;
    default:
        break;
    }
}

void void_object_signal(void_object *o)
{
    pthread_mutex_lock(&wait_lock);
    o->signaled = 1;
    wake_object_waiters_locked(o);
    pthread_mutex_unlock(&wait_lock);
}

/* A posted message: only MsgWaitForMultipleObjects-style waits (with an extra
 * wake condition) can be satisfied by it. */
void void_wait_broadcast(void)
{
    pthread_mutex_lock(&wait_lock);
    if (legacy_wake()) pthread_cond_broadcast(&wait_cond);
    else for (object_waiter *waiter = object_waiters; waiter; waiter = waiter->next)
        if (waiter->extra) pthread_cond_signal(waiter->cond);
    pthread_mutex_unlock(&wait_lock);
}

/* Optional extra wake condition (MsgWaitForMultipleObjects: queued input). */
typedef int (*void_wait_extra)(void *);

static uint32_t wait_core(int count, void_object **objects, int wait_all, uint32_t timeout,
    void_wait_extra extra, void *extra_arg)
{
    uint32_t tid = void_thread_self()->id;
    struct timespec deadline;
    if (timeout != INFINITE_) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000u;
        deadline.tv_nsec += (long)(timeout % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }
    /* The machine is released only when the wait really has to sleep: a wait
     * satisfied at once returns as on Windows, without handing the machine to
     * the next ticket (which on the shared prime core preempted this thread
     * for that holder's whole slice: the RMX3851 main thread spent 56 % of its
     * time runnable outside the machine). FMOD's first wait keeps the old
     * order because CreateThread waits for it (first_wait); the QA flag
     * BIOSHOCK_WAIT_RELEASE_FIRST=1 restores the old order everywhere. */
    int blocking = timeout != 0, released = 0;
    void_thread *waiter_thread = void_thread_self();
    if (blocking && (wait_release_first() || (waiter_thread->start == 0x10E4B2C6u && !waiter_thread->first_wait))) {
        void_blocking_begin();
        released = 1;
    }
    pthread_mutex_lock(&wait_lock);
    object_waiter self = { object_waiters, legacy_wake() || !t_self ? &wait_cond : &t_self->object_cond,
                           objects, count, extra != NULL };
    object_waiters = &self;
    uint64_t pulses[64];
    for (int i = 0; i < count; ++i)
        pulses[i] = objects[i]->kind == VOID_OBJ_EVENT ? objects[i]->pulse_generation : 0;
    uint32_t result = WAIT_TIMEOUT_;
    for (;;) {
        if (wait_all) {
            int all = 1;
            for (int i = 0; i < count; ++i) {
                int pulsed = objects[i]->kind == VOID_OBJ_EVENT && objects[i]->pulse_generation != pulses[i] &&
                (objects[i]->manual_reset || objects[i]->pulse_claimed < objects[i]->pulse_generation);
                if (!pulsed && !object_ready(objects[i], tid)) { all = 0; break; }
            }
            if (all) {
                for (int i = 0; i < count; ++i) object_consume(objects[i], tid);
                result = WAIT_OBJECT_0_;
                break;
            }
        } else {
            int hit = -1;
            for (int i = 0; i < count; ++i) {
                int pulsed = objects[i]->kind == VOID_OBJ_EVENT && objects[i]->pulse_generation != pulses[i] &&
                (objects[i]->manual_reset || objects[i]->pulse_claimed < objects[i]->pulse_generation);
                if (pulsed || object_ready(objects[i], tid)) { hit = i; break; }
            }
            if (hit >= 0) {
                object_consume(objects[hit], tid);
                result = WAIT_OBJECT_0_ + (uint32_t)hit;
                break;
            }
        }
        if (extra && extra(extra_arg)) { result = WAIT_OBJECT_0_ + (uint32_t)count; break; }
        if (!timeout) break;
        if (!released) {
            /* About to sleep: release the machine first. void_blocking_begin may
             * itself take wait_lock, so drop it; the registration stays and the
             * conditions are checked again before sleeping. */
            pthread_mutex_unlock(&wait_lock);
            void_blocking_begin();
            released = 1;
            pthread_mutex_lock(&wait_lock);
            continue;
        }
        int rc = timeout == INFINITE_ ? pthread_cond_wait(self.cond, &wait_lock)
                                      : pthread_cond_timedwait(self.cond, &wait_lock, &deadline);
        if (rc == ETIMEDOUT) {
            result = WAIT_TIMEOUT_;
            /* One last check: a signal may have raced the timeout. */
            timeout = 0;
        }
    }
    for (object_waiter **link = &object_waiters; *link; link = &(*link)->next)
        if (*link == &self) { *link = self.next; break; }
    pthread_mutex_unlock(&wait_lock);
    if (released) void_blocking_end();
    return result;
}

static uint32_t unreal_wait_handles[32], unreal_wait_signals[32], unreal_wait_count;

uint32_t void_wait(int count, const uint32_t *hs, int wait_all, uint32_t timeout)
{
    void_object *objects[64];
    if (count <= 0 || count > 64) { void_set_last_error(ERROR_INVALID_PARAMETER_); return WAIT_FAILED_; }
    uint32_t caller = g_cur_func;
    int trace = t_self && t_self->start == 0x180124D0u ? 1 : caller == 0x1800C140u ? 2 : caller == 0x10953D60u ? 3 : 0;
    static unsigned trace_counts[4];
    if (trace && trace_counts[trace]++ < 40)
        void_log("[bink-wait] begin kind=%d tid=%u caller=%08X handle=%08X timeout=%u", trace, void_thread_id(), caller, hs[0], timeout);
    if (trace == 3) {
        uint32_t i = 0;
        while (i < unreal_wait_count && unreal_wait_handles[i] != hs[0]) ++i;
        if (i == unreal_wait_count && unreal_wait_count < 32) unreal_wait_handles[unreal_wait_count++] = hs[0];
        VLOG("[unreal-event] wait tid=%u handle=%08X slot=%u", void_thread_id(), hs[0], i);
    }
    for (int i = 0; i < count; ++i) {
        objects[i] = void_handle_get(hs[i], VOID_OBJ_NONE);
        if (!objects[i]) {
            void_log("[void] wait on invalid handle %08X (caller %08X)", hs[i], g_cur_func);
            void_set_last_error(ERROR_INVALID_HANDLE_);
            return WAIT_FAILED_;
        }
        if (objects[i]->kind == VOID_OBJ_EVENT && !event_handle_allows(hs[i], 0x100000u)) {
            void_set_last_error(ERROR_ACCESS_DENIED_); return WAIT_FAILED_;
        }
    }
    if (caller == 0x1800CA20u) {
        static unsigned pause_waits;
        if (pause_waits++ < 40)
            void_log("[bink-pause] wait tid=%u handle=%08X kind=%d owner=%u recursion=%d signaled=%d",
                void_thread_id(), hs[0], objects[0]->kind, objects[0]->owner, objects[0]->recursion, objects[0]->signaled);
    }
    uint32_t result = wait_core(count, objects, wait_all, timeout, NULL, NULL);
    if (trace && trace_counts[trace] <= 40)
        void_log("[bink-wait] end kind=%d tid=%u caller=%08X handle=%08X result=%08X", trace, void_thread_id(), caller, hs[0], result);
    return result;
}

uint32_t void_wait_extra_cond(int count, const uint32_t *hs, uint32_t timeout, int (*extra)(void *), void *arg)
{
    void_object *objects[64];
    if (count < 0 || count > 63) { void_set_last_error(ERROR_INVALID_PARAMETER_); return WAIT_FAILED_; }
    for (int i = 0; i < count; ++i) {
        objects[i] = void_handle_get(hs[i], VOID_OBJ_NONE);
        if (!objects[i]) { void_set_last_error(ERROR_INVALID_HANDLE_); return WAIT_FAILED_; }
        if (objects[i]->kind == VOID_OBJ_EVENT && !event_handle_allows(hs[i], 0x100000u)) {
            void_set_last_error(ERROR_ACCESS_DENIED_); return WAIT_FAILED_;
        }
    }
    return wait_core(count, objects, 0, timeout, extra, arg);
}

/* ------------------------------------------------------------- kernel32 */
#define K32 "kernel32.dll"

WINAPI_EXPORT(K32, GetLastError, 0) { return void_get_last_error(); }
WINAPI_EXPORT(K32, SetLastError, 1) { void_set_last_error(ARG(0)); return 0; }
WINAPI_EXPORT(K32, GetCurrentThreadId, 0) { return void_thread_id(); }
WINAPI_EXPORT(K32, GetCurrentThread, 0) { return 0xFFFFFFFEu; }
WINAPI_EXPORT(K32, GetCurrentProcess, 0) { return 0xFFFFFFFFu; }
WINAPI_EXPORT(K32, GetCurrentProcessId, 0) { return 0x100u; }

WINAPI_EXPORT(K32, TlsAlloc, 0)
{
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 0; i < TLS_SLOTS; ++i) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            tls_clear_everywhere(i);
            return i;
        }
    }
    pthread_mutex_unlock(&tls_lock);
    return 0xFFFFFFFFu;              /* TLS_OUT_OF_INDEXES */
}

WINAPI_EXPORT(K32, TlsFree, 1)
{
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS || !tls_used[i]) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    tls_clear_everywhere(i);
    tls_used[i] = 0;
    return 1;
}

WINAPI_EXPORT(K32, TlsGetValue, 1)
{
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    void_set_last_error(0);
    return void_thread_self()->tls[i];
}

WINAPI_EXPORT(K32, TlsSetValue, 2)
{
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    void_thread *t = void_thread_self();
    t->tls[i] = ARG(1);
    if (i < 64) MEM32(t->tib + 0xE10 + 4 * i) = ARG(1);
    return 1;
}

/* Critical sections live in guest memory; contention waits on a generation
 * counter so an unlock between the owner check and the wait is never lost.
 * cs_waiters counts threads parked on it: a waiter registers while it still
 * holds the machine and only then lets it go, and a release runs on the
 * machine too, so a release that sees no waiters skips the wake-up (the
 * game's CRT takes a few thousand uncontended sections a frame). */
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cs_cond = PTHREAD_COND_INITIALIZER;
static uint64_t cs_generation;
static uint32_t cs_waiters;
/* Contended EnterCriticalSection waiters, each woken only by a release of its
 * own critical section (cs_cond/cs_generation remain for the legacy policy). */
typedef struct cs_waiter {
    struct cs_waiter *next;
    uint32_t cs;
    pthread_cond_t *cond;
    int released;
} cs_waiter;
static cs_waiter *cs_waiter_list;   /* guarded by cs_lock */

static uint32_t cs_effective_spin(uint32_t spin)
{
    return sysconf(_SC_NPROCESSORS_CONF) > 1 ? spin : 0;
}

static void cs_init(uint32_t cs, uint32_t spin)
{
    MEM32(cs + 0) = 0xFFFFFFFFu;     /* DebugInfo: none, as Windows 8+ leaves it */
    MEM32(cs + 4) = 0xFFFFFFFFu;     /* LockCount: free */
    MEM32(cs + 8) = 0;               /* RecursionCount */
    MEM32(cs + 12) = 0;              /* OwningThread */
    MEM32(cs + 16) = 0;              /* LockSemaphore */
    MEM32(cs + 20) = cs_effective_spin(spin); /* SpinCount */
}

WINAPI_EXPORT(K32, InitializeCriticalSection, 1) { cs_init(ARG(0), 0); return 0; }
WINAPI_EXPORT(K32, InitializeCriticalSectionAndSpinCount, 2) { cs_init(ARG(0), ARG(1)); return 1; }
WINAPI_EXPORT(K32, SetCriticalSectionSpinCount, 2)
{
    uint32_t cs = ARG(0);
    if (!void_vm_writable(cs, 24)) void_lift_trap(cs, "invalid critical-section storage");
    uint32_t previous = MEM32(cs + 20);
    /* Only one guest runs at a time; cs_enter parks to let the owner progress. */
    MEM32(cs + 20) = cs_effective_spin(ARG(1));
    return previous;
}
WINAPI_EXPORT(K32, DeleteCriticalSection, 1) { MEM32(ARG(0) + 12) = 0; MEM32(ARG(0) + 8) = 0; return 0; }

static void cs_enter(uint32_t cs)
{
    uint32_t tid = void_thread_id();
    for (;;) {
        uint32_t owner = MEM32(cs + 12);
        if (!owner) {
            MEM32(cs + 12) = tid;
            MEM32(cs + 8) = 1;
            MEM32(cs + 4) = 0;
            return;
        }
        if (owner == tid) {
            MEM32(cs + 8) += 1;
            MEM32(cs + 4) += 1;
            return;
        }
        /* Registered while this thread still holds the machine, so the owner
         * cannot leave before the registration is visible. */
        pthread_mutex_lock(&cs_lock);
        uint64_t seen = cs_generation;
        cs_waiter self = { cs_waiter_list, cs, legacy_wake() || !t_self ? &cs_cond : &t_self->cs_cond_self, 0 };
        cs_waiter_list = &self;
        __atomic_add_fetch(&cs_waiters, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&cs_lock);
        void_blocking_begin();
        pthread_mutex_lock(&cs_lock);
        if (self.cond == &cs_cond) while (cs_generation == seen) pthread_cond_wait(&cs_cond, &cs_lock);
        else while (!self.released) pthread_cond_wait(self.cond, &cs_lock);
        for (cs_waiter **link = &cs_waiter_list; *link; link = &(*link)->next)
            if (*link == &self) { *link = self.next; break; }
        __atomic_sub_fetch(&cs_waiters, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&cs_lock);
        void_blocking_end();
    }
}

static void cs_leave(uint32_t cs)
{
    uint32_t tid = void_thread_id();
    if (MEM32(cs + 12) != tid) {
        void_log("[void] LeaveCriticalSection(%08X) by non-owner %u (owner %u, caller %08X)",
            cs, tid, MEM32(cs + 12), g_cur_func);
        return;
    }
    if (--MEM32(cs + 8) == 0) {
        MEM32(cs + 12) = 0;
        MEM32(cs + 4) = 0xFFFFFFFFu;
        if (__atomic_load_n(&cs_waiters, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&cs_lock);
            ++cs_generation;
            pthread_cond_broadcast(&cs_cond);   /* legacy-policy waiters */
            for (cs_waiter *waiter = cs_waiter_list; waiter; waiter = waiter->next) {
                if (waiter->cs == cs && waiter->cond != &cs_cond) {
                    waiter->released = 1;
                    pthread_cond_signal(waiter->cond);
                }
            }
            pthread_mutex_unlock(&cs_lock);
        }
    } else {
        MEM32(cs + 4) -= 1;
    }
}

WINAPI_EXPORT(K32, EnterCriticalSection, 1) { cs_enter(ARG(0)); return 0; }
WINAPI_EXPORT(K32, LeaveCriticalSection, 1) { cs_leave(ARG(0)); return 0; }
WINAPI_EXPORT(K32, TryEnterCriticalSection, 1)
{
    uint32_t cs = ARG(0), tid = void_thread_id(), owner = MEM32(cs + 12);
    if (owner && owner != tid) return 0;
    cs_enter(cs);
    return 1;
}

/* Interlocked operations run under the machine lock: plain arithmetic is
 * already atomic with respect to every other guest thread. */
WINAPI_EXPORT(K32, InterlockedIncrement, 1) { return ++MEM32(ARG(0)); }
WINAPI_EXPORT(K32, InterlockedDecrement, 1) { return --MEM32(ARG(0)); }
WINAPI_EXPORT(K32, InterlockedExchange, 2) { uint32_t old = MEM32(ARG(0)); MEM32(ARG(0)) = ARG(1); return old; }
WINAPI_EXPORT(K32, InterlockedExchangeAdd, 2) { uint32_t old = MEM32(ARG(0)); MEM32(ARG(0)) = old + ARG(1); return old; }
WINAPI_EXPORT(K32, InterlockedCompareExchange, 3)
{
    uint32_t old = MEM32(ARG(0));
    if (old == ARG(2)) MEM32(ARG(0)) = ARG(1);
    return old;
}

/* Events, mutexes, semaphores. Names are accepted and ignored except for
 * the single-instance mutex the launcher creates, which is process-local. */
uint32_t void_event_create(int manual, int initial)
{
    void_object *o = void_object_new(VOID_OBJ_EVENT, 0);
    o->manual_reset = manual;
    o->signaled = initial;
    return void_handle_new(o);
}

typedef struct named_event {
    char *name;
    void_object *object;
    struct named_event *next;
} named_event;
static named_event *named_events;
static pthread_mutex_t event_names_lock = PTHREAD_MUTEX_INITIALIZER;

static void named_event_destroy(void_object *object)
{
    named_event *entry = object->data;
    pthread_mutex_lock(&event_names_lock);
    named_event **link = &named_events;
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) *link = entry->next;
    pthread_mutex_unlock(&event_names_lock);
    free(entry->name); free(entry);
}

static char *event_name(uint32_t address, int wide, uint32_t *error)
{
    *error = 0;
    if (!address) return NULL;
    unsigned width = wide ? 2 : 1;
    for (uint32_t i = 0; i <= 32767; ++i) {
        uint64_t at = (uint64_t)address + (uint64_t)i * width;
        if (at + width > 0x100000000ull || !void_vm_readable((uint32_t)at, width)) { *error = 998; return NULL; }
        if (wide ? !MEM16((uint32_t)at) : !MEM8((uint32_t)at)) {
            char *name = wide ? void_wstr_utf8(address) : void_astr_utf8(address);
            if (!name) { *error = ERROR_NOT_ENOUGH_MEMORY_; return NULL; }
            /* Local\\X and X identify the same virtual session object. */
            if (!strncmp(name, "Local\\", 6)) memmove(name, name + 6, strlen(name + 6) + 1);
            return name;
        }
    }
    *error = 206; return NULL;
}

static uint32_t event_named_api(int wide, int create)
{
    uint32_t previous_error = void_get_last_error();
    uint32_t error, access = create ? 0x1F0003u : ARG(0);
    char *name = event_name(ARG(create ? 3 : 2), wide, &error);
    if (error) { void_set_last_error(error); return 0; }
    if (!name || !*name) {
        int null_name = !name;
        free(name);
        if (!create) { void_set_last_error(null_name ? ERROR_INVALID_PARAMETER_ : ERROR_INVALID_HANDLE_); return 0; }
        uint32_t event = void_event_create(ARG(1) != 0, ARG(2) != 0);
        void_set_last_error(0); return event;
    }
    if (access == 0x02000000u || access == 0x10000000u) access = 0x1F0003u;
    if (access & ~0x1F0003u) { free(name); void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
    /* shortcut: This namespace contains events owned by this process only.
     * Cross-type collisions and external/global IPC are not modeled; add a
     * shared namespace only when an actual consumer requires those objects. */
    pthread_mutex_lock(&event_names_lock);
    void_object *object = NULL;
    for (named_event *entry = named_events; entry; entry = entry->next) {
        if (strcmp(entry->name, name)) continue;
        pthread_mutex_lock(&handle_lock);
        if (entry->object->refs > 0) { object = entry->object; ++object->refs; }
        pthread_mutex_unlock(&handle_lock);
        if (object) break;
    }
    uint32_t handle;
    if (object) {
        pthread_mutex_unlock(&event_names_lock);
        free(name); handle = void_handle_new(object);
        error = create ? 183u : previous_error; /* ERROR_ALREADY_EXISTS; Open preserves LastError. */
    } else if (!create) {
        pthread_mutex_unlock(&event_names_lock); free(name);
        void_set_last_error(ERROR_FILE_NOT_FOUND_); return 0;
    } else {
        handle = void_event_create(ARG(1) != 0, ARG(2) != 0);
        object = void_handle_get(handle, VOID_OBJ_EVENT);
        named_event *entry = calloc(1, sizeof *entry);
        if (!entry) void_fatal("named event allocation failed");
        entry->name = name; entry->object = object; entry->next = named_events;
        named_events = entry; object->data = entry; object->destroy = named_event_destroy;
        pthread_mutex_unlock(&event_names_lock); error = 0;
    }
    pthread_mutex_lock(&handle_lock);
    handle_access[handle_index(handle)] = access;
    pthread_mutex_unlock(&handle_lock);
    void_set_last_error(error); return handle;
}
WINAPI_EXPORT(K32, CreateEventA, 4) { return event_named_api(0, 1); }
WINAPI_EXPORT(K32, CreateEventW, 4) { return event_named_api(1, 1); }
WINAPI_EXPORT(K32, OpenEventA, 3) { return event_named_api(0, 0); }
WINAPI_EXPORT(K32, OpenEventW, 3) { return event_named_api(1, 0); }

static uint32_t event_op(uint32_t h, int op, int guest_handoff)
{
    void_object *o = void_handle_get(h, VOID_OBJ_EVENT);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (!event_handle_allows(h, 2)) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
    for (uint32_t i = 0; i < unreal_wait_count; ++i) {
        if (unreal_wait_handles[i] == h && unreal_wait_signals[i]++ < 64)
            VLOG("[unreal-event] op=%d tid=%u handle=%08X slot=%u caller=%08X", op, void_thread_id(), h, i, g_cur_func);
    }
    pthread_mutex_lock(&wait_lock);
    int wakes = op != 0 && (op != 1 || !o->signaled);
    if (op == 1) { o->signaled = 1; wake_object_waiters_locked(o); }
    else if (op == 0) {
        if (o->signaled) ++o->pulse_generation;
        o->signaled = 0;
        wake_object_waiters_locked(o);
    }
    else { o->signaled = 0; ++o->pulse_generation; wake_object_waiters_locked(o); }
    pthread_mutex_unlock(&wait_lock);
    /* A notified guest may run before the setter's next instruction. Keep
     * host-internal notifications inside their serialized host operation. */
    if (guest_handoff && wakes && void_machine_is_owned() && machine_contended()) yield_hook();
    return 1;
}

uint32_t void_event_op(uint32_t h, int op) { return event_op(h, op, 0); }

WINAPI_EXPORT(K32, SetEvent, 1) { return event_op(ARG(0), 1, 1); }
WINAPI_EXPORT(K32, ResetEvent, 1) { return void_event_op(ARG(0), 0); }
WINAPI_EXPORT(K32, PulseEvent, 1) { return event_op(ARG(0), 2, 1); }

static uint32_t create_mutex(int initial_owner)
{
    void_object *o = void_object_new(VOID_OBJ_MUTEX, 0);
    if (initial_owner) { o->owner = void_thread_id(); o->recursion = 1; }
    void_set_last_error(0);
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, CreateMutexA, 3) { return create_mutex(ARG(1) != 0); }
WINAPI_EXPORT(K32, CreateMutexW, 3) { return create_mutex(ARG(1) != 0); }
WINAPI_EXPORT(K32, ReleaseMutex, 1)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_MUTEX);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (t_self && t_self->start == 0x180124D0u) {
        static unsigned releases;
        if (releases++ < 64)
            void_log("[bink-mutex] release tid=%u handle=%08X owner=%u recursion=%d", void_thread_id(), ARG(0), o->owner, o->recursion);
    }
    pthread_mutex_lock(&wait_lock);
    if (o->owner != void_thread_id()) { pthread_mutex_unlock(&wait_lock); void_set_last_error(ERROR_NOT_OWNER_); return 0; }
    if (--o->recursion == 0) { o->owner = 0; wake_object_waiters_locked(o); }
    pthread_mutex_unlock(&wait_lock);
    return 1;
}

static uint32_t create_semaphore(int initial, int maximum)
{
    void_object *o = void_object_new(VOID_OBJ_SEMAPHORE, 0);
    o->count = initial;
    o->max_count = maximum;
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, CreateSemaphoreA, 4) { return create_semaphore((int)ARG(1), (int)ARG(2)); }
WINAPI_EXPORT(K32, CreateSemaphoreW, 4) { return create_semaphore((int)ARG(1), (int)ARG(2)); }
WINAPI_EXPORT(K32, ReleaseSemaphore, 3)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_SEMAPHORE);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    pthread_mutex_lock(&wait_lock);
    int previous = o->count;
    if ((int)ARG(1) <= 0 || previous + (int)ARG(1) > o->max_count) {
        pthread_mutex_unlock(&wait_lock);
        void_set_last_error(0x12Au);  /* ERROR_TOO_MANY_POSTS */
        return 0;
    }
    o->count += (int)ARG(1);
    wake_object_waiters_locked(o);
    pthread_mutex_unlock(&wait_lock);
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)previous;
    return 1;
}

WINAPI_EXPORT(K32, WaitForSingleObject, 2)
{
    uint32_t h = ARG(0);
    return void_wait(1, &h, 0, ARG(1));
}

WINAPI_EXPORT(K32, WaitForSingleObjectEx, 3)
{
    uint32_t h = ARG(0);
    return void_wait(1, &h, 0, ARG(1));
}

WINAPI_EXPORT(K32, WaitForMultipleObjects, 4)
{
    uint32_t count = ARG(0), list[64];
    if (!count || count > 64) { void_set_last_error(ERROR_INVALID_PARAMETER_); return WAIT_FAILED_; }
    for (uint32_t i = 0; i < count; ++i) list[i] = MEM32(ARG(1) + 4 * i);
    return void_wait((int)count, list, ARG(2) != 0, ARG(3));
}

static void sleep_ms(uint32_t ms)
{
    if (!ms) {
        if (machine_contended()) yield_hook();
        else sched_yield();
        return;
    }
    void_blocking_begin();
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    void_blocking_end();
}

WINAPI_EXPORT(K32, Sleep, 1) { sleep_ms(ARG(0)); return 0; }
WINAPI_EXPORT(K32, SleepEx, 2) { sleep_ms(ARG(0)); return 0; }
WINAPI_EXPORT(K32, SwitchToThread, 0) { sleep_ms(0); return 1; }

/* ---------------------------------------------------------------- threads */
static void thread_object_destroy(void_object *o) { (void)o; }

/* Every guest thread waits while another holds the machine, so a holder on a
 * little core stretches everyone's time: on the Helio G90T the Sound.dll
 * stream thread decoded Vorbis on an A55 (cpu_capacity 435 against the A76's
 * 1024) while the main thread waited for the machine. Guest threads run on
 * the biggest cores (cpu_capacity, else the highest maximum frequency).
 * Host threads may inherit their creator's mask; this code does not restore
 * a default mask for the mixer or workers created inside a driver.
 * VOID_BIG_CORES=0 turns this off. */
static cpu_set_t big_cores;
static int big_cores_count;
static cpu_set_t offload_cores;   /* see void_guest_offload_begin */
static int offload_count;
static pthread_once_t big_cores_once = PTHREAD_ONCE_INIT;
static cpu_set_t native_worker_original_mask;
static int native_worker_original_valid;

static long cpu_value(int cpu, const char *name)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", cpu, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long value = -1;
    if (fscanf(f, "%ld", &value) != 1) value = -1;
    fclose(f);
    return value;
}

/* Placement policy, VOID_GUEST_CORES:
 *   prime (default) only the top cpu_capacity tier. The machine serializes the
 *         guest, so its work must stay on one fast core: spread over cores it
 *         left each at ~25 % load and DVFS kept them near minimum frequency
 *         (RMX3851 A/B: the X4 idled at 633 MHz 94 % of the time, the menu fell
 *         from 58 to 23 FPS; perf_optimization_295.md). The historical cost of
 *         this policy was the wake-up herd, now removed by targeted wake-ups.
 *   perf  every core with at least half the top capacity (A/B only).
 *   off   Android's mask (also VOID_BIG_CORES=0).
 * The mask is re-applied after lifecycle changes (Android resets masks when the
 * process moves between cpusets) and every 256 acquisitions as a safety net,
 * not on every machine acquisition; VOID_GUEST_PIN_EVERY=1 restores that. */
static const char *guest_core_policy = "off";
static int guest_pin_every;
static unsigned affinity_epoch = 1;
static __thread unsigned t_affinity_epoch, t_affinity_acquires;

void void_guest_affinity_refresh(void)
{
    __atomic_add_fetch(&affinity_epoch, 1, __ATOMIC_RELAXED);
}

static void big_cores_init(void)
{
    /* Capture Android's allowed mask before the first guest pin. Native
     * audio/driver pthreads otherwise inherit the guest's mask. */
    native_worker_original_valid = sched_getaffinity(0, sizeof native_worker_original_mask,
                                                    &native_worker_original_mask) == 0;
    const char *every = getenv("VOID_GUEST_PIN_EVERY");
    guest_pin_every = every && *every == '1';
    const char *env = getenv("VOID_BIG_CORES");
    if (env && *env == '0') return;
    const char *policy = getenv("VOID_GUEST_CORES");
    if (policy && !strcmp(policy, "off")) return;
    int prime = !policy || strcmp(policy, "perf");
    long value[CPU_SETSIZE];
    int cpus = 0, use_capacity = cpu_value(0, "cpu_capacity") > 0;
    for (; cpus < CPU_SETSIZE; ++cpus) {
        value[cpus] = use_capacity ? cpu_value(cpus, "cpu_capacity") : cpu_value(cpus, "cpufreq/cpuinfo_max_freq");
        if (value[cpus] <= 0) break;
    }
    long best = 0, worst = 0;
    for (int i = 0; i < cpus; ++i) {
        if (value[i] > best) best = value[i];
        if (!worst || value[i] < worst) worst = value[i];
    }
    if (cpus < 2 || best == worst) return;       /* homogeneous or unknown: leave the scheduler alone */
    long threshold = prime ? best : (best + 1) / 2;
    CPU_ZERO(&big_cores);
    for (int i = 0; i < cpus; ++i) if (value[i] >= threshold) { CPU_SET(i, &big_cores); ++big_cores_count; }
    CPU_ZERO(&offload_cores);
    if (prime)
        for (int i = 0; i < cpus; ++i)
            if (value[i] >= (best + 1) / 2 && value[i] < best &&
                (!native_worker_original_valid || CPU_ISSET(i, &native_worker_original_mask))) {
                CPU_SET(i, &offload_cores); ++offload_count;
            }
    guest_core_policy = prime ? "prime" : "perf";
    char list[64] = "";
    for (int i = 0, n = 0; i < cpus && n < 56; ++i)
        if (CPU_ISSET(i, &big_cores)) n += snprintf(list + n, sizeof list - (size_t)n, "%s%d", n ? "," : "", i);
    void_log("[void] guest threads run on cpus %s (policy %s, %s >= %ld; max %ld, min %ld; re-pin %s)", list,
        guest_core_policy, use_capacity ? "capacity" : "max frequency", threshold, best, worst,
        guest_pin_every ? "every acquisition" : "lifecycle epoch/256 acquisitions");
}

/* Long native work a guest thread does with the machine released (for example
 * the audio DSP in bioshock_audio_dsp.c) moves off the prime core so it runs
 * beside the machine holder instead of time-slicing with it. The offload set
 * is every core with at least half the top capacity outside the prime tier;
 * empty (no-op) unless the prime policy is active. */
void void_guest_offload_begin(void)
{
    pthread_once(&big_cores_once, big_cores_init);
    if (offload_count) sched_setaffinity(0, sizeof offload_cores, &offload_cores);
}

void void_guest_offload_end(void)
{
    if (offload_count) sched_setaffinity(0, sizeof big_cores, &big_cores);
}

static void pin_guest_thread(void)
{
    pthread_once(&big_cores_once, big_cores_init);
    if (!big_cores_count) return;
    unsigned epoch = __atomic_load_n(&affinity_epoch, __ATOMIC_RELAXED);
    if (!guest_pin_every && t_affinity_epoch == epoch && (++t_affinity_acquires & 255u)) return;
    t_affinity_epoch = epoch;
    sched_setaffinity(0, sizeof big_cores, &big_cores);
}

enum { NATIVE_AFFINITY_SCOPES = 16 };
static __thread cpu_set_t native_affinity_saved[NATIVE_AFFINITY_SCOPES];
static __thread unsigned char native_affinity_valid[NATIVE_AFFINITY_SCOPES];
static __thread unsigned native_affinity_depth;

/* Private creation-only scope. Existing guest threads retain their policy;
 * newly created native workers may use Android's original allowed CPUs. */
void void_native_worker_affinity_begin(void)
{
    int saved_errno = errno;
    pthread_once(&big_cores_once, big_cores_init);
    unsigned slot = native_affinity_depth++;
    if (slot < NATIVE_AFFINITY_SCOPES) {
        native_affinity_valid[slot] = native_worker_original_valid &&
            sched_getaffinity(0, sizeof native_affinity_saved[slot], &native_affinity_saved[slot]) == 0;
        /* Never expand a mask that cannot subsequently be restored. */
        if (native_affinity_valid[slot])
            (void)sched_setaffinity(0, sizeof native_worker_original_mask, &native_worker_original_mask);
    }
    errno = saved_errno;
}

void void_native_worker_affinity_end(void)
{
    int saved_errno = errno;
    if (native_affinity_depth) {
        unsigned slot = --native_affinity_depth;
        if (slot < NATIVE_AFFINITY_SCOPES && native_affinity_valid[slot]) {
            if (sched_setaffinity(0, sizeof native_affinity_saved[slot], &native_affinity_saved[slot]))
                void_log("[void] native creation affinity restore failed: %s", strerror(errno));
            native_affinity_valid[slot] = 0;
        }
    }
    errno = saved_errno;
}

/* The same scope for latency-sensitive native workers (the renderer's present thread, the GPU driver's event
 * threads): the offload cores (the big cores outside the prime tier) when the prime policy has them, else
 * Android's mask.  With Android's mask the scheduler put the present thread on the Dimensity 8300's A510
 * cores, where vkQueuePresentKHR took 2.5-3.2 ms instead of 1.3 ms (work/ha22-perf-20261122). */
void void_native_worker_offload_begin(void)
{
    void_native_worker_affinity_begin();
    if (offload_count && native_worker_original_valid) (void)sched_setaffinity(0, sizeof offload_cores, &offload_cores);
}

/* Native worker entry only, before its user function. Lazy driver workers
 * may be born after a creation scope has ended; they need a permanent reset
 * on their own thread, with no scope stack and no parent-affinity change. */
int void_native_worker_affinity_apply(void)
{
    int saved_errno = errno, result = 0;
    pthread_once(&big_cores_once, big_cores_init);
    if (t_self && t_self->depth > 0)
        void_log("[void] refused native-worker affinity reset on a guest machine holder");
    else if (!native_worker_original_valid)
        void_log("[void] native-worker original Android affinity is unavailable");
    else if (sched_setaffinity(0, sizeof native_worker_original_mask, &native_worker_original_mask))
        void_log("[void] native-worker affinity reset failed: %s", strerror(errno));
    else result = 1;
    errno = saved_errno;
    return result;
}

static void *thread_main(void *arg)
{
    void_thread *t = (void_thread *)arg;
    t_self = t;
    t->native_tid = gettid();
    int bink = t->start == 0x180124D0u;
    if (bink) void_log("[bink-thread] entered native thread id=%u param=%08X suspended=%d", t->id, t->param, t->suspended);
    pin_guest_thread();
    if (t->suspended) {
        if (bink) void_log("[bink-thread] waiting for resume id=%u", t->id);
        pthread_mutex_lock(&wait_lock);
        while (t->suspended) pthread_cond_wait(&wait_cond, &wait_lock);
        pthread_mutex_unlock(&wait_lock);
    }
    if (bink) void_log("[bink-thread] acquiring machine id=%u", t->id);
    void_machine_enter();
    if (bink) void_log("[bink-thread] acquired machine id=%u", t->id);
    if (!setjmp(t->exit_jb)) {
        t->exit_armed = 1;
        uint32_t args[1] = { t->param };
        g_esp = t->stack_hi - 0x100;
        g_fs_base = t->tib;
        tls_thread_callbacks(2);            /* DLL_THREAD_ATTACH */
        if (bink) void_log("[bink-thread] calling lifted entry id=%u", t->id);
        t->exit_code = void_call_guest(t->start, 1, args, 2);
        if (bink) void_log("[bink-thread] lifted entry returned id=%u code=%u", t->id, t->exit_code);
    }
    t->exit_armed = 0;
    g_esp = t->stack_hi - 0x100;
    g_fs_base = t->tib;
    tls_thread_callbacks(3);                /* DLL_THREAD_DETACH, including ExitThread */
    VLOG("[void] guest thread %u exits with %u", t->id, t->exit_code);
    void_machine_leave();
    pthread_mutex_lock(&wait_lock);
    t->object->signaled = 1;
    t->object->count = (int)t->exit_code;
    wake_object_waiters_locked(t->object);
    wake_predicate_waiters_locked();
    pthread_mutex_unlock(&wait_lock);
    void_object_release(t->object);
    return NULL;
}

WINAPI_EXPORT(K32, CreateThread, 6)
{
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), tid_out = ARG(5);
    void_thread *t = thread_new((flags & 0x10000u) ? stack : (stack > GUEST_STACK_DEFAULT ? stack : 0));
    t->start = start;
    t->param = param;
    t->guest_owned = 1;
    t->suspended = (flags & 4u) != 0;     /* CREATE_SUSPENDED */
    t->object = void_object_new(VOID_OBJ_THREAD, 0);
    t->object->data = t;
    t->object->destroy = thread_object_destroy;
    t->object->refs = 2;                  /* the handle and the running thread */
    uint32_t handle = void_handle_new(t->object);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &attr, thread_main, t)) {
        pthread_attr_destroy(&attr);
        void_handle_close(handle);
        void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
        return 0;
    }
    pthread_attr_destroy(&attr);
    if (tid_out) MEM32(tid_out) = t->id;
    if (!t->suspended && start == 0x10E4B2C6u) {
        /* FMOD marks its thread running before the first metered-section wait. */
        void_blocking_begin();
        pthread_mutex_lock(&wait_lock);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        int rc = 0;
        while (!t->first_wait && !t->object->signaled && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&wait_cond, &wait_lock, &deadline);
        int started = t->first_wait;
        pthread_mutex_unlock(&wait_lock);
        void_blocking_end();
        if (!started) void_log("[void] FMOD worker did not reach its first wait before CreateThread returned");
    }
    VLOG("[void] CreateThread start=%08X param=%08X id=%u handle=%08X%s", start, param, t->id, handle,
        t->suspended ? " suspended" : "");
    return handle;
}

WINAPI_EXPORT(K32, SuspendThread, 1)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_THREAD);
    if (!o || !o->data) return 0xFFFFFFFFu;
    void_thread *t = (void_thread *)o->data;
    pthread_mutex_lock(&wait_lock);
    uint32_t previous = (uint32_t)t->suspended++;
    pthread_mutex_unlock(&wait_lock);
    return previous;
}

WINAPI_EXPORT(K32, ResumeThread, 1)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_THREAD);
    if (!o || !o->data) return 0xFFFFFFFFu;
    void_thread *t = (void_thread *)o->data;
    int bink = t->start == 0x180124D0u;
    pthread_mutex_lock(&wait_lock);
    if (t->suspended <= 0) {
        pthread_mutex_unlock(&wait_lock);
        if (bink) void_log("[bink-thread] resume rejected id=%u already running", t->id);
        return 0xFFFFFFFFu;
    }
    uint32_t previous = (uint32_t)t->suspended--;
    if (bink) void_log("[bink-thread] resume id=%u previous=%u", t->id, previous);
    if (!t->suspended) wake_predicate_waiters_locked();
    pthread_mutex_unlock(&wait_lock);
    return previous;
}

RAW_EXPORT(K32, ExitThread)
{
    void_thread *t = void_thread_self();
    t->exit_code = ARG(0);
    if (!t->exit_armed) void_fatal("ExitThread on a thread without a guest root frame");
    void_eh_reset_thread();
    longjmp(t->exit_jb, 1);
}

WINAPI_EXPORT(K32, GetExitCodeThread, 2)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_THREAD);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    MEM32(ARG(1)) = o->signaled ? (uint32_t)o->count : 259u;   /* STILL_ACTIVE */
    return 1;
}

WINAPI_EXPORT(K32, TerminateThread, 2)
{
    void_log("[void] TerminateThread(%08X, %u) requested by %08X: unsupported, ignored", ARG(0), ARG(1), g_cur_func);
    return 0;
}

WINAPI_EXPORT(K32, GetThreadPriority, 1) { return 0; }
WINAPI_EXPORT(K32, SetThreadPriority, 2) { return 1; }
static int current_process_handle(uint32_t handle)
{
    return handle == 0xFFFFFFFFu || void_handle_get(handle, VOID_OBJ_PROCESS) != NULL;
}

WINAPI_EXPORT(K32, GetProcessAffinityMask, 3)
{
    if (!current_process_handle(ARG(0))) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (!ARG(1) || !ARG(2) || !void_vm_writable(ARG(1), 4) || !void_vm_writable(ARG(2), 4)) {
        void_set_last_error(998); return 0;
    }
    pthread_once(&cpu_model_once, cpu_model_init);
    MEM32(ARG(1)) = guest_process_mask;
    MEM32(ARG(2)) = guest_system_mask;
    return 1;
}

WINAPI_EXPORT(K32, SetProcessAffinityMask, 2)
{
    uint32_t mask = ARG(1);
    if (!mask) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    if (!current_process_handle(ARG(0))) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    pthread_once(&cpu_model_once, cpu_model_init);
    if (mask & ~guest_system_mask) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    pthread_mutex_lock(&threads_lock);
    guest_process_mask = mask;
    /* Native SetProcess resets every existing thread's mask, including when
     * the new process mask equals the previous value. New threads inherit it. */
    for (void_thread *thread = all_threads; thread; thread = thread->next_thread) {
        thread->affinity_mask = mask; select_virtual_cpu(thread);
    }
    pthread_mutex_unlock(&threads_lock);
    return 1;
}

WINAPI_EXPORT(K32, SetThreadAffinityMask, 2)
{
    void_object *object = void_handle_get(ARG(0), VOID_OBJ_THREAD);
    void_thread *thread = object ? object->data : NULL;
    if (!thread) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    uint32_t mask = ARG(1);
    pthread_once(&cpu_model_once, cpu_model_init);
    if (!mask || (mask & ~guest_process_mask)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    uint32_t previous = thread->affinity_mask;
    thread->affinity_mask = mask; select_virtual_cpu(thread);
    return previous;
}
WINAPI_EXPORT(K32, SetThreadIdealProcessor, 2) { return 0; }

WINAPI_EXPORT(K32, DuplicateHandle, 7)
{
    uint32_t source = ARG(1), target_out = ARG(3), options = ARG(6);
    uint32_t h;
    if (source == 0xFFFFFFFEu) {
        void_object *o = void_thread_self()->object;
        pthread_mutex_lock(&handle_lock);
        ++o->refs;
        pthread_mutex_unlock(&handle_lock);
        h = void_handle_new(o);
    } else if (source == 0xFFFFFFFFu) {
        void_object *o = void_object_new(VOID_OBJ_PROCESS, 0);
        h = void_handle_new(o);
    } else {
        h = void_handle_dup(source);
    }
    if (!h) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (options & 1u) void_handle_close(source);   /* DUPLICATE_CLOSE_SOURCE */
    if (target_out) MEM32(target_out) = h;
    return 1;
}

WINAPI_EXPORT(K32, CloseHandle, 1)
{
    uint32_t h = ARG(0);
    if (h == 0xFFFFFFFEu || h == 0xFFFFFFFFu) return 1;
    if (!void_handle_close(h)) {
        void_set_last_error(ERROR_INVALID_HANDLE_);
        return 0;
    }
    return 1;
}

void void_threads_init(void)
{
    recomp_yield_hook = yield_hook;
    /* RECOMP_SPIN_WAIT back edges hand over at once when contended;
     * BIOSHOCK_SPIN_HINT=0 keeps the plain spin for A/B measurements. */
    const char *spin = getenv("BIOSHOCK_SPIN_HINT");
    recomp_spin_hook = spin && *spin == '0' ? NULL : spin_hook;
}

/* The thread that runs the game's entry point. */
void void_thread_bind_main(void)
{
    void_thread *t = void_thread_self();
    (void)t;
}

void void_thread_exit_current_for_process(void) {}
