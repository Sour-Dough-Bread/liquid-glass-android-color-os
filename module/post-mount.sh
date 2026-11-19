#!/system/bin/sh
# Records whether the overlay is really mounted this boot.
#
# KernelSU (with or without a metamodule), KernelSU-Next and APatch run this
# after they have mounted modules, still inside the post-fs-data stage, so
# before init starts SurfaceFlinger (`class_start core` lives in `on boot`).
# Magisk has no such stage and never runs it.
#
# The boot guard in post-fs-data.sh reads the result on the NEXT boot. A boot
# that did not complete is held against the module only if the patched
# compositor could have run in it. When this script saw the ROM's own
# compositor, nothing of ours ran, and the failure was something else: on a
# Poco F1 the GPU driver timed out (kgsl "Switch to unsecure failed to idle"),
# SurfaceFlinger aborted on every start, and the guard then switched Stock Glass
# off although its overlay had not even been mounted that boot.
#
# Only the absence is recorded, as a file whose existence is the whole message,
# and synced. A hard reset is exactly how such a boot ends, and an unsynced
# file can come back missing or zero-filled. A lost marker means "mounted, or
# unknown", which keeps the guard as strict as before.
STATE=/data/adb/glass
mkdir -p "$STATE"
if [ -f /system/lib64/libglassengine.so ]; then
  rm -f "$STATE/overlay_absent"
  echo "post-mount: overlay mounted" >> "$STATE/last_boot.log"
else
  : > "$STATE/overlay_absent"
  echo "post-mount: overlay NOT mounted; SurfaceFlinger runs the ROM's own build this boot" \
    >> "$STATE/last_boot.log"
fi
sync
