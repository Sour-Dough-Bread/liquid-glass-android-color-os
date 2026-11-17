#!/usr/bin/env bash
# qs_hold.sh <out.png> <to_y> [from_y]: open the notification shade, then drag
# Quick Settings from QQS (from_y, default 350) down to to_y and hold the
# finger there, capture the screen mid-expansion, and let go. The panel stays
# exactly where the finger is, so a capture shows a frame of the QQS-to-QS
# transition that is otherwise gone in a few hundred milliseconds.
set -u
A="adb -s ${ANDROID_SERIAL:-emulator-5564}"; OUT="$1"; TO="$2"; FROM="${3:-350}"
$A shell cmd statusbar collapse; sleep 1.2
$A shell cmd statusbar expand-notifications; sleep 1.5
$A shell "input motionevent DOWN 540 $FROM; for y in \$(seq $FROM 40 $TO) $TO; do input motionevent MOVE 540 \$y; done"
sleep 1.2
$A exec-out screencap -p > "$OUT"
$A shell input motionevent UP 540 "$TO"
sleep 0.8
$A shell cmd statusbar collapse
echo "$(basename "$OUT"): held at y=$TO"
