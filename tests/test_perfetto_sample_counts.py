"""Bursty linux.perf counts must not be presented as sampled CPU milliseconds."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class PerfettoSampleCountsTest(unittest.TestCase):
    def test_burst_sample_counts_do_not_invent_on_cpu_time(self):
        spec = importlib.util.spec_from_file_location("sample_count_analyzer", ROOT / "tools/analyze_perfetto.py")
        analyzer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(analyzer)

        class Trace:
            def __init__(self, **kwargs):
                pass

            def query(self, sql):
                if "from trace_bounds" in sql:
                    return [SimpleNamespace(start_ts=0, end_ts=14960000000)]
                if "select upid, pid from process" in sql:
                    return [SimpleNamespace(upid=1, pid=28977)]
                if "from thread_state" in sql:
                    return [SimpleNamespace(tid=29100, name="mixer", state="Running", dur=195600000, n=100)]
                if "from sched s" in sql:
                    return [SimpleNamespace(tid=29100, cpu=7, dur=195600000)]
                if "from perf_sample" in sql:
                    return [SimpleNamespace(cs=1, tid=29100, mode="user", cpu=7)] * 894
                if "from stack_profile_callsite" in sql:
                    return [SimpleNamespace(id=1, parent_id=None, fname="mixer_kernel", mname="/libbioshock.so", rel_pc=0)]
                if any(table in sql for table in ("actual_frame_timeline_slice", "cpu_counter_track", "gpu_counter_track")):
                    return []
                raise AssertionError("Unexpected query: " + sql)

            def close(self):
                pass

        with tempfile.TemporaryDirectory(prefix="perfetto-counts-") as temporary:
            trace = Path(temporary) / "trace.pftrace"
            with patch.object(analyzer, "TraceProcessor", Trace), contextlib.redirect_stdout(io.StringIO()):
                analyzer.main([str(trace), "--frequency", "400"])
            result = json.loads(trace.with_suffix(".summary.json").read_text(encoding="utf-8"))
        sample = result["samples"]["per_thread"]["29100"]
        self.assertEqual(sample["samples"], 894)
        self.assertEqual(result["samples"]["nominal_period_ms"], 2.5)
        self.assertFalse(result["samples"]["counts_are_cpu_time_weights"])
        self.assertNotIn("ms", sample)
        self.assertNotIn("period_ms", result["samples"])
        self.assertEqual(sample["scheduler_running_ms"], 195.6)
        self.assertNotEqual(sample["scheduler_running_ms"], 894 * 2.5)
        self.assertEqual(result["threads"][0]["running_ms"], 195.6)
        self.assertEqual(result["threads"][0]["running_pct"], 1.31)
        self.assertEqual(result["self_by_thread"]["29100"][0]["pct_of_thread"], 100)


if __name__ == "__main__":
    unittest.main()
