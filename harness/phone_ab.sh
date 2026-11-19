#!/usr/bin/env bash
# phone_ab.sh <scene> <rounds> <name=props> <name=props> ... : what the glass
# costs on a real phone's GPU, with the real UI.
#
# The emulator's numbers (ab.sh, qs_cost.sh) come from a desktop GPU, where a
# full-screen copy is cheap and every render pass costs about the same. A
# phone's tile-based GPU prices those the other way round, so the settings that
# matter have to be compared there. This drives the phone's own UI through adb
# and reads SurfaceFlinger's TimeStats, by turns over the variants (as ab.sh),
# so a phone that warms up or throttles does so under all of them alike.
#
#   ANDROID_SERIAL=192.168.50.140:37243 harness/phone_ab.sh qs 3 \
#       two= one=paneBlur=through stock=mode=stock
#
#   scene  qs      Quick Settings opened and closed, three times
#          shade   the notification shade, likewise
#          home    the launcher dragged to the next page and back, three times
#          drawer  the app drawer opened, dragged down and up three times, closed
#
# Each variant is a name and the properties it sets, `prop=value` separated by
# commas (persist.sys.glass. is prefixed), `mode=stock` for Android's own
# blur, and `app.<part>=0` to take one of the companion's parts out. Needs root (su) for the properties, an unlocked screen, and the module
# installed. Every property a variant names is put back as it was found.
#
# Prints, per variant: RenderEngine time per composited frame (median of the
# rounds' averages; with a fence it runs to the GPU finishing the frame), the
# frames SurfaceFlinger missed, and the engine's last `stats:` line.
set -u
SERIAL="${ANDROID_SERIAL:?set ANDROID_SERIAL to the phone}"
A="adb -s $SERIAL"
P=persist.sys.glass
SCENE="${1:?scene: qs | shade | home | drawer}"; ROUNDS="${2:?rounds}"; shift 2
VARIANTS=("$@")
[ ${#VARIANTS[@]} -gt 0 ] || { echo "no variants" >&2; exit 1; }
root() { $A shell "su -c \"$1\""; }
[ "$(root 'id -u' | tr -d '\r')" = 0 ] || { echo "no root on $SERIAL" >&2; exit 1; }

# Every property named anywhere, and what it holds now.
ALL=$(printf '%s\n' "${VARIANTS[@]}" | sed 's/^[^=]*=//' | tr ',' '\n' | sed -n 's/=.*//p' | sort -u)
declare -A WAS
for k in $ALL stats; do WAS[$k]="$($A shell getprop $P.$k | tr -d '\r')"; done
restore() {
  for k in $ALL stats; do root "setprop $P.$k '${WAS[$k]}'"; done
  $A shell dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1
  $A shell cmd statusbar collapse >/dev/null 2>&1
}
trap restore EXIT

apply() {
  local spec="${1#*=}" kv kvs
  for k in $ALL; do root "setprop $P.$k '${WAS[$k]}'"; done
  if [ -n "$spec" ]; then
    IFS=, read -r -a kvs <<<"$spec"
    for kv in "${kvs[@]}"; do root "setprop $P.${kv%%=*} '${kv#*=}'"; done
  fi
  # The companion's parts (app.<part>) are read by the apps: have them run
  # their property callbacks now (SYSPROPS_TRANSACTION, as glassctl does).
  case " $ALL " in *" app."*) root "service call activity 1599295570" >/dev/null 2>&1 ;; esac
}

drive() {
  case "$SCENE" in
    qs)    for _c in 1 2 3; do $A shell cmd statusbar expand-settings; sleep 2.2
                               $A shell cmd statusbar collapse; sleep 1.5; done ;;
    shade) for _c in 1 2 3; do $A shell cmd statusbar expand-notifications; sleep 2.2
                               $A shell cmd statusbar collapse; sleep 1.5; done ;;
    # Dragged, not flicked: a launcher may snap a flicked page in a few
    # frames, and a drag has it draw every frame the finger moves.
    home)  for _c in 1 2 3; do $A shell input swipe 860 1300 220 1300 700; sleep 0.6
                               $A shell input swipe 220 1300 860 1300 700; sleep 0.6; done ;;
    drawer) $A shell input swipe 540 1900 540 700 300; sleep 1
            for _c in 1 2 3; do $A shell input swipe 540 1700 540 700 700; sleep 0.5
                                $A shell input swipe 540 700 540 1700 700; sleep 0.5; done
            $A shell input keyevent BACK; sleep 1 ;;
    *) echo "no scene '$SCENE'" >&2; exit 1 ;;
  esac
}

# A phone left alone for two minutes is asleep on its lock screen, and a scene
# driven there measures the lock screen. Wake it and ask the keyguard to go
# before every variant; that works without credentials only while the phone
# still counts as unlocked (a swipe lock, or the owner's unlock not yet
# expired), and where it does not, stop rather than report nonsense.
ready() {
  $A shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1
  sleep 1
  if $A shell dumpsys window | tr -d '\r' | grep -q 'isKeyguardShowing=true'; then
    # Quick Settings and the shade open over a lock screen too (PHONE_AB_LOCKED=1
    # says that is what is wanted): the same panes, the lock screen under them
    # where the launcher would be.
    case "${PHONE_AB_LOCKED:-0}:$SCENE" in
      1:qs|1:shade) return 0 ;;
    esac
    echo "the lock screen is up on $SERIAL and will not go without the owner: unlock it" >&2
    echo "(qs and shade can run over it: PHONE_AB_LOCKED=1)" >&2
    exit 1
  fi
  # HOME only when something else is in front: on a launcher that is already
  # home it can open the app drawer (crDroid's does), and every second variant
  # then measures the drawer. BACK closes a drawer or a folder and does
  # nothing on the home screen.
  if ! $A shell dumpsys window | tr -d '\r' | grep -q 'mCurrentFocus=.*[Ll]auncher'; then
    $A shell input keyevent HOME; sleep 1.2
  fi
  $A shell input keyevent BACK; sleep 0.8
}

root "setprop $P.stats 1"
ready
# Where the time goes: the RenderEngine thread's own CPU time (utime + stime,
# 10 ms ticks, so only meaningful over a whole variant), and how long the GPU
# sat at each clock (kgsl's devfreq, where there is one). Read, never set.
SFPID=$(root 'pidof surfaceflinger' | tr -d '\r')
RETID=$(root "grep -l RenderEngine /proc/$SFPID/task/*/comm" | head -1 | tr -d '\r' | sed 's#.*/task/\([0-9]*\)/comm#\1#')
cpu_ticks() { root "cat /proc/$SFPID/task/$RETID/stat" | tr -d '\r' | awk '{print $14 + $15}'; }
KGSL=/sys/class/kgsl/kgsl-3d0/devfreq/trans_stat
gpu_states() { root "cat $KGSL 2>/dev/null" | tr -d '\r' | awk '$1 ~ /^\*?[0-9]+:$/ || ($1 == "*" && $2 ~ /^[0-9]+:$/) {f = ($1 == "*") ? $2 : $1; gsub(/[*:]/, "", f); printf "%s=%s ", f, $NF}'; }
declare -A SUM MISS FR STAT CPU GPU FRS LOCK
for v in "${VARIANTS[@]}"; do SUM[${v%%=*}]=""; MISS[${v%%=*}]=0; CPU[${v%%=*}]=0; FRS[${v%%=*}]=0; GPU[${v%%=*}]=""; done
for _ in $(seq "$ROUNDS"); do
  for v in "${VARIANTS[@]}"; do
    name="${v%%=*}"
    apply "$v"; sleep 1.5
    ready
    # What the scene runs over is part of the result: Quick Settings over a
    # lock screen has the lock screen under it, unlocked it has the launcher
    # and its glass icons, and the two cost very different frames.
    k=$($A shell dumpsys window 2>/dev/null | tr -d '\r' | grep -c 'isKeyguardShowing=true')
    LOCK[$name]="${LOCK[$name]:-} ${k:-0}"
    $A logcat -c
    $A shell dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null
    c0=$(cpu_ticks); g0=$(gpu_states)
    drive
    c1=$(cpu_ticks); g1=$(gpu_states)
    out=$($A shell dumpsys SurfaceFlinger --timestats -dump -maxlayers 0 | tr -d '\r')
    CPU[$name]=$(( ${CPU[$name]} + ${c1:-0} - ${c0:-0} ))
    GPU[$name]="${GPU[$name]} | $g0 > $g1"
    SUM[$name]="${SUM[$name]} $(sed -n 's/^averageRenderEngineTiming = \([0-9.]*\).*/\1/p' <<<"$out")"
    f="$(sed -n 's/^clientCompositionFrames = //p' <<<"$out")"
    FR[$name]="${FR[$name]:-} ${f:-0}"
    FRS[$name]=$(( ${FRS[$name]} + ${f:-0} ))
    m=$(sed -n 's/^missedFrames = //p' <<<"$out"); MISS[$name]=$(( ${MISS[$name]} + ${m:-0} ))
    s=$($A logcat -d -s StockGlass | grep -o 'stats: .*' | tail -2 | head -1)
    [ -n "$s" ] && STAT[$name]="$s"
  done
done
for v in "${VARIANTS[@]}"; do
  name="${v%%=*}"
  python3 - "$SCENE" "$name" "${FR[$name]:-0}" "${MISS[$name]}" "${STAT[$name]:-}" "${CPU[$name]}" "${FRS[$name]}" "${GPU[$name]}" "${LOCK[$name]:-0}" ${SUM[$name]} <<'PY'
import re, statistics, sys
scene, name, frames, missed, stat, cpu, frs, gpu, locks = sys.argv[1:10]
x = [float(v) for v in sys.argv[10:]]
frames = [int(v) for v in frames.split()] or [0]
locks = [int(v) for v in locks.split()] or [0]
short = ''
m = re.search(r'blurs/frame=([0-9.]+).*derived/frame=([0-9.]+).*snapshot-copies/frame=([0-9.]+).*hook=([0-9.]+)us', stat)
if m:
    short = 'blurs=%s derived=%s copies=%s hook=%sus' % m.groups()
# The RenderEngine thread's CPU time per composited frame, over all rounds.
cpu_ms = 10.0 * int(cpu) / max(int(frs), 1)
# The GPU clock, weighted by the time spent at each, over all rounds.
at = {}
for pair in gpu.split('|'):
    if '>' not in pair:
        continue
    a, b = (dict((kv.split('=')[0], int(kv.split('=')[1])) for kv in side.split()) for side in pair.split('>'))
    for f in b:
        at[f] = at.get(f, 0) + b[f] - a.get(f, b[f])
total = sum(at.values())
clock = ' gpu %.0f MHz avg' % (sum(int(f) * t for f, t in at.items()) / total / 1e6) if total else ''
# A scene that ran unlocked draws hundreds of frames a round. A few dozen
# means the phone was doing something else in that round (asleep, in its
# owner's hands), so every round's count is printed. Over a lock screen a few
# dozen is all there is: the F1 opens Quick Settings there in five frames.
if all(locks):
    where = '  over the lock screen'
elif any(locks):
    where = '  [the lock screen was up in %d of %d rounds: not one scene]' % (sum(1 for k in locks if k), len(locks))
else:
    where = '  unlocked'
few = '  [TOO FEW FRAMES in a round: the scene did not run]' if min(frames) < 100 and not all(locks) else ''
print('%-6s %-10s re=%.3f ms median  (min %.2f over %d: %s)  frames a round: %s, %s missed in all  cpu %.2f ms/frame%s  %s%s%s' % (
    scene, name, statistics.median(x), min(x), len(x), ' '.join('%.2f' % v for v in x),
    ' '.join(str(v) for v in frames), missed, cpu_ms, clock, short, where, few))
PY
done
