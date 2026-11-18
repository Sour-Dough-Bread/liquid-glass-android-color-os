#!/usr/bin/env bash
# stale_repro.sh [out-prefix]: the shortest way found to leave home screen
# glass behind (VERIFIED.md, "stale glass on the home screen"). The launcher
# window stops while region moves are queued and every region is at alpha 0:
#
#   open Settings, HOME twice (default page), three swipes left (page 4),
#   swipe up and hold (overview), tap the card in the middle (Settings comes
#   back and the launcher stops), HOME twice, then four swipe pairs between
#   the default page and the next
#
# then one line: regions with glass whose control is off screen (stale) or on
# screen (shown), regions in windows that are gone, the launcher layer's
# blurRegions.size(), how many times the companion applied moves from an
# earlier surface (its log line "applied N region moves ...") and how many
# times its sweep put rects back ("put N region rects ... back"). With the bug:
# stale > 0 and sf = shown + stale. Keeps <out-prefix>.{txt,png,log}.
#
# Needs the companion in the launcher (dev_attach.sh launcher) and a page 4
# with something on it. Coordinates are for a 1080x2400 screen. Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
OUT="${1:-stale}"
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }
PKG=$($A shell cmd package resolve-activity --brief -a android.intent.action.MAIN \
        -c android.intent.category.HOME | tr -d '\r' | tail -1 | cut -d/ -f1)
s() { $A shell "$@" >/dev/null 2>&1; }

$A logcat -c
s am start -n com.android.settings/.Settings; sleep 2
s input keyevent HOME; sleep 1.5; s input keyevent HOME; sleep 1.5
for _ in 1 2 3; do s input swipe 900 1300 150 1300 120; sleep 0.8; done
s "input motionevent DOWN 540 2395; input motionevent MOVE 540 2200; input motionevent MOVE 540 1900; input motionevent MOVE 540 1700; sleep 0.6; input motionevent UP 540 1700"
sleep 1.5
s input tap 540 1200; sleep 2.5
s input keyevent HOME; sleep 1.5
s input keyevent HOME; sleep 1.5
for _ in 1 2 3 4; do s input swipe 900 1300 150 1300 120; sleep 0.7; s input swipe 150 1300 900 1300 120; sleep 0.7; done
sleep 1
pid=$($A shell pidof "$PKG" | tr -d '\r')
$A logcat -d --pid="$pid" -s StockGlassApp | grep -E 'applied|restarted|back where' > "$OUT.log"
$A shell setprop persist.sys.glass.app.debug 1
$A logcat -c
s service call activity 1599295570
sleep 1
$A logcat -d --pid="$pid" -s StockGlassApp | grep -E ' dump |dump: ' | sed -E 's/^.*StockGlassApp: //' > "$OUT.txt"
$A shell setprop persist.sys.glass.app.debug 0
sf=$($A shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | awk '
  /Layer \[/ {name=$0}
  /blurRegions.size\(\)=/ && name ~ /LauncherActivity|Launcher#|launcher3/ {s=$0; sub(/.*blurRegions.size\(\)=/, "", s); print s+0; f=1; exit}
  END {if (!f) print 0}')
$A exec-out screencap -p > "$OUT.png"
awk -v sf="$sf" '
  /^dump [0-9a-f]+ / && / host -?[0-9]+,-?[0-9]+ [0-9]+x[0-9]+ (shown|off)/ {
    a = 0; if (match($0, /alpha=[0-9]+/)) a = substr($0, RSTART + 6, RLENGTH - 6) + 0
    if (!match($0, /Rect\(-?[0-9]+, -?[0-9]+ - -?[0-9]+, -?[0-9]+\)/)) next
    split(substr($0, RSTART + 5, RLENGTH - 6), q, /, | - /)
    if (!(a > 0 && q[3] + 0 > q[1] + 0 && q[4] + 0 > q[2] + 0)) next
    if (/ detached/) gone++; else if (/ off /) stale++; else shown++
  }
  END { printf "stale=%d shown=%d gone=%d sf=%s", stale, shown, gone, sf }' "$OUT.txt"
echo " replays=$(grep -c applied "$OUT.log") sweeps=$(grep -c 'back where' "$OUT.log")"
