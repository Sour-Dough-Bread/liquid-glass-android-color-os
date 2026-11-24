#!/usr/bin/env bash
# Host test for the watchdog (watchdog.sh): the keep-or-revert question after
# glass is switched on, and the health watch after boot. No device: the real
# script runs under busybox ash with the Android tools it calls stubbed out.
#
#   module/tools/watchdog_test.sh
#
# Time is the stubs' business. `sleep` advances a tick counter instead of
# waiting, and pidof and getprop answer from per-tick sequences, so "System UI
# restarted three times in a minute" is a file with four PIDs in it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SH="busybox ash"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail=0

# A scratch device: props/<name> holds a property (props/<name>.seq one value
# per tick), seq/<process> one PID per tick, proc/<pid>/maps what is mapped.
stage() {
  local r=$1
  rm -rf "$r"; mkdir -p "$r/bin" "$r/props" "$r/seq" "$r/proc" "$r/state"
  echo 0 > "$r/tick"
  cat > "$r/bin/getprop" <<EOF
#!/bin/sh
t=\$(cat "$r/tick")
if [ -f "$r/props/\$1.seq" ]; then
  n=\$(wc -l < "$r/props/\$1.seq")
  i=\$((t + 1)); [ "\$i" -gt "\$n" ] && i=\$n
  sed -n "\${i}p" "$r/props/\$1.seq"
else
  cat "$r/props/\$1" 2>/dev/null || echo ""
fi
EOF
  cat > "$r/bin/setprop" <<EOF
#!/bin/sh
printf '%s\n' "\$2" > "$r/props/\$1"
rm -f "$r/props/\$1.seq"
EOF
  cat > "$r/bin/sleep" <<EOF
#!/bin/sh
echo \$((\$(cat "$r/tick") + 1)) > "$r/tick"
EOF
  cat > "$r/bin/pidof" <<EOF
#!/bin/sh
f="$r/seq/\$1"
[ -f "\$f" ] || exit 1
t=\$(cat "$r/tick")
n=\$(wc -l < "\$f")
i=\$((t + 1)); [ "\$i" -gt "\$n" ] && i=\$n
sed -n "\${i}p" "\$f"
EOF
  # Volume Up is "pressed" while key exists; no blocking either way.
  cat > "$r/bin/getevent" <<EOF
#!/bin/sh
[ -f "$r/key" ] && echo "/dev/input/event0: EV_KEY       KEY_VOLUMEUP         DOWN"
exit 0
EOF
  cat > "$r/bin/timeout" <<'EOF'
#!/bin/sh
shift
exec "$@"
EOF
  cat > "$r/bin/cmd" <<EOF
#!/bin/sh
echo "\$*" >> "$r/notes"
EOF
  cat > "$r/bin/svc" <<EOF
#!/bin/sh
echo "\$*" >> "$r/rebooted"
EOF
  printf '#!/bin/sh\nexit 0\n' > "$r/bin/sync"
  chmod +x "$r/bin/"*
  # busybox ash runs its own sleep and pidof applets ahead of PATH, so the
  # copy under test calls the stubs by path. Nothing else is changed.
  sed -e "s#\bsleep \([0-9]\)#$r/bin/sleep \1#g" -e "s#pidof \"\$1\"#$r/bin/pidof \"\$1\"#" \
    -e "s#/system/bin/svc#$r/bin/svc#" "$HERE/watchdog.sh" > "$r/watchdog.sh"
  grep -q "$r/bin/sleep 2" "$r/watchdog.sh" && grep -q "$r/bin/pidof" "$r/watchdog.sh" || {
    echo "watchdog_test: could not point the copy at the stubs" >&2; exit 1; }
}

wd() {  # root args...
  local r=$1; shift
  PATH="$r/bin:$PATH" GLASS_STATE="$r/state" GLASS_PROC="$r/proc" \
    $SH "$r/watchdog.sh" "$@" >"$r/out" 2>&1 || true
}

prop() { cat "$1/props/$2" 2>/dev/null || true; }

check() {  # name condition...
  local name=$1; shift
  if "$@"; then echo "  ok    $name"; else echo "  FAIL  $name"; fail=1; fi
}
has() { grep -q "$2" "$1" 2>/dev/null; }
hasnt() { ! grep -q "$2" "$1" 2>/dev/null; }

r="$WORK/d"

echo "confirm"
stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"
echo 100 > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
wd "$r" confirm 5
check "unanswered, glass goes back to Android's blur" [ "$(prop "$r" persist.sys.glass.mode)" = stock ]
check "and says why" has "$r/props/sys.glass.reverted" "not confirmed within 5 seconds"
check "the question was posted" has "$r/notes" "Press Volume Up within 5 seconds"
check "and the revert" has "$r/notes" "Switched back to Android's blur"
check "nothing left pending" [ ! -f "$r/state/confirm" ]
check "the pending deadline is cleared" [ -z "$(prop "$r" sys.glass.confirm)" ]

stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"
echo 100 > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
: > "$r/key"
wd "$r" confirm 5
check "Volume Up keeps the glass" [ "$(prop "$r" persist.sys.glass.mode)" = glass ]
check "and says so" has "$r/notes" "Liquid Glass kept"
check "then watches the UI" has "$r/state/watchdog.log" "health: steady"

stage "$r"; echo stock > "$r/props/persist.sys.glass.mode"
wd "$r" confirm 5
check "glass already off: nothing to revert" [ -z "$(prop "$r" sys.glass.reverted)" ]

stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"; echo 0 > "$r/props/persist.sys.glass.confirm"
echo 100 > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
wd "$r" confirm
check "confirm=0 never asks" hasnt "$r/notes" "Press Volume Up"
check "and keeps the glass" [ "$(prop "$r" persist.sys.glass.mode)" = glass ]

stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"; echo "abc" > "$r/props/persist.sys.glass.confirm"
echo 100 > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
wd "$r" confirm
check "a confirm time that is not a number means the default" has "$r/notes" "within 20 seconds"

echo "keep, cancel, pending"
stage "$r"
echo "4242 $(( $(date +%s) + 30 ))" > "$r/state/confirm"
wd "$r" pending
check "pending says the seconds left" grep -qE '^(29|30)$' "$r/out"
wd "$r" keep
check "keep answers a pending confirm" [ ! -f "$r/state/confirm" ]
check "and says so" has "$r/out" "Liquid Glass kept"
wd "$r" keep
check "keep with nothing pending says so" has "$r/out" "nothing to confirm"
echo "4242 $(( $(date +%s) - 5 ))" > "$r/state/confirm"
wd "$r" pending
check "an expired deadline is not pending" [ ! -s "$r/out" ]
echo "4242 99" > "$r/state/confirm"
wd "$r" cancel
check "cancel drops it" [ ! -f "$r/state/confirm" ]

echo "health"
stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"
printf '100\n100\n101\n102\n' > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
wd "$r" health 20
check "SurfaceFlinger restarting with glass on: glass off" [ "$(prop "$r" persist.sys.glass.mode)" = stock ]
check "and says why" has "$r/props/sys.glass.reverted" "SurfaceFlinger restarted 2 times"
check "no reboot for that" [ ! -f "$r/rebooted" ]

stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"
echo 100 > "$r/seq/surfaceflinger"; printf '200\n201\n202\n203\n' > "$r/seq/com.android.systemui"
wd "$r" health 20
check "System UI restarting with glass on: glass off" [ "$(prop "$r" persist.sys.glass.mode)" = stock ]
check "and says why" has "$r/props/sys.glass.reverted" "System UI restarted 3 times"

stage "$r"; echo stock > "$r/props/persist.sys.glass.mode"
echo 100 > "$r/seq/surfaceflinger"; printf '200\n201\n202\n203\n204\n' > "$r/seq/com.android.systemui"
wd "$r" health 20
check "System UI restarting with glass off is not ours" [ -z "$(prop "$r" sys.glass.reverted)" ]
check "and turns nothing off" [ ! -f "$r/state/disabled" ]

stage "$r"; echo stock > "$r/props/persist.sys.glass.mode"
printf '100\n101\n102\n103\n' > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
for p in 100 101 102 103; do mkdir -p "$r/proc/$p"; echo "7f00 r-xp /system/lib64/libglassengine.so" > "$r/proc/$p/maps"; done
wd "$r" health 20
check "SurfaceFlinger restarting with Android's blur and the engine in it: module off" \
  has "$r/state/disabled" "^watchdog: SurfaceFlinger restarted 3 times"
check "and a reboot" has "$r/rebooted" "power reboot"

stage "$r"; echo stock > "$r/props/persist.sys.glass.mode"
printf '100\n101\n102\n103\n' > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
wd "$r" health 20
check "without the engine in SurfaceFlinger, its crashes are not ours" [ ! -f "$r/state/disabled" ]
check "and nothing reboots" [ ! -f "$r/rebooted" ]

stage "$r"; echo stock > "$r/props/persist.sys.glass.mode"
printf '100\n101\n102\n103\n' > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
for p in 100 101 102 103; do mkdir -p "$r/proc/$p"; echo "/system/lib64/libglassengine.so" > "$r/proc/$p/maps"; done
echo switching > "$r/props/sys.glass.live"
wd "$r" health 20
check "live mode's own restarts are not crashes" [ ! -f "$r/state/disabled" ]

stage "$r"; echo stock > "$r/props/persist.sys.glass.mode"
printf '100\n100\n101\n' > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
printf '\n\n1700000000\n' > "$r/props/sys.glass.tripped.seq"
mkdir -p "$r/proc/101"; echo "/system/lib64/libglassengine.so" > "$r/proc/101/maps"
wd "$r" health 20
check "a crash the engine's guard caught is reported" has "$r/props/sys.glass.reverted" "crashed drawing Liquid Glass"
check "and not counted against Android's blur" [ ! -f "$r/state/disabled" ]

stage "$r"; echo glass > "$r/props/persist.sys.glass.mode"
echo 100 > "$r/seq/surfaceflinger"; echo 200 > "$r/seq/com.android.systemui"
wd "$r" health 20
check "a steady UI keeps the glass" [ "$(prop "$r" persist.sys.glass.mode)" = glass ]
check "and the log says steady" has "$r/state/watchdog.log" "health: steady"

exit $fail
