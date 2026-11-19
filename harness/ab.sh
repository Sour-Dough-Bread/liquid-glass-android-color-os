#!/usr/bin/env bash
# ab.sh <scene> <rounds> <name=props> <name=props> ... [-- am extras...]
#
# Compares engine settings on one running scene, by turns, so that what differs
# between two launches of the app (up to 0.3 ms of RenderEngine time on the AVD)
# is not in the comparison. Each variant is a name and the properties it sets,
# `prop=value` separated by commas (persist.sys.glass. is prefixed; an empty
# value clears the property), and `stock` / `glass` for the mode:
#
#   harness/ab.sh shade 4 old=lazyBlur=0,paneBlur=own,toneShader=0 new= stock=mode=stock
#
# Every property any variant names is cleared before each turn, so a variant
# only has to list what it changes. Prints each variant's RenderEngine time per
# frame (the median over the rounds of each round's average: a round that
# shared the host with something else is high, never low, and TimeStats'
# histogram is too coarse, 1 ms, to trim frames by), the lowest round, the
# spread, and the last `stats:` line the engine logged under it. Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
P=persist.sys.glass
SCENE="${1:?scene}"; ROUNDS="${2:?rounds}"; shift 2
VARIANTS=(); EXTRAS=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; EXTRAS=("$@"); break; fi
  VARIANTS+=("$1"); shift
done
SECS="${AB_SECS:-3}"
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }

# Every property named anywhere, so each turn starts from the same place.
ALL=$(printf '%s\n' "${VARIANTS[@]}" | sed 's/^[^=]*=//' | tr ',' '\n' | sed -n 's/=.*//p' | sort -u)
apply() {
  local spec="${1#*=}" kv
  for k in $ALL; do
    if [ "$k" = mode ]; then $A shell "setprop $P.mode glass"; else $A shell "setprop $P.$k ''"; fi
  done
  [ -n "$spec" ] || return 0
  local kvs
  IFS=, read -r -a kvs <<<"$spec"
  for kv in "${kvs[@]}"; do
    $A shell "setprop $P.${kv%%=*} '${kv#*=}'"
  done
}

$A shell setprop $P.stats 1
# Each extra is quoted for the device's shell: a custom scene's spec has `;`.
QUOTED=()
for e in "${EXTRAS[@]}"; do QUOTED+=("'${e//\'/}'"); done
$A shell am force-stop dev.stockglass.harness
sleep 1
$A shell "am start -S -W -n dev.stockglass.harness/.BenchActivity --es scene '$SCENE' ${QUOTED[*]}" >/dev/null
sleep "${BENCH_SETTLE:-4}"
# A scene that is not up measures whatever else is on screen.
LAYER=$($A shell dumpsys SurfaceFlinger | tr -d '\r' | grep -A1 'Layer \[[0-9]*\] VRI-dev.stockglass.harness' \
        | grep -o 'backgroundBlurRadius=[0-9]*\|blurRegions.size()=[0-9]*' | sort -u | tr '\n' ' ')
[ -n "$($A shell dumpsys SurfaceFlinger | grep -c 'VRI-dev.stockglass.harness' | grep -v '^0$')" ] \
  || { echo "the bench scene did not start" >&2; exit 1; }
echo "scene $SCENE: ${LAYER:-no blur in the layer}"

declare -A SUM STAT
for v in "${VARIANTS[@]}"; do SUM[${v%%=*}]=""; done
for _ in $(seq "$ROUNDS"); do
  for v in "${VARIANTS[@]}"; do
    name="${v%%=*}"
    apply "$v"; sleep 1.2
    $A logcat -c
    $A shell dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null
    sleep "$SECS"
    re=$($A shell dumpsys SurfaceFlinger --timestats -dump -maxlayers 0 | tr -d '\r' \
          | sed -n 's/^averageRenderEngineTiming = \([0-9.]*\).*/\1/p')
    SUM[$name]="${SUM[$name]} ${re:-nan}"
    s=$($A logcat -d -s StockGlass | grep -o 'stats: .*' | tail -1)
    [ -n "$s" ] && STAT[$name]="$s"
  done
done
$A shell dumpsys SurfaceFlinger --timestats -disable >/dev/null
apply "reset="
for v in "${VARIANTS[@]}"; do
  name="${v%%=*}"
  python3 - "$SCENE" "$name" "${STAT[$name]:-}" ${SUM[$name]} <<'PY'
import statistics, sys
scene, name, stat = sys.argv[1:4]
x = [float(v) for v in sys.argv[4:]]
sd = statistics.pstdev(x) if len(x) > 1 else 0.0
short = ''
if stat:
    import re
    m = re.search(r'blurs/frame=([0-9.]+).*derived/frame=([0-9.]+).*snapshot-copies/frame=([0-9.]+).*hook=([0-9.]+)us', stat)
    if m:
        short = 'blurs=%s derived=%s copies=%s hook=%sus' % m.groups()
# The median: a round that shared the host's GPU with something else is high,
# never low, and a mean carries it.
print('%-9s %-14s re=%.3f ms median  (min %.2f, +-%.2f over %d: %s)  %s' % (
    scene, name, statistics.median(x), min(x), sd, len(x), ' '.join('%.2f' % v for v in x),
    short))
PY
done
