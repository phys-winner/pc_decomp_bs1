"""Every BIOSHOCK_*/VOID_* switch the host reads is either a perf-flags.env QA switch or named here on purpose.

android_main.c loads perf-flags.env only for names in its allowed[] list; a QA switch missing there is ignored
without a trace (BIOSHOCK_BINK_THREADED_IO, BIOSHOCK_BINK_TRACE, November 2026).
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Not perf-flags QA switches: set by the launcher for the process, or developer-only traces.
NOT_PERF_FLAGS = {
    "BIOSHOCK_EXECUTION_MODE", "BIOSHOCK_MOBILE_SHADERS", "BIOSHOCK_RENDERER", "BIOSHOCK_VULKAN_DRIVER",
    "VOID_BIG_CORES", "VOID_COUNT_THUNKS", "VOID_DEBUG_IAT", "VOID_DSOUND_TRACE", "VOID_TRACE_CALLS",
}


class PerfFlagsWhitelistTest(unittest.TestCase):
    def test_every_switch_is_whitelisted_or_exempt(self):
        main = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8", errors="replace")
        block = main[main.index("allowed[] = {"):]
        allowed = set(re.findall(r'"([A-Z0-9_]+)"', block[:block.index("};")]))
        used = set()
        for folder in ("android/host", "android/render10"):
            for path in (ROOT / folder).glob("*.c"):
                used.update(re.findall(r'getenv\("((?:BIOSHOCK|VOID)_[A-Z0-9_]+)"\)',
                                       path.read_text(encoding="utf-8", errors="replace")))
        self.assertIn("BIOSHOCK_BINK_TRACE", used)
        self.assertEqual(sorted(used - allowed - NOT_PERF_FLAGS), [])
        self.assertEqual(sorted(NOT_PERF_FLAGS & allowed), [])


if __name__ == "__main__":
    unittest.main()
