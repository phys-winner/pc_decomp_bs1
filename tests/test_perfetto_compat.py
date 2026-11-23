import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import perf_session


class PerfettoCompatibility(unittest.TestCase):
    def test_stdin_and_old_enum_fallback_keep_actual_config(self):
        config = "data_sources {\n user_frames: UNWIND_FRAME_POINTER\n}\nduration_ms: 15000\n"
        failed = subprocess.CompletedProcess([], 1, b"", b"Unexpected value 'UNWIND_FRAME_POINTER'")
        passed = subprocess.CompletedProcess([], 0, b"Trace written", b"")
        with tempfile.TemporaryDirectory() as temporary, patch.object(perf_session, "adb") as adb, \
                patch.object(perf_session.subprocess, "run", side_effect=[failed, passed]) as run:
            directory = Path(temporary)
            result = perf_session.record_perfetto(config, directory, 15)
            self.assertEqual(result, {"unwind_mode": "default_dwarf", "config_transport": "stdin", "attempts": 2})
            self.assertEqual(run.call_args_list[0].kwargs["input"], config.encode())
            actual = (directory / "perfetto.pbtxt").read_text()
            self.assertNotIn("user_frames", actual)
            self.assertIn("duration_ms: 15000", actual)
            self.assertEqual(run.call_args_list[1].kwargs["input"], actual.encode())
            self.assertEqual(adb.call_args_list[0].args[0], "pull")
            self.assertEqual(adb.call_args_list[-1].args[:3], ("shell", "rm", "-f"))

    def test_other_errors_do_not_retry_or_pull(self):
        failed = subprocess.CompletedProcess([], 1, b"", b"permission denied")
        with tempfile.TemporaryDirectory() as temporary, patch.object(perf_session, "adb") as adb, \
                patch.object(perf_session.subprocess, "run", return_value=failed) as run:
            with self.assertRaisesRegex(RuntimeError, "permission denied"):
                perf_session.record_perfetto("duration_ms: 15000\n", Path(temporary), 15)
            self.assertEqual(run.call_count, 1)
            self.assertEqual(adb.call_count, 1)
            self.assertEqual(adb.call_args.args[:3], ("shell", "rm", "-f"))
