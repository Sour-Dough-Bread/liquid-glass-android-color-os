#!/usr/bin/env bash
# shots.sh <outdir> <label> [scene...]: a screenshot of each bench scene with
# its backdrop frozen, so two engines (or two settings) can be compared pixel
# for pixel with harness/shotdiff.py. Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
OUT="${1:?outdir}"; LABEL="${2:?label}"; shift 2
SCENES=("$@"); [ ${#SCENES[@]} -gt 0 ] || SCENES=(shade dialog home keyboard panel)
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }
mkdir -p "$OUT"
for sc in "${SCENES[@]}"; do
  $A shell am force-stop dev.stockglass.harness
  $A shell am start -n dev.stockglass.harness/.BenchActivity --es scene "$sc" --es animate false \
      --es phase "${SHOT_PHASE:-300}" >/dev/null
  sleep "${SHOT_SETTLE:-3.5}"
  $A exec-out screencap -p > "$OUT/${LABEL}_$sc.png"
done
$A shell am force-stop dev.stockglass.harness
ls "$OUT" | grep -c "^${LABEL}_" | sed "s/^/$LABEL: /; s/$/ shots/"
