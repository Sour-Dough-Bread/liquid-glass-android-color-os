#!/system/bin/sh
# Builds the overlay that KernelSU/Magisk mounts over /system.
#
# Runs at the post-fs-data trigger, which init fires from `on late-init` BEFORE
# `on boot` — and `class_start core`, which starts surfaceflinger, lives in
# `on boot`. Verified against the device's own init.rc. So everything written
# here is mounted before the compositor is executed.
MODDIR=${0%/*}
STATE=/data/adb/glass
LOG="$STATE/last_boot.log"
SF=/system/bin/surfaceflinger

mkdir -p "$STATE"
: > "$LOG"
# Tells the engine that the guard is handled out here, so it does not keep a
# second marker meaning the same thing. The property is the one it can see: in
# enforcing mode surfaceflinger may not look inside /data/adb at all.
: > "$STATE/module"
setprop sys.glass.module 1
log() { echo "$@" >> "$LOG"; }

# ---------------------------------------------------------------- kill switch
#
# This is the real bootloop guard, and it lives here rather than inside the
# engine on purpose. The engine's own guard cannot catch the failure that
# matters most: if the injected library is unloadable, the dynamic linker
# rejects surfaceflinger *before any of the engine's code runs*, so nothing
# inside the engine is in a position to disarm anything. A marker written here
# and cleared only once the system finishes booting catches that case, because
# it does not depend on our code running at all.
#
# Deliberately does NOT trip on a crash after boot completed: by then the device
# is usable and the user can uninstall normally, whereas disabling on any crash
# would turn an unrelated SurfaceFlinger restart into a silent uninstall.
if [ -f "$STATE/boot_pending" ]; then
  rm -f "$STATE/boot_pending"
  : > "$STATE/disabled"
  log "previous boot did not complete with the module active; disabling"
fi

# Every way out below that must leave the compositor stock does BOTH of these:
# removes our system/ and sets skip_mount. Removing system/ is enough for a
# manager that mounts straight from the module directory (Magisk, KernelSU-Next,
# APatch). It is not enough under a KernelSU 3 metamodule such as
# meta-overlayfs, which mounts a COPY of system/ taken into its own image at
# install time and which this script cannot reach before the mount. Every
# mounter honours skip_mount, and the metamodule's mount runs after all
# post-fs-data scripts, so the flag set here is seen in time.
stand_down() {
  rm -rf "$MODDIR/system"
  : > "$MODDIR/skip_mount"
  log "$@"
  exit 0
}

if [ -f "$STATE/disabled" ]; then
  # Leaving nothing to mount is what makes this safe: the device boots exactly
  # as it would without the module installed.
  stand_down "disabled by $STATE/disabled - remove it and reboot to re-enable"
fi

ABI=$(cat "$MODDIR/abi" 2>/dev/null)
[ -n "$ABI" ] || case "$(getprop ro.product.cpu.abi)" in
  arm64-v8a) ABI=arm64-v8a ;;
  x86_64)    ABI=x86_64 ;;
esac
if [ -z "$ABI" ] || [ ! -x "$MODDIR/bin/$ABI/patchelf" ]; then
  stand_down "no usable ABI ($ABI); standing down"
fi

# Under a metamodule the overlay that gets mounted is the copy taken at install,
# so after an OTA it would be a patched copy of the OLD compositor over the new
# system. Rebuilding below cannot fix that copy. Compare against what the
# module was installed over, and if the ROM's compositor has changed, mount
# nothing and ask for a reinstall, which takes a fresh copy.
#
# Nothing is mounted yet at this point, so $SF here is always the ROM's own
# file. An update installed over a running copy of the module may leave no
# stamp (customize.sh could only see its own overlay then); the first boot
# after it records the real one.
if [ -e /data/adb/metamodule ] && [ ! -f "$MODDIR/sf.stamp" ]; then
  stat -c '%s %Y' "$SF" > "$MODDIR/sf.stamp" 2>/dev/null
  log "recorded $SF as the compositor the overlay is for"
fi
if [ -e /data/adb/metamodule ] && [ -f "$MODDIR/sf.stamp" ] \
   && [ "$(stat -c '%s %Y' "$SF" 2>/dev/null)" != "$(cat "$MODDIR/sf.stamp")" ]; then
  stand_down "$SF changed since install (an OTA?); reinstall the module to rebuild its overlay"
fi

# The overlay is normally built at install time; this only has to notice an OTA
# replacing the compositor underneath us, in which case the stamp no longer
# matches and it is rebuilt here.
if ! sh "$MODDIR/build-overlay.sh" "$MODDIR" "$ABI" "$LOG"; then
  stand_down "no overlay; booting stock"
fi
# Mounting is wanted this boot; clear a skip_mount left by an earlier stand-down.
rm -f "$MODDIR/skip_mount"

# KernelSU 3.0 and later no longer mount modules themselves; a metamodule
# (meta-overlayfs is the reference one) does. Without one the overlay above is
# built and never mounted, the engine never loads, and the device boots exactly
# as stock — which is safe, but looks like the module silently doing nothing.
if [ -x /data/adb/ksud ] && grep -q metamodule /data/adb/ksud 2>/dev/null \
   && [ ! -e /data/adb/metamodule ]; then
  log "KernelSU without a metamodule: the overlay will not be mounted."
  log "Install one (e.g. meta-overlayfs) and reboot."
fi

: > "$STATE/boot_pending"
