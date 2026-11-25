"""Host checks for the 2026-11-25 critical-section hand-over, negative stat cache and shader warm-up order.

The C fixtures compile production functions out of android/host/threads.c and android/host/files.c with the same
extractor as the other host tests, then call them on a single thread. threads.c is CRLF; the snippets are
normalized before they are compiled.
"""
from pathlib import Path
import struct
import subprocess
import sys
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "tests" / "perf-20261125"
sys.path.insert(0, str(ROOT / "tools"))
import build_shader_warmup as warmup  # noqa: E402


def production(source, signature):
    return function(source, signature).replace("\r\n", "\n").replace("\r", "\n")


def snippet(source, start, end):
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j].replace("\r\n", "\n").replace("\r", "\n")


def run_harness(test, name, program):
    OUT.mkdir(parents=True, exist_ok=True)
    probe, obj, binary = OUT / f"{name}.c", OUT / f"{name}.obj", OUT / f"{name}.exe"
    probe.write_text(program, encoding="utf-8", newline="\n")
    env, linker = environment("x64")
    for command in (
        [str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
         "/c", str(probe), "/Fo" + str(obj)],
        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)],
        [str(binary)],
    ):
        result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=60)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return result.stdout


def recipe(tag):
    return struct.pack("<Q", tag).ljust(warmup.RECIPE_SIZE, b"\0")


def capture(records):
    return b"".join(struct.pack("<Q", frame) + body for frame, body in records)


def parse_warmup(blob):
    magic, version, pair_count, pipeline_count, recipe_size, reserved = struct.unpack_from("<4s5I", blob)
    if (magic, version, recipe_size, reserved) != (b"BSWP", 1, warmup.RECIPE_SIZE, 0):
        raise AssertionError((magic, version, recipe_size, reserved))
    at = 24
    pairs = [blob[at + 16 * i:at + 16 * (i + 1)] for i in range(pair_count)]
    at += 16 * pair_count
    recipes = [blob[at + recipe_size * i:at + recipe_size * (i + 1)] for i in range(pipeline_count)]
    if at + recipe_size * pipeline_count != len(blob):
        raise AssertionError((len(blob), at, pipeline_count, recipe_size))
    return pairs, [struct.unpack_from("<Q", body)[0] for body in recipes]


CS_HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
static uint8_t guest[256];
#define MEM32(a) (*(uint32_t *)(guest + (a)))
typedef struct void_thread { pthread_cond_t cs_cond_self; } void_thread;
static void_thread thread_storage;
static void_thread *t_self = &thread_storage;
static uint32_t current_tid;
static uint32_t void_thread_id(void) { return current_tid; }
static uint32_t g_cur_func;
static void void_log(const char *format, ...) { (void)format; }
static int legacy_wake(void) { return 0; }
static unsigned blocks_begun, blocks_ended;
static void void_blocking_begin(void) { ++blocks_begun; }
static void void_blocking_end(void) { ++blocks_ended; }
enum { CS = 32, FREE_CS = 96, TID_A = 10, TID_B = 20, TID_C = 30 };
static unsigned lock_depth, lock_entries, signal_count, broadcast_count;
static pthread_cond_t *signaled[4];
static int handoff_on_wait, saw_released_owner;
static uint32_t wait_cs, leave_tid;
static pthread_cond_t cond_b, cond_c;
@CS_STATE@
static void pthread_mutex_lock(pthread_mutex_t *lock)
{
    CHECK(lock == &cs_lock && lock_depth == 0);
    lock_depth = 1;
    ++lock_entries;
}
static void pthread_mutex_unlock(pthread_mutex_t *lock)
{
    CHECK(lock == &cs_lock && lock_depth == 1);
    lock_depth = 0;
}
static void cs_leave(uint32_t cs);
static void pthread_cond_signal(pthread_cond_t *cond)
{
    CHECK(lock_depth == 1 && signal_count < 4);
    signaled[signal_count++] = cond;
}
static void pthread_cond_broadcast(pthread_cond_t *cond)
{
    (void)cond;
    CHECK(lock_depth == 1);
    ++broadcast_count;
}
static int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    CHECK(mutex == &cs_lock && lock_depth == 1);
    lock_depth = 0; /* a real wait releases the mutex for the owner */
    CHECK(handoff_on_wait);
    handoff_on_wait = 0;
    uint32_t saved = current_tid;
    current_tid = leave_tid;
    cs_leave(wait_cs);
    current_tid = saved;
    /* cs_leave has handed this stack waiter the section and has not unlinked it yet. */
    CHECK(cs_waiter_list && cs_waiter_list->next == NULL);
    CHECK(cs_waiter_list->tid == current_tid && cs_waiter_list->released == 2);
    CHECK(cs_waiter_list->cond == cond);
    CHECK(MEM32(wait_cs + 12) == current_tid);
    CHECK(MEM32(wait_cs + 8) == 1);
    CHECK(MEM32(wait_cs + 4) == 0);
    CHECK(signal_count == 1 && signaled[0] == &t_self->cs_cond_self && broadcast_count == 0);
    saw_released_owner = 1;
    CHECK(lock_depth == 0);
    lock_depth = 1;
    return 0;
}
@CS_FUNCTIONS@
static void reset_sync(void)
{
    memset(guest, 0, sizeof guest);
    cs_waiter_list = NULL;
    cs_waiters = 0;
    cs_legacy_waiters = 0;
    cs_generation = 0;
    signal_count = broadcast_count = lock_entries = 0;
    lock_depth = 0;
    handoff_on_wait = 0;
    saw_released_owner = 0;
    blocks_begun = blocks_ended = 0;
    memset(&cond_b, 0, sizeof cond_b);
    memset(&cond_c, 0, sizeof cond_c);
}
/* Registration prepends, so the waiter recorded first is the tail. */
static void park(cs_waiter *waiter, cs_waiter *newer, uint32_t tid, pthread_cond_t *cond)
{
    memset(waiter, 0, sizeof *waiter);
    waiter->cs = CS;
    waiter->cond = cond;
    waiter->tid = tid;
    waiter->released = 0;
    waiter->next = newer;
}
int main(void)
{
    cs_waiter older, newer;
    /* A leaves a section B is parked on: B becomes the owner and is the only thread signalled. */
    reset_sync();
    current_tid = TID_A;
    MEM32(CS + 12) = TID_A;
    MEM32(CS + 8) = 1;
    MEM32(CS + 4) = 0;
    park(&older, NULL, TID_B, &cond_b);
    cs_waiter_list = &older;
    cs_waiters = 1;
    cs_leave(CS);
    CHECK(MEM32(CS + 12) == TID_B);
    CHECK(MEM32(CS + 8) == 1);
    CHECK(MEM32(CS + 4) == 0);
    CHECK(older.released == 2);
    CHECK(signal_count == 1 && signaled[0] == &cond_b && broadcast_count == 0);
    CHECK(lock_entries == 1 && lock_depth == 0);

    /* B registered first, then C. The recursive leave hands nothing; the final leave wakes only B. */
    reset_sync();
    current_tid = TID_A;
    MEM32(CS + 12) = TID_A;
    MEM32(CS + 8) = 2;
    MEM32(CS + 4) = 1;
    park(&older, NULL, TID_B, &cond_b);
    park(&newer, &older, TID_C, &cond_c);
    cs_waiter_list = &newer;
    cs_waiters = 2;
    cs_leave(CS);
    CHECK(MEM32(CS + 12) == TID_A);
    CHECK(MEM32(CS + 8) == 1);
    CHECK(MEM32(CS + 4) == 0);
    CHECK(older.released == 0 && newer.released == 0);
    CHECK(signal_count == 0 && broadcast_count == 0 && lock_entries == 0);
    cs_leave(CS);
    CHECK(MEM32(CS + 12) == TID_B);
    CHECK(MEM32(CS + 8) == 1);
    CHECK(MEM32(CS + 4) == 0);
    CHECK(older.released == 2 && newer.released == 0);
    CHECK(signal_count == 1 && signaled[0] == &cond_b && broadcast_count == 0);
    CHECK(cs_legacy_waiters == 0 && lock_depth == 0);

    /* No parked thread: the section is free and the wake path does not run. */
    reset_sync();
    current_tid = TID_A;
    cs_enter(FREE_CS);
    CHECK(MEM32(FREE_CS + 12) == TID_A && MEM32(FREE_CS + 8) == 1 && MEM32(FREE_CS + 4) == 0);
    CHECK(lock_entries == 0 && blocks_begun == 0);
    cs_leave(FREE_CS);
    CHECK(MEM32(FREE_CS + 12) == 0);
    CHECK(MEM32(FREE_CS + 4) == 0xFFFFFFFFu);
    CHECK(MEM32(FREE_CS + 8) == 0);
    CHECK(lock_entries == 0 && signal_count == 0 && broadcast_count == 0);

    /* B waits in cs_enter. A's leave sets released to 2, and B returns with recursion still 1. */
    reset_sync();
    current_tid = TID_A;
    cs_enter(CS);
    CHECK(MEM32(CS + 12) == TID_A && MEM32(CS + 8) == 1 && MEM32(CS + 4) == 0);
    current_tid = TID_B;
    t_self = &thread_storage;
    handoff_on_wait = 1;
    wait_cs = CS;
    leave_tid = TID_A;
    cs_enter(CS);
    CHECK(saw_released_owner == 1);
    CHECK(MEM32(CS + 12) == TID_B);
    CHECK(MEM32(CS + 8) == 1); /* not 2: the wake did not fall through into the owner's recursion bump */
    CHECK(MEM32(CS + 4) == 0);
    CHECK(cs_waiter_list == NULL && cs_waiters == 0 && cs_legacy_waiters == 0);
    CHECK(blocks_begun == 1 && blocks_ended == 1);
    CHECK(signal_count == 1 && signaled[0] == &thread_storage.cs_cond_self && broadcast_count == 0);
    puts("PASS critical-section hand-over");
    return 0;
}
"""

STAT_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define ERROR_FILE_NOT_FOUND_ 2u
#define ERROR_PATH_NOT_FOUND_ 3u
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
struct stat { int st_mode; };
static uint32_t listing_generation;
static unsigned lookups;
static int bump_on_lookup;
static uint32_t lookup_error;
static const char *last_path;
static unsigned lock_depth, lock_entries;
/* files.c: the listing cache's lock and its inotify drain (an external edit of a watched folder bumps the generation). */
static pthread_mutex_t listing_lock;
static int pending_inotify;
static unsigned drains;
static void listing_watch_drain(void)
{
    ++drains;
    if (pending_inotify) { pending_inotify = 0; __atomic_add_fetch(&listing_generation, 1, __ATOMIC_RELEASE); }
}
@STAT_DATA@
static void pthread_mutex_lock(pthread_mutex_t *lock)
{
    CHECK((lock == &stat_miss_lock || lock == &listing_lock) && lock_depth == 0);
    lock_depth = 1;
    ++lock_entries;
}
static void pthread_mutex_unlock(pthread_mutex_t *lock)
{
    CHECK((lock == &stat_miss_lock || lock == &listing_lock) && lock_depth == 1);
    lock_depth = 0;
}
static uint32_t stat_guest_path_lookup(const char *path, struct stat *st)
{
    ++lookups;
    last_path = path;
    if (st) st->st_mode = 0;
    if (bump_on_lookup) {
        bump_on_lookup = 0;
        __atomic_add_fetch(&listing_generation, 1, __ATOMIC_RELEASE);
    }
    return lookup_error;
}
@STAT_FUNCTIONS@
static void reset_cache(void)
{
    memset(stat_misses, 0, sizeof stat_misses);
    __atomic_store_n(&listing_generation, 0, __ATOMIC_RELAXED);
    lookups = 0;
    bump_on_lookup = 0;
    lookup_error = ERROR_FILE_NOT_FOUND_;
    last_path = NULL;
    lock_depth = lock_entries = 0;
}
int main(void)
{
    struct stat st;
    /* A repeated miss is answered from the slot and does not call the lookup again. */
    reset_cache();
    CHECK(stat_guest_path("DefUser.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 1 && last_path && !strcmp(last_path, "DefUser.ini"));
    CHECK(stat_guest_path("DefUser.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 1);
    CHECK(stat_guest_path("other.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 2);
    /* Dropping a listing invalidates the slot. */
    __atomic_add_fetch(&listing_generation, 1, __ATOMIC_RELEASE);
    CHECK(stat_guest_path("DefUser.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 3);
    CHECK(stat_guest_path("DefUser.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 3);
    /* A hit drains inotify first: an external create in a watched folder makes the next probe look again. */
    CHECK(drains >= 5);
    pending_inotify = 1;
    CHECK(stat_guest_path("DefUser.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 4);

    /* A path that exists is never stored. */
    reset_cache();
    lookup_error = 0;
    CHECK(stat_guest_path("found.ini", &st) == 0);
    CHECK(stat_guest_path("found.ini", &st) == 0);
    CHECK(lookups == 2);

    /* The slot holds STAT_MISS_PATH bytes including the NUL, so that length bypasses it. */
    reset_cache();
    char edge[STAT_MISS_PATH];
    char big[STAT_MISS_PATH + 1];
    memset(edge, 'e', STAT_MISS_PATH - 1);
    edge[STAT_MISS_PATH - 1] = 0;
    memset(big, 'p', STAT_MISS_PATH);
    big[STAT_MISS_PATH] = 0;
    CHECK(strlen(edge) == STAT_MISS_PATH - 1 && strlen(big) == STAT_MISS_PATH);
    CHECK(stat_guest_path(edge, &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(stat_guest_path(edge, &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 1);
    unsigned before = lookups;
    CHECK(stat_guest_path(big, &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(stat_guest_path(big, &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == before + 2);
    CHECK(stat_guest_path(edge, &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == before + 2); /* the long miss did not evict the short one */

    /* The generation moved while the lookup ran, so the answer is not stored. */
    reset_cache();
    bump_on_lookup = 1;
    CHECK(stat_guest_path("race.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 1 && bump_on_lookup == 0);
    CHECK(stat_guest_path("race.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 2);
    CHECK(stat_guest_path("race.ini", &st) == ERROR_FILE_NOT_FOUND_);
    CHECK(lookups == 2);
    CHECK(lock_depth == 0 && lock_entries > 0);
    puts("PASS negative stat cache");
    return 0;
}
"""


class Perf20261125Test(unittest.TestCase):
    def test_critical_section_handover(self):
        source = (ROOT / "android/host/threads.c").read_text(encoding="utf-8")
        state = snippet(source, "static pthread_mutex_t cs_lock", "static uint32_t cs_effective_spin")
        bodies = "\n".join(production(source, signature) for signature in (
            "static void cs_enter(uint32_t cs)", "static void cs_leave(uint32_t cs)"))
        program = CS_HARNESS.replace("@CS_STATE@", state).replace("@CS_FUNCTIONS@", bodies)
        self.assertIn("PASS critical-section hand-over", run_harness(self, "cs-handover", program))

    def test_negative_stat_cache(self):
        source = (ROOT / "android/host/files.c").read_text(encoding="utf-8")
        data = snippet(source, "#define STAT_MISS_SLOTS", "static uint32_t stat_miss_slot(")
        bodies = "\n".join(production(source, signature) for signature in (
            "static uint32_t stat_miss_slot(", "static uint32_t stat_guest_path("))
        program = STAT_HARNESS.replace("@STAT_DATA@", data).replace("@STAT_FUNCTIONS@", bodies)
        self.assertIn("PASS negative stat cache", run_harness(self, "stat-miss", program))

    def test_shader_warmup_common_recipes_then_usual_order(self):
        """Recipes in every capture come first; the rest keep build()'s usual order.

        common_first is a stable partition of the old list: gameplay recipes sorted by
        the size of their frame's burst (descending), then loading records (frame at or
        below the loaded frame). With this fixture the common menu recipe is tag 2; the
        busy later frame (tag 4, recorded twice) leads the rest, the setting-specific
        menus (tags 1 and 5) come last. The background warm-up makes every recipe, so the
        order only decides which ones are ready first. (Grok's test of 2026-11-25 found
        the first version of the build() comment promising first-use order; the comment
        was corrected to this behaviour.)
        """
        menu_low, common, early, later, menu_high, last = (recipe(tag) for tag in (1, 2, 3, 4, 5, 6))
        low = capture([(2, menu_low), (3, common), (11, early), (50, later), (50, later)])
        high = capture([(1, menu_high), (2, common), (60, last)])
        _, tags = parse_warmup(warmup.build([(low, 10), (high, 10)], [], [], limit=10, common_first=True))
        self.assertEqual(tags, [2, 4, 3, 6, 1, 5])

    def test_shader_warmup_drops_duplicates_and_applies_limit(self):
        hot, cold = recipe(1), recipe(2)
        # Frame 9 is recorded three times, so it outranks frame 4; the copies collapse to one recipe.
        cap = capture([(4, cold), (9, hot), (9, hot), (3, hot)])
        _, tags = parse_warmup(warmup.build([(cap, 0)], [], [], limit=10, common_first=False))
        self.assertEqual(tags, [1, 2])
        _, limited = parse_warmup(warmup.build([(cap, 0)], [], [], limit=1, common_first=True))
        self.assertEqual(limited, [1])
        lines = [f"GLES first use frame {20 + i}: vs {i:016x} ps {i:016x}" for i in range(1, 8)]
        gles = "\n".join(lines + [lines[0]]).encode()
        pairs, tags = parse_warmup(warmup.build([(cap, 0)], [], [(gles, 0)], limit=3, common_first=False))
        self.assertEqual(len(tags), 2)
        self.assertEqual(len(pairs), 3)
        self.assertEqual(pairs[0], struct.pack("<QQ", 1, 1))

    def test_shader_warmup_default_keeps_limit_512(self):
        uniques = [recipe(tag) for tag in range(1, 514)]
        cap = capture([(frame, body) for frame, body in enumerate(uniques, start=1)])
        default = warmup.build([(cap, 0)], [], [])
        pinned = warmup.build([(cap, 0)], [], [], warmup.LIMIT, False)
        self.assertEqual(warmup.LIMIT, 512)
        self.assertEqual(default, pinned)
        pairs, tags = parse_warmup(default)
        self.assertEqual(tags, list(range(1, 513)))
        self.assertEqual(len(pairs), 512)
        # A recipe shared by both captures but stored as a loading record sits past the old 512.
        # The default call leaves it out; common_first keeps it and still stops at 512.
        common = recipe(9000)
        gameplay = [recipe(tag) for tag in range(1, 513)]
        low = capture([(100 + frame, body) for frame, body in enumerate(gameplay)] + [(1, common)])
        high = capture([(1, common)])
        _, plain = parse_warmup(warmup.build([(low, 50), (high, 50)], [], []))
        _, promoted = parse_warmup(warmup.build([(low, 50), (high, 50)], [], [], common_first=True))
        self.assertEqual(plain, list(range(1, 513)))
        self.assertEqual(promoted[0], 9000)
        self.assertEqual(promoted[1:], list(range(1, 512)))
        self.assertEqual(len(promoted), 512)


if __name__ == "__main__":
    unittest.main()
