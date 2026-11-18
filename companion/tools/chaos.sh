#!/usr/bin/env bash
# chaos.sh [trials] [seed] [outdir]: stale launcher glass hunt.
#
# Each trial runs 4-9 random home screen actions with random gaps (page
# flings and drags, home key and home gesture mid-animation, overview hold,
# quick switch, back, app launches from icons, the app drawer, long presses,
# an icon picked up and put back, screen off/on, Wallpaper & style), lets the
# UI settle, then checks every companion region in the launcher (the
# region_dump.sh dump) and the launcher layer's blurRegions.size() in
# `dumpsys SurfaceFlinger`. It checks twice: where the trial left the
# launcher, and again after a HOME press.
#
#   stale     alpha > 0, a non-empty rect and "off" (no part of its control
#             on screen) in a live window: the region still reaches
#             SurfaceFlinger
#   misplaced alpha > 0, a non-empty rect, its control "shown", but the rect
#             is not inside the control's bounds (a rect from another frame)
#   excess    SurfaceFlinger's count for the launcher's window minus the
#             regions shown in it (DecorView; every window if the dump does
#             not name windows)
#   gone      alpha > 0 and a non-empty rect in a window that is gone (the
#             dump says "detached"; Wallpaper & style's home screen preview
#             leaves these): they cannot reach SurfaceFlinger, so they are
#             counted apart and are not incidents
#
# A trial with any of the three is an incident; its dump, SurfaceFlinger
# lines and a screenshot are kept in outdir. The same seed replays the same
# actions and gaps (input latency still varies). Needs the companion in the
# launcher (dev_attach.sh launcher) and glass mode. Emulator only: it refuses
# a device that is not one. It sets persist.sys.glass.app.debug=1 for each
# check and 0 after.
set -u
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
TRIALS="${1:-30}"; SEED="${2:-1}"; OUT="${3:-chaos-out}"
RANDOM=$SEED
mkdir -p "$OUT"

[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }
PKG=$($A shell cmd package resolve-activity --brief -a android.intent.action.MAIN \
        -c android.intent.category.HOME | tr -d '\r' | tail -1 | cut -d/ -f1)
[ -n "$($A shell pidof "$PKG" | tr -d '\r')" ] || { echo "no $PKG process" >&2; exit 1; }
PICKER=com.google.android.apps.wallpaper/com.android.customization.picker.CustomizationPickerActivity

# Random picks set R in this shell (a $(...) subshell would not advance RANDOM here).
rnd() { R=$(( RANDOM % $1 )); }
sh_() { $A shell "$@" >/dev/null 2>&1; }

# Icon centres on a 4-column page (1080 px wide), and spots that are often empty.
ICON_X=(157 412 667 922); ICON_Y=(562 842 1090 1370 1650)
EMPTY_X=(540 540 300 800); EMPTY_Y=(600 1500 300 1450)
GAPS=(0 0 60 120 250 400 700 1200)

# act: one random action; its label goes to LABEL.
act() {
  local a x y d
  rnd 26; a=$R
  case $a in
    0|1|2|3)  # page fling left or right
      rnd 200; d=$(( 60 + R )); rnd 2
      if [ $R = 0 ]; then sh_ input swipe 900 1300 150 1300 $d; LABEL="fling<$d"
      else sh_ input swipe 150 1300 900 1300 $d; LABEL="fling>$d"; fi ;;
    4|5)      # slow page drag, released anywhere (snaps back or on)
      rnd 500; x=$(( 250 + R ))
      sh_ "input motionevent DOWN 900 1300; input motionevent MOVE 700 1300; input motionevent MOVE $x 1300; input motionevent UP $x 1300"
      LABEL="drag$x" ;;
    6|7|8)    sh_ input keyevent HOME; LABEL=home ;;
    9|10|11)  # home gesture: fast swipe up from the bottom edge
      rnd 150; d=$(( 80 + R )); rnd 600; y=$(( 900 + R ))
      sh_ input swipe 540 2395 540 $y $d; LABEL="up$d" ;;
    12|13)    # overview: swipe up and hold, then let go
      rnd 6; d=$(( 3 + R ))
      sh_ "input motionevent DOWN 540 2395; input motionevent MOVE 540 2200; input motionevent MOVE 540 1900; input motionevent MOVE 540 1700; sleep 0.$d; input motionevent UP 540 1700"
      LABEL=overview ;;
    14|15)    # quick switch along the bottom edge
      rnd 2
      if [ $R = 0 ]; then sh_ input swipe 150 2390 900 2370 120; LABEL="qs>"
      else sh_ input swipe 900 2390 150 2370 120; LABEL="qs<"; fi ;;
    16|17)    # back gesture from either edge
      rnd 2
      if [ $R = 0 ]; then sh_ input swipe 2 1200 450 1200 120; LABEL="back<"
      else sh_ input swipe 1078 1200 630 1200 120; LABEL="back>"; fi ;;
    18|19|20) # tap whatever is at an icon position: launches the app if it is an icon
      rnd 4; x=${ICON_X[$R]}; rnd 5; y=${ICON_Y[$R]}
      sh_ input tap $x $y; LABEL="tap$x,$y" ;;
    21)       # app drawer: open, sometimes close
      sh_ input swipe 540 1800 540 500 150; rnd 2
      if [ $R = 0 ]; then sh_ input keyevent BACK; LABEL=drawer+back; else LABEL=drawer; fi ;;
    22)       # long press on (often) empty space: options popup or an item's menu
      rnd 4; x=${EMPTY_X[$R]}; y=${EMPTY_Y[$R]}
      sh_ input swipe $x $y $x $y 900; rnd 2
      if [ $R = 0 ]; then sh_ input keyevent BACK; LABEL="longpress$x,$y+back"; else LABEL="longpress$x,$y"; fi ;;
    23)       # pick an icon up (spring-loaded: the workspace shrinks) and put it back
      rnd 4; x=${ICON_X[$R]}; rnd 5; y=${ICON_Y[$R]}; rnd 6; d=$(( 2 + R ))
      sh_ "input motionevent DOWN $x $y; sleep 0.8; input motionevent MOVE $(( x + 60 )) $(( y - 120 )); input motionevent MOVE $(( x + 90 )) $(( y - 200 )); sleep 0.$d; input motionevent MOVE $x $y; input motionevent UP $x $y"
      LABEL="pickup$x,$y" ;;
    24)       # screen off and on (the AVD has no lockscreen)
      rnd 6; d=$(( 3 + R ))
      sh_ input keyevent POWER; sleep 0.$d; sh_ input keyevent POWER; LABEL=power ;;
    25)       # Wallpaper & style, which draws launcher previews in the launcher process
      sh_ am start -n $PICKER; sleep 1.5; LABEL=wallpaper ;;
  esac
}

focus() { $A shell dumpsys window 2>/dev/null | tr -d '\r' | sed -n 's/.*mCurrentFocus=Window{[^ ]* [^ ]* \([^/}]*\).*/\1/p' | head -1; }

sfcount() {
  $A shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | awk '
    /Layer \[/ {name=$0}
    /blurRegions.size\(\)=/ && name ~ /LauncherActivity|Launcher#|launcher3/ {s=$0; sub(/.*blurRegions.size\(\)=/, "", s); print s+0; f=1; exit}
    END {if (!f) print 0}'
}

# check <tag>: sets RES to "stale misplaced excess sf shown gone", or to "-" when
# the launcher gave no dump (three tries); keeps the evidence on an incident.
check() {
  local tag=$1 pid dump="" sf try
  pid=$($A shell pidof "$PKG" | tr -d '\r')
  for try in 1 2 3; do
    $A shell setprop persist.sys.glass.app.debug 1
    $A logcat -c
    sh_ service call activity 1599295570
    sleep "$try"
    dump=$($A logcat -d --pid="$pid" -s StockGlassApp | grep -E ' dump |dump: ' | sed -E 's/^.*StockGlassApp: //')
    grep -q '^dump: ' <<<"$dump" && break
  done
  $A shell setprop persist.sys.glass.app.debug 0
  if ! grep -q '^dump: ' <<<"$dump"; then RES="-"; return; fi
  sf=$(sfcount)
  RES=$(awk -v sf="$sf" '
    /^dump [0-9a-f]+ / && / host -?[0-9]+,-?[0-9]+ [0-9]+x[0-9]+ (shown|off)/ {
      a = 0; if (match($0, /alpha=[0-9]+/)) a = substr($0, RSTART + 6, RLENGTH - 6) + 0
      if (!match($0, /Rect\(-?[0-9]+, -?[0-9]+ - -?[0-9]+, -?[0-9]+\)/)) next
      split(substr($0, RSTART + 5, RLENGTH - 6), q, /, | - /)
      l = q[1] + 0; t = q[2] + 0; r = q[3] + 0; b = q[4] + 0
      match($0, / host -?[0-9]+,-?[0-9]+ [0-9]+x[0-9]+ (shown|off)/)
      split(substr($0, RSTART + 6, RLENGTH - 6), h, /[ ,x]/)
      if (!(a > 0 && r > l && b > t)) next
      win = ""; if (match($0, / in [A-Za-z0-9_$]+@[0-9a-f]+/)) win = substr($0, RSTART + 4, RLENGTH - 4)
      if (/ detached/) gone++
      else if (h[5] == "off") stale++
      else if (win == "" || win ~ /^DecorView@/) {
        shown++
        if (l < h[1] - 4 || t < h[2] - 4 || r > h[1] + h[3] + 4 || b > h[2] + h[4] + 4) mis++
      }
    }
    END { printf "%d %d %d %d %d %d", stale, mis, sf - shown, sf, shown, gone }' <<<"$dump")
  set -- $RES
  if [ "$1" -gt 0 ] || [ "$2" -gt 0 ] || [ "$3" -gt 0 ]; then
    printf '%s\n' "$dump" > "$OUT/$tag.dump.txt"
    $A shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | grep -E 'Layer \[|blurRegions' > "$OUT/$tag.sf.txt"
    $A exec-out screencap -p > "$OUT/$tag.png"
  fi
}

LOG="$OUT/trials.txt"
printf '# chaos.sh trials=%s seed=%s serial=%s launcher=%s %s\n' "$TRIALS" "$SEED" "$SERIAL" "$PKG" "$(date -Iseconds)" > "$LOG"
printf '# trial check     stale misplaced excess sf shown gone  actions\n' >> "$LOG"
incidents=0; total_stale=0; total_mis=0; total_excess=0; total_gone=0; nodump=0
sh_ input keyevent HOME; sleep 1.5
for t in $(seq 1 "$TRIALS"); do
  rnd 6; n=$(( 4 + R )); acts=""
  for _ in $(seq 1 "$n"); do
    act; acts="$acts $LABEL"
    rnd ${#GAPS[@]}; g=${GAPS[$R]}
    [ "$g" -gt 0 ] && sleep "$(printf '%d.%03d' $(( g / 1000 )) $(( g % 1000 )))"
  done
  sleep 1.2
  [ "$(focus)" = "$PKG" ] || { sh_ input keyevent HOME; acts="$acts [home]"; }
  sleep 2.5
  check "t${t}a"; r1=$RES
  sh_ input keyevent HOME; sleep 2.5
  check "t${t}b"; r2=$RES
  hit=0
  for r in "$r1" "$r2"; do
    [ "$r" = - ] && { nodump=$(( nodump + 1 )); continue; }
    set -- $r
    total_stale=$(( total_stale + $1 )); total_mis=$(( total_mis + $2 )); total_gone=$(( total_gone + $6 ))
    [ "$3" -gt 0 ] && total_excess=$(( total_excess + $3 ))
    { [ "$1" -gt 0 ] || [ "$2" -gt 0 ] || [ "$3" -gt 0 ]; } && hit=1
  done
  incidents=$(( incidents + hit ))
  printf '%5d settled   %s\n%5d home      %s %s\n' "$t" "$r1" "$t" "$r2" "$acts" | tee -a "$LOG"
done
printf '# %d trials, %d with an incident; stale %d, misplaced %d, excess %d, gone %d (summed over both checks); %d checks without a dump\n' \
  "$TRIALS" "$incidents" "$total_stale" "$total_mis" "$total_excess" "$total_gone" "$nodump" | tee -a "$LOG"
