#!/system/bin/sh
# Builds the /system overlay that KernelSU/Magisk mounts. Shared by customize.sh
# (at install) and post-fs-data.sh (refresh when the ROM changes underneath us).
#
#   build-overlay.sh <moddir> <abi> <logfile>
#     0  overlay is present and correct
#     1  could not build one; caller must leave no overlay behind
#
# Run at INSTALL time as well as at boot on purpose. The module arrives in
# modules_update and is swapped into modules/ during the first boot, so an
# overlay created only by post-fs-data.sh does not exist when the module manager
# decides what to mount on that boot. Building it during installation means
# system/ is already there before any boot happens, and post-fs-data.sh is then
# only responsible for noticing an OTA.
MODDIR="$1"
ABI="$2"
LOG="${3:-/dev/null}"
SF=/system/bin/surfaceflinger
log() { echo "$@" >> "$LOG"; }

[ -n "$MODDIR" ] && [ -n "$ABI" ] || exit 1
[ -x "$MODDIR/bin/$ABI/patchelf" ] || { log "no patchelf for $ABI"; exit 1; }
[ -f "$SF" ] || { log "$SF not found"; exit 1; }

# The stamp is the source binary's size and mtime plus the engine's, so an OTA or
# a module update invalidates it and nothing else does. Copying and patching an
# 8 MB binary on every boot would be pure boot-time cost for an unchanged result.
STAMP="$MODDIR/system/.stamp"
WANT="$(stat -c '%s %Y' "$SF" 2>/dev/null) $(stat -c '%Y' "$MODDIR/libs/$ABI/libglassengine.so" 2>/dev/null)"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WANT" ]; then
  log "overlay is current for $SF"
  exit 0
fi

rm -rf "$MODDIR/system"
mkdir -p "$MODDIR/system/bin" "$MODDIR/system/lib64" || exit 1
cp "$MODDIR/libs/$ABI/libglassengine.so" "$MODDIR/system/lib64/libglassengine.so" || exit 1
cp "$SF" "$MODDIR/system/bin/surfaceflinger" || exit 1

# Injection is a DT_NEEDED entry, so the ordinary linker loads the engine at
# process start. No ptrace, no startup race, and it survives every restart of the
# compositor for free.
#
# LD_PRELOAD would need none of this and does not work: init starts
# surfaceflinger with `capabilities SYS_NICE`, so the kernel sets AT_SECURE=1 and
# bionic's linker ignores LD_PRELOAD and strips it from the environment.
# Measured, see docs/VERIFIED.md.
#
# Adding the entry twice is not harmless: each pass grows the binary and leaves a
# duplicate dependency, and it compounds. That happens whenever the source
# already carries it, so check before adding rather than after.
already=$("$MODDIR/bin/$ABI/patchelf" --print-needed "$SF" 2>/dev/null \
          | grep -cx libglassengine.so || true)
if [ "${already:-0}" -gt 0 ]; then
  log "$SF already carries the DT_NEEDED entry; leaving it alone"
elif ! "$MODDIR/bin/$ABI/patchelf" --add-needed libglassengine.so \
        "$MODDIR/system/bin/surfaceflinger" >>"$LOG" 2>&1; then
  log "patchelf failed"
  exit 1
fi

# Verify, and require EXACTLY one. A missing entry boots fine and silently does
# nothing, which is the hardest failure to notice; a duplicate is the signature
# of this having run against its own output.
n=$("$MODDIR/bin/$ABI/patchelf" --print-needed "$MODDIR/system/bin/surfaceflinger" \
    2>/dev/null | grep -cx libglassengine.so || true)
if [ "${n:-0}" -ne 1 ]; then
  log "expected exactly one libglassengine.so DT_NEEDED entry, found ${n:-0}"
  exit 1
fi

chmod 0755 "$MODDIR/system/bin/surfaceflinger"
chmod 0644 "$MODDIR/system/lib64/libglassengine.so"
chown 0:0  "$MODDIR/system/bin/surfaceflinger" "$MODDIR/system/lib64/libglassengine.so"
# The mount preserves the module file's own context, so these have to be right
# here. A wrongly-labelled surfaceflinger is refused execution by SELinux.
chcon u:object_r:surfaceflinger_exec:s0 "$MODDIR/system/bin/surfaceflinger" 2>/dev/null
chcon u:object_r:system_lib_file:s0     "$MODDIR/system/lib64/libglassengine.so" 2>/dev/null

# The install-time symbol cache (engine/src/sf_cache.h). glasscheck resolves the
# patched compositor once, here, and writes what it found next to the engine;
# at every start the engine checks those addresses against the code instead of
# reading the whole binary and inflating its MiniDebugInfo (about 0.3 s before
# SurfaceFlinger's main on the Poco F1). It is optional: with no cache, or one
# that does not match, the engine resolves symbols as it always has. Same
# directory and label as the engine, because SurfaceFlinger may read those.
CHECK="$MODDIR/bin/$ABI/glasscheck"
CACHE="$MODDIR/system/lib64/libglassengine.symcache"
rm -f "$CACHE"
if [ -x "$CHECK" ] && "$CHECK" --cache-out "$CACHE" "$MODDIR/system/bin/surfaceflinger" \
     >>"$LOG" 2>&1 && [ -f "$CACHE" ]; then
  chmod 0644 "$CACHE"
  chown 0:0 "$CACHE"
  chcon u:object_r:system_lib_file:s0 "$CACHE" 2>/dev/null
else
  rm -f "$CACHE"
  log "no symbol cache; the engine resolves symbols when SurfaceFlinger starts"
fi

echo "$WANT" > "$STAMP"
log "overlay built for $SF"
exit 0
