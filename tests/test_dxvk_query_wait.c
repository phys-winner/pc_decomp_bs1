/* Production scheduling boundary: only repeated pending FLUSH queries may
 * release the machine. Native query results and output bytes are elsewhere. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned begins, ends, sleeps, handoffs, logs;
static int released, interrupt_once;
static long expected_ns;
static void fixture_yield(void) { ++handoffs; }
static void (*recomp_yield_hook)(void) = fixture_yield;
static void void_log(const char *format, ...) { (void)format; ++logs; }
static void void_blocking_begin(void) { assert(!released); released = 1; ++begins; }
static void void_blocking_end(void) { assert(released); released = 0; ++ends; }
static int fixture_sleep(const struct timespec *delay, struct timespec *remaining)
{
    assert(released && delay->tv_sec == 0 && delay->tv_nsec == expected_ns);
    ++sleeps;
    if (interrupt_once) {
        interrupt_once = 0;
        *remaining = *delay;
        errno = EINTR;
        return -1;
    }
    return 0;
}
#define nanosleep fixture_sleep
#include "../android/dxvk/query_wait.inc"
#undef nanosleep

int main(int argc, char **argv)
{
    assert(argc == 3);
#ifdef _WIN32
    assert(!_putenv_s("UT3_DXVK_QUERY_WAIT_US", strcmp(argv[1], "--unset") ? argv[1] : ""));
#else
    if (!strcmp(argv[1], "--unset")) assert(!unsetenv("UT3_DXVK_QUERY_WAIT_US"));
    else assert(!setenv("UT3_DXVK_QUERY_WAIT_US", argv[1], 1));
#endif
    expected_ns = strtol(argv[2], NULL, 10) * 1000L;
    for (unsigned i = 0; i < 32; ++i) {
        dxvk_query_wait(0, 1);             /* S_OK */
        dxvk_query_wait(0x8876086c, 1);    /* D3DERR_INVALIDCALL */
        dxvk_query_wait(1, 0);             /* nonblocking GetData */
    }
    assert(!begins && !sleeps && !handoffs && !logs);
    for (unsigned i = 0; i < 8; ++i) dxvk_query_wait(1, 1);
    assert(!begins && !sleeps && !handoffs && !logs);
    interrupt_once = expected_ns != 0;
    dxvk_query_wait(1, 1);
    assert(begins == (expected_ns != 0) && begins == ends && !released);
    assert(sleeps == (expected_ns ? 2u : 0u));
    assert(handoffs == (expected_ns == 0));
    assert(logs == 1 && dxvk_query_wait_config() == expected_ns / 1000L);
    /* Configuration is read once, never rescanned during the hot poll loop. */
#ifdef _WIN32
    assert(!_putenv_s("UT3_DXVK_QUERY_WAIT_US", "37"));
#else
    assert(!setenv("UT3_DXVK_QUERY_WAIT_US", "37", 1));
#endif
    assert(dxvk_query_wait_config() == expected_ns / 1000L && logs == 1);
    unsigned previous = begins;
    unsigned prior_handoffs = handoffs;
    dxvk_query_wait(0, 1);
    for (unsigned i = 0; i < 8; ++i) dxvk_query_wait(1, 1);
    assert(begins == previous);
    assert(handoffs == prior_handoffs);
    dxvk_query_wait(1, 1);
    assert(begins == previous + (expected_ns != 0) && begins == ends && !released);
    assert(handoffs == prior_handoffs + (expected_ns == 0));
    prior_handoffs = handoffs;
    recomp_yield_hook = NULL;
    dxvk_query_wait(1, 1);
    assert(handoffs == prior_handoffs);
    assert(begins == ends && !released);
#ifdef _WIN32
    assert(!_putenv_s("UT3_DXVK_TRACE_DRAWS", ""));
#else
    assert(!unsetenv("UT3_DXVK_TRACE_DRAWS"));
#endif
    dxvk_trace_cache = -1;
    assert(!dxvk_trace_enabled());
    const char *flag_values[] = { "", "0", "1" };
    const int flag_enabled[] = { 0, 0, 1 };
    for (unsigned i = 0; i < 3; ++i) {
#ifdef _WIN32
        assert(!_putenv_s("UT3_DXVK_TRACE_DRAWS", flag_values[i]));
#else
        assert(!setenv("UT3_DXVK_TRACE_DRAWS", flag_values[i], 1));
#endif
        dxvk_trace_cache = -1;
        assert(dxvk_trace_enabled() == flag_enabled[i]);
    }
    /* The startup value is cached: a later environment change is not re-read. */
#ifdef _WIN32
    assert(!_putenv_s("UT3_DXVK_TRACE_DRAWS", "0"));
#else
    assert(!setenv("UT3_DXVK_TRACE_DRAWS", "0", 1));
#endif
    assert(dxvk_trace_enabled() == 1);
    puts("PASS production query wait: default/invalid zero, exact pending/FLUSH gate, bounded spin, success reset, lazy cached startup setting, EINTR retry, balanced machine release, zero-delay handoff and null hook");
    return 0;
}
