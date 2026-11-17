#!/usr/bin/env bash
# Loads the companion into a running app on the userdebug test AVD, with no
# LSPosed: the APK's JVMTI agent stands in for the Xposed framework (see
# devloader/agent.cpp).
#
#   ANDROID_SERIAL=emulator-5564 ./dev_attach.sh [systemui|launcher]
#
# Steps, and why:
#  1. Refuse anything but a debuggable build. The phone runs a user build and
#     is never a target of this script.
#  2. Let every app be debugged: persist.debug.dalvik.vm.jdwp.enabled=1. Zygote
#     reads it once at start, so the first run restarts zygote (a soft reboot
#     of the framework; SurfaceFlinger and the engine keep running).
#  3. Install out/stockglass-companion.apk, and push the agent to
#     /system/lib64 (the AVD runs with -writable-system, like the engine's dev
#     install). Not the APK's own lib dir: ART splits the agent argument at the
#     first '=', and PackageManager's install paths always contain "==". In
#     /system/lib64 it is system_lib_file, which app domains may map
#     executable, so SELinux stays enforcing.
#  4. Restart the target, so it loads this build and starts with no hooks.
#  5. cmd activity attach-agent <process> /system/lib64/libglassagent.so=<base.apk>.
#     Attaching to a process that is not debuggable makes it throw and crash,
#     so the script first checks the process is listed by `adb jdwp`.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# APK=... picks another build of the module (tools/devtest_plugins.sh makes one).
APK="${APK:-$HERE/out/stockglass-companion.apk}"
PKG=dev.stockglass.companion
TARGET="${1:-systemui}"

if [ -z "${ANDROID_SERIAL:-}" ] && [ "$(adb devices | grep -c 'device$')" -gt 1 ]; then
  echo "more than one device attached; set ANDROID_SERIAL to pick one:" >&2
  adb devices | sed -n '2,$p' | sed 's/^/    /' >&2
  exit 1
fi
[ -f "$APK" ] || { echo "no $APK; run ./build.sh" >&2; exit 1; }

sh_() { adb shell "$@" | tr -d '\r'; }

if [ "$(sh_ getprop ro.debuggable)" != "1" ]; then
  echo "refusing: $(sh_ getprop ro.build.fingerprint) is not a debuggable build" >&2
  exit 1
fi
adb root >/dev/null 2>&1 || true
adb wait-for-device

wait_framework() {
  # sys.boot_completed survives a zygote restart; wait for a focused window
  # and the target's process instead.
  # Capture, then test: `... | grep -q` exits early, SIGPIPEs adb, and
  # pipefail reports the match as a failure (as in engine/install_dev.sh).
  for _ in $(seq 120); do
    focus=$(sh_ dumpsys window 2>/dev/null || true)
    if grep -q 'mCurrentFocus=Window' <<<"$focus" \
       && [ -n "$(sh_ pidof com.android.systemui || true)" ]; then
      sleep 3; return 0
    fi
    sleep 1
  done
  echo "framework did not come back" >&2; exit 1
}

if [ "$(sh_ getprop persist.debug.dalvik.vm.jdwp.enabled)" != "1" ]; then
  echo "==> enabling JDWP for all apps (restarts zygote once)"
  sh_ setprop persist.debug.dalvik.vm.jdwp.enabled 1
  sh_ setprop ctl.restart zygote
  sleep 3
  wait_framework
fi

echo "==> install $(basename "$APK")"
adb install -r "$APK" >/dev/null
BASE=$(sh_ pm path "$PKG" | sed -n 's/^package://p' | head -1)
ABI=$(sh_ getprop ro.product.cpu.abi)
[ -f "$HERE/out/lib/$ABI/libglassagent.so" ] || { echo "no agent built for $ABI" >&2; exit 1; }
AGENT=/system/lib64/libglassagent.so
adb remount >/dev/null 2>&1 || { echo "cannot remount /system (start the AVD with -writable-system)" >&2; exit 1; }
adb push "$HERE/out/lib/$ABI/libglassagent.so" "$AGENT" >/dev/null
sh_ "chmod 644 $AGENT && chcon u:object_r:system_lib_file:s0 $AGENT"

case "$TARGET" in
  systemui)
    PROC=com.android.systemui
    echo "==> restarting SystemUI"
    sh_ "kill \$(pidof $PROC)" || true
    sleep 2
    wait_framework
    ;;
  launcher)
    PROC=$(sh_ cmd package resolve-activity --brief -a android.intent.action.MAIN \
             -c android.intent.category.HOME | tail -1 | cut -d/ -f1)
    echo "==> restarting $PROC"
    sh_ am force-stop "$PROC"
    sh_ am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null
    for _ in $(seq 30); do [ -n "$(sh_ pidof "$PROC")" ] && break; sleep 1; done
    sleep 4
    ;;
  *)
    PROC="$TARGET"
    ;;
esac

PID=$(sh_ pidof "$PROC")
[ -n "$PID" ] || { echo "$PROC is not running" >&2; exit 1; }
jdwp=$(timeout 3 adb jdwp 2>/dev/null | tr -d "\r" || true)
if ! grep -qx "$PID" <<<"$jdwp"; then
  echo "$PROC ($PID) is not debuggable; attaching would crash it" >&2
  exit 1
fi

echo "==> attach-agent $PROC ($PID)"
adb logcat -c
sh_ cmd activity attach-agent "$PROC" "$AGENT=$BASE"
sleep 2

if [ "$TARGET" = systemui ]; then
  # The hooks only see compositions that run after the attach, and SystemUI
  # composes Quick Quick Settings at start. A night-mode round trip makes it
  # compose every tile again. (Under LSPosed the hook is in place before the
  # first composition, so this is a dev-loader-only step.)
  night=$(sh_ cmd uimode night | sed -n 's/^Night mode: //p')
  case "$night" in yes) flip=no ;; *) flip=yes ;; esac
  echo "==> recomposing Quick Settings (night mode $flip, then back to $night)"
  sh_ cmd uimode night "$flip" >/dev/null
  sleep 3
  sh_ cmd uimode night "${night:-no}" >/dev/null
  sleep 2
fi
if [ "$TARGET" = launcher ]; then
  # Likewise the home screen was drawn before the attach, and an idle view
  # keeps the display list it recorded then, so an icon or folder reaches the
  # hooks only when something redraws it. Turning "show layout bounds" on and
  # off makes every window invalidate every view (ViewRootImpl's
  # MSG_INVALIDATE_WORLD, after SYSPROPS_TRANSACTION). Dev-loader-only too.
  was=$(sh_ getprop debug.layout)
  echo "==> redrawing every view (layout bounds on, then back to ${was:-false})"
  sh_ setprop debug.layout true
  sh_ service call activity 1599295570 >/dev/null
  sleep 1
  sh_ setprop debug.layout "${was:-false}"
  sh_ service call activity 1599295570 >/dev/null
  sleep 2
fi
adb logcat -d -s StockGlassDev:V StockGlassApp:V AndroidRuntime:E | grep -v '^-----' || true
