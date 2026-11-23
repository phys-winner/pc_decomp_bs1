/* The guest heaps of android/host/memory.c (the Win32 page map and dlmalloc mspaces), on the device: memory.c is
 * compiled into this file, so the checks reach guest_sys_free, the page map and the heaps directly. Ported from The
 * Sims 2 (android/tools/heap_check.c, wiki note 02) on 22 November 2026, with BioShock's purge:
 *   1. guest_sys_free on bare allocations, in the shapes dlmalloc unmaps: a top trimmed from inside one allocation
 *      across two neighbours, a head unmapped from its base after such a trim, a segment glued from three neighbours
 *      unmapped from its base, and a range running into free pages (refused, nothing changes);
 *   2. one heap grown by blocks that each need a new mapping, which dlmalloc glues into one segment: with its upper
 *      half freed (trims across allocation boundaries) nothing may stay committed or reserved beyond the footprint,
 *      and HeapDestroy must return every page and reservation;
 *   3. a random HeapAlloc/HeapReAlloc/HeapFree load with void_heap_purge between rounds: every live block keeps its
 *      bytes, a zeroed block is zero although it reuses purged pages, freeing everything and purging leaves little
 *      resident, and destroying the heap returns every page and reservation;
 *   4. the QA census (void_heap_census, void_vm_census) runs.
 * dlmalloc's own corruption checks abort the run. tests/test_heap_release.py builds and runs it. */
#define _GNU_SOURCE
#include "../android/host/memory.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

uint32_t g_cur_func;
static uint32_t last_error;
void void_set_last_error(uint32_t e) { last_error = e; }
void void_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
void void_fatal(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\nFATAL\n"); exit(3); }

static uint32_t rng = 0x9E3779B9u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static double mb(int64_t bytes) { return bytes / 1048576.0; }

/* Committed and reserved guest pages, and the committed ones that are resident. */
static void vm_stats(uint64_t *committed, uint64_t *reserved, uint64_t *resident)
{
    static unsigned char vec[(16u << 20) / 4096u];
    uintptr_t hp = (uintptr_t)sysconf(_SC_PAGESIZE);
    *committed = *reserved = *resident = 0;
    for (uint32_t p = 16; p < PAGES; ++p) {
        if (page_state[p] == ST_RESERVE) *reserved += GUEST_PAGE;
        if (page_state[p] != ST_COMMIT) continue;
        *committed += GUEST_PAGE;
        uintptr_t host = (uintptr_t)GPTR(p << 12) & ~(hp - 1);
        if (!mincore((void *)host, hp, vec) && (vec[0] & 1)) *resident += GUEST_PAGE;
    }
}

static void fill(uint32_t va, uint32_t size, uint32_t seed)
{
    uint8_t *p = (uint8_t *)GPTR(va);
    if (size <= 8192) { for (uint32_t i = 0; i < size; ++i) p[i] = (uint8_t)(seed + i * 131u); return; }
    for (uint32_t i = 0; i < size; i += 4096) p[i] = (uint8_t)(seed + i * 131u);
    for (uint32_t i = size - 64; i < size; ++i) p[i] = (uint8_t)(seed + i * 131u);
}

static int intact(uint32_t va, uint32_t size, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)GPTR(va);
    if (size <= 8192) { for (uint32_t i = 0; i < size; ++i) if (p[i] != (uint8_t)(seed + i * 131u)) return 0; return 1; }
    for (uint32_t i = 0; i < size; i += 4096) if (p[i] != (uint8_t)(seed + i * 131u)) return 0;
    for (uint32_t i = size - 64; i < size; ++i) if (p[i] != (uint8_t)(seed + i * 131u)) return 0;
    return 1;
}

static int zeroed(uint32_t va, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)GPTR(va);
    for (uint32_t i = 0; i < size; ++i) if (p[i]) return 0;
    return 1;
}

static uint32_t random_size(void)
{
    uint32_t r = next() % 100;
    if (r < 60) return 16 + next() % 2048;
    if (r < 85) return 4096 + next() % 65536;
    if (r < 97) return 200000 + next() % 900000;
    return 4u * 1024 * 1024 + next() % (2u * 1024 * 1024);
}

/* ------------------------------------------------ 1. bare allocations */
#define UNIT (256u * 1024u)                /* dlmalloc's mapping granularity here */

static uint32_t pages_in(uint32_t va, uint32_t bytes, uint8_t state)
{
    uint32_t n = 0;
    for (uint32_t p = va >> 12; p < (va + bytes) >> 12; ++p) n += page_state[p] == state;
    return n;
}

static int expect_pages(const char *what, uint32_t va, uint32_t bytes, uint8_t state)
{
    uint32_t n = pages_in(va, bytes, state), all = bytes >> 12;
    if (n == all) return 0;
    printf("FAIL: %s: %u of %u pages at %08X are %s\n", what, n, all, va,
           state == ST_FREE ? "free" : state == ST_RESERVE ? "reserved" : "committed");
    return 1;
}

static int neighbours(uint32_t count, uint32_t *va)
{
    for (uint32_t i = 0; i < count; ++i) {
        va[i] = void_vm_alloc(0, UNIT, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
        if (!va[i]) return 0;
        memset(GPTR(va[i]), 0xA5, UNIT);
    }
    for (uint32_t i = 1; i < count; ++i) if (va[i] != va[i - 1] + UNIT) return 0;
    return 1;
}

static int unmap_shapes(void)
{
    int failures = 0;
    uint64_t c0, r0, x0, c, r, x;
    vm_stats(&c0, &r0, &x0);
    uint32_t va[3];
    if (!neighbours(3, va)) { printf("FAIL: three adjacent allocations were not handed out\n"); return 1; }
    if (guest_sys_free(GPTR(va[0] + UNIT / 2), 2 * UNIT + UNIT / 2)) { printf("FAIL: trim across allocations refused\n"); ++failures; }
    failures += expect_pages("head of the trimmed allocation", va[0], UNIT / 2, ST_COMMIT);
    failures += expect_pages("tail of the trimmed allocation", va[0] + UNIT / 2, UNIT / 2, ST_RESERVE);
    failures += expect_pages("allocations after the trim point", va[1], 2 * UNIT, ST_FREE);
    if (guest_sys_free(GPTR(va[0]), UNIT / 2)) { printf("FAIL: unmap of the head refused\n"); ++failures; }
    failures += expect_pages("allocation whose head was unmapped", va[0], UNIT, ST_FREE);
    if (!neighbours(3, va)) { printf("FAIL: three adjacent allocations were not handed out again\n"); return failures + 1; }
    if (guest_sys_free(GPTR(va[0]), 3 * UNIT)) { printf("FAIL: unmap of a glued segment refused\n"); ++failures; }
    failures += expect_pages("glued segment", va[0], 3 * UNIT, ST_FREE);
    uint32_t one = void_vm_alloc(0, UNIT, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
    if (!one || pages_in(one + UNIT, UNIT, ST_FREE) != UNIT >> 12) {
        printf("FAIL: no free pages after a lone allocation\n");
        ++failures;
    } else {
        if (guest_sys_free(GPTR(one), 2 * UNIT) != -1) { printf("FAIL: a range over free pages was accepted\n"); ++failures; }
        failures += expect_pages("allocation of the refused range", one, UNIT, ST_COMMIT);
    }
    if (one) void_vm_free(one, 0, MEM_RELEASE_);
    vm_stats(&c, &r, &x);
    if (c != c0 || r != r0) {
        printf("FAIL: bare allocations left %.2f MB committed and %.2f MB reserved\n", mb((int64_t)(c - c0)), mb((int64_t)(r - r0)));
        ++failures;
    }
    printf("unmap shapes: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

/* ------------------------------------------------ 2. a glued heap */
#define GLUE_SMALL (200u * 1024u)
#define GLUE_BIG (600u * 1024u)
#define GLUE_BEFORE 4
#define GLUE_BLOCKS 32

static uint32_t glue_size(int i) { return i == GLUE_BEFORE ? GLUE_BIG : GLUE_SMALL; }

static int glued_heap(uint32_t info)
{
    int failures = 0, straddle = 0;
    uint64_t c0, r0, x0, c, r, x;
    vm_stats(&c0, &r0, &x0);
    uint32_t heap = void_heap_create(0, 0);
    uint32_t spacer = void_vm_alloc(0, UNIT, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_), block[GLUE_BLOCKS];
    for (int i = 0; i < GLUE_BLOCKS; ++i) {
        block[i] = void_heap_alloc(heap, glue_size(i), 0);
        if (!block[i]) { printf("FAIL: glued heap: alloc %d\n", i); return failures + 1; }
        fill(block[i], glue_size(i), 0x5Au + (uint32_t)i);
        void_vm_query(block[i], info);
        uint32_t first = MEM32(info + 4);
        void_vm_query(block[i] + glue_size(i) - 1, info);
        straddle += MEM32(info + 4) != first;
    }
    uint64_t grown = mspace_footprint(heap_find(heap)->space);
    if (!straddle) { printf("FAIL: glued heap: no block spans two allocations, dlmalloc glued nothing\n"); ++failures; }
    for (int i = GLUE_BLOCKS - 1; i > GLUE_BEFORE; --i) void_heap_free(heap, block[i]);
    mspace_trim(heap_find(heap)->space, 0);
    for (int i = 0; i <= GLUE_BEFORE; ++i)
        if (!intact(block[i], glue_size(i), 0x5Au + (uint32_t)i)) { printf("FAIL: glued heap: block %d changed\n", i); ++failures; }
    vm_stats(&c, &r, &x);
    uint64_t footprint = mspace_footprint(heap_find(heap)->space);
    int64_t held = (int64_t)(c + r) - (int64_t)(c0 + r0) - UNIT;      /* without the spacer */
    printf("glued heap: %d of %d blocks span two allocations; footprint %.2f MB, after freeing the blocks behind the big one "
           "%.2f MB, held %.2f MB\n", straddle, GLUE_BLOCKS, mb((int64_t)grown), mb((int64_t)footprint), mb(held));
    if (grown - footprint < (2u << 20)) { printf("FAIL: glued heap: dlmalloc trimmed nothing, the trim path was not exercised\n"); ++failures; }
    if (held > (int64_t)(footprint + UNIT)) {
        printf("FAIL: glued heap: %.2f MB committed or reserved beyond the footprint after trims\n", mb(held - (int64_t)footprint));
        ++failures;
    }
    void_heap_destroy(heap);
    void_vm_free(spacer, 0, MEM_RELEASE_);
    vm_stats(&c, &r, &x);
    if (c != c0 || r != r0) {
        printf("FAIL: glued heap: HeapDestroy left %.2f MB committed and %.2f MB reserved\n", mb((int64_t)(c - c0)), mb((int64_t)(r - r0)));
        ++failures;
    } else {
        printf("glued heap destroyed: every page and reservation returned\n");
    }
    return failures;
}

/* ------------------------------------------------ 3. a random load with purges */
typedef struct { uint32_t va, size, seed; } live_t;
#define SLOTS 1500

int main(void)
{
    if (!void_guest_reserve()) { printf("FAIL: guest window\n"); return 2; }
    uint32_t info = void_vm_alloc(0, 4096, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
    int failures = unmap_shapes();
    failures += glued_heap(info);
    uint64_t committed_before, reserved_before, resident_before;
    vm_stats(&committed_before, &reserved_before, &resident_before);
    uint32_t heap = void_heap_create(0, 0);
    uint64_t committed0, reserved0, resident0, purged = 0, zero_checked = 0;
    vm_stats(&committed0, &reserved0, &resident0);
    static live_t live[SLOTS];
    uint64_t peak_resident = 0;
    for (int round = 0; round < 6; ++round) {
        for (int op = 0; op < 20000; ++op) {
            live_t *s = &live[next() % SLOTS];
            uint32_t what = next() % 10;
            if (s->va && !intact(s->va, s->size, s->seed)) { printf("FAIL: block %08X (%u bytes) changed\n", s->va, s->size); ++failures; }
            if (s->va && what < 4) {
                if (!void_heap_free(heap, s->va)) { printf("FAIL: free %08X\n", s->va); ++failures; }
                s->va = 0;
            } else if (s->va && what < 6) {
                uint32_t size = random_size(), keep = size < s->size ? size : s->size;
                uint32_t moved = void_heap_realloc(heap, s->va, size, 0, 0);
                if (!moved) { printf("FAIL: realloc %u\n", size); ++failures; continue; }
                const uint8_t *p = (const uint8_t *)GPTR(moved);
                uint32_t step = s->size > 8192 ? 4096 : 1;
                for (uint32_t i = 0; i < keep; i += step)
                    if (p[i] != (uint8_t)(s->seed + i * 131u)) { printf("FAIL: realloc lost byte %u\n", i); ++failures; break; }
                s->va = moved; s->size = size; s->seed = next();
                fill(s->va, s->size, s->seed);
            } else if (!s->va) {
                s->size = random_size(); s->seed = next();
                int zero = next() & 1;
                s->va = void_heap_alloc(heap, s->size, zero);
                if (!s->va) { printf("FAIL: alloc %u\n", s->size); ++failures; continue; }
                if (zero && s->size <= 65536) {
                    if (!zeroed(s->va, s->size)) { printf("FAIL: zeroed block %08X (%u bytes) is not zero\n", s->va, s->size); ++failures; }
                    zero_checked += s->size;
                }
                fill(s->va, s->size, s->seed);
            }
            /* The shadow heap's sweep purges between frames: here several times per round. */
            if (op % 5000 == 4999) purged += void_heap_purge(heap, 0);
        }
        for (int i = 0; i < SLOTS; ++i)
            if (live[i].va && !intact(live[i].va, live[i].size, live[i].seed)) { printf("FAIL: block %08X changed after purges\n", live[i].va); ++failures; }
        uint64_t committed, reserved, resident;
        vm_stats(&committed, &reserved, &resident);
        if (resident > peak_resident) peak_resident = resident;
        uint64_t live_bytes = 0;
        for (int i = 0; i < SLOTS; ++i) if (live[i].va) live_bytes += live[i].size;
        printf("round %d: live %.1f MB, committed %.1f MB, resident %.1f MB, footprint %.1f MB, purged so far %.1f MB\n", round,
               live_bytes / 1048576.0, committed / 1048576.0, resident / 1048576.0,
               mspace_footprint(heap_find(heap)->space) / 1048576.0, purged / 1048576.0);
    }
    for (int i = 0; i < SLOTS; ++i) {
        if (live[i].va && !intact(live[i].va, live[i].size, live[i].seed)) { printf("FAIL: block %08X changed at the end\n", live[i].va); ++failures; }
        if (live[i].va) void_heap_free(heap, live[i].va);
        live[i].va = 0;
    }
    purged += void_heap_purge(heap, 0);
    uint64_t committed, reserved, resident;
    vm_stats(&committed, &reserved, &resident);
    uint64_t footprint = mspace_footprint(heap_find(heap)->space);
    int64_t held = (int64_t)(committed + reserved) - (int64_t)(committed_before + reserved_before);
    printf("all freed and purged: committed %.1f MB (start %.1f), resident %.1f MB (peak %.1f), footprint %.1f MB, held beyond it "
           "%.1f MB, purged %.1f MB, %.1f MB of zeroed blocks checked\n", committed / 1048576.0, committed0 / 1048576.0,
           resident / 1048576.0, peak_resident / 1048576.0, footprint / 1048576.0, mb(held - (int64_t)footprint), purged / 1048576.0,
           zero_checked / 1048576.0);
    if (resident > resident0 + (4u << 20)) { printf("FAIL: %.1f MB still resident after freeing and purging\n", (resident - resident0) / 1048576.0); ++failures; }
    if (!purged) { printf("FAIL: the purges released nothing\n"); ++failures; }
    /* A trim that ends inside an allocation leaves that allocation's tail reserved (one per segment at most); committed
     * pages beyond the footprint would be pages nobody owns. */
    int64_t committed_held = (int64_t)committed - (int64_t)committed_before;
    if (committed_held > (int64_t)footprint + (int64_t)UNIT) {
        printf("FAIL: %.1f MB committed beyond the footprint\n", mb(committed_held - (int64_t)footprint));
        ++failures;
    }
    void_heap_census();
    void_vm_census();
    void_heap_destroy(heap);
    vm_stats(&committed, &reserved, &resident);
    if (committed != committed_before || reserved != reserved_before) {
        printf("FAIL: HeapDestroy after the random load left %.2f MB committed and %.2f MB reserved\n",
               mb((int64_t)(committed - committed_before)), mb((int64_t)(reserved - reserved_before)));
        ++failures;
    } else {
        printf("random-load heap destroyed: every page and reservation returned\n");
    }
    printf(failures ? "FAILED %d\n" : "PASS guest heaps\n", failures);
    return failures ? 1 : 0;
}
