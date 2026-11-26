"""Production machine/object/critical-section wake-ups with real pthreads on ARM64.

The functions are extracted unchanged from android/host/threads.c and linked
with minimal stand-ins for the guest register file and handle table. The
program runs on the user-selected Android device and checks, in both the
targeted policy and the legacy broadcast policy:
  * the FIFO machine ticket lock: mutual exclusion, progress and yield order;
  * auto/manual-reset events, Pulse, WaitAny/WaitAll, timeouts, semaphores,
    mutex release and the message-queue (extra condition) broadcast;
  * contended critical sections across blocking calls;
  * how often each sleeping thread was woken. In the targeted policy a waiter
    is woken only for its own object; the legacy policy wakes every waiter.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
SERIAL = os.environ.get("BIOSHOCK_TEST_ANDROID")   # the explicitly selected device; no default (shared phones)
SIGNATURES = (
    "static int legacy_wake(void)",
    "static int wait_release_first(void)",
    "static void machine_wake_turn_locked(void)",
    "static int machine_wait_turn_locked(uint64_t ticket)",
    "static void machine_prioritize_audio_locked(void)",
    "static void serve_next_locked(void)",
    "static void machine_acquire(void)",
    "static void machine_release(void)",
    "static int machine_contended(void)\n{",  # the definition; a forward declaration precedes it
    "int void_machine_is_owned(void)",
    "void void_blocking_begin(void)",
    "void void_blocking_end(void)",
    "static void yield_hook(void)",
    "static void wake_object_waiters_locked(const void_object *o)",
    "static void wake_predicate_waiters_locked(void)",
    "void void_object_signal(void_object *o)",
    "void void_wait_broadcast(void)",
    "static int object_ready(void_object *o, uint32_t tid)",
    "static void object_consume(void_object *o, uint32_t tid)",
    "static uint32_t wait_core(",
    "static uint32_t event_op(uint32_t h, int op, int guest_handoff)",
    "static void cs_enter(uint32_t cs)",
    "static void cs_leave(uint32_t cs)",
)
TYPEDEFS = ("machine_waiter", "object_waiter", "cs_waiter")

HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #x); abort(); } } while (0)
typedef struct { uint32_t words[32]; } void_regs;
enum { VOID_OBJ_NONE, VOID_OBJ_EVENT, VOID_OBJ_THREAD, VOID_OBJ_PROCESS, VOID_OBJ_TIMER, VOID_OBJ_SEMAPHORE, VOID_OBJ_MUTEX };
typedef struct void_object {
    int kind, signaled, manual_reset;
    uint64_t pulse_generation, pulse_claimed;
    int count, max_count;
    uint32_t owner;
    int recursion;
} void_object;
typedef struct void_thread {
    void_regs regs;
    int depth;
    uint32_t id, start, trace_index, trace[256];
    int first_wait, suspended;
    pthread_cond_t machine_cond, object_cond, cs_cond_self;
} void_thread;
#define INFINITE_ 0xFFFFFFFFu
#define WAIT_TIMEOUT_ 258u
#define WAIT_OBJECT_0_ 0u
#define ERROR_INVALID_HANDLE_ 6u
#define ERROR_ACCESS_DENIED_ 5u
#define VLOG(...) ((void)0)
typedef int (*void_wait_extra)(void *);
static __thread void_thread *t_self;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mcond = PTHREAD_COND_INITIALIZER;
static uint64_t ticket_next, ticket_serving;
static uint32_t machine_holder;
static __thread uint64_t t_lock_wait_ns;
static __thread int t_machine_owned;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cs_cond = PTHREAD_COND_INITIALIZER;
static uint64_t cs_generation;
static uint32_t cs_waiters;
static uint32_t g_cur_func, unreal_wait_count, unreal_wait_handles[32], unreal_wait_signals[32];
/* The urgent producer's back-edge poke and its report. */
#define RECOMP_YIELD_EVERY 0x10000u
#define ICALL_TRACE_SIZE 128
static uint32_t g_backedges, g_icall_trace_idx, g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
static const char *void_thunk_name(uint32_t address) { (void)address; return NULL; }
static uint8_t guest[4096];
#define MEM32(a) (*(uint32_t *)(guest + (a)))
static void void_log(const char *format, ...) { (void)format; }
static _Noreturn void void_fatal(const char *message) { fprintf(stderr, "fatal %s\n", message); abort(); }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static void pin_guest_thread(void) {}
static void suspend_checkpoint(void) {}
static uint32_t void_thread_id(void) { return t_self->id; }
static void_thread *void_thread_self(void) { return t_self; }
static uint32_t last_error;
static void void_set_last_error(uint32_t error) { last_error = error; }
static void void_regs_save(void_regs *saved) { (void)saved; }
static void void_regs_load(const void_regs *saved) { (void)saved; }
void d3d_gl_thread_acquire(void) __attribute__((weak));
void d3d_gl_thread_release(void) __attribute__((weak));
/* Handles 1..64 name the fixture objects below. */
static void_object objects_table[65];
static void_object *void_handle_get(uint32_t h, int kind) { return h && h <= 64 && objects_table[h].kind == kind ? &objects_table[h] : NULL; }
static int event_handle_allows(uint32_t h, uint32_t access) { (void)h; (void)access; return 1; }
/* Count every return from a condition wait, split by purpose. */
static __thread unsigned long t_object_wakes, t_machine_wakes, t_cs_wakes;
static void count_wake(pthread_cond_t *c) {
    if (c == &mcond || (t_self && c == &t_self->machine_cond)) ++t_machine_wakes;
    else if (c == &cs_cond || (t_self && c == &t_self->cs_cond_self)) ++t_cs_wakes;
    else ++t_object_wakes;
}
static int counted_wait(pthread_cond_t *c, pthread_mutex_t *m) { int r = pthread_cond_wait(c, m); count_wake(c); return r; }
static int counted_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { int r = pthread_cond_timedwait(c, m, t); count_wake(c); return r; }
#define pthread_cond_wait counted_wait
#define pthread_cond_timedwait counted_timedwait
/* PRODUCTION */
#undef pthread_cond_wait
#undef pthread_cond_timedwait

static void thread_init(void_thread *t, uint32_t id) {
    memset(t, 0, sizeof *t);
    t->id = id;
    CHECK(!pthread_cond_init(&t->machine_cond, NULL) && !pthread_cond_init(&t->object_cond, NULL) && !pthread_cond_init(&t->cs_cond_self, NULL));
}
static void enter(void) { CHECK(t_self->depth == 0); t_self->depth = 1; machine_acquire(); }
static void leave(void) { CHECK(t_self->depth == 1); machine_release(); t_self->depth = 0; }
static void reset_counts(void) { t_object_wakes = t_machine_wakes = t_cs_wakes = 0; }

/* ---------------- scenario 1: machine FIFO mutual exclusion and yields */
#define M_THREADS 8
#define M_ROUNDS 20000
static int inside;
static unsigned long machine_total;
static void *machine_worker(void *arg) {
    void_thread self; thread_init(&self, 0x200 + (uint32_t)(uintptr_t)arg * 4); t_self = &self;
    for (unsigned i = 0; i < M_ROUNDS; ++i) {
        enter();
        CHECK(!__atomic_exchange_n(&inside, 1, __ATOMIC_ACQ_REL));
        ++machine_total;
        if ((i & 63) == 0) { __atomic_store_n(&inside, 0, __ATOMIC_RELEASE); yield_hook(); CHECK(!__atomic_exchange_n(&inside, 1, __ATOMIC_ACQ_REL)); }
        __atomic_store_n(&inside, 0, __ATOMIC_RELEASE);
        leave();
    }
    return (void *)(uintptr_t)t_machine_wakes;
}
static unsigned long scenario_machine(void) {
    pthread_t threads[M_THREADS]; unsigned long wakes = 0; machine_total = 0;
    uint64_t start = now_ns();
    for (uintptr_t i = 0; i < M_THREADS; ++i) CHECK(!pthread_create(&threads[i], NULL, machine_worker, (void *)i));
    for (int i = 0; i < M_THREADS; ++i) { void *r; CHECK(!pthread_join(threads[i], &r)); wakes += (unsigned long)(uintptr_t)r; }
    CHECK(machine_total == (unsigned long)M_THREADS * M_ROUNDS && ticket_next == ticket_serving && !machine_holder);
    printf("machine legacy=%d: %lu acquisitions, %lu machine wake-ups, %.1f ms\n", wake_legacy,
           machine_total, wakes, (now_ns() - start) / 1e6);
    return wakes;
}

static unsigned urgent_order[2], urgent_count;
static void *urgent_worker(void *arg) {
    unsigned index = (unsigned)(uintptr_t)arg;
    void_thread self; thread_init(&self, 0x240 + index * 4); t_self = &self;
    void_machine_urgent(index == 1);
    enter();
    CHECK(urgent_count < 2);
    urgent_order[urgent_count++] = index;
    leave();
    return NULL;
}
static void scenario_urgent(void) {
    pthread_t normal, audio;
    void_thread self; thread_init(&self, 0x250); t_self = &self;
    urgent_count = 0;
    enter();
    CHECK(!pthread_create(&normal, NULL, urgent_worker, (void *)(uintptr_t)0));
    pthread_mutex_lock(&mlock);
    while (!machine_waiters) {
        pthread_mutex_unlock(&mlock); usleep(100); pthread_mutex_lock(&mlock);
    }
    pthread_mutex_unlock(&mlock);
    CHECK(!pthread_create(&audio, NULL, urgent_worker, (void *)(uintptr_t)1));
    pthread_mutex_lock(&mlock);
    while (!urgent_waiter || now_ns() - urgent_since < URGENT_SLICE_NS) {
        pthread_mutex_unlock(&mlock); usleep(100); pthread_mutex_lock(&mlock);
    }
    pthread_mutex_unlock(&mlock);
    yield_hook();
    CHECK(urgent_count == 2 && urgent_order[0] == 1 && urgent_order[1] == 0);
    leave();
    CHECK(!pthread_join(audio, NULL) && !pthread_join(normal, NULL));
    CHECK(ticket_next == ticket_serving && !machine_waiters && !urgent_waiter);
}

/* ---------------- scenario 2: per-object waiters versus one signaller */
#define WAITERS 10
#define SIGNALS 3000
static unsigned consumed[WAITERS], waiter_object_wakes[WAITERS];
static int stop_waiters;
static void *event_waiter(void *arg) {
    unsigned index = (unsigned)(uintptr_t)arg;
    void_thread self; thread_init(&self, 0x300 + index * 4); t_self = &self; reset_counts();
    enter();
    for (;;) {
        void_object *object = &objects_table[1 + index];
        void_object *list[1] = { object };
        CHECK(wait_core(1, list, 0, INFINITE_, NULL, NULL) == WAIT_OBJECT_0_);
        if (__atomic_load_n(&stop_waiters, __ATOMIC_ACQUIRE)) break;
        __atomic_add_fetch(&consumed[index], 1, __ATOMIC_ACQ_REL);
    }
    leave();
    waiter_object_wakes[index] = (unsigned)t_object_wakes;
    return NULL;
}
static void scenario_events(unsigned *max_foreign) {
    memset(consumed, 0, sizeof consumed); memset(waiter_object_wakes, 0, sizeof waiter_object_wakes);
    stop_waiters = 0;
    for (unsigned i = 0; i < WAITERS; ++i) objects_table[1 + i] = (void_object){ .kind = VOID_OBJ_EVENT };
    pthread_t threads[WAITERS];
    for (uintptr_t i = 0; i < WAITERS; ++i) CHECK(!pthread_create(&threads[i], NULL, event_waiter, (void *)i));
    void_thread self; thread_init(&self, 0x400); t_self = &self;
    unsigned sent[WAITERS] = {0}; unsigned seed = 12345;
    enter();
    for (unsigned n = 0; n < SIGNALS; ++n) {
        seed = seed * 1103515245u + 12345u; unsigned index = (seed >> 16) % WAITERS;
        unsigned before = __atomic_load_n(&consumed[index], __ATOMIC_ACQUIRE);
        CHECK(event_op(2 + index - 1, 1, 1) == 1);   /* SetEvent with guest hand-off */
        ++sent[index];
        void_blocking_begin();
        while (__atomic_load_n(&consumed[index], __ATOMIC_ACQUIRE) == before) sched_yield();
        void_blocking_end();
    }
    __atomic_store_n(&stop_waiters, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < WAITERS; ++i) CHECK(event_op(2 + i - 1, 1, 0) == 1);
    leave();
    for (int i = 0; i < WAITERS; ++i) CHECK(!pthread_join(threads[i], NULL));
    unsigned worst = 0;
    for (unsigned i = 0; i < WAITERS; ++i) {
        CHECK(consumed[i] == sent[i]);
        unsigned foreign = waiter_object_wakes[i] > sent[i] + 1 ? waiter_object_wakes[i] - sent[i] - 1 : 0;
        if (foreign > worst) worst = foreign;
    }
    *max_foreign = worst;
    printf("events legacy=%d: %u signals, worst waiter woken %u times for other objects\n", wake_legacy, SIGNALS, worst);
}

/* ---------------- scenario 3: contended critical sections across blocking calls */
#define CS_THREADS 6
#define CS_ROUNDS 3000
static uint32_t cs_owner_check[3];
static void *cs_worker(void *arg) {
    unsigned index = (unsigned)(uintptr_t)arg;
    void_thread self; thread_init(&self, 0x500 + index * 4); t_self = &self; reset_counts();
    enter();
    for (unsigned i = 0; i < CS_ROUNDS; ++i) {
        uint32_t cs = 64u * ((i + index) % 3);
        cs_enter(cs);
        CHECK(MEM32(cs + 12) == self.id);
        CHECK(!__atomic_exchange_n(&cs_owner_check[cs / 64], self.id, __ATOMIC_ACQ_REL));
        if ((i & 7) == 0) { void_blocking_begin(); usleep(20); void_blocking_end(); }
        CHECK(__atomic_exchange_n(&cs_owner_check[cs / 64], 0, __ATOMIC_ACQ_REL) == self.id);
        cs_leave(cs);
    }
    leave();
    return (void *)(uintptr_t)t_cs_wakes;
}
static void scenario_cs(void) {
    memset(guest, 0, sizeof guest);
    for (unsigned k = 0; k < 3; ++k) { MEM32(64 * k + 4) = 0xFFFFFFFFu; }
    pthread_t threads[CS_THREADS]; unsigned long wakes = 0;
    uint64_t start = now_ns();
    for (uintptr_t i = 0; i < CS_THREADS; ++i) CHECK(!pthread_create(&threads[i], NULL, cs_worker, (void *)i));
    for (int i = 0; i < CS_THREADS; ++i) { void *r; CHECK(!pthread_join(threads[i], &r)); wakes += (unsigned long)(uintptr_t)r; }
    for (unsigned k = 0; k < 3; ++k) CHECK(!MEM32(64 * k + 12) && !MEM32(64 * k + 8));
    CHECK(!cs_waiters && !cs_waiter_list);
    printf("critical sections legacy=%d: %u enters, %lu waiter wake-ups, %.1f ms\n", wake_legacy,
           CS_THREADS * CS_ROUNDS, wakes, (now_ns() - start) / 1e6);
}

/* ---------------- scenario 4: exact wait semantics */
static int extra_flag;
static int extra_ready(void *arg) { (void)arg; return __atomic_load_n(&extra_flag, __ATOMIC_ACQUIRE); }
static void *extra_waiter(void *arg) {
    (void)arg; void_thread self; thread_init(&self, 0x600); t_self = &self; reset_counts();
    enter();
    void_object *list[1] = { &objects_table[40] };
    uint32_t r = wait_core(1, list, 0, INFINITE_, extra_ready, NULL);
    leave();
    return (void *)(uintptr_t)r;
}
static void *plain_waiter(void *arg) {
    (void)arg; void_thread self; thread_init(&self, 0x604); t_self = &self; reset_counts();
    enter();
    void_object *list[1] = { &objects_table[41] };
    CHECK(wait_core(1, list, 0, INFINITE_, NULL, NULL) == WAIT_OBJECT_0_);
    leave();
    return (void *)(uintptr_t)t_object_wakes;
}
static void *pulse_waiter(void *arg) {
    (void)arg; void_thread self; thread_init(&self, 0x608); t_self = &self;
    enter();
    void_object *list[1] = { &objects_table[42] };
    uint32_t r = wait_core(1, list, 0, 5000, NULL, NULL);
    leave();
    return (void *)(uintptr_t)r;
}
static void scenario_semantics(void) {
    void_thread self; thread_init(&self, 0x700); t_self = &self;
    enter();
    objects_table[30] = (void_object){ .kind = VOID_OBJ_EVENT };
    void_object *one[1] = { &objects_table[30] };
    uint64_t start = now_ns();
    CHECK(wait_core(1, one, 0, 30, NULL, NULL) == WAIT_TIMEOUT_);
    uint64_t waited = now_ns() - start;
    CHECK(waited >= 25000000u && waited < 1000000000u);
    CHECK(wait_core(1, one, 0, 0, NULL, NULL) == WAIT_TIMEOUT_);
    CHECK(event_op(30, 1, 0) == 1 && wait_core(1, one, 0, 0, NULL, NULL) == WAIT_OBJECT_0_);
    /* A satisfied blocking wait keeps the machine: no ticket is taken or served. */
    CHECK(event_op(30, 1, 0) == 1);
    uint64_t next_before = ticket_next, serving_before = ticket_serving;
    CHECK(wait_core(1, one, 0, INFINITE_, NULL, NULL) == WAIT_OBJECT_0_);
    CHECK(ticket_next == next_before && ticket_serving == serving_before && machine_holder == self.id);
    CHECK(!objects_table[30].signaled);                       /* auto reset consumed */
    objects_table[31] = (void_object){ .kind = VOID_OBJ_EVENT, .manual_reset = 1 };
    void_object *manual[1] = { &objects_table[31] };
    CHECK(event_op(31, 1, 0) == 1);
    CHECK(wait_core(1, manual, 0, 0, NULL, NULL) == WAIT_OBJECT_0_ && wait_core(1, manual, 0, 0, NULL, NULL) == WAIT_OBJECT_0_);
    CHECK(event_op(31, 0, 0) == 1 && wait_core(1, manual, 0, 0, NULL, NULL) == WAIT_TIMEOUT_);
    objects_table[32] = (void_object){ .kind = VOID_OBJ_EVENT };
    objects_table[33] = (void_object){ .kind = VOID_OBJ_EVENT };
    void_object *pair[2] = { &objects_table[32], &objects_table[33] };
    CHECK(event_op(33, 1, 0) == 1 && wait_core(2, pair, 0, 0, NULL, NULL) == WAIT_OBJECT_0_ + 1);
    CHECK(event_op(32, 1, 0) == 1 && wait_core(2, pair, 1, 0, NULL, NULL) == WAIT_TIMEOUT_);
    CHECK(objects_table[32].signaled);                        /* WaitAll consumed nothing */
    CHECK(event_op(33, 1, 0) == 1 && wait_core(2, pair, 1, 0, NULL, NULL) == WAIT_OBJECT_0_);
    CHECK(!objects_table[32].signaled && !objects_table[33].signaled);
    objects_table[34] = (void_object){ .kind = VOID_OBJ_SEMAPHORE, .count = 0, .max_count = 4 };
    void_object *semaphore[1] = { &objects_table[34] };
    CHECK(wait_core(1, semaphore, 0, 0, NULL, NULL) == WAIT_TIMEOUT_);
    pthread_mutex_lock(&wait_lock); objects_table[34].count += 2; wake_object_waiters_locked(&objects_table[34]); pthread_mutex_unlock(&wait_lock);
    CHECK(wait_core(1, semaphore, 0, 0, NULL, NULL) == WAIT_OBJECT_0_ && objects_table[34].count == 1);
    objects_table[35] = (void_object){ .kind = VOID_OBJ_MUTEX, .owner = 0x999, .recursion = 1 };
    void_object *mutex[1] = { &objects_table[35] };
    CHECK(wait_core(1, mutex, 0, 0, NULL, NULL) == WAIT_TIMEOUT_);
    pthread_mutex_lock(&wait_lock); objects_table[35].owner = 0; objects_table[35].recursion = 0; wake_object_waiters_locked(&objects_table[35]); pthread_mutex_unlock(&wait_lock);
    CHECK(wait_core(1, mutex, 0, 0, NULL, NULL) == WAIT_OBJECT_0_ && objects_table[35].owner == self.id);
    /* Message-queue broadcast reaches only waits with an extra condition. */
    objects_table[40] = (void_object){ .kind = VOID_OBJ_EVENT };
    objects_table[41] = (void_object){ .kind = VOID_OBJ_EVENT };
    pthread_t extra, plain; extra_flag = 0;
    CHECK(!pthread_create(&extra, NULL, extra_waiter, NULL) && !pthread_create(&plain, NULL, plain_waiter, NULL));
    void_blocking_begin(); usleep(50000); void_blocking_end();
    /* Spaced so a woken legacy waiter re-waits before the next broadcast. */
    for (int i = 0; i < 20; ++i) { void_wait_broadcast(); void_blocking_begin(); usleep(3000); void_blocking_end(); }
    __atomic_store_n(&extra_flag, 1, __ATOMIC_RELEASE);
    void_wait_broadcast();
    void *r;
    void_blocking_begin(); CHECK(!pthread_join(extra, &r)); void_blocking_end();
    CHECK((uint32_t)(uintptr_t)r == WAIT_OBJECT_0_ + 1);
    CHECK(event_op(41, 1, 0) == 1);
    void_blocking_begin(); CHECK(!pthread_join(plain, &r)); void_blocking_end();
    unsigned long plain_wakes = (unsigned long)(uintptr_t)r;
    printf("semantics legacy=%d: message broadcasts woke the plain waiter %lu times\n", wake_legacy, plain_wakes);
    if (!wake_legacy) CHECK(plain_wakes <= 2);
    else CHECK(plain_wakes >= 15);
    /* PulseEvent releases a current waiter even though the event ends unsignaled. */
    objects_table[42] = (void_object){ .kind = VOID_OBJ_EVENT };
    pthread_t pulse; CHECK(!pthread_create(&pulse, NULL, pulse_waiter, NULL));
    void_blocking_begin(); usleep(50000); void_blocking_end();
    CHECK(event_op(42, 2, 1) == 1 && !objects_table[42].signaled);
    void_blocking_begin(); CHECK(!pthread_join(pulse, &r)); void_blocking_end();
    CHECK((uint32_t)(uintptr_t)r == WAIT_OBJECT_0_);
    leave();
    CHECK(!object_waiters && !machine_waiters);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(600);   /* the legacy broadcasts alone took 91 s on the Lenovo TB373FU (8 cores, 22 November 2026) */
    unsigned long targeted_machine, legacy_machine;
    unsigned targeted_foreign, legacy_foreign;
    for (int legacy = 1; legacy >= 0; --legacy) {
        wake_legacy = legacy;
        unsigned long machine = scenario_machine();
        scenario_urgent();
        unsigned foreign;
        scenario_events(&foreign);
        scenario_cs();
        scenario_semantics();
        if (legacy) { legacy_machine = machine; legacy_foreign = foreign; }
        else { targeted_machine = machine; targeted_foreign = foreign; }
    }
    CHECK(targeted_foreign <= 2 && legacy_foreign > 100);
    printf("PASS targeted wakes: FIFO machine exclusion with yields, exact event/semaphore/mutex/timeout/WaitAll/Pulse/message semantics, contended critical sections; foreign wake-ups %u -> %u, machine wake-ups %lu -> %lu\n",
           legacy_foreign, targeted_foreign, legacy_machine, targeted_machine);
    return 0;
}
'''


def typedef(source, name):
    start = source.index(f"typedef struct {name} {{")
    end = source.index(f"}} {name};", start) + len(f"}} {name};")
    return source[start:end]


class TargetedWakesTest(unittest.TestCase):
    @unittest.skipUnless(SERIAL, "Set BIOSHOCK_TEST_ANDROID to the explicitly selected device")
    def test_production_wakes_on_device(self):
        source = (ROOT / "android/host/threads.c").read_text(encoding="utf-8")
        parts = [typedef(source, name) for name in TYPEDEFS]
        parts.insert(0, "static int wake_legacy = -1, wait_release_first_flag = -1;")
        for anchor in ("static machine_waiter *machine_waiters;", "static object_waiter *object_waiters;",
                       "static cs_waiter *cs_waiter_list;", "static int audio_waiting;"):
            self.assertIn(anchor, source)
            parts.append(anchor)
        # The urgent producer's state and the machine loans (2026-11-20), as production defines them.
        start = source.index("#define URGENT_POKE_NS")
        end = source.index("\n", source.index("void void_machine_urgent(int urgent)", start))
        parts.append(source[start:end + 1])
        start = source.index("enum { LOAN_NONE, LOAN_OFFERED, LOAN_TAKEN };")
        end = source.index("static __thread int t_borrowing, t_lending;", start)
        parts.append(source[start:end] + "static __thread int t_borrowing, t_lending;")
        start = source.index("/* QA diagnostic, BIOSHOCK_MACHINE_STATS=1")
        stats = source[start:source.index("static void machine_acquire(void)", start)]
        parts.append(stats)
        parts += [function(source, signature) for signature in SIGNATURES]
        program = HARNESS.replace("/* PRODUCTION */", "\n".join(parts))
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
        clang = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
        adb = [str(sdk / "platform-tools/adb.exe"), "-s", SERIAL]
        with tempfile.TemporaryDirectory(prefix="bioshock-wakes-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            (folder / "wakes.c").write_text(program, encoding="utf-8")
            binary = folder / "test-wakes"
            built = subprocess.run([str(clang), "--target=aarch64-linux-android29", "-O2", "-std=gnu11", "-Wall", "-Werror",
                                    "-Wno-unused-function", "-Wno-unused-variable", str(folder / "wakes.c"), "-o", str(binary)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            remote = "/data/local/tmp/bioshock-test-wakes"
            subprocess.run([*adb, "push", str(binary), remote], check=True, capture_output=True, timeout=60)
            result = subprocess.run([*adb, "shell", f"chmod 755 {remote} && {remote}; echo EXIT=$?"],
                                    capture_output=True, text=True, timeout=660)
            subprocess.run([*adb, "shell", "rm", "-f", remote], capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        print(output)
        self.assertIn("EXIT=0", output)
        self.assertIn("PASS targeted wakes", output)


if __name__ == "__main__":
    unittest.main()
