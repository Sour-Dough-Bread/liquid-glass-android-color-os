#!/usr/bin/env bash
# Loads the Maps prototype into Google Maps' running main process on the
# userdebug lab AVD, with no LSPosed (adapted from companion/dev_attach.sh;
# the dev loader in devloader/ is a copy of the companion's).
#
#   ANDROID_SERIAL=emulator-5564 ./dev_attach.sh
#
# Differences from the companion's script, all because the AVD is a shared lab
# (captures/2026-11-22-android-auto/lab/README.md):
#  - Only emulator-5564, named explicitly; anything else is refused.
#  - Never restarts zygote: JDWP for every app must already be on
#    (persist.debug.dalvik.vm.jdwp.enabled=1, set by the lab).
#  - Does not restart Maps unless RESTART=1. Maps owns the car window, and the
#    module finds it in the running process (MapsCar.scanExisting), so a
#    rebuild is loaded into the same process: the new build retires the old
#    one through a tag on the car window's root view.
#  - The agent is not pushed to /system/lib64 (that copy is the companion's).
#    Each build's agent goes to /data/local/tmp/sg-auto-maps/ under a name of
#    its own, because an agent library refuses a second attach, and a new
#    path is a new library instance. The AVD is permissive, so an app may map
#    it from there.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APK="${APK:-$HERE/out/stockglass-auto-maps.apk}"
PKG=dev.stockglass.auto.maps
PROC=com.google.android.apps.maps

[ "${ANDROID_SERIAL:-}" = emulator-5564 ] || { echo "set ANDROID_SERIAL=emulator-5564 (the lab AVD); nothing else is a target" >&2; exit 1; }
[ -f "$APK" ] || { echo "no $APK; run ./build.sh" >&2; exit 1; }

sh_() { adb shell "$@" | tr -d '\r'; }

if [ "$(sh_ getprop ro.debuggable)" != "1" ]; then
  echo "refusing: $(sh_ getprop ro.build.fingerprint) is not a debuggable build" >&2
  exit 1
fi
if [ "$(sh_ getprop persist.debug.dalvik.vm.jdwp.enabled)" != "1" ]; then
  echo "JDWP for all apps is off; this script will not restart zygote on a shared lab" >&2
  exit 1
fi

echo "==> install $(basename "$APK")"
adb install -r "$APK" >/dev/null
BASE=$(sh_ pm path "$PKG" | sed -n 's/^package://p' | head -1)
ABI=$(sh_ getprop ro.product.cpu.abi)
LIB="$HERE/out/lib/$ABI/libglassagent.so"
[ -f "$LIB" ] || { echo "no agent built for $ABI" >&2; exit 1; }
AGENT=/data/local/tmp/sg-auto-maps/agent-$(sha256sum "$APK" | cut -c1-12).so
sh_ mkdir -p /data/local/tmp/sg-auto-maps
adb push "$LIB" "$AGENT" >/dev/null
sh_ chmod 755 /data/local/tmp/sg-auto-maps
sh_ chmod 644 "$AGENT"

if [ "${RESTART:-0}" = 1 ]; then
  echo "==> restarting $PROC"
  sh_ "kill \$(pidof $PROC)" || true
  for _ in $(seq 30); do sleep 1; [ -n "$(sh_ pidof "$PROC" || true)" ] && break; done
  sleep 6
fi

PID=$(sh_ pidof "$PROC" || true)
[ -n "$PID" ] || { echo "$PROC is not running" >&2; exit 1; }
jdwp=$(timeout 3 adb jdwp 2>/dev/null | tr -d "\r" || true)
if ! grep -qx "$PID" <<<"$jdwp"; then
  echo "$PROC ($PID) is not debuggable; attaching would crash it" >&2
  exit 1
fi

echo "==> attach-agent $PROC ($PID) $AGENT"
since=$(sh_ "date '+%m-%d %H:%M:%S.000'")
sh_ cmd activity attach-agent "$PROC" "$AGENT=$BASE"
sleep 3
adb logcat -d -T "$since" -s StockGlassDev:V StockGlassAuto:V AndroidRuntime:E | grep -v '^-----' || true
