#!/usr/bin/env bash
# cost.sh <label> [prop=value...]: sets the properties, then times 6 Quick Settings
# open/close cycles with TimeStats and prints RenderEngine time per composited frame.
set -u
A="adb -s ${ANDROID_SERIAL:-emulator-5564}"; label="$1"; shift
for kv in "$@"; do $A shell "setprop ${kv%%=*} '${kv#*=}'"; done
$A shell cmd statusbar collapse; sleep 2
$A shell dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null
for _ in 1 2 3 4 5 6; do
  $A shell cmd statusbar expand-settings; sleep 3.5
  $A shell cmd statusbar collapse; sleep 2
done
out=$($A shell dumpsys SurfaceFlinger --timestats -dump -maxlayers 0 | tr -d '\r')
$A shell dumpsys SurfaceFlinger --timestats -disable >/dev/null
printf '%-34s frames=%-4s client=%-4s %s\n' "$label" \
  "$(sed -n 's/^totalFrames = //p' <<<"$out")" \
  "$(sed -n 's/^clientCompositionFrames = //p' <<<"$out")" \
  "$(grep -o 'averageRenderEngineTiming = .*' <<<"$out")"
