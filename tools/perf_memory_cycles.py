"""Repeated in-process reload cycles with memory snapshots (BioShock, user-selected device).

Launch a private userdata-perf-* profile -> original Continue -> gameplay_ready ->
original QuickSave (F8) -> N x original QuickLoad (F9) -> settle/play ->
snapshot; then Home (background) / resume. Each snapshot: /proc status
(VmRSS/RssAnon/RssFile/VmSwap/VmHWM), smaps_rollup (Pss/Pss_Anon/Private_Dirty/
SwapPss) via run-as, `dumpsys meminfo` (Java/Native heap, Gfx dev/Graphics,
Unknown, TOTAL PSS) and, when the QA trace flag is on, the bridge's api-memory
ledger lines. Writes JSONL plus least-squares slopes per reload cycle.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_session as ps  # noqa: E402

PACKAGE = ps.PACKAGE


def snapshot(pid, label):
    status = ps.adb("shell", f"cat /proc/{pid}/status", check=False)
    rollup = ps.adb("shell", f"run-as {PACKAGE} cat /proc/{pid}/smaps_rollup", check=False)
    meminfo = ps.adb("shell", f"dumpsys meminfo {pid}", check=False, timeout=120)
    row = {"label": label, "epoch": float(ps.adb("shell", "date +%s.%N").strip()), "pid": pid}
    for key in ("VmRSS", "VmHWM", "RssAnon", "RssFile", "RssShmem", "VmSwap", "VmSize", "Threads"):
        found = re.search(rf"(?m)^{key}:\s+(\d+)", status)
        if found:
            row[key] = int(found[1])
    for key in ("Pss", "Pss_Anon", "Pss_File", "Private_Dirty", "Private_Clean", "Anonymous", "Swap", "SwapPss"):
        found = re.search(rf"(?m)^{key}:\s+(\d+) kB", rollup)
        if found:
            row["rollup_" + key] = int(found[1])
    for name, pattern in (("java_heap", r"Java Heap:\s+(\d+)"), ("native_heap", r"Native Heap:\s+(\d+)"),
                          ("graphics", r"Graphics:\s+(\d+)"), ("gfx_dev", r"Gfx dev\s+(\d+)"),
                          ("unknown", r"Unknown\s+(\d+)"), ("total_pss", r"TOTAL PSS:\s+(\d+)"),
                          ("total_rss", r"TOTAL RSS:\s+(\d+)"), ("total_swap_pss", r"TOTAL SWAP PSS:\s+(\d+)")):
        found = re.search(pattern, meminfo)
        if found:
            row["meminfo_" + name] = int(found[1])
    return row


def slope(points):
    n = len(points)
    if n < 3:
        return None
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    den = sum((x - mx) ** 2 for x, _ in points)
    return sum((x - mx) * (y - my) for x, y in points) / den if den else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="Explicit user-selected adb device")
    parser.add_argument("--screen", nargs=2, type=int, metavar=("WIDTH", "HEIGHT"))
    parser.add_argument("--profile", required=True)
    parser.add_argument("--flags", default="")
    parser.add_argument("--cycles", type=int, default=8)
    parser.add_argument("--play-seconds", type=float, default=25)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expect-apk", required=True)
    parser.add_argument("--expect-native", required=True)
    args = parser.parse_args()
    ps.SERIAL = args.serial
    if not re.fullmatch(r"userdata-perf-[A-Za-z0-9._-]+", args.profile):
        parser.error("Only private userdata-perf-* QA profiles are allowed")
    args.out.mkdir(parents=True, exist_ok=False)
    rows, receipt = [], {"serial": args.serial, "profile": args.profile, "flags": args.flags, "cycles": args.cycles,
                         "utc_start": datetime.now(timezone.utc).isoformat()}
    journal = (args.out / "snapshots.jsonl").open("w", encoding="utf-8")

    def record(pid, label):
        row = snapshot(pid, label)
        rows.append(row)
        journal.write(json.dumps(row) + "\n")
        journal.flush()
        print(json.dumps({k: row.get(k) for k in ("label", "VmRSS", "rollup_Pss", "meminfo_gfx_dev", "VmSwap")}), flush=True)

    try:
        state = ps.thermal()
        receipt["thermal_before"] = state
        ps.adb("shell", "am", "force-stop", PACKAGE)
        time.sleep(1)
        profile = f"/sdcard/BioShock/game/{args.profile}"
        flags = [item for item in args.flags.split() if item]
        if flags:
            ps.adb("shell", f"printf '%s\\n' {' '.join(flags)} > {profile}/perf-flags.env")
        else:
            ps.adb("shell", f"rm -f {profile}/perf-flags.env")
        # The original QuickSave writes into the private QA profile: keep its
        # saves as they were for the next session.
        saves, backup = ps.save_paths(args.profile)
        ps.adb("shell", f"rm -rf '{backup}' && mkdir -p '{backup}' && cp -p '{saves}'/* '{backup}/'")
        receipt["saves_before"] = ps.save_listing(saves)
        if not receipt["saves_before"] or ps.save_listing(backup) != receipt["saves_before"]:
            raise RuntimeError("QA save backup failed")
        ps.adb("logcat", "-c")
        launch = float(ps.adb("shell", "date +%s.%N").strip())
        ps.adb("shell", "am", "start", "-n", f"{PACKAGE}/.GameActivity", "--es", "userdata", args.profile,
               "--es", "renderer", "dxvk-turnip")
        menu, _ = ps.wait_for(r"accepted unique vkQueuePresentKHR", 120, launch)
        if menu is None:
            raise RuntimeError("menu not reached")
        pid = int(ps.adb("shell", f"pidof {PACKAGE}").strip())
        paths = ps.adb("shell", f"pm path {PACKAGE}").strip().splitlines()
        installed = ps.adb("shell", f"run-as {PACKAGE} sha256sum {paths[0][8:]}").split()[0]
        if installed != args.expect_apk:
            raise RuntimeError("installed APK differs from the expected build")
        record(pid, "menu")
        time.sleep(max(0.0, 42.0 - (float(ps.adb("shell", "date +%s.%N").strip()) - menu)))
        screen = tuple(args.screen) if args.screen else ps.perf_route.screen_size(ps.adb)
        continue_point = ps.perf_route.scale_point(ps.CONTINUE, screen)
        ps.adb("shell", "input", "tap", *continue_point)
        time.sleep(2.0)
        tap = float(ps.adb("shell", "date +%s.%N").strip())
        ps.adb("shell", "input", "tap", *continue_point)
        ready, _ = ps.wait_for(r"gameplay_ready=1", 150, tap)
        if ready is None:
            raise RuntimeError("first load did not reach gameplay")
        receipt["first_load_s"] = round(ready - tap, 3)
        time.sleep(args.play_seconds)
        record(pid, "loaded")
        ps.adb("shell", "input", "keyevent", "KEYCODE_F8")      # original QuickSave
        time.sleep(8)
        loads = []
        for cycle in range(1, args.cycles + 1):
            press = float(ps.adb("shell", "date +%s.%N").strip())
            ps.adb("shell", "input", "keyevent", "KEYCODE_F9")  # original QuickLoad
            gone, _ = ps.wait_for(r"gameplay_ready=0", 30, press)
            back, _ = ps.wait_for(r"gameplay_ready=1", 150, press + 0.5 if gone is None else gone)
            loads.append(None if back is None else round(back - press, 3))
            if back is None:
                raise RuntimeError(f"QuickLoad cycle {cycle} did not return to gameplay")
            time.sleep(args.play_seconds)
            record(pid, f"reload-{cycle}")
        receipt["quickload_seconds"] = loads
        ps.adb("shell", "input", "keyevent", "KEYCODE_HOME")
        time.sleep(12)
        record(pid, "background")
        ps.adb("shell", "am", "start", "-n", f"{PACKAGE}/.GameActivity")
        time.sleep(15)
        record(pid, "resumed")
        if int(ps.adb("shell", f"pidof {PACKAGE}").strip() or 0) != pid:
            raise RuntimeError("process identity changed")
        cycles = [row for row in rows if row["label"].startswith("reload-")]
        receipt["slopes_kib_per_cycle"] = {
            key: slope([(i, row[key]) for i, row in enumerate(cycles) if key in row])
            for key in ("VmRSS", "rollup_Pss", "rollup_Private_Dirty", "rollup_Anonymous", "meminfo_native_heap",
                        "meminfo_gfx_dev", "meminfo_total_pss", "VmSwap")}
        receipt["result"] = "DONE"
    except BaseException as error:
        receipt.update(result="FAILED", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        journal.close()
        (args.out / "logcat.txt").write_text(ps.logcat_dump(), encoding="utf-8")
        ps.adb("shell", "am", "force-stop", PACKAGE, check=False)
        if receipt.get("saves_before"):
            try:
                saves, backup = ps.save_paths(args.profile)
                after = ps.save_listing(saves)
                for name in sorted(set(after) - set(receipt["saves_before"])):
                    ps.adb("shell", f"rm -f '{saves}/{name}'")
                for name, digest in receipt["saves_before"].items():
                    if after.get(name) != digest:
                        ps.adb("shell", f"cp -p '{backup}/{name}' '{saves}/{name}'")
                receipt["saves_after"] = {"new_removed": sorted(set(after) - set(receipt["saves_before"])),
                                          "identical": ps.save_listing(saves) == receipt["saves_before"]}
                ps.adb("shell", f"rm -rf '{backup}'", check=False)
            except Exception as error:
                receipt["saves_after"] = f"{type(error).__name__}: {error}"
        receipt["utc_end"] = datetime.now(timezone.utc).isoformat()
        (args.out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: receipt.get(k) for k in ("result", "first_load_s", "quickload_seconds", "slopes_kib_per_cycle")}))


if __name__ == "__main__":
    main()
