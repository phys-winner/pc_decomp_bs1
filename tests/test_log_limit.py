"""L3a: the session log file stops at its size limit, logcat keeps the lines, and a crash dump still lands.

The production logging block of android/host/core.c is compiled on the PC with a 4 KiB limit (the
literal is the only thing replaced), so the cap, the resume from the Java session header and the crash
reserve are checked without writing 48 MiB.
"""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/log-limit"

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#define UT3_ANDROID_APP 1   /* the APK build appends to the Java session header */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
static unsigned logcat_lines;
/* Stand-in for stderr/logcat: every line still reaches it. */
static FILE *logcat_sink(void) { ++logcat_lines; static FILE *nul; if (!nul) nul = fopen("NUL", "w"); return nul; }
/* BLOCK */
static long file_size(const char *path) { FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
static void say(const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_va(fmt, ap); va_end(ap); }
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *header = fopen(argv[1], "wb"); fputs("[session] header written by Java\n", header); fclose(header);
    void_log_open(argv[1]);
    assert(log_file_bytes == 33);                      /* resumes after the Java header */
    char line[100]; memset(line, 'x', 99); line[99] = 0;
    for (int i = 0; i < 200; ++i) say("%s", line);    /* 20 KB of lines into a 4 KiB file */
    long capped = file_size(argv[1]);
    assert(capped <= 4096 + 200 && log_file_capped && logcat_lines == 200);
    say("ordinary line after the cap");
    assert(file_size(argv[1]) == capped);
    log_crash_mode = 1;                                /* the fault handler's dump still lands */
    say("[void] host signal 11");
    assert(file_size(argv[1]) > capped);
    for (int i = 0; i < 200; ++i) say("%s", line);    /* but not without a bound */
    assert(file_size(argv[1]) <= 4096 + 4096 + 400);
    void_log_open(NULL);
    FILE *f = fopen(argv[1], "rb"); char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    assert(strstr(text, "[session] header written by Java\n") == text);
    assert(strstr(text, "[log] this file reached") && strstr(text, "[void] host signal 11"));
    assert(!strstr(text, "ordinary line after the cap"));
    puts("PASS session log: capped file, header kept, lines continue in logcat, crash dump within its reserve");
    return 0;
}
'''


class LogLimitTest(unittest.TestCase):
    def test_production_log_cap(self):
        source = (ROOT / "android/host/core.c").read_text(encoding="utf-8").replace("\r\n", "\n")
        start = source.index("static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;")
        end = source.index("void void_log(const char *fmt, ...)")
        block = source[start:end]
        self.assertEqual(block.count("#define LOG_FILE_LIMIT (48u << 20)"), 1)
        self.assertEqual(block.count("#define LOG_CRASH_RESERVE (4u << 20)"), 1)
        block = block.replace("#define LOG_FILE_LIMIT (48u << 20)", "#define LOG_FILE_LIMIT 4096u")
        block = block.replace("#define LOG_CRASH_RESERVE (4u << 20)", "#define LOG_CRASH_RESERVE 4096u")
        self.assertEqual(block.count("fprintf(stderr, "), 1)
        block = block.replace("fprintf(stderr, ", "fprintf(logcat_sink(), ")
        # The MSVC CRT rejects a zero-size line buffer (bionic accepts it); buffering is not under test.
        block = block.replace("setvbuf(log_file, NULL, _IOLBF, 0)", "setvbuf(log_file, NULL, _IONBF, 0)")
        # A reported fatal error uses the crash reserve. The signal path can only write directly.
        fatal = function(source, "_Noreturn void void_fatal(")
        self.assertLess(fatal.index("log_crash_mode = 1;"), fatal.index('void_log("[void] FATAL'))
        handler = function(source, "static void on_signal(")
        self.assertIn("port_crash_marker_signal(crash_marker, signo);", handler)
        self.assertIn("write(signal_log, line, sizeof line - 1)", handler)
        self.assertNotIn("void_log", handler)
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        probe.write_text(HARNESS.replace("/* BLOCK */", block), encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        for command in ([str(CLANG), "/nologo", "/O2", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)],
                        [str(binary), str(OUT / "bioshock.log")]):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS session log", result.stdout)


if __name__ == "__main__":
    unittest.main()
