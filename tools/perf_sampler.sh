#!/system/bin/sh
# BioShock read-only device sampler. Usage: PID LAYER SECONDS OUTDIR
# Separate workers prevent a slow /proc scan from overflowing the frame ring.
P=$1
LAYER=$2
SECS=$3
OUT=$4
[ -n "$P" ] && [ -n "$LAYER" ] && [ -n "$SECS" ] && [ -n "$OUT" ] || { echo "usage" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
: > "$OUT/latency.txt"; : > "$OUT/threads.txt"; : > "$OUT/system.txt"; : > "$OUT/thermal.txt"
echo $$ > "$OUT/sampler.pid"
ZONES=""
for z in /sys/class/thermal/thermal_zone*; do
  [ -r "$z/type" ] || continue
  IFS= read -r t < "$z/type"
  case "$t" in cpu*|gpu*|*skin*|*gpuss*|*cpuss*) ZONES="$ZONES $z";; esac
done
read -r START ignored < /proc/uptime
END=$(awk -v s="$START" -v d="$SECS" 'BEGIN{printf "%.2f", s+d}')
CHILD=""
WORKERS=""
active() {
  read -r NOW ignored < /proc/uptime
  [ -d "/proc/$P" ] && awk -v n="$NOW" -v e="$END" 'BEGIN{exit !(n<e)}'
}
stop_command() {
  if [ -n "$CHILD" ]; then
    kill "$CHILD" 2>/dev/null
    wait "$CHILD" 2>/dev/null
    CHILD=""
  fi
}
run() {
  "$@" &
  CHILD=$!
  wait "$CHILD"
  result=$?
  CHILD=""
  return "$result"
}
worker_start() {
  CHILD=""
  trap - EXIT
  trap 'stop_command; exit 0' HUP INT TERM
}
cleanup() {
  stop_command
  if [ -n "$WORKERS" ]; then
    kill $WORKERS 2>/dev/null
    wait $WORKERS 2>/dev/null
    WORKERS=""
  fi
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM

frames() {
  worker_start
  i=0
  trap 'read -r NOW ignored < /proc/uptime; echo "#END $NOW iterations=$i" >> "$OUT/latency.txt"' EXIT
  while active; do
    echo "#U $NOW" >> "$OUT/latency.txt"
    run dumpsys SurfaceFlinger --latency "$LAYER" >> "$OUT/latency.txt" 2>&1
    i=$((i + 1))
    run sleep 0.5
  done
}
process_stats() {
  worker_start
  while active; do
    {
      read -r NOW ignored < /proc/uptime
      echo "#U $NOW"
      echo "#CPU_BEGIN $NOW"
      if IFS= read -r process_stat < "/proc/$P/stat"; then
        printf 'P %s | %s\n' "$NOW" "$process_stat"
      fi
      IFS= read -r cpu < /proc/stat
      printf '%s\n' "$cpu"
      rows=0
      for t in /proc/$P/task/*; do
        [ -r "$t/stat" ] || continue
        IFS= read -r stat < "$t/stat" || continue
        sched=""
        if [ -r "$t/schedstat" ]; then IFS= read -r sched < "$t/schedstat"; fi
        printf 'T %s %s | %s\n' "${t##*/}" "$sched" "$stat"
        rows=$((rows + 1))
      done
      read -r SCAN_END ignored < /proc/uptime
      echo "#CPU_END $SCAN_END rows=$rows"
    } >> "$OUT/threads.txt"
    {
      read -r NOW ignored < /proc/uptime
      echo "#U $NOW"
      for c in 0 1 2 3 4 5 6 7; do
        cur=""; cap=""
        path=/sys/devices/system/cpu/cpu$c/cpufreq
        if [ -r "$path/scaling_cur_freq" ]; then read -r cur < "$path/scaling_cur_freq"; fi
        if [ -r "$path/scaling_max_freq" ]; then read -r cap < "$path/scaling_max_freq"; fi
        echo "F $c $cur $cap"
      done
      for z in $ZONES; do
        read -r kind < "$z/type"
        read -r temperature < "$z/temp"
        echo "Z $kind $temperature"
      done
      if [ -r "/proc/$P/status" ]; then
        while IFS= read -r line; do
          case "$line" in VmRSS:*|VmHWM:*|RssAnon:*|RssFile:*|VmSwap:*|Threads:*) printf 'M %s\n' "$line";; esac
        done < "/proc/$P/status"
      fi
    } >> "$OUT/system.txt"
    run sleep 2
  done
}
thermal_status() {
  worker_start
  while active; do
    echo "#U $NOW" >> "$OUT/thermal.txt"
    run dumpsys thermalservice > "$OUT/thermal.sample" 2>/dev/null
    grep -m1 'Thermal Status' "$OUT/thermal.sample" >> "$OUT/thermal.txt"
    run sleep 10
  done
}
frames &
WORKERS="$!"
process_stats &
WORKERS="$WORKERS $!"
thermal_status &
WORKERS="$WORKERS $!"
# The coordinator also bounds a worker blocked in dumpsys or a disappearing /proc read.
while active; do run sleep 0.5; done
[ -d "/proc/$P" ] || echo "#GONE $NOW" >> "$OUT/latency.txt"
cleanup
trap - EXIT
touch "$OUT/done"
