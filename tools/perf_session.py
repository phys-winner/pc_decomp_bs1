"""One reproducible BioShock performance session on the user-selected device.

Validate QA profile -> force-stop -> cool-down gate -> optional QA perf-flags.env in the private QA
profile -> launch -> original menu -> original "Continue" (two taps: the first
is consumed by the original attract-accept trap) -> wait for the game's own
gameplay_ready=1 (120 s load limit kept as a verdict, never extended) -> settle
-> tools/perf_capture.py -> optional perfetto CPU trace.

Only private QA profiles named userdata-perf-* are accepted; the player's
default profile is never launched or written.
"""
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time
try:
    from . import perf_route
except ImportError:
    import perf_route

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "org.bioshock.port"
SERIAL = None
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
ADB = str(SDK / "platform-tools/adb.exe") if (SDK / "platform-tools/adb.exe").is_file() else shutil.which("adb")
CONTINUE = (1393, 726)


def adb(*args, binary=False, timeout=60, check=True):
    result = subprocess.run([ADB, "-s", SERIAL, *map(str, args)], capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f"adb {args[:3]} failed: {result.stderr[:200]!r}")
    return result.stdout if binary else result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")


def thermal():
    text = adb("shell", "dumpsys thermalservice")
    status = re.search(r"Thermal Status: (\d+)", text)
    cached, _, current = text.partition("Current temperatures from HAL:")
    temps, sources = {}, {}
    for source, block in (("cached", cached), ("current_hal", current)):
        for value, name in re.findall(r"Temperature\{mValue=([^,}]+), mType=\d+, mName=([^,}]+)", block):
            temps[name.strip()] = float(value)
            sources[name.strip()] = source
    caps = {}
    for cpu in (0, 3, 7):
        caps[cpu] = int(adb("shell", f"cat /sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_max_freq").strip() or 0)
    return {"status": int(status[1]) if status else -1, "temps": temps, "temp_sources": sources, "scaling_max_khz": caps}


def logcat_dump():
    return adb("logcat", "-d", "-v", "epoch", "-s", "BioShock:I", timeout=120)


def record_perfetto(config, directory, seconds):
    remote = f"/data/misc/perfetto-traces/bioshock-session-{os.getpid()}.pftrace"
    unwind = "frame_pointer"
    try:
        for attempt in range(2):
            (directory / "perfetto.pbtxt").write_text(config, encoding="utf-8")
            result = subprocess.run([ADB, "-s", SERIAL, "shell", "perfetto", "--txt", "-c", "-", "-o", remote],
                                    input=config.encode(), capture_output=True, timeout=seconds + 120)
            output = (result.stdout + result.stderr).decode("utf-8", "replace")
            (directory / f"perfetto-record-{attempt + 1}.log").write_text(output, encoding="utf-8")
            if not result.returncode:
                break
            # Older Perfetto accepts DWARF sampling but does not know the FP enum.
            if attempt == 0 and "Unexpected value 'UNWIND_FRAME_POINTER'" in output:
                config, removed = re.subn(r"(?m)^\s*user_frames:\s*UNWIND_FRAME_POINTER\s*\n", "", config)
                if removed:
                    unwind = "default_dwarf"
                    continue
            raise RuntimeError(f"Perfetto rejected capture: {output[-1000:]}")
        adb("pull", remote, str(directory / "trace.pftrace"), timeout=300)
        return {"unwind_mode": unwind, "config_transport": "stdin", "attempts": attempt + 1}
    finally:
        adb("shell", "rm", "-f", remote, check=False)


def save_paths(profile):
    base = f"/sdcard/BioShock/game/{profile}"
    return f"{base}/Documents/Bioshock/SaveGames", f"{base}/SaveGames-route-backup"


def save_listing(folder):
    text = adb("shell", f"cd '{folder}' 2>/dev/null && sha256sum * 2>/dev/null", check=False)
    return {parts[1]: parts[0] for parts in (line.split() for line in text.splitlines()) if len(parts) == 2}


def wait_for(pattern, deadline_s, since_epoch):
    """A process that was never seen yet is still starting (cold start after force-stop)."""
    start = time.monotonic()
    regex = re.compile(pattern)
    seen = False
    while time.monotonic() - start < deadline_s:
        for line in logcat_dump().splitlines():
            parts = line.split()
            if len(parts) > 2 and re.fullmatch(r"\d+\.\d+", parts[0]) and float(parts[0]) >= since_epoch and regex.search(line):
                return float(parts[0]), line.strip()
        running = bool(adb("shell", f"pidof {PACKAGE}", check=False).strip())
        seen = seen or running
        if not running and (seen or time.monotonic() - start > 15):
            return None, "process exited" if seen else "process did not start"
        time.sleep(1.0)
    return None, "timeout"


def main(argv=None):
    global SERIAL
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="Explicit user-selected adb device")
    parser.add_argument("--screen", nargs=2, type=int, metavar=("WIDTH", "HEIGHT"), help="Landscape input viewport; defaults to current wm size")
    parser.add_argument("--profile", required=True)
    parser.add_argument("--flags", default="", help="Whitespace separated KEY=VALUE QA flags; empty removes perf-flags.env")
    parser.add_argument("--renderer", default="dxvk-turnip", choices=["dxvk-turnip", "dxvk", "gles"])
    parser.add_argument("--label", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--capture-seconds", type=float, default=60)
    parser.add_argument("--settle-seconds", type=float, default=15)
    parser.add_argument("--perfetto-seconds", type=float, default=0)
    parser.add_argument("--tu-debug", default="",
                        help="QA Turnip flags written to the profile's tu-debug.txt for this session only (TU_DEBUG_FILE)")
    parser.add_argument("--route", default="static", choices=["static", "walk"],
                        help="walk: original touch stick/look drags during the capture (tools/perf_route.py)")
    parser.add_argument("--max-thermal-status", type=int, default=1)
    parser.add_argument("--cool-wait-seconds", type=float, default=900)
    parser.add_argument("--expect-apk", required=True)
    parser.add_argument("--expect-native", required=True)
    args = parser.parse_args(argv)
    SERIAL = args.serial
    if not re.fullmatch(r"userdata-perf-[A-Za-z0-9._-]+", args.profile):
        parser.error("Only private userdata-perf-* QA profiles are allowed")
    args.out.mkdir(parents=True, exist_ok=False)
    receipt = {"label": args.label, "serial": args.serial, "profile": args.profile, "flags": args.flags, "renderer": args.renderer,
               "utc_start": datetime.now(timezone.utc).isoformat(), "tool_sha256": __import__("hashlib").sha256(Path(__file__).read_bytes()).hexdigest()}
    try:
        profile = f"/sdcard/BioShock/game/{args.profile}"
        if adb("shell", f"[ -d {profile} ] && echo yes || echo no").strip() != "yes":
            raise RuntimeError(f"Missing QA profile {profile}")
        adb("shell", "am", "force-stop", PACKAGE)
        receipt["stopped_before_cooldown"] = True
        time.sleep(1.0)
        start = time.monotonic()
        while True:
            state = thermal()
            skin = [(value, name) for name, value in state["temps"].items()
                    if name in ("skin", "shell_skin") and math.isfinite(value)]
            if not skin or state["status"] < 0:
                raise RuntimeError(f"Thermal gate needs an available skin/shell_skin temperature and status: {state}")
            value, name = max(skin)
            state["cooldown_sensor"] = {"name": name, "celsius": value,
                                        "source": state.get("temp_sources", {}).get(name, "unspecified")}
            if state["status"] <= args.max_thermal_status and value < 38.0:
                break
            if time.monotonic() - start > args.cool_wait_seconds:
                raise RuntimeError(f"Device did not cool down: {state}")
            time.sleep(20)
        receipt["thermal_before"] = state
        receipt["cooldown_wait_s"] = round(time.monotonic() - start, 1)
        flags = [item for item in args.flags.split() if item]
        for item in flags:
            if not re.fullmatch(r"[A-Z][A-Z0-9_]*=[A-Za-z0-9._-]+", item):
                raise ValueError(f"Invalid flag {item}")
        if flags:
            adb("shell", f"printf '%s\\n' {' '.join(flags)} > {profile}/perf-flags.env")
        else:
            adb("shell", f"rm -f {profile}/perf-flags.env")
        receipt["flags_file"] = adb("shell", f"cat {profile}/perf-flags.env 2>/dev/null", check=False)
        if args.tu_debug:
            if not re.fullmatch(r"[a-z0-9_,]+", args.tu_debug):
                raise ValueError(f"Invalid Turnip flags {args.tu_debug}")
            adb("shell", f"echo {args.tu_debug} > {profile}/tu-debug.txt")
        else:
            adb("shell", f"rm -f {profile}/tu-debug.txt")
        receipt["tu_debug"] = args.tu_debug
        if args.route == "walk":
            saves, backup = save_paths(args.profile)
            adb("shell", f"rm -rf '{backup}' && mkdir -p '{backup}' && cp -p '{saves}'/* '{backup}/'")
            receipt["saves_before"] = save_listing(saves)
            if not receipt["saves_before"] or save_listing(backup) != receipt["saves_before"]:
                raise RuntimeError("QA save backup failed")
        adb("logcat", "-c")
        launch_epoch = float(adb("shell", "date +%s.%N").strip())
        adb("shell", "am", "start", "-n", f"{PACKAGE}/.GameActivity", "--es", "userdata", args.profile,
            "--es", "renderer", args.renderer)
        menu_epoch, menu_line = wait_for(r"accepted unique vkQueuePresentKHR|\[gl\] frame \d+:", 120, launch_epoch)
        receipt["menu"] = {"epoch": menu_epoch, "seconds": None if menu_epoch is None else round(menu_epoch - launch_epoch, 3),
                           "evidence": menu_line}
        if menu_epoch is None:
            raise RuntimeError("Original menu not reached within 120 s")
        # The original attract timer arms bTrapNextAccept 40 s into the menu.
        time.sleep(max(0.0, 42.0 - (float(adb("shell", "date +%s.%N").strip()) - menu_epoch)))
        (args.out / "01-menu.png").write_bytes(adb("exec-out", "screencap", "-p", binary=True))
        screen = tuple(args.screen) if args.screen else perf_route.screen_size(adb)
        continue_point = perf_route.scale_point(CONTINUE, screen)
        receipt["input_screen"] = list(screen)
        receipt["continue_point"] = list(continue_point)
        adb("shell", "input", "tap", *continue_point)
        time.sleep(2.0)
        tap_epoch = float(adb("shell", "date +%s.%N").strip())
        adb("shell", "input", "tap", *continue_point)
        ready_epoch, ready_line = wait_for(r"gameplay_ready=1", 150, tap_epoch)
        load = None if ready_epoch is None else round(ready_epoch - tap_epoch, 3)
        receipt["load"] = {"tap_epoch": tap_epoch, "ready_epoch": ready_epoch, "seconds": load,
                           "limit_s": 120, "within_limit": load is not None and load <= 120, "evidence": ready_line}
        if ready_epoch is None:
            raise RuntimeError(f"gameplay_ready not reached: {ready_line}")
        time.sleep(args.settle_seconds)
        (args.out / "02-capture-start.png").write_bytes(adb("exec-out", "screencap", "-p", binary=True))
        pid = int(adb("shell", f"pidof {PACKAGE}").strip())
        receipt["pid"] = pid
        receipt["thermal_capture_start"] = thermal()
        # GPU busy (KGSL busy/total microseconds, readable in the app's context)
        # sampled once per second while the frames are captured.
        gpu_samples, gpu_stop, memory_samples = [], threading.Event(), []
        capture_start = time.monotonic()

        def sample_gpu():
            next_memory = 0.0
            while not gpu_stop.is_set():
                text = adb("shell", f"run-as {PACKAGE} cat /sys/class/kgsl/kgsl-3d0/gpubusy", check=False).split()
                if len(text) == 2 and text[0].isdigit() and text[1].isdigit() and int(text[1]):
                    gpu_samples.append(round(100.0 * int(text[0]) / int(text[1]), 1))
                elapsed = time.monotonic() - capture_start
                if elapsed >= next_memory:
                    # Process memory every 30 s (kB from /proc status), for long-run slopes.
                    status = adb("shell", f"cat /proc/{pid}/status", check=False)
                    row = {"t": round(elapsed, 1)}
                    for key in ("VmRSS", "RssAnon", "RssFile", "VmSwap", "VmHWM"):
                        found = re.search(rf"(?m)^{key}:\s+(\d+)", status)
                        if found:
                            row[key] = int(found[1])
                    memory_samples.append(row)
                    next_memory = elapsed + 30.0
                gpu_stop.wait(1.0)
        gpu_thread = threading.Thread(target=sample_gpu, daemon=True)
        gpu_thread.start()
        walker = None
        if args.route == "walk":
            walker = perf_route.start_walk(adb, args.capture_seconds, screen)
        capture = subprocess.run([sys.executable, str(ROOT / "tools/perf_capture.py"), "--serial", args.serial, "--pid", str(pid),
                                  "--seconds", str(args.capture_seconds), "--out", str(args.out / "capture"),
                                  "--label", args.label, "--expect-apk", args.expect_apk, "--expect-native", args.expect_native],
                                 capture_output=True, text=True, timeout=args.capture_seconds + 300)
        gpu_stop.set()
        gpu_thread.join(timeout=10)
        if walker is not None:
            walker[1].set()
            walker[0].join(timeout=30)
            receipt["route"] = {"name": "walk", "steps": walker[2][0] if walker[2] else None}
        if memory_samples:
            receipt["memory_samples_kb"] = memory_samples
            rss = [(row["t"], row["VmRSS"]) for row in memory_samples if "VmRSS" in row]
            if len(rss) >= 3:
                mean_t = sum(t for t, _ in rss) / len(rss)
                mean_v = sum(v for _, v in rss) / len(rss)
                den = sum((t - mean_t) ** 2 for t, _ in rss)
                receipt["rss_slope_kb_per_min"] = round(60 * sum((t - mean_t) * (v - mean_v) for t, v in rss) / den, 1) if den else None
        if gpu_samples:
            ordered = sorted(gpu_samples)
            receipt["gpu_busy_pct"] = {"samples": len(ordered), "mean": round(sum(ordered) / len(ordered), 1),
                                       "p50": ordered[len(ordered) // 2], "p90": ordered[int(len(ordered) * 0.9)],
                                       "max": ordered[-1]}
        (args.out / "capture.log").write_text(capture.stdout + capture.stderr, encoding="utf-8")
        receipt["capture_returncode"] = capture.returncode
        receipt["capture_summary"] = capture.stdout.strip().splitlines()[-1] if capture.stdout.strip() else None
        if capture.returncode:
            raise RuntimeError(f"Performance capture rejected (exit {capture.returncode}); inspect capture/receipt.json")
        (args.out / "03-capture-end.png").write_bytes(adb("exec-out", "screencap", "-p", binary=True))
        if args.perfetto_seconds:
            config = (ROOT / "tools/perfetto/bioshock-cpu-fp.pbtxt").read_text(encoding="utf-8")
            config = re.sub(r"duration_ms: \d+", f"duration_ms: {int(args.perfetto_seconds * 1000)}", config)
            receipt["perfetto"] = record_perfetto(config, args.out, args.perfetto_seconds)
        receipt["thermal_end"] = thermal()
        (args.out / "logcat.txt").write_text(logcat_dump(), encoding="utf-8")
        receipt["result"] = "DONE"
    except BaseException as error:
        receipt.update(result="FAILED", error=f"{type(error).__name__}: {error}")
        try:
            (args.out / "logcat.txt").write_text(logcat_dump(), encoding="utf-8")
            (args.out / "failure.png").write_bytes(adb("exec-out", "screencap", "-p", binary=True))
        except Exception:
            pass
        raise
    finally:
        # A QA session that keeps rendering after its capture heats the device
        # through the next session's cool-down gate (batch 2's last run timed out
        # at status 3 behind it): stop it as soon as the evidence is collected.
        try:
            adb("shell", "am", "force-stop", PACKAGE, check=False)
            receipt["stopped_after_session"] = True
        except Exception as error:
            receipt["stopped_after_session"] = f"{type(error).__name__}: {error}"
        if args.tu_debug:
            adb("shell", f"rm -f /sdcard/BioShock/game/{args.profile}/tu-debug.txt", check=False)
        if args.route == "walk" and receipt.get("saves_before") is not None:
            # Walking may reach an original autosave: the private QA profile must
            # start the next session from the same save.
            try:
                saves, backup = save_paths(args.profile)
                after = save_listing(saves)
                for name in sorted(set(after) - set(receipt["saves_before"])):
                    adb("shell", f"rm -f '{saves}/{name}'")
                for name, digest in receipt["saves_before"].items():
                    if after.get(name) != digest:
                        adb("shell", f"cp -p '{backup}/{name}' '{saves}/{name}'")
                restored = save_listing(saves)
                receipt["saves_after"] = {"new_removed": sorted(set(after) - set(receipt["saves_before"])),
                                          "identical": restored == receipt["saves_before"]}
                adb("shell", f"rm -rf '{backup}'", check=False)
            except Exception as error:
                receipt["saves_after"] = f"{type(error).__name__}: {error}"
        receipt["utc_end"] = datetime.now(timezone.utc).isoformat()
        (args.out / "session.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"label": args.label, "load_s": receipt["load"]["seconds"], "capture": receipt.get("capture_summary")},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
