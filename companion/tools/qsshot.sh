#!/usr/bin/env bash
# qsshot.sh <out.png> [min_regions]: open full Quick Settings, wait until the
# shade layer has at least min_regions blur regions (0 = just let it settle),
# capture the screen, and print the count.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
A="adb -s ${ANDROID_SERIAL:-emulator-5564}"; OUT="$1"; MIN="${2:-0}"
$A shell cmd statusbar collapse; sleep 1.5
$A shell cmd statusbar expand-settings
for _ in $(seq 20); do
  sleep 0.7
  [ "$MIN" -gt 0 ] && [ "$("$HERE/shade_count.sh")" -ge "$MIN" ] && break
done
sleep 2.5   # let the expansion settle
n=$("$HERE/shade_count.sh")
$A exec-out screencap -p > "$OUT"
echo "$(basename "$OUT"): shade blurRegions=$n"
