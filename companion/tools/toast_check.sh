#!/usr/bin/env bash
# toast_check.sh [out-prefix]: a real text toast, and whether its glass goes
# with it.
#
# Opens Settings' About page and taps "Build number" four times: Settings
# posts a toast ("You are now N steps away from being a developer", or "No
# need, you are already a developer") from the third tap on, and the fourth
# replaces it with a fresh one. Four taps change nothing; seven would turn
# developer options on, so it never taps more, and it starts Settings afresh
# (am start -S) so that taps from an earlier run do not add up. The taps are
# D-pad presses: uiautomator cannot read the About page, whose Uptime row
# changes every second so the page never goes idle, but D-pad focus runs down
# the list and stops on its last row, Build number. While the toast shows (a short
# toast: 2 s), and again 4 s later, it prints one line:
#
#   regions   SystemUI's companion regions in the toast's window
#             (LinearLayout root): shown = alpha > 0, a non-empty rect and
#             its control on screen; stale = the same but "off"; gone = the
#             window is gone (the dump says "detached"; such a region cannot
#             reach SurfaceFlinger); idle = alpha 0 or an empty rect (not sent)
#   sf        blurRegions.size() of SurfaceFlinger's toast layer (VRI-Toast):
#             0 when the layer has none (A17 prints the size only when it
#             is not 0), "-" when there is no such layer
#
# and keeps <out-prefix>.{shown,hidden}.{png,dump.txt,log} (the log: SystemUI's
# companion lines before the dump; with app.debug=1 it says which share of the
# plate the toast kept and why). A pass: shown=1 sf=1
# while it shows, then no shown or stale region and sf=- after.
# Needs the companion in SystemUI (dev_attach.sh systemui). Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
OUT="${1:-toast}"
SYSUI=com.android.systemui
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }
sh_() { $A shell "$@" >/dev/null 2>&1; }

check() {  # check <tag>: one line, and the evidence kept; fast, within the toast's 2 s
  local tag=$1 pid dump sf
  pid=$($A shell pidof $SYSUI | tr -d '\r')
  $A exec-out screencap -p > "$OUT.$tag.png"
  sf=$($A shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | awk '
    /Layer \[/ {name=$0; if (name ~ /VRI-Toast#/) seen=1}
    /blurRegions.size\(\)=/ && name ~ /VRI-Toast#/ {s=$0; sub(/.*blurRegions.size\(\)=/, "", s); print s+0; f=1; exit}
    END {if (!f) print (seen ? 0 : "-")}')
  $A logcat -d --pid="$pid" -s StockGlassApp | sed -E 's/^.*StockGlassApp: //' > "$OUT.$tag.log"
  $A shell setprop persist.sys.glass.app.debug 1
  $A logcat -c
  sh_ service call activity 1599295570
  sleep 1
  dump=$($A logcat -d --pid="$pid" -s StockGlassApp | grep -E ' dump |dump: ' | sed -E 's/^.*StockGlassApp: //')
  $A shell setprop persist.sys.glass.app.debug 0
  printf '%s\n' "$dump" > "$OUT.$tag.dump.txt"
  awk -v tag="$tag" -v sf="$sf" '
    /^dump [0-9a-f]+ / && / in LinearLayout@/ {
      a = 0; if (match($0, /alpha=[0-9]+/)) a = substr($0, RSTART + 6, RLENGTH - 6) + 0
      if (!match($0, /Rect\(-?[0-9]+, -?[0-9]+ - -?[0-9]+, -?[0-9]+\)/)) next
      split(substr($0, RSTART + 5, RLENGTH - 6), q, /, | - /)
      if (!(a > 0 && q[3] + 0 > q[1] + 0 && q[4] + 0 > q[2] + 0)) { idle++; next }
      if (/ detached/) gone++; else if (/ off /) stale++; else shown++
    }
    END { printf "%-7s regions shown=%d stale=%d gone=%d idle=%d sf=%s\n", tag, shown, stale, gone, idle, sf }' <<<"$dump"
}

sh_ am start -S -W -a android.settings.DEVICE_INFO_SETTINGS; sleep 1.5
sh_ input keyevent $(printf 'DPAD_DOWN %.0s' $(seq 40)); sleep 0.5   # focus on Build number
for _ in 1 2 3; do sh_ input keyevent ENTER; sleep 0.25; done
sleep 0.5
sh_ input keyevent ENTER   # a fresh toast: the 4th tap
sleep 0.8
check shown
sleep 4
check hidden
sh_ input keyevent BACK
