#!/usr/bin/env bash
# gate_matrix.sh: shade region counts for every companion switch. Needs the
# companion attached to SystemUI (dev_attach.sh) and the engine in glass mode.
HERE="$(cd "$(dirname "$0")" && pwd)"; A="adb -s ${ANDROID_SERIAL:-emulator-5564}"
openqs() { $A shell cmd statusbar collapse; sleep 1.5; $A shell cmd statusbar expand-settings; sleep 4; }
row() { printf '%-52s %s\n' "$1" "$($HERE/shade_count.sh)"; }
push() { $A shell service call activity 1599295570 >/dev/null; sleep 2; }
openqs; openqs; row "glass (default), full QS"
$A shell cmd statusbar collapse; sleep 1.5; $A shell cmd statusbar expand-notifications; sleep 4; row "glass (default), notification shade (QQS)"
$A shell setprop persist.sys.glass.mode stock; openqs; row "mode=stock, full QS"
$A shell setprop persist.sys.glass.mode glass; openqs; row "mode=glass again, full QS"
$A shell setprop persist.sys.glass.app.qs 0; openqs; row "app.qs=0, full QS"
$A shell "setprop persist.sys.glass.app.qs ''"; openqs; row "app.qs cleared, full QS"
$A shell setprop sys.glass.engine no-glass; openqs; row "engine reports no-glass, full QS"
$A shell setprop sys.glass.engine hooked; openqs; row "engine reports hooked, full QS"
$A shell "setprop sys.glass.engine ''"; openqs; row "engine report empty (debuggable), full QS"
$A shell setprop persist.sys.glass.mode stock; sleep 1; row "QS held open: mode=stock, setprop only"
push; row "QS held open: + SYSPROPS_TRANSACTION"
$A shell setprop persist.sys.glass.mode glass; push; row "QS held open: mode=glass + SYSPROPS_TRANSACTION"
$A shell cmd statusbar collapse
