"""Execute the production shell collector with slow /proc and dumpsys fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from tools import perf_capture

ROOT = Path(__file__).resolve().parents[1]


class PerfSamplerTest(unittest.TestCase):
    def test_independent_frame_polling_and_bounded_worker_lifetime(self):
        bash = Path("C:/Program Files/Git/bin/bash.exe") if os.name == "nt" else Path(shutil.which("bash") or "")
        if not bash.is_file():
            self.skipTest("Requires Bash to execute the shell collector fixture")
        with tempfile.TemporaryDirectory(prefix="perf-sampler-") as temporary:
            root = Path(temporary)
            proc = root / "proc"
            target = proc / "9487"
            target.mkdir(parents=True)
            (proc / "stat").write_text("cpu 100 0 100 100 0 0 0 0 0 0\n")
            (target / "status").write_text("Name:\ttest\nVmRSS:\t123 kB\nThreads:\t2\n")
            for tid in (9487, 9619):
                task = target / "task" / str(tid)
                task.mkdir(parents=True)
                (task / "stat").write_text(
                    f"{tid} (Audio (mixer)) S 2453 2453 0 0 -1 1077952832 19169 0 4 0 "
                    "2701 200 0 0 10 -10 2 0 2348996 13181009920 239600 "
                    "18446744073709551615 1 1 0 0 0 0 4612 1 1073775864 0 0 0 17 4 0 0\n")
            (target / "stat").write_bytes((target / "task/9487/stat").read_bytes())
            binaries = root / "bin"
            binaries.mkdir()
            dumpsys = binaries / "dumpsys"
            dumpsys.write_text("""#!/bin/bash
if [ "$1" = thermalservice ]; then exec sleep 30; fi
count=128
if [ -f "$FIXTURE_COUNTER" ]; then read -r count < "$FIXTURE_COUNTER"; fi
echo $((count + 30)) > "$FIXTURE_COUNTER"
echo 16666666
i=$((count - 127))
while [ "$i" -le "$count" ]; do
  ns=$((1000000000 + i * 16666666))
  echo "$ns $ns $ns"
  i=$((i + 1))
done
""", newline="\n")
            dumpsys.chmod(0o755)
            # Redirect only fixture data; the real host uptime drives all deadlines.
            source = (ROOT / "tools/perf_sampler.sh").read_text()
            source = source.replace("/proc/", proc.as_posix() + "/").replace(proc.as_posix() + "/uptime", "/proc/uptime")
            source = source.replace("/sys/", (root / "sys").as_posix() + "/")
            source = source.replace("  CHILD=$!", '  CHILD=$!\n  echo "$CHILD" >> "$FIXTURE_CHILDREN"')
            source = source.replace("worker_start() {", 'worker_start() {\n  echo "$BASHPID" >> "$FIXTURE_CHILDREN"')
            # Two slow reads occupy the proc worker for 1.6 s, longer than three frame polls.
            source = source.replace("frames &", """read() {
  if [ "$*" = '-r stat' ]; then run sleep 0.8; fi
  builtin read "$@"
}
frames &""")
            script = root / "sampler.sh"
            script.write_text(source, newline="\n")
            environment = os.environ.copy()
            environment["PATH"] = binaries.as_posix() + os.pathsep + environment["PATH"]
            children = root / "children.txt"
            environment.update(FIXTURE_CHILDREN=children.as_posix(), FIXTURE_COUNTER=(root / "counter").as_posix())

            def start(name, seconds):
                children.write_text("")
                output = root / name
                process = subprocess.Popen([str(bash), str(script), "9487", "FixtureLayer", str(seconds), str(output)],
                                           env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                deadline = time.monotonic() + 3
                while not (output / "sampler.pid").is_file() and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertTrue((output / "sampler.pid").is_file())
                return process, output

            def finished(process):
                stdout, stderr = process.communicate(timeout=6)
                self.assertEqual(stdout, b"")
                tracked = children.read_text().split()
                self.assertGreater(len(tracked), 3)
                result = subprocess.run([str(bash), "-c", 'for p; do kill -0 "$p" 2>/dev/null && echo "$p"; done',
                                         "fixture", *tracked], capture_output=True)
                self.assertEqual(result.stdout, b"", f"Sampler left live children: {result.stdout!r}; {stderr!r}")
                return stderr

            started = time.monotonic()
            process, output = start("deadline", 3.3)
            finished(process)
            self.assertEqual(process.returncode, 0)
            self.assertLess(time.monotonic() - started, 5)
            self.assertTrue((output / "done").exists())
            refresh, polls, records, _ = perf_capture.parse_latency_file((output / "latency.txt").read_text())
            self.assertGreaterEqual(len(polls), 6)
            self.assertLess(max(b[0] - a[0] for a, b in zip(polls, polls[1:])), 1.1)
            blocks = perf_capture.parse_threads((output / "threads.txt").read_text())
            self.assertEqual(set(blocks[0]["threads"]), {9487, 9619})
            self.assertEqual(blocks[0]["threads"][9487]["comm"], "Audio (mixer)")
            self.assertIsNone(blocks[0]["threads"][9487]["run_ns"])
            self.assertEqual(perf_capture.coverage_gaps(polls), [])
            self.assertTrue(perf_capture.frame_stats(records, perf_capture.coverage_gaps(polls), refresh)["publication_eligible"])
            missed_polls = perf_capture.coverage_gaps([polls[0], polls[-1]])
            self.assertEqual(len(missed_polls), 1)
            self.assertFalse(perf_capture.frame_stats(records, missed_polls, refresh)["publication_eligible"])

            process, output = start("target-gone", 20)
            time.sleep(.7)
            shutil.rmtree(target)
            finished(process)
            self.assertEqual(process.returncode, 0)
            self.assertIn("#GONE ", (output / "latency.txt").read_text())
            self.assertTrue((output / "done").exists())

            target.mkdir()
            process, output = start("signal", 20)
            time.sleep(.7)
            sampler_pid = (output / "sampler.pid").read_text().strip()
            subprocess.run([str(bash), "-c", 'kill -TERM "$1"', "fixture", sampler_pid], check=True)
            finished(process)
            self.assertNotEqual(process.returncode, 0)
            self.assertFalse((output / "done").exists())


if __name__ == "__main__":
    unittest.main()
