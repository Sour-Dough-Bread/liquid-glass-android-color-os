#!/usr/bin/env bash
# bench.sh <label> <scene> [seconds] [-- am extras...]
#
# What one composited frame of a scene costs. Starts BenchActivity (a backdrop
# that scrolls every frame under a window of blur regions), lets it settle,
# then reads SurfaceFlinger's TimeStats over <seconds> (default 5): the
# RenderEngine time per client-composited frame, which runs to the GPU fence,
# so it is GPU time where the AVD has a real GPU (`-gpu host`).
#
#   harness/bench.sh base shade
#   harness/bench.sh base home 5 -- --es tiles 4
#   BENCH_REPEAT=5 harness/bench.sh base shade      # median of five windows
#
# Prints one line: label, scene, frames, mean and median RenderEngine ms, the
# 90th percentile, and whatever the engine logged under `stats:` in the window
# (persist.sys.glass.stats=1). Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
LABEL="${1:?label}"; SCENE="${2:?scene}"; SECS="${3:-5}"
shift; shift; [ $# -gt 0 ] && shift
[ "${1:-}" = "--" ] && shift
REPEAT="${BENCH_REPEAT:-1}"
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }

# Each extra is quoted for the device's shell: a custom scene's spec has `;`.
QUOTED=()
for e in "$@"; do QUOTED+=("'${e//\'/}'"); done
$A shell am force-stop dev.stockglass.harness
sleep 1
$A shell "am start -S -W -n dev.stockglass.harness/.BenchActivity --es scene '$SCENE' ${QUOTED[*]}" >/dev/null
sleep "${BENCH_SETTLE:-4}"
[ -n "$($A shell dumpsys SurfaceFlinger | grep -c 'VRI-dev.stockglass.harness' | grep -v '^0$')" ] \
  || { echo "the bench scene did not start" >&2; exit 1; }

one() {
  $A logcat -c
  $A shell dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null
  sleep "$SECS"
  $A shell dumpsys SurfaceFlinger --timestats -dump -maxlayers 0 | tr -d '\r'
  $A shell dumpsys SurfaceFlinger --timestats -disable >/dev/null
}

rows=()
for _ in $(seq "$REPEAT"); do
  out=$(one)
  row=$(python3 - "$out" <<'PY'
import re, sys
t = sys.argv[1]
def num(k):
    m = re.search(r'^%s = ([0-9.]+)' % k, t, re.M)
    return float(m.group(1)) if m else float('nan')
m = re.search(r'renderEngineTiming histogram is as below:\n(.*)', t)
p50 = p90 = float('nan')
if m:
    b = [(int(k), int(v)) for k, v in re.findall(r'(\d+)ms=(\d+)', m.group(1))]
    n = sum(v for _, v in b)
    def pct(q):
        acc = 0
        for k, v in b:
            acc += v
            if n and acc >= q * n:
                return k
        return float('nan')
    p50, p90 = pct(0.5), pct(0.9)
print('%d %d %.3f %s %s' % (num('totalFrames'), num('clientCompositionFrames'),
                            num('averageRenderEngineTiming'), p50, p90))
PY
)
  rows+=("$row")
done
stats=$($A logcat -d -s StockGlass | grep -o 'stats: .*' | tail -1)
regions=$($A shell dumpsys SurfaceFlinger | tr -d '\r' | grep -o 'blurRegions.size()=[0-9]*' | head -1 | cut -d= -f2)
python3 - "$LABEL" "$SCENE" "${regions:-0}" "$stats" "${rows[@]}" <<'PY'
import statistics, sys
label, scene, regions, stats = sys.argv[1:5]
rows = [r.split() for r in sys.argv[5:]]
re_ms = [float(r[2]) for r in rows]
frames = [int(r[0]) for r in rows]
med = statistics.median(re_ms)
spread = (' (%.2f-%.2f over %d)' % (min(re_ms), max(re_ms), len(re_ms))) if len(re_ms) > 1 else ''
print('%-22s %-9s regions=%-2s frames=%-4d re=%.3fms%s p50=%s p90=%s %s' % (
    label, scene, regions, statistics.median(frames), med, spread, rows[-1][3], rows[-1][4], stats))
PY
