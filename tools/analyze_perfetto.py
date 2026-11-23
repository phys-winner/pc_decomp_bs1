"""Summarize a BioShock perfetto capture (tools/perfetto/bioshock-cpu.pbtxt).

Outputs JSON + text with: per-thread running/runnable/sleep time from the
scheduler (thread_state), per-thread CPU placement, callstack-sample self and
inclusive counts (traced_perf linux.perf), user vs kernel split, frame timeline
of the game SurfaceView, CPU and GPU frequency residency. Sample counts and
their percentages are not CPU durations: periodic sampling can alias with
periodic workloads and can be bursty or biased.
Scheduler running_ms is authoritative; inclusive sample counts overlap.
"""
import argparse
import collections
import json
from pathlib import Path
import re

from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig

BIN = Path.home() / ".local/share/perfetto/prebuilts/trace_processor_shell-100334b6091596fb.exe"
PACKAGE = "org.bioshock.port"


def sampling_frequency(trace, explicit=None):
    if explicit is not None:
        if explicit <= 0:
            raise ValueError("Sampling frequency must be positive")
        return explicit
    config = trace.with_name("perfetto.pbtxt")
    if not config.is_file():
        config = Path(__file__).resolve().parent / "perfetto/bioshock-cpu-fp.pbtxt"
    values = re.findall(r"\bfrequency:\s*(\d+)", config.read_text())
    if len(values) != 1 or int(values[0]) <= 0:
        raise ValueError(f"Missing or ambiguous linux.perf sampling frequency: {config}")
    return float(values[0])


class Symbolizer:
    """rel_pc (file offset within the mapping) -> function name from an unstripped ELF."""
    def __init__(self, path):
        import bisect
        from elftools.elf.elffile import ELFFile
        self.bisect = bisect
        with open(path, "rb") as stream:
            elf = ELFFile(stream)
            self.loads = [(seg["p_offset"], seg["p_filesz"], seg["p_vaddr"]) for seg in elf.iter_segments()
                          if seg["p_type"] == "PT_LOAD"]
            symbols = []
            for section in elf.iter_sections():
                if section.name != ".symtab":
                    continue
                for symbol in section.iter_symbols():
                    if symbol["st_info"]["type"] == "STT_FUNC" and symbol["st_value"]:
                        symbols.append((symbol["st_value"], symbol["st_size"], symbol.name))
        symbols.sort()
        self.starts = [row[0] for row in symbols]
        self.symbols = symbols

    def name(self, rel_pc):
        # traced_perf reports rel_pc already as the ELF virtual address (load
        # bias applied): root frames land exactly in run_game/ut3_game_main.
        vaddr = rel_pc
        index = self.bisect.bisect_right(self.starts, vaddr) - 1
        if index < 0:
            return None
        start, size, name = self.symbols[index]
        return name if vaddr < start + max(size, 1) or not size else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--frequency", type=float, help="Override linux.perf Hz; default reads adjacent perfetto.pbtxt or the 400 Hz FP config")
    parser.add_argument("--focus-tids", default="", help="Comma separated TIDs for inclusive call trees")
    parser.add_argument("--symbols", type=Path, help="Unstripped libbioshock.so of the traced build (frame-pointer captures)")
    parser.add_argument("--candidate", type=Path, help="Verify symbols against this candidate APK and receipt")
    parser.add_argument("--capture-receipt", type=Path, help="Matching perf_capture receipt proving the installed APK, loaded library and PID")
    args = parser.parse_args(argv)
    if bool(args.candidate) != bool(args.capture_receipt):
        parser.error("Use --candidate and --capture-receipt together to verify traced symbols")
    verification, target_pid = None, None
    if args.candidate:
        try:
            from .profile_cpu import digest, verify_candidate
        except ImportError:
            from profile_cpu import digest, verify_candidate
        receipt, _, args.symbols, identity = verify_candidate(args.candidate, args.symbols)
        capture = json.loads(args.capture_receipt.read_text())
        if (not capture["identity_unchanged"] or
                capture["installed"]["apk"]["sha256"] != receipt["sha256"] or
                capture["installed"]["loaded_native"]["sha256"] != receipt["library_sha256"] or
                capture["target_before"]["pid"] != capture["target_after"]["pid"]):
            raise ValueError("Capture PID or installed runtime differs from the symbol candidate")
        target_pid = capture["target_before"]["pid"]
        verification = {"status": "RUNTIME_PAYLOAD_VERIFIED", "candidate": str(args.candidate),
                        "capture_receipt": str(args.capture_receipt), "pid": target_pid,
                        "shipped_library_sha256": receipt["library_sha256"], "symbols_sha256": digest(args.symbols),
                        "allocated_identity": identity["allocated_sha256"]}
    elif args.symbols:
        verification = {"status": "BUILD_NOT_VERIFIED", "symbols": str(args.symbols)}
    args.frequency = sampling_frequency(args.trace, args.frequency)
    tp = TraceProcessor(trace=str(args.trace), config=TraceProcessorConfig(bin_path=str(BIN)))
    q = lambda sql: list(tp.query(sql))
    out = {"trace": str(args.trace), "sampling_frequency_hz": args.frequency}
    if verification:
        out["symbol_verification"] = verification
    bounds = q("select start_ts, end_ts from trace_bounds")[0]
    pid_filter = f" and pid = {int(target_pid)}" if target_pid is not None else ""
    upid_rows = q(f"select upid, pid from process where (name like '{PACKAGE}%' or cmdline like '{PACKAGE}%'){pid_filter}")
    if not upid_rows:
        raise SystemExit("game process not found")
    upids = ",".join(str(r.upid) for r in upid_rows)
    out["pids"] = [r.pid for r in upid_rows]
    # Scheduler: per-thread time in each state within the trace window.
    states = q(f"""
        select t.tid, coalesce(t.name, '') name, ts.state, sum(ts.dur) dur, count(*) n
        from thread_state ts join thread t using(utid)
        where t.upid in ({upids}) group by 1, 2, 3""")
    per = collections.defaultdict(lambda: {"name": "", "running_ms": 0.0, "runnable_ms": 0.0, "sleep_ms": 0.0,
                                           "uninterruptible_ms": 0.0, "other_ms": 0.0, "switches_in": 0})
    for r in states:
        row = per[r.tid]
        row["name"] = r.name
        ms = r.dur / 1e6
        if r.state == "Running":
            row["running_ms"] += ms
            row["switches_in"] += r.n
        elif r.state in ("R", "R+"):
            row["runnable_ms"] += ms
        elif r.state == "S":
            row["sleep_ms"] += ms
        elif r.state in ("D", "DK"):
            row["uninterruptible_ms"] += ms
        else:
            row["other_ms"] += ms
    cpu_place = q(f"""
        select t.tid, s.cpu, sum(s.dur) dur from sched s join thread t using(utid)
        where t.upid in ({upids}) group by 1, 2""")
    placement = collections.defaultdict(dict)
    for r in cpu_place:
        placement[r.tid][r.cpu] = round(r.dur / 1e6, 1)
    window_ms = (bounds.end_ts - bounds.start_ts) / 1e6
    threads = []
    for tid, row in per.items():
        row = {k: round(v, 1) if isinstance(v, float) else v for k, v in row.items()}
        row["tid"] = tid
        row["running_pct"] = round(100 * row["running_ms"] / window_ms, 2)
        row["runnable_pct"] = round(100 * row["runnable_ms"] / window_ms, 2)
        row["cpu_ms"] = placement.get(tid, {})
        threads.append(row)
    threads.sort(key=lambda r: -r["running_ms"])
    out["window_ms"] = round(window_ms, 1)
    out["threads"] = threads[:30]
    # Callstack samples.
    samples = q(f"""
        select ps.callsite_id cs, t.tid, ps.cpu_mode mode, ps.cpu
        from perf_sample ps join thread t using(utid) where t.upid in ({upids}) and ps.callsite_id is not null""")
    frames = {}
    symbolizer = Symbolizer(args.symbols) if args.symbols else None
    for r in q("""select c.id, c.parent_id, f.name fname, m.name mname, f.rel_pc
                  from stack_profile_callsite c join stack_profile_frame f on c.frame_id = f.id
                  join stack_profile_mapping m on f.mapping = m.id"""):
        lib = (r.mname or "?").rsplit("/", 1)[-1]
        name = r.fname
        if not name and symbolizer and lib == "libbioshock.so":
            name = symbolizer.name(r.rel_pc)
        frames[r.id] = (r.parent_id, f"{name or hex(r.rel_pc)} [{lib}]")
    self_cost = collections.Counter()
    self_thread = collections.defaultdict(collections.Counter)
    incl = collections.defaultdict(collections.Counter)
    mode = collections.defaultdict(collections.Counter)
    per_thread_samples = collections.Counter()
    focus = {int(x) for x in args.focus_tids.split(",") if x}
    for r in samples:
        chain, seen, node = [], set(), r.cs
        while node is not None and node in frames and len(chain) < 256:
            parent, name = frames[node]
            chain.append(name)
            node = parent
        if not chain:
            continue
        per_thread_samples[r.tid] += 1
        mode[r.tid][r.mode] += 1
        self_cost[chain[0]] += 1
        self_thread[r.tid][chain[0]] += 1
        for name in chain:
            if name not in seen:
                seen.add(name)
                incl[r.tid][name] += 1
                incl[0][name] += 1
    total = sum(per_thread_samples.values())
    out["samples"] = {"total": total, "nominal_period_ms": 1000.0 / args.frequency,
                      "counts_are_cpu_time_weights": False,
                      "cpu_time_source": "scheduler thread_state Running",
                      "interpretation": "Self/inclusive percentages are sample-count shares, not measured CPU-time shares. Periodic sampling can alias with periodic workloads; nominal frequency does not weight individual samples.",
                      "per_thread": {tid: {"samples": n,
                                           "scheduler_running_ms": round(per[tid]["running_ms"], 1) if tid in per else None,
                                           "modes": dict(mode[tid])}
                                     for tid, n in per_thread_samples.most_common(20)}}
    out["self_top"] = [{"fn": fn, "samples": n, "pct": round(100 * n / total, 2)}
                       for fn, n in self_cost.most_common(args.top)]
    out["self_by_thread"] = {tid: [{"fn": fn, "samples": n, "pct_of_thread": round(100 * n / per_thread_samples[tid], 2)}
                                   for fn, n in self_thread[tid].most_common(args.top)]
                             for tid, _ in per_thread_samples.most_common(8)}
    out["inclusive_by_thread"] = {tid: [{"fn": fn, "samples": n, "pct_of_thread": round(100 * n / per_thread_samples[tid], 2)}
                                        for fn, n in incl[tid].most_common(args.top)]
                                  for tid in (focus or [t for t, _ in per_thread_samples.most_common(4)])
                                  if per_thread_samples[tid]}
    lib_cost = collections.Counter()
    for fn, n in self_cost.items():
        lib_cost[fn.rsplit("[", 1)[-1].rstrip("]")] += n
    out["self_by_library"] = {lib: round(100 * n / total, 2) for lib, n in lib_cost.most_common(15)}
    # Frame timeline for the game's layer.
    try:
        rows = q(f"""select a.dur, a.jank_type, a.present_type, a.on_time_finish, a.layer_name
                     from actual_frame_timeline_slice a join process p using(upid)
                     where p.name like '{PACKAGE}%'""")
        if rows:
            durs = sorted(r.dur / 1e6 for r in rows)
            jank = collections.Counter(r.jank_type for r in rows)
            out["frametimeline"] = {"frames": len(rows), "p50_ms": durs[len(durs) // 2],
                                    "p95_ms": durs[int(len(durs) * .95)], "jank_types": dict(jank.most_common(8))}
    except Exception as error:
        out["frametimeline_error"] = str(error)
    # CPU / GPU frequency residency (time-weighted median per CPU).
    freq = {}
    for r in q("""select cct.cpu, c.value, c.ts, lead(c.ts) over (partition by cct.cpu order by c.ts) nts
                  from counter c join cpu_counter_track cct on c.track_id = cct.id where cct.name = 'cpufreq'"""):
        if r.nts is None:
            continue
        freq.setdefault(r.cpu, collections.Counter())[int(r.value)] += r.nts - r.ts
    out["cpu_freq_mhz_time_weighted"] = {cpu: {str(k // 1000): round(100 * v / sum(c.values()), 1)
                                               for k, v in sorted(c.items())} for cpu, c in sorted(freq.items())}
    gpu = q("""select c.value, c.ts from counter c join gpu_counter_track g on c.track_id = g.id
               where g.name like '%freq%' order by c.ts""")
    if gpu:
        out["gpu_freq_samples"] = [(round((r.ts - bounds.start_ts) / 1e9, 3), r.value) for r in gpu[:200]]
    tp.close()
    text = json.dumps(out, indent=1, ensure_ascii=False)
    target = args.trace.with_suffix(".summary.json")
    target.write_text(text + "\n", encoding="utf-8")
    print(target)


if __name__ == "__main__":
    main()
