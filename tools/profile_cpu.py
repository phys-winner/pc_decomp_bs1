"""Sample an already running BioShock PID with Simpleperf; never start or restart the app.

Symbols must belong to the exact installed build: the tool checks the installed
base APK and the libbioshock.so actually mapped by the PID against the given
candidate receipt, then symbolizes with the archived unstripped library of that
candidate. The default event `cpu-cycles` counts user AND kernel time (this
device reports perf_event_paranoid=-1); pass `--event cpu-cycles:u` only for a
user-space-only comparison.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "org.bioshock.port"
LIBRARY = "libbioshock.so"
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
NDK = Path(os.environ.get("ANDROID_NDK_HOME", SDK / "ndk/27.2.12479018"))


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_candidate(candidate, symbols=None):
    """Pin APK payload and symbols separately, including stripped runtime parity."""
    try:
        from .elf_identity import assert_runtime_identity
    except ImportError:
        from elf_identity import assert_runtime_identity
    receipt = json.loads((candidate / "bioshock.json").read_text())
    apk = candidate / "bioshock.apk"
    symbols = symbols or candidate / "libbioshock.unstripped.so"
    if digest(apk) != receipt["sha256"] or digest(symbols) != receipt.get("symbols_sha256", receipt["library_sha256"]):
        raise ValueError("Candidate APK/symbols differ from their receipt")
    with zipfile.ZipFile(apk) as archive:
        shipped = archive.read("lib/arm64-v8a/" + LIBRARY)
    if hashlib.sha256(shipped).hexdigest() != receipt["library_sha256"]:
        raise ValueError("Candidate APK native payload differs from its receipt")
    identity = assert_runtime_identity(symbols.read_bytes(), shipped)
    if not identity["has_symbols"]:
        raise ValueError("Archived symbols library has no symbol table")
    if receipt.get("allocated_identity", identity["allocated_sha256"]) != identity["allocated_sha256"]:
        raise ValueError("Candidate allocated identity differs from its receipt")
    return receipt, apk, symbols, identity


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="Already running BioShock PID supplied by the caller")
    parser.add_argument("--serial", required=True, help="Explicit user-selected adb device")
    parser.add_argument("--duration", type=float, default=15)
    parser.add_argument("--event", default="cpu-cycles")
    parser.add_argument("--frequency", type=int, default=1000)
    parser.add_argument("--callgraph", default="fp", choices=["fp", "dwarf", "none"])
    parser.add_argument("--label", required=True)
    parser.add_argument("--candidate", type=Path, required=True,
                        help="Candidate folder holding bioshock.apk, bioshock.json and libbioshock.unstripped.so")
    parser.add_argument("--out-root", type=Path, default=ROOT / "work/evidence/perf-295/cpu-profile")
    args = parser.parse_args(argv)
    if args.pid <= 0 or not 1 <= args.duration <= 60:
        parser.error("Use a positive existing PID and a duration between 1 and 60 seconds")
    adb_path = shutil.which("adb") or str(SDK / "platform-tools/adb.exe")
    host_perf = NDK / "simpleperf/bin/windows/x86_64/simpleperf.exe"
    for path in (Path(adb_path), host_perf):
        if not path.is_file():
            parser.error(f"Required profiling input is missing: {path}")
    receipt, apk, symbols_lib, identity = verify_candidate(args.candidate)
    adb = [adb_path, "-s", args.serial]

    def device(*words, binary=False):
        options = {} if binary else {"text": True, "encoding": "utf-8", "errors": "replace"}
        return subprocess.check_output(adb + ["exec-out", *words], **options)

    uid = int(device("run-as", PACKAGE, "id", "-u").strip())
    cmdline = device("run-as", PACKAGE, "cat", f"/proc/{args.pid}/cmdline", binary=True)
    status = device("run-as", PACKAGE, "cat", f"/proc/{args.pid}/status")
    ids = next((line.split()[1:] for line in status.splitlines() if line.startswith("Uid:")), [])
    if cmdline.split(b"\0", 1)[0] != PACKAGE.encode() or len(ids) != 4 or any(int(v) != uid for v in ids):
        raise ValueError("PID is not the BioShock application process of the debuggable package UID")
    apk_paths = device("pm", "path", PACKAGE).strip().splitlines()
    if len(apk_paths) != 1 or not apk_paths[0].startswith("package:"):
        raise RuntimeError("Expected one installed base APK")
    installed_sha = device("run-as", PACKAGE, "sha256sum", apk_paths[0][8:]).split()[0]
    maps = device("run-as", PACKAGE, "cat", f"/proc/{args.pid}/maps")
    loaded = {m[1] for line in maps.splitlines() if (m := re.search(r"\s(/data/app/\S+/libbioshock\.so)$", line.strip()))}
    if len(loaded) != 1:
        raise RuntimeError("No unique loaded libbioshock.so")
    loaded_sha = device("run-as", PACKAGE, "sha256sum", loaded.pop()).split()[0]
    if installed_sha != receipt["sha256"] or loaded_sha != receipt["library_sha256"]:
        raise RuntimeError("Installed APK or loaded libbioshock.so differs from the candidate; refusing wrong symbols")

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = args.out_root / f"{args.pid}-{stamp}"
    out.mkdir(parents=True, exist_ok=False)
    remote = f"/data/data/{PACKAGE}/cache/cpu-{args.pid}-{stamp}.data"
    record = ["run-as", PACKAGE, "/system/bin/simpleperf", "record", "-p", str(args.pid), "-e", args.event,
              "-f", str(args.frequency), "--duration", str(args.duration), "-o", remote]
    if args.callgraph != "none":
        record[-2:-2] = ["--call-graph", args.callgraph]
    metadata = {"status": "RECORDING", "label": args.label, "pid": args.pid, "uid": uid, "serial": args.serial,
                "event": args.event, "frequency": args.frequency, "callgraph": args.callgraph,
                "duration_seconds": args.duration, "record_command": record, "candidate": str(args.candidate),
                "installed_apk_sha256": installed_sha, "loaded_native_sha256": loaded_sha,
                "symbols_sha256": digest(symbols_lib), "allocated_identity": identity["allocated_sha256"],
                "tool_sha256": digest(__file__), "utc": stamp,
                "scope": "Existing PID only; no app launch/stop, install, clear, adb root, setprop or performance policy changes."}
    meta_path = out / "profile.json"
    try:
        symbols = out / "local_symbols"
        symbols.mkdir()
        with zipfile.ZipFile(apk) as archive:
            for member in archive.namelist():
                if member.startswith("lib/arm64-v8a/") and member.endswith(".so"):
                    with archive.open(member) as source, (symbols / Path(member).name).open("wb") as target:
                        shutil.copyfileobj(source, target)
        shutil.copy2(symbols_lib, symbols / LIBRARY)
        metadata["symbol_snapshots"] = {path.name: digest(path) for path in sorted(symbols.glob("*.so"))}
        result = subprocess.run(adb + ["shell", *record], capture_output=True, text=True, encoding="utf-8", errors="replace")
        (out / "record.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        metadata["record_returncode"] = result.returncode
        if result.returncode:
            raise RuntimeError(f"Simpleperf record failed; inspect {out / 'record.log'}")
        data = device("run-as", PACKAGE, "cat", remote, binary=True)
        subprocess.run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", remote], check=False)
        if len(data) < 104 or data[:8] != b"PERFILE2":
            raise RuntimeError("Simpleperf returned an empty or invalid perf.data")
        perf_data = out / "perf.data"
        perf_data.write_bytes(data)
        metadata["perf_data_sha256"] = hashlib.sha256(data).hexdigest()
        base = [str(host_perf), "report", "-i", str(perf_data), "--symfs", str(symbols), "--percent-limit", "0.05", "-n"]
        reports = [("report-threads.txt", ["--sort", "tid,comm"]),
                   ("report-dso.txt", ["--sort", "dso"]),
                   ("report-self.txt", ["--sort", "tid,dso,symbol"]),
                   ("report-symbol.txt", ["--sort", "dso,symbol"])]
        if args.callgraph != "none":
            reports.append(("report-children.txt", ["--sort", "dso,symbol", "--children"]))
            reports.append(("report-callgraph.txt", ["--sort", "tid,dso,symbol", "--children", "-g", "caller",
                                                     "--max-stack", "32", "--percent-limit", "0.5"]))
        for name, extra in reports:
            result = subprocess.run(base + extra, capture_output=True, text=True, encoding="utf-8", errors="replace")
            (out / name).write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError(f"NDK report failed; inspect {out / name}")
        report = (out / "report-self.txt").read_text(encoding="utf-8")
        counts = re.findall(r"Samples:\s*(\d+)", report)
        if not counts or sum(map(int, counts)) == 0:
            raise RuntimeError("No samples were reported; raw perf.data was retained")
        metadata["samples"] = sum(map(int, counts))
        metadata["status"] = "CAPTURED_AND_REPORTED"
    except Exception as error:
        metadata.update(status="FAILED", error=str(error))
        raise
    finally:
        meta_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
