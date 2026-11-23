"""Run production profiling orchestration with a fake adb boundary, never a device."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    result = importlib.util.module_from_spec(spec)
    with patch.object(sys, "path", [str(ROOT / "tools"), *sys.path]):
        spec.loader.exec_module(result)
    return result


class PerfToolsTest(unittest.TestCase):
    def test_symbol_candidate_matches_stripped_payload(self):
        from tools import profile_cpu as profile
        from tools.elf_identity import runtime_identity
        toolchain = profile.NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin"
        clang, strip = toolchain / "clang.exe", toolchain / "llvm-strip.exe"
        if not clang.is_file() or not strip.is_file():
            self.skipTest("Requires the Android NDK for a real ELF symbol/payload test")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "jni.c"
            source.write_text("int JNI_OnLoad(void *a, void *b) { return a == b ? 0x10006 : 0; }\n")
            symbols = root / "libbioshock.unstripped.so"
            subprocess.run([str(clang), "--target=aarch64-linux-android29", "-shared", "-fPIC", "-g0",
                            str(source), "-o", str(symbols)], check=True, capture_output=True)
            shipped = root / "stripped.so"
            subprocess.run([str(strip), "--strip-unneeded", "-o", str(shipped), str(symbols)], check=True, capture_output=True)
            original, stripped = symbols.read_bytes(), shipped.read_bytes()
            identity = runtime_identity(original)

            def package(payload, separate_symbols=True):
                with zipfile.ZipFile(root / "bioshock.apk", "w") as archive:
                    archive.writestr("lib/arm64-v8a/libbioshock.so", payload)
                receipt = {"sha256": profile.digest(root / "bioshock.apk"),
                           "library_sha256": profile.hashlib.sha256(payload).hexdigest()}
                if separate_symbols:
                    receipt.update(symbols_sha256=profile.digest(symbols), allocated_identity=identity["allocated_sha256"])
                (root / "bioshock.json").write_text(json.dumps(receipt))
                return receipt

            package(original, separate_symbols=False)
            self.assertEqual(profile.verify_candidate(root)[3]["sha256"], identity["sha256"])
            package(stripped)
            self.assertLess(len(stripped), len(original))
            self.assertEqual(profile.verify_candidate(root)[3]["sha256"], identity["sha256"])
            # Even self-consistent APK and receipt hashes cannot disguise different code.
            forged = bytearray(stripped)
            text = next(section for section in identity["allocated_sections"] if section["name"] == ".text")
            forged[text["offset"] + text["size"] // 2] ^= 1
            package(forged)
            with self.assertRaisesRegex(ValueError, "runtime payload or ABI differs"):
                profile.verify_candidate(root)
            receipt = package(stripped)
            receipt["symbols_sha256"] = "0" * 64
            (root / "bioshock.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "APK/symbols differ"):
                profile.verify_candidate(root)
            symbols.write_bytes(stripped)
            package(stripped)
            with self.assertRaisesRegex(ValueError, "no symbol table"):
                profile.verify_candidate(root)

    def test_thread_cpu_with_missing_and_sparse_schedstat(self):
        capture = module("perf_capture_thread_fixture", "tools/perf_capture.py")
        # Same stat layout emitted by TEST_SERIAL, including spaces/parentheses in comm.
        def task(tid, user, system, cpu, schedstat=None, threads=1):
            stat = (f"{tid} (Audio (mixer)) S 2453 2453 0 0 -1 1077952832 19169 0 4 0 "
                    f"{user} {system} 0 0 10 -10 {threads} 0 2348996 13181009920 239600 "
                    "18446744073709551615 1 1 0 0 0 0 4612 1 1073775864 0 0 0 "
                    f"17 {cpu} 0 0 0 0 0 0 0 0 0 0 0 0 0")
            return f"T {tid} {schedstat or ''} | {stat}"

        for kind, first_sched, last_sched, expected_cpu in (
                ("absent", None, None, 3.0),
                ("available", "1000000000 2000000000 100", "3000000000 5000000000 150", 20.0),
                ("only_first", "1000000000 2000000000 100", None, 3.0),
                ("only_last", None, "3000000000 5000000000 150", 3.0)):
            with self.subTest(kind=kind):
                raw = "\n".join(("#U 100.0", task(9487, 2701, 200, 4, first_sched),
                                 "#U 105.0", "T 9999  | ", "T 9998 1 2 3 | 9998 (gone) R 1",
                                 "#U 110.0", task(9487, 2721, 210, 6, last_sched)))
                blocks = capture.parse_threads(raw)
                self.assertEqual(len(blocks), 3)
                self.assertFalse(blocks[1]["threads"])
                self.assertEqual(blocks[0]["threads"][9487]["comm"], "Audio (mixer)")
                if first_sched is None:
                    self.assertTrue(all(blocks[0]["threads"][9487][key] is None
                                        for key in ("run_ns", "wait_ns", "slices")))
                summary = capture.thread_summary(blocks)
                row, = summary["threads"]
                self.assertEqual(row["cpu_pct"], expected_cpu)
                self.assertEqual(summary["process_cpu_cores"], expected_cpu / 100)
                self.assertEqual((row["user_pct"], row["sys_pct"]), (2.0, 1.0))
                self.assertEqual(row["cpus_seen"], [4, 6])
                available = kind == "available"
                self.assertEqual(row["cpu_time_source"], "schedstat" if available else "proc_stat")
                self.assertEqual(row["runqueue_wait_pct"], 30.0 if available else None)
                self.assertEqual(row["timeslices_per_s"], 5.0 if available else None)

        # An intact schedstat thread keeps precise timings beside a fallback thread.
        mixed = "\n".join(("#U 100", task(1, 100, 0, 0, threads=2), task(2, 100, 0, 1, "0 0 0", threads=2),
                           "#U 110", task(1, 200, 0, 0, threads=2), task(2, 200, 0, 1, "2500000000 500000000 30", threads=2)))
        summary = capture.thread_summary(capture.parse_threads(mixed))
        self.assertEqual(summary["process_cpu_cores"], .35)
        self.assertEqual([row["cpu_time_source"] for row in summary["threads"]], ["schedstat", "proc_stat"])
        self.assertIsNone(capture.thread_summary(capture.parse_threads("#U 100\n#U 110"))["process_cpu_cores"])

    def test_explicit_target_forwarding_and_current_viewport(self):
        session = module("perf_session_fixture", "tools/perf_session.py")
        capture = module("perf_capture_fixture", "tools/perf_capture.py")
        profile = module("profile_cpu_fixture", "tools/profile_cpu.py")
        batch = module("perf_batch_fixture", "work/evidence/perf-295/run_batch.py")
        analyze = module("perf_analyze_fixture", "tools/analyze_perfetto.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Missing an explicit device is rejected before every tool can perform I/O.
            for tool, argv in ((session, ["--profile", "userdata-perf-test", "--label", "test", "--out", str(root / "missing"), "--expect-apk", "a", "--expect-native", "b"]),
                               (capture, ["--seconds", "60", "--out", str(root / "missing"), "--label", "test"]),
                               (profile, ["--pid", "42", "--candidate", str(root), "--label", "test"]),
                               (batch, ["[]"])):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                    tool.main(argv)
                self.assertEqual(error.exception.code, 2)

            commands = []
            def run(command, **options):
                commands.append(command)
                if command[0] == sys.executable:
                    return SimpleNamespace(returncode=0, stdout='{"result":"CAPTURED"}\n', stderr="")
                self.assertEqual(command[1:3], ["-s", "TEST_SERIAL"])
                joined = " ".join(command)
                data = b"100" if "date +%s.%N" in joined else b"4242" if "pidof" in joined else b"yes" if "[ -d " in joined else b""
                return SimpleNamespace(returncode=0, stdout=data, stderr=b"")
            output = root / "session"
            argv = ["--serial", "TEST_SERIAL", "--screen", "2400", "1080", "--profile", "userdata-perf-test",
                    "--label", "test", "--out", str(output), "--capture-seconds", "60", "--settle-seconds", "0",
                    "--expect-apk", "a", "--expect-native", "b"]
            cached_thermal = """Thermal Status: 1
Cached temperatures:
  Temperature{mValue=37.0, mType=3, mName=shell_skin, mStatus=1}
HAL Ready: true
Current temperatures from HAL:
Current cooling devices from HAL:
  CoolingDevice{mValue=9999.0, mType=0, mName=cpu, mStatus=1}
"""
            def thermal_adb(*args, **options):
                return cached_thermal if "dumpsys" in " ".join(args) else "12345"
            with patch.object(session, "adb", side_effect=thermal_adb):
                thermal = session.thermal()
            self.assertEqual(thermal["temps"], {"shell_skin": 37.0})
            self.assertEqual(thermal["temp_sources"], {"shell_skin": "cached"})
            current_thermal = cached_thermal.replace("Current temperatures from HAL:", "Current temperatures from HAL:\nTemperature{mValue=35.5, mType=3, mName=shell_skin, mStatus=0}")
            with patch.object(session, "adb", side_effect=lambda *args: current_thermal if "dumpsys" in " ".join(args) else "12345"):
                current = session.thermal()
            self.assertEqual(current["temps"], {"shell_skin": 35.5})
            self.assertEqual(current["temp_sources"], {"shell_skin": "current_hal"})
            def thermal_after_stop():
                self.assertTrue(any(command[-3:] == ["am", "force-stop", "org.bioshock.port"] for command in commands))
                return thermal
            with patch.object(session.subprocess, "run", side_effect=run), patch.object(session, "thermal", side_effect=thermal_after_stop), \
                 patch.object(session, "wait_for", side_effect=[(100, "menu"), (101, "gameplay_ready=1")]), \
                 patch.object(session, "logcat_dump", return_value=""), patch.object(session.time, "sleep"), \
                 contextlib.redirect_stdout(io.StringIO()):
                session.main(argv)
            nested = next(command for command in commands if command[0] == sys.executable)
            self.assertEqual(nested[nested.index("--serial") + 1], "TEST_SERIAL")
            self.assertEqual(nested[nested.index("--pid") + 1], "4242")
            taps = [command for command in commands if "tap" in command]
            self.assertEqual(len(taps), 2)
            self.assertTrue(all(command[-2:] == ["1203", "620"] for command in taps))
            receipt = json.loads((output / "session.json").read_text())
            self.assertEqual(receipt["serial"], "TEST_SERIAL")
            self.assertEqual(receipt["input_screen"], [2400, 1080])
            self.assertTrue(receipt["stopped_after_session"])
            self.assertTrue(receipt["stopped_before_cooldown"])
            self.assertEqual(receipt["thermal_before"]["cooldown_sensor"], {"name": "shell_skin", "celsius": 37.0, "source": "cached"})

            def rejected_run(command, **options):
                if command[0] == sys.executable:
                    return SimpleNamespace(returncode=1, stdout='{"result":"REJECTED","coverage_gaps":1}\n', stderr="")
                return run(command, **options)
            rejected_output = root / "rejected-session"
            rejected_argv = [str(rejected_output) if value == str(output) else value for value in argv]
            with patch.object(session.subprocess, "run", side_effect=rejected_run), patch.object(session, "thermal", return_value=thermal), \
                 patch.object(session, "wait_for", side_effect=[(100, "menu"), (101, "gameplay_ready=1")]), \
                 patch.object(session, "logcat_dump", return_value=""), patch.object(session.time, "sleep"), \
                 contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "capture rejected"):
                session.main(rejected_argv)
            rejected = json.loads((rejected_output / "session.json").read_text())
            self.assertEqual(rejected["result"], "FAILED")
            self.assertEqual(rejected["capture_returncode"], 1)
            self.assertTrue(rejected["stopped_after_session"])
            missing_output = root / "missing-thermal"
            missing_argv = [str(missing_output) if value == str(output) else value for value in argv]
            with patch.object(session.subprocess, "run", side_effect=run), patch.object(session, "thermal", return_value={"status": 1, "temps": {}}), \
                 patch.object(session, "logcat_dump", return_value=""), patch.object(session.time, "sleep"), \
                 self.assertRaisesRegex(RuntimeError, "available skin/shell_skin"):
                session.main(missing_argv)
            missing = json.loads((missing_output / "session.json").read_text())
            self.assertEqual(missing["result"], "FAILED")
            self.assertTrue(missing["stopped_before_cooldown"])

            route = session.perf_route
            self.assertEqual(route.screen_size(lambda *args: "Physical size: 1080x2400\n"), (2400, 1080))
            self.assertEqual(route.screen_size(lambda *args: "Physical size: 1080x2400\nOverride size: 720x1600\n"), (1600, 720))
            swipes = []
            with patch.object(route.time, "monotonic", side_effect=[0, .1, .2, .3, 2]):
                self.assertEqual(route.walk(lambda *args, **kwargs: swipes.append(args), 1, screen=(2400, 1080)), 1)
            self.assertEqual(swipes[0][3:7], (672, 648, 672, 885))
            self.assertEqual(swipes[1][3:7], (2158, 214, 1899, 214))
            self.assertEqual(route.scale_point(route.STICK, route.CANONICAL_SCREEN), route.STICK)

            candidate = root / "build/candidates/touch-mobile-304"
            candidate.mkdir(parents=True)
            (candidate / "bioshock.json").write_text(json.dumps({"sha256": "abc", "library_sha256": "def"}))
            commands.clear()
            def batch_run(command, **options):
                commands.append(command)
                stdout = "package:/data/app/test/base.apk\n" if "path" in command else "abc base.apk\n" if "sha256sum" in command else "Success\n"
                return SimpleNamespace(returncode=0, stdout=stdout, stderr="")
            plan = [{"build": 304, "profile": "userdata-perf-test", "label": "test", "out": "batch"}]
            with patch.object(batch, "ROOT", root), patch.object(batch.subprocess, "run", side_effect=batch_run), contextlib.redirect_stdout(io.StringIO()):
                batch.main(["--serial", "TEST_SERIAL", "--screen", "2400", "1080", json.dumps(plan)])
            self.assertTrue(all(command[1:3] == ["-s", "TEST_SERIAL"] for command in commands if command[0] != sys.executable))
            nested = next(command for command in commands if command[0] == sys.executable)
            self.assertEqual(nested[nested.index("--serial") + 1], "TEST_SERIAL")
            self.assertEqual(nested[nested.index("--screen") + 1:], ["2400", "1080"])
            with patch.object(batch.subprocess, "run", return_value=SimpleNamespace(returncode=1, stdout="REJECTED", stderr="")), \
                 contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "session rejected"):
                batch.session(304, "userdata-perf-test", "", "test", "rejected")

            trace = root / "trace.pftrace"
            self.assertEqual(analyze.sampling_frequency(trace), 400)
            (root / "perfetto.pbtxt").write_text("timebase { frequency: 1000 }")
            self.assertEqual(analyze.sampling_frequency(trace), 1000)
            self.assertEqual(analyze.sampling_frequency(trace, 200), 200)
            (root / "perfetto.pbtxt").write_text("frequency: 400 frequency: 1000")
            with self.assertRaises(ValueError):
                analyze.sampling_frequency(trace)


if __name__ == "__main__":
    unittest.main()
