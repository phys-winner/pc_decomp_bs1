"""A console command the port injects (QA F10, travel, the cheats dialog) runs only at the message pump.

At a lifted loop's yield point (recomp_yield_hook) it ran inside whatever the guest was doing: inside
BinkDoFrame it closed and reopened the PlasmiNow movie under the running decoder (QA repro crashes,
25 November 2026). These checks keep the command at PeekMessage/GetMessage on the main thread.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "android/host"


def body(source, header):
    start = source.index(header)
    end = source.index("\n}\n", start)
    return source[start:end]


class ConsoleSafePointTest(unittest.TestCase):
    def setUp(self):
        self.threads = (HOST / "threads.c").read_text(encoding="utf-8")
        self.user32 = (HOST / "user32.c").read_text(encoding="utf-8")
        self.core = (HOST / "core.c").read_text(encoding="utf-8")

    def test_yield_hook_runs_no_guest_work(self):
        hook = body(self.threads, "static void yield_hook(void)")
        self.assertNotIn("navigation", hook)
        self.assertNotIn("console", hook)

    def test_message_pump_runs_the_command(self):
        self.assertIn("void_bioshock_poll_main_navigation();", body(self.user32, "static void pump_messages(void)"))
        self.assertEqual(re.search(r"static void pump_host_input\(void\)[^\n]*", self.user32).group(0),
                         "static void pump_host_input(void) { void_input_pump(); }")
        for header in ["WINAPI_EXPORT(U32, PeekMessageA, 5)", "WINAPI_EXPORT(U32, PeekMessageW, 5)",
                       "static uint32_t get_message("]:
            self.assertIn("pump_messages();", body(self.user32, header), header)

    def test_waits_only_pump_input(self):
        for header in ["WINAPI_EXPORT(U32, GetQueueStatus, 1)", "WINAPI_EXPORT(U32, MsgWaitForMultipleObjects, 5)"]:
            text = body(self.user32, header)
            self.assertIn("pump_host_input();", text, header)
            self.assertNotIn("pump_messages", text, header)

    def test_command_runs_on_the_main_thread_only(self):
        poll = body(self.core, "void void_bioshock_poll_main_navigation(void)")
        self.assertIn("void_thread_id() == 260", poll)
        callers = sorted(p.name for p in HOST.glob("*.c")
                         if "void_bioshock_poll_main_navigation()" in p.read_text(encoding="utf-8", errors="replace"))
        self.assertEqual(callers, ["user32.c"])


if __name__ == "__main__":
    unittest.main()
