#!/usr/bin/env bash
# Loads the module into Android Auto's running ":projection" process on the
# userdebug test AVD, with no LSPosed (an adapted copy of
# companion/dev_attach.sh; the JVMTI dev loader is the companion's).
#
#   ANDROID_SERIAL=emulator-5564 ./dev_attach.sh          attach, or reload if attached
#   ANDROID_SERIAL=emulator-5564 ./dev_attach.sh cmd <c>  run a probe command (Host CMD)
#
# Commands (glass/CardGlass.command): state, on, off, auto, set <key> <value>
# (through, bthrough, blur, fill, tint, effect, or a Lens.Look field), windows,
# tree <title>, draws <title> (software draw of a window: cache/probe-draw.txt),
# trace <n> [all] (the next n recording calls: cache/trace.txt), trace-stop.
# Read the cache files as root: adb shell cat
# /data/data/com.google.android.projection.gearhead/cache/<file>.
#
# Only one dev-loader module can be attached per process: this one and
# auto/media cannot both be in ":projection" at once (this one carries a copy
# of auto/media's card glass).
#
# Steps, and why:
#  1. Refuse anything but a debuggable build (never a phone).
#  2. Install out/stockglass-auto-lists.apk.
#  3. The agent: /system/lib64/libglassagent.so, the companion's, which the lab
#     already has. It is pushed only if it is missing or differs (another
#     agent may have it mapped).
#  4. Find the process that draws the car's Dashboard window: the pid of the
#     window manager's session for "Window{... Dashboard}". Android Auto is
#     NOT restarted (that drops the car's projection for everyone).
#  5. If the agent is not in that process yet: attach-agent. If it is (the dev
#     loader attaches once per process), broadcast RELOAD instead: Host stops
#     the running build and starts the newly installed one.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APK="${APK:-$HERE/out/stockglass-auto-lists.apk}"
PKG=dev.stockglass.auto.lists
: "${ANDROID_SERIAL:?set ANDROID_SERIAL (emulator-5564)}"
export ANDROID_SERIAL

sh_() { adb shell "$@" | tr -d '\r'; }

if [ "$(sh_ getprop ro.debuggable)" != "1" ]; then
  echo "refusing: $(sh_ getprop ro.build.fingerprint) is not a debuggable build" >&2
  exit 1
fi

PID=$(sh_ dumpsys window windows | grep -A1 -E 'Window\{[0-9a-f]+ u0 Dashboard\}:' \
        | sed -nE 's/.*mSession=Session\{[0-9a-f]+ ([0-9]+):.*/\1/p' | head -1)
[ -n "$PID" ] || { echo "no Dashboard window (is projection up?)" >&2; exit 1; }
logs() { adb logcat -d --pid="$PID" -s SGAutoLists:V StockGlassAuto:V StockGlassDev:V AndroidRuntime:E | grep -v '^-----' || true; }

if [ "${1:-}" = cmd ]; then
  shift
  adb logcat -c
  sh_ am broadcast -a $PKG.CMD --es cmd "'$*'" >/dev/null
  sleep "${WAIT:-2}"
  logs | sed -E 's/^.{31}//'
  exit 0
fi

[ -f "$APK" ] || { echo "no $APK; run ./build.sh" >&2; exit 1; }
adb root >/dev/null 2>&1 || true
adb wait-for-device

echo "==> install $(basename "$APK")"
adb install -r "$APK" >/dev/null
BASE=$(sh_ pm path "$PKG" | sed -n 's/^package://p' | head -1)

AGENT=/system/lib64/libglassagent.so
ABI=$(sh_ getprop ro.product.cpu.abi)
LOCAL="$HERE/out/lib/$ABI/libglassagent.so"
if [ "$(sh_ "md5sum $AGENT 2>/dev/null" | cut -d' ' -f1)" != "$(md5sum "$LOCAL" | cut -d' ' -f1)" ]; then
  echo "==> pushing the agent to $AGENT"
  adb remount >/dev/null 2>&1 || { echo "cannot remount /system (start the AVD with -writable-system)" >&2; exit 1; }
  adb push "$LOCAL" "$AGENT" >/dev/null
  sh_ "chmod 644 $AGENT && chcon u:object_r:system_lib_file:s0 $AGENT"
fi

PROC=$(sh_ cat /proc/$PID/cmdline | tr '\0' '\n' | head -1)
jdwp=$(timeout 3 adb jdwp 2>/dev/null | tr -d "\r" || true)
if ! grep -qx "$PID" <<<"$jdwp"; then
  echo "$PROC ($PID) is not debuggable; attaching would crash it" >&2
  exit 1
fi

adb logcat -c
# Capture, then test: `... | grep -q` exits early, SIGPIPEs adb, and pipefail
# reports the match as a failure (as companion/dev_attach.sh notes).
maps=$(sh_ cat /proc/$PID/maps)
if grep -q libglassagent <<<"$maps"; then
  echo "==> $PROC ($PID) has the dev loader: reloading"
  sh_ am broadcast -a $PKG.RELOAD --es apk "$BASE" >/dev/null
else
  echo "==> attach-agent $PROC ($PID)"
  sh_ cmd activity attach-agent "$PROC" "$AGENT=$BASE"
fi
sleep 3
logs
