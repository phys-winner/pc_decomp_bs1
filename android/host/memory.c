/* The guest address space: one 4 GiB reservation, a Win32-style page map
 * (64 KiB allocation granularity, reserve/commit, protection) and Win32 heaps
 * built on dlmalloc mspaces whose system memory comes from the same map.
 *
 * The guest's page stays 4 KiB on every kernel. With 16 KiB host pages
 * (Android 15+ devices) a host page holds four guest pages and gets the union
 * of their protections: guest accesses are checked against the guest map
 * (range_access) and a guest fault is fatal either way, so all that is lost
 * is the trap of a stray access next to a committed page. */
#define _GNU_SOURCE
#include "host.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGES (1u << 20)               /* 4 GiB / 4 KiB */
#define GRANULES (1u << 16)            /* 4 GiB / 64 KiB */

enum { ST_FREE = 0, ST_RESERVE = 1, ST_COMMIT = 2 };
#define MEM_COMMIT_ 0x1000u
#define MEM_RESERVE_ 0x2000u
#define MEM_DECOMMIT_ 0x4000u
#define MEM_RELEASE_ 0x8000u
#define MEM_TOP_DOWN_ 0x100000u
#define MEM_PRIVATE_ 0x20000u
#define MEM_IMAGE_ 0x1000000u

#define PAGE_NOACCESS_ 0x01u
#define PAGE_READONLY_ 0x02u
#define PAGE_READWRITE_ 0x04u
#define PAGE_WRITECOPY_ 0x08u
#define PAGE_EXECUTE_ 0x10u
#define PAGE_EXECUTE_READ_ 0x20u
#define PAGE_EXECUTE_READWRITE_ 0x40u
#define PAGE_EXECUTE_WRITECOPY_ 0x80u
#define PAGE_GUARD_ 0x100u

static uint8_t page_state[PAGES];
static uint16_t page_protect[PAGES];   /* Win32 PAGE_* of committed pages */
static uint32_t page_alloc[PAGES];     /* allocation base (VA >> 12), 0 if free */
static uint16_t alloc_protect[PAGES];  /* initial protection, valid at the base */
static uint8_t granule_used[GRANULES];
/* What made an allocation (at its base granule, for the census): the game's VirtualAlloc, a heap's segment,
 * an image or other fixed claim, a file view. */
enum { AK_GUEST = 0, AK_HEAP = 1, AK_CLAIM = 2, AK_FILE = 3 };
static uint8_t alloc_kind[GRANULES];
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t host_pages = 1;        /* guest pages per host page: 1 (4 KiB) or 4 (16 KiB) */

/* The last VOID_VM_EVENTS allocations, commits, decommits and releases with the host return addresses of their
 * callers (lifted functions name the guest code), so a fault report can tell what made a page inaccessible.
 * Written under vm_lock; reports read it without the lock. */
static void_vm_event vm_events[VOID_VM_EVENTS];
static uint32_t vm_event_seq;
static __thread uintptr_t vm_stack_lo, vm_stack_hi;

static __attribute__((noinline)) void vm_record(char op, uint32_t va, uint32_t size)
{
    if (!vm_stack_hi) {
        pthread_attr_t attr;
        void *lo = NULL;
        size_t bytes = 0;
        vm_stack_hi = 1;   /* unknown: no walk */
        if (!pthread_getattr_np(pthread_self(), &attr)) {
            if (!pthread_attr_getstack(&attr, &lo, &bytes) && bytes) {
                vm_stack_lo = (uintptr_t)lo;
                vm_stack_hi = (uintptr_t)lo + bytes;
            }
            pthread_attr_destroy(&attr);
        }
    }
    void_vm_event *e = &vm_events[vm_event_seq % VOID_VM_EVENTS];
    memset(e, 0, sizeof *e);
    e->seq = ++vm_event_seq;
    e->op = op;
    e->va = va;
    e->size = size;
    e->tid = gettid();
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0), previous = 0;
    for (unsigned i = 0; i < VOID_VM_FRAMES; ++i) {
        if ((fp & 15) || fp <= previous || fp < vm_stack_lo || fp + 2 * sizeof(uintptr_t) > vm_stack_hi) break;
        const uintptr_t *pair = (const uintptr_t *)fp;
        e->frames[i] = pair[1];
        previous = fp;
        fp = pair[0];
    }
}

const void_vm_event *void_vm_events(uint32_t *seq)
{
    *seq = vm_event_seq;
    return vm_events;
}

/* Lock-free (fault reports): the state of the page holding va and the extent of its allocation. */
void void_vm_page_info(uint32_t va, void_vm_page *out)
{
    uint32_t page = va >> 12;
    memset(out, 0, sizeof *out);
    out->state = page_state[page];
    out->protect = page_protect[page];
    if (out->state == ST_FREE) return;
    uint32_t base = page_alloc[page], q = base;
    while (q < PAGES && q - base < (1u << 19) && page_state[q] != ST_FREE && page_alloc[q] == base) ++q;
    out->base = base << 12;
    out->size = (q - base) << 12;
    out->kind = alloc_kind[base >> 4];
    out->alloc_protect = alloc_protect[base];
}

static uint32_t guest_user_top = GUEST_USER_TOP;
uint32_t void_vm_user_top(void) { return guest_user_top; }
uint32_t void_vm_available(void)
{
    uint32_t free_granules = 0;
    pthread_mutex_lock(&vm_lock);
    for (uint32_t g = 1; g < (guest_user_top >> 16); ++g) free_granules += !granule_used[g];
    pthread_mutex_unlock(&vm_lock);
    return free_granules << 16;
}

int void_guest_reserve(void)
{
    const char *large_address = getenv("PORT_OPTION_LARGE_ADDRESS_AWARE");
    guest_user_top = large_address && (!strcmp(large_address, "1") || !strcmp(large_address, "true"))
        ? 0xFFFE0000u : GUEST_USER_TOP;
    long size = sysconf(_SC_PAGESIZE);
    if (size > (long)GUEST_PAGE && size <= (long)GUEST_GRANULARITY && !(size & (size - 1)))
        host_pages = (uint32_t)size / GUEST_PAGE;
    else if (size != (long)GUEST_PAGE) {
        void_log("[void] unsupported host page size %ld", size);
        return 0;
    }
    void *want = (void *)(uintptr_t)VOID_GUEST_BASE;
    void *got = mmap(want, VOID_GUEST_SPAN, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (got == MAP_FAILED) {
        void_log("[void] guest window reservation failed: %s", strerror(errno));
        return 0;
    }
    if (got != want) {
        void_log("[void] guest window landed at %p instead of %p", got, want);
        munmap(got, VOID_GUEST_SPAN);
        return 0;
    }
    /* Null guard: the first 64 KiB are never allocatable, as on Windows. */
    granule_used[0] = 1;
    /* Synthetic thunk VAs: reserved, never accessible. */
    for (uint32_t g = GUEST_THUNK_BASE >> 16; g < (GUEST_THUNK_LIMIT >> 16); ++g) granule_used[g] = 1;
    /* Everything above the user top stays unavailable. */
    for (uint32_t g = guest_user_top >> 16; g < GRANULES; ++g) granule_used[g] = 1;
    void_log("[void] guest window %p..%p reserved, host page %u KiB", want,
             (void *)((uintptr_t)want + VOID_GUEST_SPAN), host_pages * 4);
    return 1;
}

static int host_prot(uint32_t protect)
{
    if (protect & PAGE_GUARD_) return PROT_NONE;
    switch (protect & 0xFFu) {
    case PAGE_NOACCESS_: return PROT_NONE;
    case PAGE_READONLY_: case PAGE_EXECUTE_: case PAGE_EXECUTE_READ_: return PROT_READ;
    default: return PROT_READ | PROT_WRITE;
    }
}

static int valid_protect(uint32_t protect)
{
    switch (protect & ~PAGE_GUARD_ & 0xFFFu) {
    case PAGE_NOACCESS_: case PAGE_READONLY_: case PAGE_READWRITE_: case PAGE_WRITECOPY_:
    case PAGE_EXECUTE_: case PAGE_EXECUTE_READ_: case PAGE_EXECUTE_READWRITE_: case PAGE_EXECUTE_WRITECOPY_:
        return 1;
    default:
        return 0;
    }
}

static uint32_t find_free(uint32_t size, int top_down)
{
    uint32_t need = (uint32_t)(((uint64_t)size + GUEST_GRANULARITY - 1) / GUEST_GRANULARITY);
    if (!need) return 0;
    uint32_t limit = guest_user_top >> 16;
    if (top_down) {
        uint32_t run = 0;
        for (uint32_t g = limit; g-- > 1;) {
            run = granule_used[g] ? 0 : run + 1;
            if (run == need) return g << 16;
        }
    } else {
        uint32_t run = 0;
        for (uint32_t g = 1; g < limit; ++g) {
            run = granule_used[g] ? 0 : run + 1;
            if (run == need) return (g - need + 1) << 16;
        }
    }
    return 0;
}

static void set_pages(uint32_t first, uint32_t count, uint8_t state, uint16_t protect, uint32_t base_page)
{
    for (uint32_t p = first; p < first + count; ++p) {
        page_state[p] = state;
        page_protect[p] = protect;
        page_alloc[p] = base_page;
    }
}

/* The host protection of the host page holding guest page p: the union of its guest
 * pages', where the pages [first, last] are about to get the host protection prot
 * (0: decommitted). */
static int host_page_prot(uint32_t p, uint32_t first, uint32_t last, int prot)
{
    int out = 0;
    for (uint32_t q = p - p % host_pages, end = q + host_pages; q < end; ++q) {
        if (q >= first && q <= last) out |= prot;
        else if (page_state[q] == ST_COMMIT) out |= host_prot(page_protect[q]);
    }
    return out;
}

/* Gives the host pages over the guest pages [first, last] their protection as if those
 * pages had prot (0: not committed), in runs of equal protection. Caller holds vm_lock
 * and updates the guest map after a success. */
static int host_protect(uint32_t first, uint32_t last, int prot)
{
    uint32_t from = first - first % host_pages, to = last + host_pages - 1 - last % host_pages;
    for (uint32_t run = from; run <= to;) {
        int want = host_page_prot(run, first, last, prot);
        uint32_t end = run + host_pages;
        while (end <= to && host_page_prot(end, first, last, prot) == want) end += host_pages;
        if (mprotect(GPTR(run << 12), (size_t)(end - run) << 12, want)) {
            void_log("[void] mprotect %08X+%08X failed: %s", run << 12, (end - run) << 12, strerror(errno));
            return 0;
        }
        run = end;
    }
    return 1;
}

static int commit_range(uint32_t address, uint32_t size, uint32_t protect)
{
    return host_protect(address >> 12, (uint32_t)(((uint64_t)address + size - 1) >> 12), host_prot(protect));
}

/* Whether the host page holding guest page p has committed guest pages outside [first, last]. */
static int host_page_shared(uint32_t p, uint32_t first, uint32_t last)
{
    for (uint32_t q = p - p % host_pages, end = q + host_pages; q < end; ++q)
        if ((q < first || q > last) && page_state[q] == ST_COMMIT) return 1;
    return 0;
}

/* Decommits the guest pages [first, last]: committed again they read as zero, as on
 * Windows. Whole host pages are dropped; guest pages sharing a host page with
 * committed ones are cleared. Caller holds vm_lock and updates the guest map. */
static void decommit_range(uint32_t first, uint32_t last)
{
    uint32_t from = first - first % host_pages, to = last + host_pages - 1 - last % host_pages;
    for (uint32_t run = from; run <= to;) {
        uint32_t end = run + host_pages;
        if (host_page_shared(run, first, last)) {
            mprotect(GPTR(run << 12), (size_t)host_pages << 12, PROT_READ | PROT_WRITE);
            for (uint32_t q = run; q < end; ++q)
                if (q >= first && q <= last && page_state[q] == ST_COMMIT) memset(GPTR(q << 12), 0, GUEST_PAGE);
        } else {
            while (end <= to && !host_page_shared(end, first, last)) end += host_pages;
            madvise(GPTR(run << 12), (size_t)(end - run) << 12, MADV_DONTNEED);
        }
        run = end;
    }
    host_protect(first, last, PROT_NONE);
}

/* Reserve [address, address+size) as one allocation. Caller holds vm_lock. */
static int reserve_exact(uint32_t address, uint32_t size, uint32_t protect)
{
    if (!size || (uint64_t)address + size > guest_user_top) return 0;
    uint32_t g0 = address >> 16, g1 = (uint32_t)(((uint64_t)address + size + GUEST_GRANULARITY - 1) >> 16);
    if ((address & (GUEST_GRANULARITY - 1)) || g1 > (guest_user_top >> 16) || g1 <= g0) return 0;
    for (uint32_t g = g0; g < g1; ++g) if (granule_used[g]) return 0;
    for (uint32_t g = g0; g < g1; ++g) granule_used[g] = 1;
    alloc_kind[g0] = AK_GUEST;
    uint32_t pages = (uint32_t)(((uint64_t)size + GUEST_PAGE - 1) / GUEST_PAGE);
    set_pages(address >> 12, pages, ST_RESERVE, 0, address >> 12);
    alloc_protect[address >> 12] = (uint16_t)protect;
    return 1;
}

uint32_t void_vm_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect)
{
    if (!size || !(type & (MEM_COMMIT_ | MEM_RESERVE_)) || !valid_protect(protect)) {
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    if ((uint64_t)address + size > guest_user_top) {
        void_set_last_error(address ? ERROR_INVALID_ADDRESS_ : ERROR_NOT_ENOUGH_MEMORY_);
        return 0;
    }
    pthread_mutex_lock(&vm_lock);
    uint32_t result = 0;
    /* Windows implicitly reserves when MEM_COMMIT chooses its own address. */
    if ((type & MEM_RESERVE_) || !address) {
        uint32_t base = address & ~(GUEST_GRANULARITY - 1);
        uint32_t span = (uint32_t)((((uint64_t)address + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1)) - base);
        if (!address) {
            span = (uint32_t)(((uint64_t)size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1));
            base = find_free(span, (type & MEM_TOP_DOWN_) != 0);
            if (!base) { pthread_mutex_unlock(&vm_lock); void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); return 0; }
        }
        if (!reserve_exact(base, span, protect)) {
            pthread_mutex_unlock(&vm_lock);
            void_set_last_error(ERROR_INVALID_ADDRESS_);
            return 0;
        }
        if (type & MEM_COMMIT_) {
            if (!commit_range(base, span, protect)) {
                pthread_mutex_unlock(&vm_lock);
                void_vm_free(base, 0, MEM_RELEASE_);
                void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
                return 0;
            }
            set_pages(base >> 12, span >> 12, ST_COMMIT, (uint16_t)protect, base >> 12);
        }
        result = base;
        vm_record((type & MEM_COMMIT_) ? 'A' : 'r', base, span);
    } else {
        /* Commit inside an existing reservation. */
        uint32_t first = address >> 12;
        uint32_t last = (uint32_t)(((uint64_t)address + size - 1) >> 12);
        for (uint32_t p = first; p <= last; ++p) {
            if (page_state[p] == ST_FREE || page_alloc[p] != page_alloc[first]) {
                pthread_mutex_unlock(&vm_lock);
                void_set_last_error(ERROR_INVALID_ADDRESS_);
                return 0;
            }
        }
        uint32_t start = first << 12, span = (last - first + 1) << 12;
        if (!commit_range(start, span, protect)) { pthread_mutex_unlock(&vm_lock); return 0; }
        for (uint32_t p = first; p <= last; ++p) { page_state[p] = ST_COMMIT; page_protect[p] = (uint16_t)protect; }
        result = start;
        vm_record('C', start, span);
    }
    pthread_mutex_unlock(&vm_lock);
    return result;
}

int void_vm_claim(uint32_t address, uint32_t size, uint32_t protect)
{
    if (!size || !valid_protect(protect) || (uint64_t)address + size > guest_user_top) return 0;
    pthread_mutex_lock(&vm_lock);
    uint32_t span = (uint32_t)(((uint64_t)size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1));
    int reserved = reserve_exact(address, span, protect);
    int ok = reserved && commit_range(address, span, protect);
    if (ok) { set_pages(address >> 12, span >> 12, ST_COMMIT, (uint16_t)protect, address >> 12); alloc_kind[address >> 16] = AK_CLAIM; }
    pthread_mutex_unlock(&vm_lock);
    if (reserved && !ok) void_vm_free(address, 0, MEM_RELEASE_);
    return ok;
}

int void_vm_free(uint32_t address, uint32_t size, uint32_t type)
{
    if (address >= guest_user_top || (size && (uint64_t)address + size > guest_user_top)) {
        void_set_last_error(ERROR_INVALID_ADDRESS_);
        return 0;
    }
    pthread_mutex_lock(&vm_lock);
    uint32_t page = address >> 12;
    if (type & MEM_RELEASE_) {
        if (size || (address & 0xFFFu) || page_state[page] == ST_FREE || page_alloc[page] != page) {
            pthread_mutex_unlock(&vm_lock);
            void_set_last_error(ERROR_INVALID_PARAMETER_);
            return 0;
        }
        uint32_t p = page;
        while (p < PAGES && page_state[p] != ST_FREE && page_alloc[p] == page) ++p;
        uint32_t span = (p - page) << 12;
        void *host = GPTR(address);
        mmap(host, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        set_pages(page, p - page, ST_FREE, 0, 0);
        vm_record('R', address, span);
        uint32_t g1 = (uint32_t)(((uint64_t)address + span + GUEST_GRANULARITY - 1) >> 16);
        for (uint32_t g = address >> 16; g < g1; ++g) granule_used[g] = 0;
    } else if (type & MEM_DECOMMIT_) {
        uint32_t first = page;
        uint32_t last = size ? (uint32_t)(((uint64_t)address + size - 1) >> 12) : first;
        if (!size) {
            uint32_t base = page_alloc[page];
            first = base;
            last = base;
            while (last + 1 < PAGES && page_state[last + 1] != ST_FREE && page_alloc[last + 1] == base) ++last;
        }
        for (uint32_t p = first; p <= last; ++p) {
            if (page_state[p] == ST_FREE) { pthread_mutex_unlock(&vm_lock); void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
        }
        decommit_range(first, last);
        for (uint32_t p = first; p <= last; ++p) { page_state[p] = ST_RESERVE; page_protect[p] = 0; }
        vm_record('D', first << 12, (last - first + 1) << 12);
    } else {
        pthread_mutex_unlock(&vm_lock);
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

int void_vm_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old)
{
    if (!size || !valid_protect(protect)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    if ((uint64_t)address + size > guest_user_top) { void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
    pthread_mutex_lock(&vm_lock);
    uint32_t first = address >> 12, last = (uint32_t)(((uint64_t)address + size - 1) >> 12);
    for (uint32_t p = first; p <= last; ++p) {
        if (page_state[p] != ST_COMMIT) { pthread_mutex_unlock(&vm_lock); void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
    }
    if (old) *old = page_protect[first];
    if (!host_protect(first, last, host_prot(protect))) {
        pthread_mutex_unlock(&vm_lock);
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    for (uint32_t p = first; p <= last; ++p) page_protect[p] = (uint16_t)protect;
    if (protect == PAGE_NOACCESS_ || (protect & PAGE_GUARD_)) vm_record('P', first << 12, (last - first + 1) << 12);
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

uint32_t void_vm_query(uint32_t address, uint32_t info)
{
    if (address >= guest_user_top || !void_vm_writable(info, 28)) {
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    pthread_mutex_lock(&vm_lock);
    uint32_t limit = guest_user_top >> 12;
    uint32_t page = address >> 12;
    uint8_t state = page_state[page];
    uint32_t base_page = page_alloc[page];
    uint16_t protect = page_protect[page];
    /* Thunks occupy allocator granules, but remain inaccessible in the page map. */
    int thunk = page >= (GUEST_THUNK_BASE >> 12) && page < (GUEST_THUNK_LIMIT >> 12);
    if (thunk) {
        state = ST_RESERVE;
        base_page = GUEST_THUNK_BASE >> 12;
        protect = 0;
        if (limit > (GUEST_THUNK_LIMIT >> 12)) limit = GUEST_THUNK_LIMIT >> 12;
    } else if (page < (GUEST_THUNK_BASE >> 12) && limit > (GUEST_THUNK_BASE >> 12)) {
        limit = GUEST_THUNK_BASE >> 12;
    }
    uint32_t p = thunk ? limit : page;
    while (p < limit && page_state[p] == state && page_alloc[p] == base_page && page_protect[p] == protect) ++p;
    uint32_t region = (p - page) << 12;
    if (state == ST_FREE) {
        /* A free region extends to the next allocated page. */
        p = page;
        while (p < limit && page_state[p] == ST_FREE) ++p;
        region = (p - page) << 12;
    }
    MEM32(info + 0) = page << 12;                                    /* BaseAddress */
    MEM32(info + 4) = state == ST_FREE ? 0 : base_page << 12;        /* AllocationBase */
    MEM32(info + 8) = state == ST_FREE ? 0 : thunk ? PAGE_NOACCESS_ : alloc_protect[base_page]; /* AllocationProtect */
    MEM32(info + 12) = region;                                       /* RegionSize */
    MEM32(info + 16) = state == ST_FREE ? 0x10000u : state == ST_RESERVE ? MEM_RESERVE_ : MEM_COMMIT_;
    MEM32(info + 20) = state == ST_COMMIT ? protect : 0;             /* Protect */
    MEM32(info + 24) = state == ST_FREE ? 0 : MEM_PRIVATE_;          /* Type */
    pthread_mutex_unlock(&vm_lock);
    return 28;
}

/* MapViewOfFile: a fresh reservation with the file mapped over it. */
uint32_t void_vm_map_file(int fd, uint64_t offset, uint32_t size, int writable, int shared)
{
    if (!size || size > guest_user_top) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    /* Win32 views start at a multiple of the 64 KiB granularity, so of any host page too. */
    if (offset % ((uint64_t)host_pages * GUEST_PAGE)) {
        void_log("[void] mmap view at unaligned offset %llX", (unsigned long long)offset);
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    pthread_mutex_lock(&vm_lock);
    uint32_t span = (uint32_t)(((uint64_t)size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1));
    uint32_t base = find_free(span, 0);
    if (!base || !reserve_exact(base, span, writable ? PAGE_READWRITE_ : PAGE_READONLY_)) {
        pthread_mutex_unlock(&vm_lock);
        void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
        return 0;
    }
    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *host = mmap(GPTR(base), span, prot, (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, fd, (off_t)offset);
    if (host == MAP_FAILED) {
        int e = errno;
        set_pages(base >> 12, span >> 12, ST_FREE, 0, 0);
        uint32_t g1 = (uint32_t)(((uint64_t)base + span + GUEST_GRANULARITY - 1) >> 16);
        for (uint32_t g = base >> 16; g < g1; ++g) granule_used[g] = 0;
        pthread_mutex_unlock(&vm_lock);
        void_log("[void] mmap view failed: %s", strerror(e));
        void_set_last_error(ERROR_ACCESS_DENIED_);
        return 0;
    }
    set_pages(base >> 12, span >> 12, ST_COMMIT, (uint16_t)(writable ? PAGE_READWRITE_ : PAGE_READONLY_), base >> 12);
    alloc_kind[base >> 16] = AK_FILE;
    pthread_mutex_unlock(&vm_lock);
    return base;
}

int void_vm_unmap_file(uint32_t address)
{
    return void_vm_free(address & ~(GUEST_PAGE - 1), 0, MEM_RELEASE_);
}

static int range_access(uint32_t address, uint32_t size, int write)
{
    if (!size) return 1;
    uint64_t end = (uint64_t)address + size;
    if (end > guest_user_top) return 0;
    for (uint32_t p = address >> 12; p <= (uint32_t)((end - 1) >> 12); ++p) {
        if (page_state[p] != ST_COMMIT) return 0;
        int prot = host_prot(page_protect[p]);
        if (!(prot & PROT_READ) || (write && !(prot & PROT_WRITE))) return 0;
    }
    return 1;
}

int void_vm_readable(uint32_t address, uint32_t size) { return range_access(address, size, 0); }

/* Committed guest memory in KiB (statistics; committed pages the game never touched take no RAM). */
uint32_t void_vm_committed_kb(void)
{
    uint32_t pages = 0;
    pthread_mutex_lock(&vm_lock);
    for (uint32_t p = 0; p < PAGES; ++p) pages += page_state[p] == ST_COMMIT;
    pthread_mutex_unlock(&vm_lock);
    return pages * (GUEST_PAGE / 1024);
}
int void_vm_writable(uint32_t address, uint32_t size) { return range_access(address, size, 1); }

/* Debugging (script "memfind"): every committed, readable guest address
 * holding the pattern, up to max hits. Patterns crossing a page into an
 * unreadable one are missed. */
int void_vm_find(const uint8_t *pattern, uint32_t length, uint32_t *hits, int max)
{
    int found = 0;
    if (!length) return 0;
    for (uint32_t page = 0; page < PAGES && found < max; ++page) {
        if (page_state[page] != ST_COMMIT || !(host_prot(page_protect[page]) & PROT_READ)) continue;
        uint32_t base = page << 12;
        const uint8_t *bytes = (const uint8_t *)GPTR(base);
        uint32_t span = 4096;
        if (page + 1 < PAGES && page_state[page + 1] == ST_COMMIT && (host_prot(page_protect[page + 1]) & PROT_READ))
            span += length - 1;
        for (uint32_t i = 0; i + length <= span && found < max; ++i)
            if (bytes[i] == pattern[0] && !memcmp(bytes + i, pattern, length)) hits[found++] = base + i;
    }
    return found;
}

/* Debugging: every committed writable guest page (the heaps, the images'
 * data) to a file, as records of {base, size, bytes}; the size in MiB. */
int void_vm_dump(const char *path)
{
    FILE *out = fopen(path, "wb");
    if (!out) return -1;
    uint64_t total = 0;
    pthread_mutex_lock(&vm_lock);
    for (uint32_t page = 0; page < PAGES;) {
        if (page_state[page] != ST_COMMIT || !(host_prot(page_protect[page]) & PROT_WRITE)) { ++page; continue; }
        uint32_t first = page;
        while (page < PAGES && page_state[page] == ST_COMMIT && (host_prot(page_protect[page]) & PROT_WRITE)) ++page;
        uint32_t record[2] = {first << 12, (page - first) << 12};
        fwrite(record, sizeof record, 1, out);
        fwrite(GPTR(record[0]), 1, record[1], out);
        total += record[1];
    }
    pthread_mutex_unlock(&vm_lock);
    fclose(out);
    return (int)(total >> 20);
}

/* ---------------------------------------------------------------- heaps */
#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define USE_LOCKS 1
#define DEFAULT_GRANULARITY (256u * 1024u)
#define DEFAULT_MMAP_THRESHOLD (4u * 1024u * 1024u)
#define MALLOC_INSPECT_ALL 1          /* void_heap_census, void_heap_purge */
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
static void *guest_sys_alloc(size_t size);
static int guest_sys_free(void *p, size_t size);
#define MMAP(s) guest_sys_alloc(s)
#define DIRECT_MMAP(s) guest_sys_alloc(s)
#define MUNMAP(a, s) guest_sys_free((a), (s))
#define PROCEED_ON_ERROR 0
#define ABORT void_fatal("dlmalloc detected heap corruption")
#include "../../reference/ut3/reference/dlmalloc/malloc.c"

static void *guest_sys_alloc(size_t size)
{
    if (size > 0x7FFF0000u) return (void *)MFAIL;
    uint32_t va = void_vm_alloc(0, (uint32_t)size, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
    if (va) alloc_kind[va >> 16] = AK_HEAP;
    return va ? GPTR(va) : (void *)MFAIL;
}

/* The allocation holding `page`: its first page and one past its last. Caller holds vm_lock. */
static void allocation_extent(uint32_t page, uint32_t *first, uint32_t *limit)
{
    uint32_t base = page_alloc[page], q = base;
    while (q < PAGES && page_state[q] != ST_FREE && page_alloc[q] == base) ++q;
    *first = base;
    *limit = q;
}

/* dlmalloc hands system memory back (MUNMAP) as a whole segment from its base or as a segment's top (a trim, from
 * inside the segment to its end), and a segment can span several of our allocations: a new mapping that starts
 * where the segment holding top ends, or ends at another segment's base, is glued onto it, and find_free hands out
 * the lowest free granules, so neighbours are common. Each allocation the range touches is handled by itself
 * (The Sims 2's rule of 7 November 2026, wiki note 02): covered whole, it is released; covered in part, those
 * pages are decommitted, and the allocation is released once none of its pages stay committed. Releasing only a
 * base made every trim inside an allocation fail and left a glued segment's other allocations committed, owned by
 * nobody, for the rest of the session (BioShock, 22 November 2026: 21 MiB of heap segments above the heaps'
 * footprint once the shadow heap returned its segments). A range that is not made of live pages is refused before
 * anything changes; after that the range is always reported returned, since dlmalloc must not reuse pages that
 * are already gone, and a failed release or decommit only leaks. */
static int guest_sys_free(void *p, size_t size)
{
    uint32_t va = GVA(p);
    if (!size || (va & (GUEST_PAGE - 1)) || (uint64_t)va + size > guest_user_top) return -1;
    uint32_t start = va >> 12, end = (uint32_t)(((uint64_t)va + size + GUEST_PAGE - 1) >> 12);
    pthread_mutex_lock(&vm_lock);
    int live = 1;
    for (uint32_t q = start; q < end && live; ++q) live = page_state[q] != ST_FREE;
    pthread_mutex_unlock(&vm_lock);
    if (!live) return -1;
    static int failures;
    for (uint32_t at = start; at < end;) {
        uint32_t first, limit;
        pthread_mutex_lock(&vm_lock);
        allocation_extent(at, &first, &limit);
        pthread_mutex_unlock(&vm_lock);
        uint32_t stop = limit < end ? limit : end;
        int ok;
        if (at == first && stop == limit) {
            ok = void_vm_free(first << 12, 0, MEM_RELEASE_);
        } else {
            ok = void_vm_free(at << 12, (stop - at) << 12, MEM_DECOMMIT_);
            int committed = 0;
            pthread_mutex_lock(&vm_lock);
            for (uint32_t q = first; q < limit && !committed; ++q) committed = page_state[q] == ST_COMMIT;
            pthread_mutex_unlock(&vm_lock);
            if (ok && !committed) ok = void_vm_free(first << 12, 0, MEM_RELEASE_);
        }
        if (!ok && failures++ < 8)
            void_log("[void] heap unmap of %08X+%X: allocation %08X kept (%u failure(s))", at << 12, (stop - at) << 12,
                     first << 12, (unsigned)failures);
        at = limit;
    }
    return 0;
}

/* A heap handle is the guest VA of a small descriptor, so it looks like the
 * pointer-valued HANDLE Windows returns. */
#define MAX_HEAPS 256
#define BLOCK_MAGIC 0x48454150u      /* 'HEAP' */
typedef struct { uint32_t handle; mspace space; } heap_t;
static heap_t heaps[MAX_HEAPS];
static int heap_count;
static uint32_t process_heap;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static heap_t *heap_find(uint32_t handle)
{
    for (int i = 0; i < heap_count; ++i) if (heaps[i].handle == handle && heaps[i].space) return &heaps[i];
    return NULL;
}

uint32_t void_heap_create(uint32_t initial, uint32_t maximum)
{
    (void)initial; (void)maximum;
    pthread_mutex_lock(&heap_lock);
    if (heap_count >= MAX_HEAPS) { pthread_mutex_unlock(&heap_lock); return 0; }
    mspace space = create_mspace(0, 1);
    if (!space) { pthread_mutex_unlock(&heap_lock); return 0; }
    /* The descriptor lives in the new heap itself: a real, readable address. */
    void *descriptor = mspace_calloc(space, 1, 64);
    heap_t *h = &heaps[heap_count++];
    h->space = space;
    h->handle = GVA(descriptor);
    MEM32(h->handle) = 0xEEFFEEFFu;   /* the Windows heap signature field */
    pthread_mutex_unlock(&heap_lock);
    return h->handle;
}

int void_heap_destroy(uint32_t handle)
{
    pthread_mutex_lock(&heap_lock);
    heap_t *h = heap_find(handle);
    if (!h || handle == process_heap) { pthread_mutex_unlock(&heap_lock); return 0; }
    destroy_mspace(h->space);
    h->space = NULL;
    pthread_mutex_unlock(&heap_lock);
    return 1;
}

uint32_t void_process_heap(void)
{
    if (!process_heap) process_heap = void_heap_create(0, 0);
    return process_heap;
}

uint32_t void_heap_alloc(uint32_t handle, uint32_t size, int zero)
{
    heap_t *h = heap_find(handle);
    if (!h || size > 0x7FF00000u) return 0;
    uint8_t *p = (uint8_t *)(zero ? mspace_calloc(h->space, 1, (size_t)size + 8) : mspace_malloc(h->space, (size_t)size + 8));
    if (!p) return 0;
    uint32_t block = GVA(p) + 8;
    MEM32(block - 8) = size;
    MEM32(block - 4) = BLOCK_MAGIC ^ handle;
    return block;
}

static int block_valid(uint32_t handle, uint32_t block)
{
    return block >= 0x10008u && (block & 7u) == 0 && void_vm_readable(block - 8, 8) &&
        MEM32(block - 4) == (BLOCK_MAGIC ^ handle);
}

int void_heap_free(uint32_t handle, uint32_t block)
{
    if (!block) return 1;
    heap_t *h = heap_find(handle);
    if (!h || !block_valid(handle, block)) {
        void_log("[void] HeapFree(%08X, %08X): not a block of this heap (caller %08X)", handle, block, g_cur_func);
        return 0;
    }
    MEM32(block - 4) = 0;
    mspace_free(h->space, GPTR(block - 8));
    return 1;
}

uint32_t void_heap_size(uint32_t handle, uint32_t block)
{
    if (!heap_find(handle) || !block_valid(handle, block)) return 0xFFFFFFFFu;
    return MEM32(block - 8);
}

uint32_t void_heap_realloc(uint32_t handle, uint32_t block, uint32_t size, int zero, int in_place_only)
{
    heap_t *h = heap_find(handle);
    if (!h || !block_valid(handle, block) || size > 0x7FF00000u) return 0;
    uint32_t old = MEM32(block - 8);
    uint8_t *p;
    if (in_place_only) {
        p = (uint8_t *)mspace_realloc_in_place(h->space, GPTR(block - 8), (size_t)size + 8);
        if (!p) return 0;
    } else {
        p = (uint8_t *)mspace_realloc(h->space, GPTR(block - 8), (size_t)size + 8);
        if (!p) return 0;
    }
    uint32_t moved = GVA(p) + 8;
    MEM32(moved - 8) = size;
    MEM32(moved - 4) = BLOCK_MAGIC ^ handle;
    if (zero && size > old) memset(GPTR(moved + old), 0, size - old);
    return moved;
}

uint32_t void_galloc(uint32_t size) { return void_heap_alloc(void_process_heap(), size, 1); }
void void_gfree(uint32_t va) { if (va) void_heap_free(void_process_heap(), va); }

uint32_t void_gstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t va = void_galloc((uint32_t)n);
    if (va) memcpy(GPTR(va), s, n);
    return va;
}

/* ------------------------------------------------------------ census, purge */
static uintptr_t host_page_bytes(void)
{
    static uintptr_t page;
    if (!page) page = (uintptr_t)sysconf(_SC_PAGESIZE);
    return page;
}

typedef struct { uint64_t used, free, whole, purged; uintptr_t page, purge_min; } heap_walk_t;

/* dlmalloc's traversal (mspace_inspect_all, under the heap's lock): a free chunk's [start, end) excludes its
 * bookkeeping and the next chunk's footer, so the host pages wholly inside it hold nothing the allocator reads. */
static void heap_walk_chunk(void *start, void *end, size_t used, void *arg)
{
    heap_walk_t *w = (heap_walk_t *)arg;
    if (used) { w->used += used; return; }
    w->free += (uint64_t)((uintptr_t)end - (uintptr_t)start);
    uintptr_t a = ((uintptr_t)start + w->page - 1) & ~(w->page - 1), b = (uintptr_t)end & ~(w->page - 1);
    if (b <= a) return;
    w->whole += b - a;
    if (w->purge_min && b - a >= w->purge_min) { madvise((void *)a, b - a, MADV_DONTNEED); w->purged += b - a; }
}

/* Gives the host pages wholly inside a heap's free chunks of at least min_bytes back to the system: used again
 * they read as zero, as memory fresh from the system does (calloc clears what it takes from a free chunk).
 * Windows' heaps decommit large free blocks the same way. Returns the bytes released. */
uint64_t void_heap_purge(uint32_t handle, uint32_t min_bytes)
{
    pthread_mutex_lock(&heap_lock);
    heap_t *h = heap_find(handle);
    mspace space = h ? h->space : NULL;
    pthread_mutex_unlock(&heap_lock);
    if (!space) return 0;
    heap_walk_t w = { 0 };
    w.page = host_page_bytes();
    w.purge_min = min_bytes < w.page ? w.page : min_bytes;
    mspace_inspect_all(space, heap_walk_chunk, &w);
    mspace_trim(space, 0);
    return w.purged;
}

/* QA census (r10-memstat-now): per heap of at least 1 MiB its footprint, the bytes in use, free, and free in
 * whole host pages (what void_heap_purge could release); the smaller heaps summed. */
void void_heap_census(void)
{
    char text[2048];
    size_t n = 0;
    uint64_t small_foot = 0, small_used = 0;
    int small = 0;
    text[0] = 0;
    pthread_mutex_lock(&heap_lock);
    for (int i = 0; i < heap_count; ++i) {
        heap_t *h = &heaps[i];
        if (!h->space) continue;
        heap_walk_t w = { 0 };
        w.page = host_page_bytes();
        size_t foot = mspace_footprint(h->space);
        mspace_inspect_all(h->space, heap_walk_chunk, &w);
        if (foot < (1u << 20)) { small_foot += foot; small_used += w.used; ++small; continue; }
        if (n + 128 < sizeof text)
            n += (size_t)snprintf(text + n, sizeof text - n, "%s%08X%s %.1f MiB (used %.1f, free %.1f, %.1f in whole pages)",
                                  n ? "; " : "", h->handle, h->handle == process_heap ? " process" : "", foot / 1048576.0,
                                  w.used / 1048576.0, w.free / 1048576.0, w.whole / 1048576.0);
    }
    pthread_mutex_unlock(&heap_lock);
    void_log("[void] census heaps: %s; %d smaller heaps %.1f MiB (used %.1f)", text, small, small_foot / 1048576.0,
             small_used / 1048576.0);
}

static int guest_page_zero(const uint8_t *page)
{
    const uint64_t *w = (const uint64_t *)page;
    for (unsigned i = 0; i < GUEST_PAGE / 8; i += 8)
        if (w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7]) return 0;
    return 1;
}

/* QA census: the committed guest memory by what made it (the heaps' segments, the game's own VirtualAlloc by
 * the allocation's size, images and other fixed claims, file views): committed, resident (mincore) and resident
 * pages holding only zeros; then the allocations with the most resident memory. Reads every resident page
 * under the address-space lock (~0.1 s for 400 MiB): QA only. */
void void_vm_census(void)
{
    enum { K_HEAP, K_POOL, K_MEDIUM, K_LARGE, K_HUGE, K_CLAIM, K_FILE, K_N };
    static const char *names[K_N] = { "heap segments", "game <=64K", "game <=1M", "game <=16M", "game >16M", "images/claims", "file views" };
    uint64_t committed[K_N] = { 0 }, resident[K_N] = { 0 }, zero[K_N] = { 0 };
    uint32_t count[K_N] = { 0 };
    struct { uint32_t base, span; uint64_t resident, zero; int kind; } top[10];
    int tops = 0;
    const uintptr_t hp = host_page_bytes();
    size_t vec_size = 0;
    unsigned char *vec = NULL;
    pthread_mutex_lock(&vm_lock);
    for (uint32_t p = 16; p < PAGES;) {
        if (page_state[p] == ST_FREE) { ++p; continue; }
        uint32_t base = page_alloc[p], q = p;
        while (q < PAGES && page_state[q] != ST_FREE && page_alloc[q] == base) ++q;
        uint64_t span = (uint64_t)(q - p) * GUEST_PAGE;
        int ak = alloc_kind[base >> 4];
        int kind = ak == AK_HEAP ? K_HEAP : ak == AK_CLAIM ? K_CLAIM : ak == AK_FILE ? K_FILE
                 : span <= 0x10000u ? K_POOL : span <= 0x100000u ? K_MEDIUM : span <= 0x1000000u ? K_LARGE : K_HUGE;
        uintptr_t first = (uintptr_t)GPTR(p << 12);
        uintptr_t start = first & ~(hp - 1), end = (first + span + hp - 1) & ~(hp - 1);
        size_t need = (end - start) / hp;
        if (need > vec_size) { free(vec); vec_size = need; vec = (unsigned char *)malloc(vec_size); }
        int have = vec && !mincore((void *)start, end - start, vec);
        uint64_t res = 0, zer = 0, com = 0;
        for (uint32_t g = p; g < q; ++g) {
            if (page_state[g] != ST_COMMIT) continue;
            com += GUEST_PAGE;
            uint8_t *gp = (uint8_t *)GPTR(g << 12);
            if (!have || !(vec[((uintptr_t)gp - start) / hp] & 1)) continue;
            res += GUEST_PAGE;
            if ((host_prot(page_protect[g]) & PROT_READ) && guest_page_zero(gp)) zer += GUEST_PAGE;
        }
        committed[kind] += com; resident[kind] += res; zero[kind] += zer; ++count[kind];
        int slot = tops < 10 ? tops++ : -1;
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < 10; ++i) if (top[i].resident < top[slot].resident) slot = i;
            if (top[slot].resident >= res) slot = -1;
        }
        if (slot >= 0) { top[slot].base = base << 12; top[slot].span = (uint32_t)span; top[slot].resident = res; top[slot].zero = zer; top[slot].kind = kind; }
        p = q;
    }
    pthread_mutex_unlock(&vm_lock);
    free(vec);
    char text[2048];
    size_t n = 0;
    uint64_t tc = 0, tr = 0, tz = 0;
    text[0] = 0;
    for (int k = 0; k < K_N; ++k) {
        tc += committed[k]; tr += resident[k]; tz += zero[k];
        n += (size_t)snprintf(text + n, sizeof text - n, "; %s %.1f/%.1f/%.1f MiB (%u)", names[k], committed[k] / 1048576.0,
                              resident[k] / 1048576.0, zero[k] / 1048576.0, count[k]);
    }
    void_log("[void] census guest committed/resident/zero %.1f/%.1f/%.1f MiB (host page %u)%s", tc / 1048576.0, tr / 1048576.0,
             tz / 1048576.0, (unsigned)hp, text);
    for (int i = 0; i < tops; ++i)
        for (int j = i + 1; j < tops; ++j)
            if (top[j].resident > top[i].resident) { __typeof__(top[0]) t = top[i]; top[i] = top[j]; top[j] = t; }
    n = 0;
    text[0] = 0;
    for (int i = 0; i < tops && n + 96 < sizeof text; ++i)
        n += (size_t)snprintf(text + n, sizeof text - n, "%s%08X %.1f MiB %s (resident %.1f, zero %.1f)", i ? "; " : "", top[i].base,
                              top[i].span / 1048576.0, names[top[i].kind], top[i].resident / 1048576.0, top[i].zero / 1048576.0);
    void_log("[void] census largest: %s", text);
}
