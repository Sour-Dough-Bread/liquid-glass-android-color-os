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

# frostColor has to follow the system theme from out here; see glass-theme.sh.
MODDIR=${0%/*}
# Run through sh, not by its exec bit: set_perm_recursive leaves every file in
# the module 0644, so an `-x` test here was false on every real install and the
# theme was never followed.
if [ -f "$MODDIR/glass-theme.sh" ]; then
  sh "$MODDIR/glass-theme.sh" &
fi
