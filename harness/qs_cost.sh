#!/usr/bin/env bash
# qs_cost.sh <rounds> <name=props> ...: the real Quick Settings, opened and
# closed, under each set of engine properties by turns (as harness/ab.sh).
# Prints each variant's RenderEngine time per composited frame, median of the
# rounds. Needs the companion in SystemUI (companion/dev_attach.sh) for tiles
# to be regions. Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
P=persist.sys.glass
ROUNDS="${1:?rounds}"; shift
VARIANTS=("$@")
OPEN="${QS_OPEN:-expand-settings}"
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }
ALL=$(printf '%s\n' "${VARIANTS[@]}" | sed 's/^[^=]*=//' | tr ',' '\n' | sed -n 's/=.*//p' | sort -u)
apply() {
  local spec="${1#*=}" kv kvs
  for k in $ALL; do
    if [ "$k" = mode ]; then $A shell "setprop $P.mode glass"; else $A shell "setprop $P.$k ''"; fi
  done
  [ -n "$spec" ] || return 0
  IFS=, read -r -a kvs <<<"$spec"
  for kv in "${kvs[@]}"; do $A shell "setprop $P.${kv%%=*} '${kv#*=}'"; done
}
$A shell setprop $P.stats 1
$A shell input keyevent HOME; sleep 1.5
declare -A SUM STAT FR
for v in "${VARIANTS[@]}"; do SUM[${v%%=*}]=""; FR[${v%%=*}]=""; done
for _ in $(seq "$ROUNDS"); do
  for v in "${VARIANTS[@]}"; do
    name="${v%%=*}"
    apply "$v"; sleep 1.5
    $A shell cmd statusbar collapse; sleep 1
    $A logcat -c
    $A shell dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null
    for _c in 1 2 3; do
      $A shell cmd statusbar $OPEN; sleep 2.2
      $A shell cmd statusbar collapse; sleep 1.5
    done
    out=$($A shell dumpsys SurfaceFlinger --timestats -dump -maxlayers 0 | tr -d '\r')
    SUM[$name]="${SUM[$name]} $(sed -n 's/^averageRenderEngineTiming = \([0-9.]*\).*/\1/p' <<<"$out")"
    FR[$name]="$(sed -n 's/^clientCompositionFrames = //p' <<<"$out")"
  done
done
$A shell dumpsys SurfaceFlinger --timestats -disable >/dev/null
apply "reset="
for v in "${VARIANTS[@]}"; do
  name="${v%%=*}"
  python3 - "$name" "${FR[$name]}" ${SUM[$name]} <<'PY'
import statistics, sys
name, frames = sys.argv[1:3]
x = [float(v) for v in sys.argv[3:]]
print('%-12s re=%.3f ms median  (min %.2f over %d: %s)  ~%s frames a round' % (
    name, statistics.median(x), min(x), len(x), ' '.join('%.2f' % v for v in x), frames))
PY
done
