"""Bounded BioShock performance capture for one already running game process.

Display-delivered frames are counted from the game's SurfaceView in
`dumpsys SurfaceFlinger --latency` (unique actual-present timestamps, union of
overlapping 128-entry rings polled every 0.5 s on the device). Per-thread CPU
(/proc task stat, plus schedstat where available), system CPU, CPU frequencies, thermal zones and
the thermal-service status are sampled by independent workers in
tools/perf_sampler.sh, with a shared device deadline and uptime timestamps.

The script never starts, stops, installs or reconfigures the game and never
changes a device setting. It only reads, using the caller's explicit device.
"""
import argparse
import bisect
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

PACKAGE = "org.bioshock.port"
LIBRARY = "libbioshock.so"
ROOT = Path(__file__).resolve().parents[1]
SAMPLER = Path(__file__).resolve().parent / "perf_sampler.sh"
REMOTE_DIR = "/data/local/tmp/bioshock-perf"
INT64_MAX = (1 << 63) - 1
CLK_TCK = 100


class Adb:
    def __init__(self, serial):
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
        path = sdk / "platform-tools" / ("adb.exe" if os.name == "nt" else "adb")
        self.path = str(path if path.is_file() else shutil.which("adb"))
        self.serial = serial

    def run(self, *args, binary=False, timeout=30, check=True):
        result = subprocess.run([self.path, "-s", self.serial, *map(str, args)], capture_output=True,
                                timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f"adb {' '.join(map(str, args))[:120]} failed: {result.stderr[:300]!r}")
        return result.stdout if binary else result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")

    def shell(self, command, **kw):
        return self.run("shell", command, **kw)


def verify(adb, pid):
    package = adb.shell(f"cmd package list packages -U {PACKAGE}")
    found = re.search(rf"(?m)^package:{re.escape(PACKAGE)}\s+uid:(\d+)\s*$", package)
    uid = int(found[1]) if found else -1
    status = adb.shell(f"run-as {PACKAGE} cat /proc/{pid}/status")
    ids = re.search(r"(?m)^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", status)
    if uid < 10000 or not ids or any(int(v) != uid for v in ids.groups()):
        raise ValueError("PID does not belong to the installed BioShock package UID")
    cmdline = adb.run("shell", f"run-as {PACKAGE} cat /proc/{pid}/cmdline", binary=True)
    if cmdline.split(b"\0", 1)[0] != PACKAGE.encode():
        raise ValueError("PID is not the main BioShock process")
    stat = adb.shell(f"cat /proc/{pid}/stat")
    window = adb.shell("dumpsys window")
    focus = [line for line in window.splitlines() if line.strip().startswith("mCurrentFocus=")]
    focused = len(focus) == 1 and bool(re.search(rf"\bu\d+\s+{re.escape(PACKAGE)}/", focus[0]))
    return {"pid": pid, "uid": uid, "start_ticks": int(stat.rsplit(")", 1)[1].split()[19]),
            "foreground": focused, "focus_line": focus[0].strip() if focus else None}


def installed(adb, pid):
    apks = [line[8:].strip() for line in adb.shell(f"pm path {PACKAGE}").splitlines() if line.startswith("package:")]
    maps = adb.shell(f"run-as {PACKAGE} cat /proc/{pid}/maps")
    native = {m[1] for line in maps.splitlines()
              if (m := re.search(rf"\s(/data/app/\S+/{re.escape(LIBRARY)})$", line.strip()))}
    if len(apks) != 1 or len(native) != 1:
        raise ValueError("Expected one base APK and one loaded libbioshock.so")
    def pin(path):
        digest = adb.shell(f"run-as {PACKAGE} sha256sum {path}").split()[0]
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Invalid installed SHA256")
        return {"path": path, "sha256": digest}
    return {"apk": pin(apks[0]), "loaded_native": pin(native.pop())}


def layer_names(listing):
    """Layer names from `dumpsys SurfaceFlinger --list`, old plain or Android 16 RequestedLayerState form."""
    names = []
    for line in listing.splitlines():
        line = line.strip()
        found = re.fullmatch(r"RequestedLayerState\{(.+?#\d+)(?:\s+\w+=.*)?\}", line)
        names.append(found[1] if found else line)
    return names


def discover_layer(adb):
    marker = f"SurfaceView[{PACKAGE}/"
    names = [name for name in layer_names(adb.shell("dumpsys SurfaceFlinger --list"))
             if marker in name and not name.startswith("Background for")]
    blast = [name for name in names if "(BLAST)" in name] or names
    if len(blast) != 1:
        raise ValueError(f"No unique BioShock SurfaceView layer: {names}")
    return blast[0]


def parse_latency_file(text):
    """Return (refresh_ns, list of (uptime, set(actual)), records{actual: triple}, counts)."""
    polls, records, refresh = [], {}, set()
    counts = {"rows": 0, "zero": 0, "pending": 0, "invalid": 0}
    current, header_next = None, False
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#U "):
            current = (float(line.split()[1]), set())
            polls.append(current)
            header_next = True
            continue
        if not line or line.startswith("#") or current is None or line == "OPlusFrameTracker":
            continue
        if header_next:
            header_next = False
            if re.fullmatch(r"\d+", line):
                refresh.add(int(line))
                continue
        parts = line.split()
        if len(parts) != 3 or not all(re.fullmatch(r"-?\d+", p) for p in parts):
            continue
        triple = tuple(map(int, parts))
        counts["rows"] += 1
        actual = triple[1]
        if actual == 0:
            counts["zero"] += 1
        elif actual == INT64_MAX:
            counts["pending"] += 1
        elif actual < 0:
            counts["invalid"] += 1
        else:
            current[1].add(actual)
            records.setdefault(actual, triple)
    return refresh, polls, records, counts


def coverage_gaps(polls):
    gaps, previous = [], None
    for _, seen in polls:
        if previous and seen and not previous.intersection(seen) and max(previous) < min(seen):
            gaps.append(max(previous))
        if seen:
            previous = seen
    return gaps


def frame_stats(actual, gaps, refresh_ns):
    actual = sorted(actual)
    if len(actual) < 3:
        return {"error": "fewer than three unique presents", "publication_eligible": False}
    intervals = []
    for left, right in zip(actual, actual[1:]):
        if any(left <= g < right for g in gaps):
            continue
        intervals.append((right - left) / 1e6)
    ordered = sorted(intervals)
    n = len(ordered)
    def pct(q):
        return ordered[max(0, math.ceil(n * q) - 1)]
    worst1 = ordered[-max(1, n // 100):]
    worst01 = ordered[-max(1, n // 1000):]
    span = (actual[-1] - actual[0]) / 1e9
    windows = []
    t0 = actual[0]
    second = 0
    while t0 + (second + 1) * 1e9 <= actual[-1]:
        lo, hi = t0 + second * 1e9, t0 + (second + 1) * 1e9
        windows.append(bisect.bisect_left(actual, hi) - bisect.bisect_left(actual, lo))
        second += 1
    period = (min(refresh_ns) / 1e6) if refresh_ns else 16.6667
    on_time = sum(1 for v in intervals if v < period * 1.5)
    result = {
        "unique_presents": len(actual), "intervals": n, "span_s": round(span, 3),
        "coverage_gaps": len(gaps),
        "publication_eligible": not gaps,
        "mean_fps": round(len(intervals) * 1000 / sum(intervals), 3),
        "p50_ms": round(pct(.50), 3), "p90_ms": round(pct(.90), 3), "p95_ms": round(pct(.95), 3),
        "p99_ms": round(pct(.99), 3), "p999_ms": round(pct(.999), 3), "max_ms": round(ordered[-1], 3),
        "low1_fps_mean_worst1pct": round(1000 / statistics.fmean(worst1), 3),
        "low01_fps_mean_worst01pct": round(1000 / statistics.fmean(worst01), 3),
        "hitches": {f">{lim}ms": {"count": sum(1 for v in intervals if v > lim),
                                   "total_ms": round(sum(v for v in intervals if v > lim), 1)}
                    for lim in (20, 33.4, 50, 100)},
        "refresh_period_ms": round(period, 4),
        "single_period_fraction": round(on_time / n, 4),
        "one_second_windows": len(windows),
        "window_fps_min": min(windows) if windows else None,
        "window_fps_p5": sorted(windows)[max(0, math.ceil(len(windows) * .05) - 1)] if windows else None,
        "window_fps_median": statistics.median(windows) if windows else None,
        "windows_at_least_59": sum(1 for w in windows if w >= 59),
        "percentile_method": "nearest rank over actual-present intervals with known ring coverage",
    }
    return result


def _proc_stat(stat):
    if "(" not in stat or ")" not in stat:
        return None
    rest = stat.rsplit(")", 1)[1].split()
    if len(rest) < 37:
        return None
    try:
        return {"pid": int(stat.split("(", 1)[0]), "comm": stat[stat.find("(") + 1:stat.rfind(")")],
                "utime": int(rest[11]), "stime": int(rest[12]), "num_threads": int(rest[17]),
                "start_ticks": int(rest[19]), "cpu": int(rest[36]), "state": rest[0]}
    except ValueError:
        return None


def parse_threads(text):
    blocks, current = [], None
    for line in text.splitlines():
        if line.startswith("#U "):
            current = {"uptime": float(line.split()[1]), "threads": {}, "cpu": None,
                       "process": None, "scan_begin": False, "scan_end": None}
            blocks.append(current)
        elif current is None:
            continue
        elif line.startswith("#CPU_BEGIN "):
            current["scan_begin"] = True
            current["scan_end"] = None
        elif line.startswith("#CPU_END "):
            found = re.fullmatch(r"#CPU_END ([\d.]+) rows=(\d+)", line.strip())
            if found and current["scan_begin"]:
                current["scan_end"] = {"uptime": float(found[1]), "rows": int(found[2])}
            elif found:
                current["scan_end"] = {"uptime": float(found[1]), "rows": -1}
        elif line.startswith("P "):
            stamp, _, stat = line[2:].partition(" | ")
            process = _proc_stat(stat)
            if process:
                process["uptime"] = float(stamp)
                current["process"] = process
        elif line.startswith("cpu "):
            current["cpu"] = list(map(int, line.split()[1:]))
        elif line.startswith("T "):
            head, _, stat = line[2:].partition(" | ")
            fields = head.split()
            if not fields or ")" not in stat:
                continue
            tid = int(fields[0])
            run_ns = wait_ns = slices = None
            if len(fields) >= 4:
                run_ns, wait_ns, slices = map(int, fields[1:4])
            task = _proc_stat(stat)
            if task is None or task["pid"] != tid:
                continue
            current["threads"][tid] = {**task, "run_ns": run_ns, "wait_ns": wait_ns, "slices": slices}
    for block in blocks:
        rows = len(block["threads"])
        expected = [task["num_threads"] for task in block["threads"].values() if task["num_threads"] > 0]
        block["expected_threads"] = min(expected) if expected else None
        if block["scan_begin"] or block["scan_end"] is not None:
            block["complete"] = bool(block["scan_begin"] and block["scan_end"] and
                                     block["scan_end"]["rows"] == rows and rows and
                                     block["scan_end"]["uptime"] >= block["uptime"])
            block["completeness_source"] = "explicit_markers"
        else:
            # Old samplers could be killed halfway through the final scan. The
            # process-wide num_threads is present even in each task stat row.
            block["complete"] = bool(rows and expected and rows >= min(expected))
            block["completeness_source"] = "legacy_num_threads"
    return blocks


def thread_summary(blocks):
    all_blocks = blocks
    blocks = [block for block in blocks if block.get("complete", False)]
    coverage = {"complete_snapshots": len(blocks), "incomplete_snapshots": len(all_blocks) - len(blocks),
                "excluded_snapshots": [{"uptime": b["uptime"], "rows": len(b["threads"]),
                                        "expected_threads": b.get("expected_threads"),
                                        "source": b.get("completeness_source")}
                                       for b in all_blocks if not b.get("complete", False)]}
    if len(blocks) < 2 or blocks[-1]["uptime"] <= blocks[0]["uptime"]:
        return {**coverage, "window_s": None, "process_cpu_cores": None, "process_cpu_source": None,
                "process_cpu_exact": False, "thread_cpu_cores": None, "system": None, "threads": []}
    first, last = blocks[0], blocks[-1]
    secs = last["uptime"] - first["uptime"]
    rows = []
    for tid, end in last["threads"].items():
        start = first["threads"].get(tid)
        if not start or start["start_ticks"] != end["start_ticks"]:
            continue
        cpus = sorted({b["threads"][tid]["cpu"] for b in blocks if tid in b["threads"]})
        has_schedstat = start["run_ns"] is not None and end["run_ns"] is not None
        # Some Android kernels omit schedstat entirely; task stat still records CPU ticks.
        run = ((end["run_ns"] - start["run_ns"]) / 1e9 if has_schedstat else
               (end["utime"] + end["stime"] - start["utime"] - start["stime"]) / CLK_TCK)
        if run < 0 or end["utime"] < start["utime"] or end["stime"] < start["stime"]:
            continue
        rows.append({"tid": tid, "comm": end["comm"], "cpu_pct": round(100 * run / secs, 2),
                     "cpu_time_source": "schedstat" if has_schedstat else "proc_stat",
                     "user_pct": round(100 * (end["utime"] - start["utime"]) / CLK_TCK / secs, 2),
                     "sys_pct": round(100 * (end["stime"] - start["stime"]) / CLK_TCK / secs, 2),
                     "runqueue_wait_pct": round(100 * (end["wait_ns"] - start["wait_ns"]) / 1e9 / secs, 2) if has_schedstat else None,
                     "timeslices_per_s": round((end["slices"] - start["slices"]) / secs, 1) if has_schedstat else None,
                     "cpus_seen": cpus})
    rows.sort(key=lambda r: -r["cpu_pct"])
    total = sum(r["cpu_pct"] for r in rows) / 100 if rows else None
    process, process_source, process_window = total, "common_threads" if rows else None, secs
    a, b = first.get("process"), last.get("process")
    if a and b and a["pid"] == b["pid"] and a["start_ticks"] == b["start_ticks"] and b["uptime"] > a["uptime"]:
        ticks = b["utime"] + b["stime"] - a["utime"] - a["stime"]
        if ticks >= 0:
            process_window = b["uptime"] - a["uptime"]
            process = ticks / CLK_TCK / process_window
            process_source = "proc_pid_stat"
    system = None
    if first["cpu"] and last["cpu"]:
        delta = [b - a for a, b in zip(first["cpu"], last["cpu"])]
        busy = sum(delta) - delta[3] - delta[4]
        system = {"busy_cores": round(busy / CLK_TCK / secs, 3)}
    return {**coverage, "window_s": round(secs, 3),
            "process_cpu_cores": round(process, 3) if process is not None else None,
            "process_cpu_source": process_source, "process_window_s": round(process_window, 3),
            "process_cpu_exact": process_source == "proc_pid_stat",
            "thread_population_changed": set(first["threads"]) != set(last["threads"]),
            "thread_cpu_cores": round(total, 3) if total is not None else None,
            "thread_total_source": "common_threads" if rows else None,
            "system": system, "threads": rows[:40]}


def parse_system(text):
    samples, current = [], None
    for line in text.splitlines():
        if line.startswith("#U "):
            current = {"uptime": float(line.split()[1]), "freq": {}, "zones": {}, "mem": {}}
            samples.append(current)
        elif current is None:
            continue
        elif line.startswith("F "):
            parts = line.split()
            if len(parts) == 4:
                current["freq"][int(parts[1])] = (int(parts[2]), int(parts[3]))
        elif line.startswith("Z "):
            parts = line.split()
            if len(parts) == 3 and re.fullmatch(r"-?\d+", parts[2]):
                current["zones"][parts[1]] = int(parts[2]) / 1000
        elif line.startswith("M "):
            key, _, value = line[2:].partition(":")
            current["mem"][key] = int(value.split()[0])
    return samples


def system_summary(samples):
    if not samples:
        return {}
    out = {"samples": len(samples), "cpu": {}, "zones_max_c": {}, "memory_kib": {}}
    for cpu in sorted({c for s in samples for c in s["freq"]}):
        cur = [s["freq"][cpu][0] for s in samples if cpu in s["freq"]]
        cap = [s["freq"][cpu][1] for s in samples if cpu in s["freq"]]
        out["cpu"][cpu] = {"cur_khz_median": int(statistics.median(cur)), "scaling_max_khz_min": min(cap),
                           "scaling_max_khz_max": max(cap)}
    for zone in sorted({z for s in samples for z in s["zones"]}):
        values = [s["zones"][zone] for s in samples if zone in s["zones"]]
        out["zones_max_c"][zone] = max(values)
    for key in ("VmRSS", "VmHWM", "RssAnon", "RssFile", "VmSwap"):
        values = [s["mem"][key] for s in samples if key in s["mem"]]
        if values:
            out["memory_kib"][key] = {"first": values[0], "last": values[-1], "max": max(values)}
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="Explicit user-selected adb device")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--out", type=Path, required=True, help="New directory; never overwritten")
    parser.add_argument("--label", required=True, help="Scene/route/settings description recorded in the receipt")
    parser.add_argument("--expect-apk", help="Expected installed APK SHA256")
    parser.add_argument("--expect-native", help="Expected loaded libbioshock.so SHA256")
    args = parser.parse_args(argv)
    if not 5 <= args.seconds <= 3600:
        parser.error("seconds must be within 5..3600")
    adb = Adb(args.serial)
    pid = args.pid
    if pid is None:
        pids = adb.shell(f"pidof {PACKAGE}").split()
        if len(pids) != 1:
            parser.error(f"Expected one running {PACKAGE} process, found {pids}")
        pid = int(pids[0])
    args.out.mkdir(parents=True, exist_ok=False)
    receipt = {"tool": "tools/perf_capture.py", "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "sampler_sha256": hashlib.sha256(SAMPLER.read_bytes()).hexdigest(),
               "serial": args.serial, "label": args.label, "requested_seconds": args.seconds,
               "utc_start": datetime.now(timezone.utc).isoformat()}
    try:
        before = verify(adb, pid)
        receipt["target_before"] = before
        if not before["foreground"]:
            raise ValueError("BioShock is not the focused window; refusing a background measurement")
        receipt["installed"] = installed(adb, pid)
        for key, want in (("apk", args.expect_apk), ("loaded_native", args.expect_native)):
            if want and receipt["installed"][key]["sha256"] != want:
                raise ValueError(f"Installed {key} SHA256 differs from the expected build")
        layer = discover_layer(adb)
        receipt["layer"] = layer
        device_epoch = adb.shell("date +%s.%N").strip()
        receipt["device_epoch_start"] = device_epoch
        remote = f"{REMOTE_DIR}/{args.out.name}"
        adb.shell(f"mkdir -p {REMOTE_DIR}")
        adb.run("push", str(SAMPLER), f"{REMOTE_DIR}/perf_sampler.sh")
        quoted = "'" + layer.replace("'", "'\\''") + "'"
        adb.shell(f"rm -rf {remote}; nohup sh {REMOTE_DIR}/perf_sampler.sh {pid} {quoted} {args.seconds} {remote} "
                  f">{REMOTE_DIR}/{args.out.name}.stdout 2>&1 &")
        started = time.monotonic()
        while True:
            time.sleep(2)
            done = adb.shell(f"[ -e {remote}/done ] && echo yes || echo no", check=False).strip()
            if done == "yes":
                break
            if time.monotonic() - started > args.seconds + 60:
                raise TimeoutError("Device sampler did not finish")
        receipt["host_elapsed_s"] = round(time.monotonic() - started, 3)
        for name in ("latency.txt", "threads.txt", "system.txt", "thermal.txt"):
            (args.out / name).write_bytes(adb.run("exec-out", f"cat {remote}/{name}", binary=True, timeout=120))
        end_epoch = adb.shell("date +%s.%N").strip()
        receipt["device_epoch_end"] = end_epoch
        log = adb.run("logcat", "-d", "-v", "epoch", "--pid", str(pid), "-T", device_epoch, binary=True, timeout=60)
        lines = [l for l in log.decode("utf-8", "replace").splitlines()
                 if re.search(r"BioShock\s*:\s*\[(dxvk\] frame|gl\] frame|hitch\])", l)]
        (args.out / "game-frame-log.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        after = verify(adb, pid)
        receipt["target_after"] = after
        identity_same = (after["pid"], after["uid"], after["start_ticks"]) == (before["pid"], before["uid"], before["start_ticks"])
        receipt["identity_unchanged"] = identity_same
        receipt["foreground_at_end"] = after["foreground"]
        refresh, polls, records, counts = parse_latency_file((args.out / "latency.txt").read_text("utf-8", "replace"))
        gaps = coverage_gaps(polls)
        receipt["latency_counts"] = {**counts, "polls": len(polls), "refresh_ns": sorted(refresh),
                                     "unique_actual": len(records)}
        receipt["frames"] = frame_stats(list(records), gaps, refresh)
        actual = sorted(records)
        with (args.out / "present-intervals.csv").open("w", encoding="utf-8") as stream:
            stream.write("actual_present_ns,interval_ms,after_coverage_gap\n")
            for left, right in zip(actual, actual[1:]):
                gap = any(left <= g < right for g in gaps)
                stream.write(f"{right},{(right - left) / 1e6:.4f},{int(gap)}\n")
        receipt["threads"] = thread_summary(parse_threads((args.out / "threads.txt").read_text("utf-8", "replace")))
        receipt["system"] = system_summary(parse_system((args.out / "system.txt").read_text("utf-8", "replace")))
        receipt["thermal_status_lines"] = [l for l in (args.out / "thermal.txt").read_text("utf-8", "replace").splitlines()
                                           if "Thermal Status" in l]
        receipt["game_frame_log_lines"] = len(lines)
        receipt["publication_eligible"] = bool(identity_same and after["foreground"] and
                                                receipt["frames"].get("publication_eligible"))
        receipt["result"] = "CAPTURED" if receipt["publication_eligible"] else "REJECTED"
    except BaseException as error:
        receipt.update(result="FAILED", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        receipt["utc_end"] = datetime.now(timezone.utc).isoformat()
        (args.out / "receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    f = receipt["frames"]
    print(json.dumps({"result": receipt["result"], "label": args.label, "mean_fps": f.get("mean_fps"),
                      "p50": f.get("p50_ms"), "p95": f.get("p95_ms"), "p99": f.get("p99_ms"), "max": f.get("max_ms"),
                      "low1": f.get("low1_fps_mean_worst1pct"), "hitch33": f.get("hitches", {}).get(">33.4ms"),
                      "coverage_gaps": f.get("coverage_gaps"), "publication_eligible": receipt["publication_eligible"],
                      "process_cores": receipt["threads"].get("process_cpu_cores"), "out": str(args.out)},
                     ensure_ascii=False))
    if not receipt["publication_eligible"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
