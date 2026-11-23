"""Verify packed ReadFile handoff ordering, lifetime and short-read handling."""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/packed-read"

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ssize_t int64_t
#define off_t int64_t
#define VOID_PERF_FILE_READ 0
#define ERROR_INVALID_HANDLE_ 6
#define SEEK_SET 0
#define pthread_mutex_t int
typedef struct {
    int fd, is_dir, std_stream, packed;
    uint64_t bias, packed_size, position;
    void *delta;
    pthread_mutex_t lock;
} file_t;
static file_t file;
static unsigned references, valid, begins, ends, calls, close_during_read, interrupted, failed, last_error;
static uint32_t arguments[5];
static uint8_t guest[80000], content[70000];
#define ARG(i) arguments[i]
#define MEM32(a) (*(uint32_t *)(guest + (a)))
#define GPTR(a) (guest + (a))
#define WINAPI_EXPORT(dll,name,n) static uint32_t name(void)
static file_t *file_get(uint32_t h) { return valid && h == 42 ? &file : NULL; }
static void void_set_last_error(uint32_t e) { last_error = e; }
static int void_vm_writable(uint32_t p, uint32_t n) { return p <= sizeof guest && n <= sizeof guest-p; }
static uint32_t void_errno_to_win32(int e) { return (uint32_t)e; }
static uint64_t void_perf_now_ns(void) { return 1; }
static void void_perf_add(int k, uint64_t t, uint64_t n) { (void)k; (void)t; (void)n; }
static uint32_t void_handle_dup(uint32_t h) { assert(h == 42 && valid); ++references; return 43; }
static int void_handle_close(uint32_t h) {
    assert(h == 43 && references && !file.lock);
    if (!--references) valid = 0;
    return 1;
}
static void void_blocking_begin(void) {
    assert(valid && !file.lock && references == 2); ++begins;
    if (close_during_read) --references;
}
static void void_blocking_end(void) { assert(valid && !file.lock); ++ends; }
static void pthread_mutex_lock(pthread_mutex_t *p) { assert(valid && !*p); *p = 1; }
static void pthread_mutex_unlock(pthread_mutex_t *p) { assert(valid && *p); *p = 0; }
static void audio_io_log(file_t *f, const char *op, uint64_t at, uint32_t n, int64_t r) {
    assert(valid && references && f == &file); (void)op; (void)at; (void)n; (void)r;
}
static ssize_t fixture_pread(int fd, void *out, size_t n, off_t offset) {
    assert(fd == 7 && file.lock && offset >= 0); ++calls;
    if (interrupted) { interrupted = 0; errno = EINTR; return -1; }
    if (failed) { errno = EIO; return -1; }
    if ((uint64_t)offset >= sizeof content) return 0;
    if (n > sizeof content-(size_t)offset) n = sizeof content-(size_t)offset;
    if (n > 8192) n = 8192;
    memcpy(out, content+(size_t)offset, n); return (ssize_t)n;
}
#define pread fixture_pread
static ssize_t voice_delta_pread(void *delta, int fd, void *out, size_t n, uint64_t offset) {
    (void)delta; (void)fd; (void)out; (void)n; (void)offset; assert(0); return -1;
}
static ssize_t fixture_read(int fd, void *out, size_t n) { (void)fd; (void)out; (void)n; assert(0); return -1; }
#define read fixture_read
static int fixture_dup(int fd) { (void)fd; assert(0); return -1; }
#define dup fixture_dup
static int fixture_close(int fd) { (void)fd; assert(0); return 0; }
#define close fixture_close
static int file_restore_kept_times(file_t *f) { (void)f; return 0; }
/* ACTUAL */
static void reset(uint32_t bytes) {
    memset(&file, 0, sizeof file); memset(guest, 0, sizeof guest);
    references = valid = 1; begins = ends = calls = close_during_read = interrupted = failed = last_error = 0;
    file.fd = 7; file.packed = 1; file.bias = 13; file.packed_size = 65536;
    arguments[0] = 42; arguments[1] = 128; arguments[2] = bytes; arguments[3] = 4; arguments[4] = 0;
}
int main(void) {
    for (unsigned i = 0; i < sizeof content; ++i) content[i] = (uint8_t)(i*17u);
    reset(32768); interrupted = 1;
    assert(ReadFile() == 1 && MEM32(4) == 32768 && file.position == 32768);
    assert(calls == 5 && begins == 1 && ends == 1 && references == 1 && !file.lock);
    assert(!memcmp(guest+128, content+13, 32768));
    reset(32768); close_during_read = 1;
    assert(ReadFile() == 1 && MEM32(4) == 32768 && !valid && !references && begins == ends);
    assert(!memcmp(guest+128, content+13, 32768));
    reset(32768); arguments[4] = 16; MEM32(24) = 65000;
    assert(ReadFile() == 1 && MEM32(4) == 536 && !file.position && MEM32(20) == 536 && !MEM32(16));
    assert(!memcmp(guest+128, content+65013, 536));
    reset(100); file.position = 65500;
    assert(ReadFile() == 1 && MEM32(4) == 36 && file.position == 65536 && !begins && !ends);
    assert(ReadFile() == 1 && !MEM32(4) && file.position == 65536);
    reset(32768); failed = 1;
    assert(ReadFile() == 0 && !MEM32(4) && !file.position && last_error == EIO);
    assert(references == 1 && begins == ends && !file.lock);
    reset(32768); arguments[1] = (uint32_t)sizeof guest-4;
    assert(ReadFile() == 0 && last_error == 998 && !calls && !begins);
    puts("PASS packed ReadFile: handoff outside mutex, object retained across Close, EINTR/partial reads, bounds/EOF/overlap and errors");
    return 0;
}
'''


class PackedReadTest(unittest.TestCase):
    def test_production_read(self):
        source = (ROOT / "android/host/files.c").read_text(encoding="utf-8")
        actual = function(source, "WINAPI_EXPORT(K32, ReadFile, 5)")
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        probe.write_text(HARNESS.replace("/* ACTUAL */", actual), encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11",
                         "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(CLANG.with_name("clang.exe")), "--target=aarch64-linux-android29",
                                 "-O2", "-std=gnu11", str(probe), "-Wl,-z,max-page-size=16384", "-o", str(OUT / "test-packed-read")],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
