/* The production mode-2 probe with real protected pages, no graphics/device. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define O_CLOEXEC 0
#ifndef HOST_PAGE
#define HOST_PAGE 4096u
#endif
static uint8_t *fixture_base;
#undef GPTR
#define GPTR(va) ((void *)(fixture_base + (uint32_t)(va)))
#define MEM8(va) (*(uint8_t *)GPTR(va))
#define MEM32(va) (*(uint32_t *)GPTR(va))
static uint32_t g_esp = 0x1000, g_cur_func = 0x10D56440;
typedef struct { uint32_t base, size, start, committed; int readable; } region;
static region regions[1024];
static int fail_reserve, fail_commit, fail_protect, fail_decommit;
#else
#include "host.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
uint32_t g_esp = 0x1000, g_cur_func = 0x10D56440;
void void_set_last_error(uint32_t value) { (void)value; }
#endif
static int owned = 1, in_handler, fail_open;
static unsigned heap_calls, normal_logs, fault_writes;
static char log_line[1024], fault_line[1024], fatal_line[256];
static jmp_buf failed;
int void_machine_is_owned(void) { return owned; }
const char *void_data_dir(void) { assert(!in_handler); return "fixture"; }
static int fixture_open(const char *path, int flags, ...) {
    assert(!in_handler && strstr(path, "/dxvk-guest-guard-fault.log"));
    assert((flags & (O_WRONLY | O_CREAT | O_TRUNC | O_APPEND)) == (O_WRONLY | O_CREAT | O_TRUNC | O_APPEND));
    return fail_open ? -1 : 42;
}
static intptr_t fixture_write(int fd, const void *data, size_t bytes) {
    assert(in_handler && fd == 42 && bytes < 768);
    memcpy(fault_line, data, bytes); fault_line[bytes] = 0; ++fault_writes; return (intptr_t)bytes;
}
void void_log(const char *format, ...) {
    assert(!in_handler); ++normal_logs;
    va_list args; va_start(args, format); vsnprintf(log_line, sizeof log_line, format, args); va_end(args);
}
_Noreturn void void_fatal(const char *format, ...) {
    assert(!in_handler);
    va_list args; va_start(args, format); vsnprintf(fatal_line, sizeof fatal_line, format, args); va_end(args);
    longjmp(failed, 1);
}
#ifdef _WIN32
static uint32_t void_galloc(uint32_t bytes) { (void)bytes; ++heap_calls; return 0; }
static void void_gfree(uint32_t va) { (void)va; ++heap_calls; }
static region *find_region(uint32_t va) {
    for (unsigned i = 0; i < 1024; ++i) if (regions[i].size && va >= regions[i].base && (uint64_t)va < (uint64_t)regions[i].base + regions[i].size) return &regions[i];
    return NULL;
}
static uint32_t void_vm_alloc(uint32_t va, uint32_t bytes, uint32_t type, uint32_t protect) {
    assert(!in_handler);
    if (type == 0x2000) {
        assert(!va && !(bytes & 0xFFFF) && protect == 1);
        if (fail_reserve) return 0;
        uint32_t base = 0x10000;
        for (;;) {
            unsigned conflict = 0;
            for (unsigned i = 0; i < 1024; ++i) if (regions[i].size && base < regions[i].base + regions[i].size && (uint64_t)base + bytes > regions[i].base) {
                base = regions[i].base + regions[i].size; conflict = 1; break;
            }
            if (!conflict) break;
        }
        if ((uint64_t)base + bytes > (256u << 20)) return 0;
        for (unsigned i = 0; i < 1024; ++i) if (!regions[i].size) { regions[i] = (region){base, bytes, 0, 0, 0}; return base; }
        return 0;
    }
    assert(type == 0x1000 && protect == 4 && !(va % HOST_PAGE) && !(bytes % HOST_PAGE));
    region *r = find_region(va); assert(r && (uint64_t)va + bytes <= (uint64_t)r->base + r->size);
    if (fail_commit) return 0;
    assert(VirtualAlloc(GPTR(va), bytes, MEM_COMMIT, PAGE_READWRITE) == GPTR(va));
    r->start = va; r->committed = bytes; r->readable = 1; return va;
}
static int void_vm_protect(uint32_t va, uint32_t bytes, uint32_t protect, uint32_t *old) {
    assert(!in_handler && protect == 1 && !old && !(va % HOST_PAGE) && !(bytes % HOST_PAGE));
    region *r = find_region(va); assert(r && r->start == va && r->committed == bytes);
    if (fail_protect) return 0;
    DWORD previous; assert(VirtualProtect(GPTR(va), bytes, PAGE_NOACCESS, &previous)); r->readable = 0; return 1;
}
static int void_vm_free(uint32_t va, uint32_t bytes, uint32_t type) {
    assert(!in_handler); region *r = find_region(va); assert(r);
    if (type == 0x8000) {
        assert(va == r->base && !bytes);
        assert(VirtualFree(GPTR(r->base), r->size, MEM_DECOMMIT)); memset(r, 0, sizeof *r); return 1;
    }
    assert(type == 0x4000 && va == r->start && bytes == r->committed && !r->readable);
    if (fail_decommit) return 0;
    assert(VirtualFree(GPTR(va), bytes, MEM_DECOMMIT)); r->committed = 0; return 1;
}
static int void_vm_readable(uint32_t va, uint32_t bytes) {
    assert(!in_handler); if (va == g_esp && bytes == 4) return 1;
    region *r = find_region(va);
    return r && r->readable && va >= r->start && (uint64_t)va + bytes <= (uint64_t)r->start + r->committed;
}
#endif
#define open fixture_open
#define write fixture_write
/* PRODUCTION */
#undef open
#undef write

#ifdef _WIN32
static jmp_buf page_fault;
static volatile int expecting_fault;
static LONG CALLBACK capture_fault(EXCEPTION_POINTERS *exception) {
    if (!expecting_fault) return EXCEPTION_CONTINUE_SEARCH;
    assert(exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION);
    in_handler = 1; ut3_dxvk_guest_fault((uintptr_t)exception->ExceptionRecord->ExceptionInformation[1]); in_handler = 0;
    expecting_fault = 0; longjmp(page_fault, 1);
}
__declspec(noinline) static int store_byte(uint32_t va) {
    if (setjmp(page_fault)) return 1;
    expecting_fault = 1; *(volatile uint8_t *)GPTR(va) = 0x77; expecting_fault = 0; return 0;
}
#else
static int fault_pipe;
static void capture_fault(int signal_number, siginfo_t *info, void *context) {
    (void)context; assert(signal_number == SIGSEGV || signal_number == SIGBUS);
    in_handler = 1; ut3_dxvk_guest_fault((uintptr_t)info->si_addr); in_handler = 0;
    assert(write(fault_pipe, fault_line, sizeof fault_line) == sizeof fault_line); _exit(0);
}
static int store_byte(uint32_t va) {
    int fds[2]; assert(!pipe(fds)); pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(fds[0]); fault_pipe = fds[1];
        struct sigaction action = {.sa_sigaction=capture_fault, .sa_flags=SA_SIGINFO}; sigemptyset(&action.sa_mask);
        assert(!sigaction(SIGSEGV, &action, NULL) && !sigaction(SIGBUS, &action, NULL));
        *(volatile uint8_t *)GPTR(va) = 0x77; _exit(2);
    }
    close(fds[1]); ssize_t count = read(fds[0], fault_line, sizeof fault_line); close(fds[0]);
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    if (WEXITSTATUS(status) == 2) return 0;
    assert(!WEXITSTATUS(status) && count == sizeof fault_line); return 1;
}
#endif
static void drain(void) {
    assert(!dxvk_guest_blocks && !dxvk_guest_committed);
    while (dxvk_guest_retired) dxvk_guest_evict();
    assert(!dxvk_guest_count && !dxvk_guest_reserved && !dxvk_guest_retired_count && !dxvk_guest_retired_bytes);
    for (unsigned i = 0; i < DXVK_GUEST_BLOCK_LIMIT; ++i) assert(!dxvk_guest_pool[i].used && !dxvk_guest_pool[i].published);
}
int main(void) {
#ifdef _WIN32
    assert(AddVectoredExceptionHandler(1, capture_fault));
    fixture_base = VirtualAlloc(NULL, 256u << 20, MEM_RESERVE, PAGE_NOACCESS); assert(fixture_base);
    assert(VirtualAlloc(GPTR(g_esp), 4096, MEM_COMMIT, PAGE_READWRITE));
#else
    assert(void_guest_reserve());
    g_esp = void_vm_alloc(0, 4096, 0x3000, 4); assert(g_esp);
#endif
    MEM32(g_esp) = 0x10D56517;
    const uint32_t sizes[] = {0,1,7,8,9,16384-33,16384-32,16384-16,16384,16385,0x10DA0};
    for (unsigned n = 0; n < sizeof sizes / sizeof *sizes; ++n) {
        uint32_t bytes = sizes[n], va = ut3_dxvk_guest_alloc(bytes); assert(va && !(va & 7));
        dxvk_guest_block *b = dxvk_guest_blocks;
        assert(!(b->raw & 0xFFFF) && !(b->reserved & 0xFFFF) && !(b->committed & 0x3FFF));
        assert(va - 32 >= b->raw + 0x4000 && va + bytes <= b->raw + 0x4000 + b->committed);
        for (uint32_t i = 0; i < bytes; ++i) assert(!MEM8(va + i));
        memset(GPTR(va), 0x22, bytes);
        g_cur_func = 0x10D562E0;
        assert(store_byte(b->raw + 0x4000 + b->committed));
        assert(strstr(fault_line, "state=live") && strstr(fault_line, "origin=0000000010d56440") && strstr(fault_line, "current=0000000010d562e0"));
        assert(store_byte(b->raw)); assert(strstr(fault_line, "state=live"));
        g_cur_func = 0x10D56440; MEM32(g_esp) = 0x10D5652E;
        ut3_dxvk_guest_free(va); assert(!void_vm_readable(va, 1));
        assert(store_byte(va)); assert(strstr(fault_line, "state=retired") && strstr(fault_line, "free_caller=0000000010d5652e"));
        MEM32(g_esp) = 0x10D56517;
    }
    drain();
    for (unsigned i = 0; i < 600; ++i) {
        uint32_t va = ut3_dxvk_guest_alloc(64); assert(va); ut3_dxvk_guest_free(va);
        assert(dxvk_guest_retired_count <= 128 && dxvk_guest_retired_bytes <= (32u << 20));
        assert(dxvk_guest_reserved <= (128u << 20) && dxvk_guest_count <= 512);
    }
    assert(dxvk_guest_retired_count == 128 && dxvk_guest_retired->serial + 127 == dxvk_guest_serial); drain();
    for (unsigned i = 0; i < 20; ++i) { uint32_t va = ut3_dxvk_guest_alloc(4u << 20); assert(va); ut3_dxvk_guest_free(va); assert(dxvk_guest_retired_bytes <= (32u << 20)); }
    assert(dxvk_guest_retired_count < 8); drain();
    uint32_t live[512];
    for (unsigned i = 0; i < 512; ++i) { live[i] = ut3_dxvk_guest_alloc(8); assert(live[i]); }
    assert(!ut3_dxvk_guest_alloc(8));
    ut3_dxvk_guest_free(live[0]); live[0] = ut3_dxvk_guest_alloc(8); assert(live[0] && dxvk_guest_count == 512);
    for (unsigned i = 0; i < 512; ++i) ut3_dxvk_guest_free(live[i]); drain();
    assert(!ut3_dxvk_guest_alloc(UINT32_MAX) && !ut3_dxvk_guest_alloc(128u << 20));
    for (unsigned i = 0; i < 3; ++i) { live[i] = ut3_dxvk_guest_alloc(32u << 20); assert(live[i]); }
    assert(!ut3_dxvk_guest_alloc(32u << 20) && dxvk_guest_committed < (128u << 20));
    for (unsigned i = 0; i < 3; ++i) ut3_dxvk_guest_free(live[i]); drain();
    unsigned writes_before = fault_writes, logs_before = normal_logs;
    in_handler = 1; owned = 0; ut3_dxvk_guest_fault((uintptr_t)GPTR(0x10000)); owned = 1;
    ut3_dxvk_guest_fault(0); ut3_dxvk_guest_fault((uintptr_t)GPTR(0xFFFFFFFFu) + 1); in_handler = 0;
    assert(fault_writes == writes_before && normal_logs == logs_before);
#ifdef _WIN32
    fail_reserve = 1; assert(!ut3_dxvk_guest_alloc(8)); fail_reserve = 0; drain();
    fail_commit = 1; assert(!ut3_dxvk_guest_alloc(8)); fail_commit = 0; drain();
    dxvk_guest_fault_fd = -1; fail_open = 1; assert(!ut3_dxvk_guest_alloc(8)); fail_open = 0; drain();
    uint32_t va = ut3_dxvk_guest_alloc(23); assert(va);
    MEM8(va - 1) ^= 0xFF;
    if (!setjmp(failed)) { ut3_dxvk_guest_free(va); assert(!"prefix accepted"); }
    assert(strstr(fatal_line, "boundary overwritten") && dxvk_guest_count == 1); MEM8(va - 1) ^= 0xFF;
    MEM8(va + 23) ^= 0xFF;
    if (!setjmp(failed)) { ut3_dxvk_guest_free(va); assert(!"tail accepted"); }
    assert(strstr(fatal_line, "boundary overwritten")); MEM8(va + 23) ^= 0xFF;
    fail_protect = 1;
    if (!setjmp(failed)) { ut3_dxvk_guest_free(va); assert(!"failed protection accepted"); }
    assert(strstr(fatal_line, "retirement failed") && !dxvk_guest_retired && dxvk_guest_count == 1);
    fail_protect = 0; ut3_dxvk_guest_free(va); drain();
    assert(!heap_calls); assert(VirtualFree(fixture_base, 0, MEM_RELEASE));
#endif
    puts("PASS production mode2 real protected-page overflow/underflow/UAF, fixed provenance callback, 4K/16K alignment, FIFO count/byte/total caps, slot exhaustion/eviction, budget/OOM/no-fallback and checked retirement; not a game crash fix");
    return 0;
}
