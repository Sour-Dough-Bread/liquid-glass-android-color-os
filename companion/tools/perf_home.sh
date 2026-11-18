#!/usr/bin/env bash
# perf_home.sh <label> [swipes]: what the home screen's glass costs, on a fixed
# workload. Starts on the default page, goes to the 4th page (the stress page:
# a large widget, a 4x1 widget and icons), opens and scrolls the app drawer
# once, then swipes between the 3rd and 4th page <swipes> times (default 10
# each way). Prints one line:
#
#   sync      the companion's per-frame work in the launcher's main window
#             (Region.syncWindow, UI thread), average per pre-draw, from the
#             region dump (CPU)
#   ui / rt   median UI-thread traversal (input to draw, which holds the
#             pre-draw) and RenderThread sync (where hwui reports region
#             positions) of the launcher's last 120 frames (CPU)
#   p50/p90   the launcher's frame time percentiles over the workload (gfxinfo)
#   re        SurfaceFlinger's average RenderEngine time per composited frame
#             (TimeStats; on the AVD's SwiftShader that GPU work is CPU)
#   sflog     lines SurfaceFlinger logged under the engine's tag (StockGlass)
#   regions   live companion regions, and the launcher layer's SF regions at
#             the end (on the stress page)
#   valid     NO when the launcher was not in front at the end (a swipe taken
#             as a tap opened an app) or its layer had no regions: compare
#             only valid runs
#
# Needs the companion in the launcher (dev_attach.sh launcher). Emulator only.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
LABEL="${1:-run}"; SWIPES="${2:-10}"
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }
PKG=$($A shell cmd package resolve-activity --brief -a android.intent.action.MAIN \
        -c android.intent.category.HOME | tr -d '\r' | tail -1 | cut -d/ -f1)
sh_() { $A shell "$@" >/dev/null 2>&1; }
# Page swipes start in the gaps between icon columns (x 790 and 290 on a
# 1080 px 4-column grid): one that starts on an icon can be taken as a tap
# when the launcher lags, and opens the app.
left() { sh_ input swipe 790 1300 90 1300 120; sleep 0.8; }
right() { sh_ input swipe 290 1300 990 1300 120; sleep 0.8; }

dump() {  # prints the dump's summary line
  local pid; pid=$($A shell pidof "$PKG" | tr -d '\r')
  $A shell setprop persist.sys.glass.app.debug 1
  $A logcat -c
  sh_ service call activity 1599295570
  sleep 1
  $A logcat -d --pid="$pid" -s StockGlassApp | grep -E 'dump: ' | tail -1 | sed -E 's/^.*StockGlassApp: //'
  $A shell setprop persist.sys.glass.app.debug 0
}

sh_ input keyevent HOME; sleep 1.5; sh_ input keyevent HOME; sleep 1.5
left; left; left
dump >/dev/null
sleep 1  # the companion re-reads app.debug at most every 500 ms: no debug tracing in the workload
sh_ dumpsys gfxinfo "$PKG" reset
$A shell dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null
SFPID=$($A shell pidof surfaceflinger | tr -d '\r')
$A logcat -c
sh_ input swipe 540 1800 540 500 150; sleep 1.2
sh_ input swipe 540 1800 540 700 200; sleep 1
sh_ input keyevent BACK; sleep 1.2
for _ in $(seq 1 "$SWIPES"); do right; left; done
sleep 0.5
# A run that lost the launcher (a swipe taken as a tap opens an app) is not comparable.
focus=$($A shell dumpsys window 2>/dev/null | tr -d '\r' | sed -n 's/.*mCurrentFocus=Window{[^ ]* [^ ]* \([^/}]*\).*/\1/p' | head -1)
sflog=$($A logcat -d --pid="$SFPID" -s StockGlass | grep -c ' StockGlass')
ts=$($A shell dumpsys SurfaceFlinger --timestats -dump -maxlayers 0 | tr -d '\r')
$A shell dumpsys SurfaceFlinger --timestats -disable >/dev/null
gfx=$($A shell dumpsys gfxinfo "$PKG" framestats | tr -d '\r')
summary=$(dump)
sfregions=$($A shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | awk '
  /Layer \[/ {name=$0}
  /blurRegions.size\(\)=/ && name ~ /LauncherActivity|Launcher#|launcher3/ {s=$0; sub(/.*blurRegions.size\(\)=/, "", s); print s+0; f=1; exit}
  END {if (!f) print 0}')

sync=$(sed -nE 's/.*DecorView=[0-9]+ \([0-9]+ since the last dump, ([0-9.]+) us each\).*/\1/p' <<<"$summary")
predraws=$(sed -nE 's/.*DecorView=[0-9]+ \(([0-9]+) since the last dump.*/\1/p' <<<"$summary")
live=$(sed -nE 's/^dump: ([0-9]+) regions.*/\1/p' <<<"$summary")
# framestats rows: FLAGS,IntendedVsync,Vsync,...; columns by name from the header.
read -r ui rt <<<"$(awk -F, '
  /^Flags,/ { for (i = 1; i <= NF; i++) col[$i] = i; next }
  col["Flags"] && $1 ~ /^[0-9]+$/ && $1 == 0 {
    u[n] = ($(col["DrawStart"]) - $(col["HandleInputStart"])) / 1e6
    r[n] = ($(col["IssueDrawCommandsStart"]) - $(col["SyncStart"])) / 1e6
    n++
  }
  function med(a, n,   i, j, t) { for (i = 0; i < n; i++) for (j = i + 1; j < n; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }; return n ? a[int(n / 2)] : 0 }
  END { printf "%.3f %.3f", med(u, n), med(r, n) }' <<<"$gfx")"
p50=$(sed -nE 's/^50th percentile: ([0-9]+)ms/\1/p' <<<"$gfx" | head -1)
p90=$(sed -nE 's/^90th percentile: ([0-9]+)ms/\1/p' <<<"$gfx" | head -1)
frames=$(sed -nE 's/^Total frames rendered: ([0-9]+)/\1/p' <<<"$gfx" | head -1)
re=$(grep -o 'averageRenderEngineTiming = [0-9.]*' <<<"$ts" | head -1 | sed 's/.*= //')
valid=yes; [ "$focus" = "$PKG" ] && [ "$sfregions" -gt 0 ] || valid=NO
printf '%-28s sync=%sus/%s ui=%sms rt=%sms frames=%s p50=%sms p90=%sms re=%sms sflog=%s regions=%s sf=%s valid=%s\n' \
  "$LABEL" "${sync:-?}" "${predraws:-?}" "$ui" "$rt" "${frames:-?}" "${p50:-?}" "${p90:-?}" "${re:-?}" "$sflog" "${live:-?}" "$sfregions" "$valid"
