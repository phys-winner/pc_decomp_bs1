"""Complete CPU snapshots, aggregate process accounting and real partial scans."""
from pathlib import Path
import unittest
from tools import perf_capture as capture

ROOT = Path(__file__).resolve().parents[1]


def stat(tid, user, system=0, count=2, start=123, name="worker (audio)"):
    fields = ["0"] * 37
    fields[0] = "R"
    for index, value in ((11, user), (12, system), (17, count), (19, start), (36, 4)):
        fields[index] = str(value)
    return f"{tid} ({name}) " + " ".join(fields)


def scan(time, tasks, process_ticks=None, complete=True, begin=True, end_rows=None):
    lines = [f"#U {time}"]
    if begin:
        lines.append(f"#CPU_BEGIN {time}")
    if process_ticks is not None:
        lines.append(f"P {time} | " + stat(100, process_ticks, count=len(tasks)))
    lines += [f"T {tid} | {row}" for tid, row in tasks]
    if complete:
        lines.append(f"#CPU_END {time + .1} rows={len(tasks) if end_rows is None else end_rows}")
    return "\n".join(lines)


class CpuScanTest(unittest.TestCase):
    def test_marked_first_and_last_partial_scans_are_ignored(self):
        tasks = lambda user: [(1, stat(1, user)), (2, stat(2, user))]
        text = "\n".join((scan(0, tasks(0)[:1], 0, complete=False),
                          scan(10, tasks(100), 200), scan(20, tasks(200), 400),
                          scan(30, tasks(300)[:1], 100000, complete=False)))
        summary = capture.thread_summary(capture.parse_threads(text))
        self.assertEqual((summary["complete_snapshots"], summary["incomplete_snapshots"]), (2, 2))
        self.assertEqual(summary["window_s"], 10)
        self.assertEqual(summary["process_cpu_cores"], .2)
        self.assertEqual(summary["thread_cpu_cores"], .2)
        self.assertEqual(summary["process_cpu_source"], "proc_pid_stat")
        self.assertTrue(summary["process_cpu_exact"])

    def test_missing_begin_and_mismatched_end_cannot_commit_a_scan(self):
        tasks = [(1, stat(1, 100)), (2, stat(2, 100))]
        text = "\n".join((scan(0, tasks, 200, begin=False),
                          scan(10, tasks, 200, end_rows=3),
                          scan(20, tasks, 200, complete=False)))
        summary = capture.thread_summary(capture.parse_threads(text))
        self.assertEqual(summary["complete_snapshots"], 0)
        self.assertIsNone(summary["process_cpu_cores"])
        self.assertIsNone(summary["thread_cpu_cores"])

    def test_process_aggregate_counts_a_vanished_thread(self):
        first = [(1, stat(1, 100)), (2, stat(2, 100))]
        last = [(1, stat(1, 110, count=1))]
        summary = capture.thread_summary(capture.parse_threads(scan(0, first, 200) + "\n" + scan(10, last, 400)))
        self.assertEqual(summary["process_cpu_cores"], .2)
        self.assertEqual(summary["thread_cpu_cores"], .01)
        self.assertTrue(summary["thread_population_changed"])
        self.assertEqual([r["tid"] for r in summary["threads"]], [1])

    def test_reused_tid_and_empty_data_do_not_become_zero_cpu(self):
        text = scan(0, [(1, stat(1, 100, count=1, start=1))]) + "\n" + scan(10, [(1, stat(1, 1000, count=1, start=2))])
        for raw in (text, "#U 0\n#U 10", scan(0, [(1, stat(1, 100, count=1))], complete=False)):
            summary = capture.thread_summary(capture.parse_threads(raw))
            self.assertIsNone(summary["process_cpu_cores"])
            self.assertIsNone(summary["thread_cpu_cores"])

    def test_legacy_declared_thread_count_rejects_a_partial_tail(self):
        legacy = lambda time, user, partial=False: "\n".join([f"#U {time}", f"T 1 | {stat(1, user)}"] +
                                                            ([] if partial else [f"T 2 | {stat(2, user)}"]))
        summary = capture.thread_summary(capture.parse_threads("\n".join((legacy(0, 100), legacy(10, 200), legacy(20, 300, True)))))
        self.assertEqual(summary["window_s"], 10)
        self.assertEqual(summary["process_cpu_cores"], .2)
        self.assertEqual(summary["thread_cpu_cores"], .2)
        self.assertEqual(summary["process_cpu_source"], "common_threads")
        self.assertFalse(summary["process_cpu_exact"])
        self.assertEqual(summary["incomplete_snapshots"], 1)

    def test_real_a1_b1_keep_complete_windows_and_dxvk_queue(self):
        for label, duration, cores, queue, incomplete in (("p1-a1-valid", 56.31, 1.523, 28.73, 0),
                                                        ("p2-b1", 55.89, 1.385, 3.92, 1)):
            path = ROOT / "work/perf-minscale-20261113" / label / "capture/threads.txt"
            if not path.is_file():
                self.skipTest("Real capture fixture is not retained in this checkout")
            summary = capture.thread_summary(capture.parse_threads(path.read_text()))
            self.assertAlmostEqual(summary["window_s"], duration, places=2)
            self.assertEqual(summary["thread_cpu_cores"], cores)
            self.assertEqual(summary["process_cpu_cores"], cores)
            self.assertEqual(summary["incomplete_snapshots"], incomplete)
            row, = [row for row in summary["threads"] if row["comm"] == "dxvk-queue"]
            self.assertEqual(row["cpu_pct"], queue)


if __name__ == "__main__":
    unittest.main()
