/* The script cycle counter of UObject::ProcessEvent without a clock.
 *
 * Bioshock.exe's UObject::ProcessEvent (10967870), the dispatcher of every UnrealScript event, reads the clock
 * around each outermost event: appCycles() (1093CD60, QueryPerformanceCounter's low part) at 1096792E when its
 * nesting counter (111C03A8) becomes 1, QueryPerformanceCounter through the import at 10967A04 when it returns
 * to 0, and both only feed GScriptCycles (111BE2C0).  That counter is never read: the main loop resets it
 * (10914438) and 26 other places add or subtract clock values (Bioshock.exe, every reference checked with IDA,
 * work/ha22-perf-20261122).  On the Lenovo TB373FU (Dimensity 8300) the two reads took about 6 % of the game
 * thread's samples (vdso clock_gettime under QueryPerformanceCounter_entry, thousands of events per frame).
 *
 * The seams of tools/build_android.py (SEAMS, 1096792E and 10967A04, in ProcessEvent and in the copy of its body
 * that the generator inlined into sub_109EF150, the probe-masked event call) skip both reads: appCycles' result is 0
 * and the import's LARGE_INTEGER is zeroed, so GScriptCycles stays unchanged, as nobody can observe.  Every
 * other clock of the game (its frame time, the timers that compare QueryPerformanceCounter with a deadline)
 * is untouched.  BIOSHOCK_SCRIPT_CYCLES=1 (QA perf-flags.env) runs the original calls for A/B measurements. */
#include <stdlib.h>

int void_bioshock_script_clock(void)
{
    static int on = -1;
    if (on < 0) {
        const char *text = getenv("BIOSHOCK_SCRIPT_CYCLES");
        on = text && text[0] == '1';
    }
    return on;
}
