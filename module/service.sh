#!/system/bin/sh
# Disarms the bootloop guard once the system is actually up.
#
# If the boot never completes, boot_pending survives and post-fs-data.sh
# disables the module next time. That is the whole mechanism: nothing here has
# to detect a failure, it only has to fail to run.
STATE=/data/adb/glass
i=0
while [ "$(getprop sys.boot_completed)" != "1" ]; do
  i=$((i + 1))
  [ "$i" -gt 240 ] && exit 0     # ~4 min; leave it armed rather than guess
  sleep 1
done
rm -f "$STATE/boot_pending"
MODDIR=${0%/*}

# The boot completed, but that says nothing about a screen anyone can use.
# watchdog.sh watches SurfaceFlinger and System UI for three minutes and takes
# glass (or, if even Android's blur keeps crashing, the module) off when they
# keep restarting. On the first boot after an install or update with Liquid
# Glass already on, a new engine is drawing for the first time: glass stays only
# if Volume Up is pressed within 90 s (customize.sh says so), the way it does
# when glass is switched on by hand.
WD="$MODDIR/watchdog.sh"
if [ -f "$WD" ]; then
  confirm_boot=0
  if [ -f "$STATE/confirm_boot" ]; then
    rm -f "$STATE/confirm_boot"
    case "$(getprop persist.sys.glass.mode)" in
      glass|on) [ "$(getprop persist.sys.glass.confirm)" != 0 ] && confirm_boot=1 ;;
    esac
  fi
  if [ "$confirm_boot" = 1 ]; then
    sh "$WD" confirm 90 </dev/null >/dev/null 2>&1 &
  else
    sh "$WD" health 180 </dev/null >/dev/null 2>&1 &
  fi
fi

# frostColor has to follow the system theme from out here; see glass-theme.sh.
# Run through sh, not by its exec bit: set_perm_recursive leaves every file in
# the module 0644, so an `-x` test here was false on every real install and the
# theme was never followed.
if [ -f "$MODDIR/glass-theme.sh" ]; then
  sh "$MODDIR/glass-theme.sh" &
fi
