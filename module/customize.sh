#!/system/bin/sh
# Runs once, while the module is being installed.
#
# The job here is to REFUSE loudly rather than install something that will
# bootloop. The engine drives SurfaceFlinger's own Skia through symbols resolved
# out of its MiniDebugInfo, so whether it can work on this ROM is a question with
# a definite answer, and glasscheck answers it before anything is written.
SKIPUNZIP=0

ui_print ""
ui_print "  Stock Glass"
ui_print "  An optional Liquid Glass mode for Android's blur"
ui_print ""

case "$ARCH" in
  arm64) ABI=arm64-v8a ;;
  x64)   ABI=x86_64 ;;
  *)     ui_print "! $ARCH is not supported (need a 64-bit device)"
         abort   "! aborting" ;;
esac
ui_print "- Architecture: $ARCH -> $ABI"

SF=/system/bin/surfaceflinger
[ -f "$SF" ] || abort "! $SF not found; this is not a device Stock Glass can patch"

CHECK="$MODPATH/bin/$ABI/glasscheck"
chmod 755 "$CHECK" 2>/dev/null
ui_print "- Checking this ROM's Skia..."
if ! "$CHECK" "$SF" >"$MODPATH/glasscheck.log" 2>&1; then
  while IFS= read -r line; do ui_print "  $line"; done < "$MODPATH/glasscheck.log"
  ui_print ""
  ui_print "! This ROM's SurfaceFlinger does not expose what the engine needs."
  ui_print "! Nothing has been installed and the device is untouched."
  abort   "! aborting"
fi
while IFS= read -r line; do ui_print "  $line"; done < "$MODPATH/glasscheck.log"

# Keep only the architecture this device uses.
for d in "$MODPATH/bin" "$MODPATH/libs"; do
  for a in "$d"/*; do
    [ -d "$a" ] && [ "$(basename "$a")" != "$ABI" ] && rm -rf "$a"
  done
done
echo "$ABI" > "$MODPATH/abi"

# Build the overlay now rather than on first boot. The module is swapped in from
# modules_update during that boot, so an overlay created only by post-fs-data.sh
# may not exist when the module manager decides what to mount.
ui_print "- Preparing the patched compositor..."
# The managers' unzip does not keep exec bits, and set_perm below runs only
# after the overlay is built, so make patchelf runnable before it is needed.
chmod 755 "$MODPATH/bin/$ABI/patchelf" 2>/dev/null
if ! sh "$MODPATH/build-overlay.sh" "$MODPATH" "$ABI" "$MODPATH/overlay.log"; then
  while IFS= read -r line; do ui_print "  $line"; done < "$MODPATH/overlay.log"
  abort "! could not prepare the overlay; aborting"
fi
ui_print "  $(sed -n '$p' "$MODPATH/overlay.log")"
# What the overlay was built over. Under a KernelSU 3 metamodule the mounted
# overlay is a copy taken now, so post-fs-data.sh compares the ROM's compositor
# against this at every boot and mounts nothing if an OTA has replaced it.
#
# It has to describe the ROM's own binary. When this is an update over a copy
# of the module that is running, what is visible at $SF is our own patched
# overlay, not the ROM's file, and its size and time can never match the real
# one seen at boot (before anything is mounted): 0.4.2, installed over a running
# 0.4.1, stood itself down on its first boot for exactly that reason. The
# installed copy's stamp was taken over the real file, so carry it over. With no
# stamp to carry, leave none: post-fs-data.sh records the real one at the next
# boot.
OLD_STAMP=/data/adb/modules/stockglass/sf.stamp
if "$MODPATH/bin/$ABI/patchelf" --print-needed "$SF" 2>/dev/null | grep -qx libglassengine.so; then
  if [ -f "$OLD_STAMP" ] && [ "$(cat "$OLD_STAMP")" != "$(stat -c '%s %Y' "$SF")" ]; then
    cp "$OLD_STAMP" "$MODPATH/sf.stamp"
    ui_print "- Updating over a running copy; kept the stamp of the ROM's compositor"
  else
    rm -f "$MODPATH/sf.stamp"
    ui_print "- Updating over a running copy; the ROM's compositor is stamped next boot"
  fi
else
  stat -c '%s %Y' "$SF" > "$MODPATH/sf.stamp"
fi
rm -f "$MODPATH/skip_mount"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/$ABI/patchelf"   0 0 0755
set_perm "$MODPATH/bin/$ABI/glasscheck" 0 0 0755
set_perm "$MODPATH/glassctl"            0 0 0755
set_perm "$MODPATH/action.sh"           0 0 0755
# set_perm_recursive above flattened these; the mount preserves them verbatim.
[ -f "$MODPATH/system/bin/surfaceflinger" ] && {
  set_perm "$MODPATH/system/bin/surfaceflinger" 0 0 0755
  chcon u:object_r:surfaceflinger_exec:s0 "$MODPATH/system/bin/surfaceflinger" 2>/dev/null
  chcon u:object_r:system_lib_file:s0 "$MODPATH/system/lib64/libglassengine.so" 2>/dev/null
}

# An update must not rewrite files the running system has mapped. Right after
# this script, meta-overlayfs (KernelSU 3's usual metamodule) copies system/
# with `cp -af` into the directory the live /system overlay is mounted from. cp
# rewrites an existing file in place, and truncating a file throws away every
# process's private pages of it, even the ones the dynamic linker wrote
# relocations into. SurfaceFlinger maps libglassengine.so, so its next call
# through the PLT jumped to an unrelocated address and it died: 0.4.9 over a
# running 0.4.8 on alioth (docs/VERIFIED.md, "Updating over a running copy").
# Giving each live path a new file by rename leaves the mapped one alone until
# the reboot, and the metamodule's cp then rewrites a file nothing maps.
same_label() {
  # busybox chcon has --reference, toybox chcon does not; toybox stat has %C.
  chcon --reference="$1" "$2" 2>/dev/null && return 0
  _ctx=$(stat -c %C "$1" 2>/dev/null) && [ -n "$_ctx" ] && chcon "$_ctx" "$2" 2>/dev/null
}
LIVE="/data/adb/metamodule/mnt/${MODID:-stockglass}/system"
if [ -d "$LIVE" ] && [ -d "$MODPATH/system" ]; then
  swapped=0
  for f in $(cd "$MODPATH/system" && find . -type f); do
    f=${f#./}
    dst="$LIVE/$f"
    [ -f "$dst" ] || continue
    tmp="${dst%/*}/.stockglass-new"
    if cp -p "$MODPATH/system/$f" "$tmp" && same_label "$MODPATH/system/$f" "$tmp" \
       && mv -f "$tmp" "$dst"; then
      swapped=$((swapped + 1))
    else
      rm -f "$tmp"
      ui_print "! Could not swap /system/$f for a new file; the copy that"
      ui_print "! follows may crash the compositor (it comes back without glass)."
    fi
  done
  [ "$swapped" -gt 0 ] && \
    ui_print "- Swapped $swapped running file(s) for new ones; the running copies stay in use until the reboot"
fi

# Installing again is how a stand-down is retried (under a metamodule it is
# also the only way to give it a fresh copy of the overlay), so it clears the
# boot guard's verdict. The guard itself stays armed for the next boot.
if [ -f /data/adb/glass/disabled ]; then
  rm -f /data/adb/glass/disabled /data/adb/glass/boot_pending
  ui_print "- Cleared the earlier stand-down after a failed boot; the guard"
  ui_print "  still protects the next one"
fi

ui_print ""
ui_print "- Installed. Reboot to apply."
. "$MODPATH/meta.sh" 2>/dev/null && case "$(mount_state)" in
  none)
    ui_print ""
    ui_print "! KernelSU 3+ mounts modules only through a metamodule, and"
    ui_print "! none is installed. Without one this module is installed but"
    ui_print "! never mounted, and the engine will not load. Install a"
    ui_print "! metamodule such as meta-overlayfs, then reboot." ;;
  broken)
    ui_print ""
    ui_print "! The metamodule ($META_NAME) is installed, but KernelSU will"
    ui_print "! not run it: $MOUNT_WHY."
    ui_print "! Until it is reinstalled, no module is mounted, this one"
    ui_print "! included. Reinstall it from the manager, then reboot." ;;
  off)
    ui_print ""
    ui_print "! The metamodule ($META_NAME) is $MOUNT_WHY,"
    ui_print "! so no module is mounted. Enable it, then reboot." ;;
esac
ui_print "- If a boot ever fails with this installed, the module disables"
ui_print "  itself on the next boot rather than looping."
ui_print ""
ui_print "- Liquid Glass is opt-in. Until you switch it on, Android's"
ui_print "  own blur is drawn exactly as before."
ui_print "- After the reboot, switch it on with any of:"
ui_print "    the module's Action button"
ui_print "    the module's WebUI (KernelSU / APatch): presets + tuning"
ui_print "    sh /data/adb/modules/stockglass/glassctl glass"
ui_print "  Switching is live; no reboot after the first one."
ui_print "- Panes (notification cards, the volume panel, dialogs) get the"
ui_print "  glass. The shade keeps Android's blur or gets a toned one:"
ui_print "    sh /data/adb/modules/stockglass/glassctl backdrop stock|tone|glass"
case "$(getprop persist.sys.glass.mode)" in
  glass|on) ui_print "- Liquid Glass was already switched on here; keeping it." ;;
esac
ui_print ""
