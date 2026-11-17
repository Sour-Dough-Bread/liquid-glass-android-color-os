#!/usr/bin/env bash
# region_dump.sh [process]: every live companion region in a process (default
# the launcher) as it stands, without drawing a frame: its alpha, the rect hwui
# last reported for it (what SurfaceFlinger gets), and where its control is
# now ("off" = no part of it on screen). Needs persist.sys.glass.app.debug=1;
# the dump runs on the property change broadcast this sends.
set -u
A="adb -s ${ANDROID_SERIAL:-emulator-5564}"
P="${1:-com.google.android.apps.nexuslauncher}"
pid=$($A shell pidof "$P" | tr -d '\r')
[ -n "$pid" ] || { echo "no $P process" >&2; exit 1; }
[ "$($A shell getprop persist.sys.glass.app.debug | tr -d '\r')" = 1 ] || $A shell setprop persist.sys.glass.app.debug 1
$A logcat -c
$A shell service call activity 1599295570 >/dev/null
sleep 1
$A logcat -d --pid="$pid" -s StockGlassApp | grep -E ' dump' | sed -E 's/^.*StockGlassApp: //'
