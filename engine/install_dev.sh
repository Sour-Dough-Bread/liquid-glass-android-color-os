#!/usr/bin/env bash
# Installs libglassengine.so into a *development* device (emulator with
# `adb root` + `-writable-system`). NOT the production path — the KernelSU
# module will magic-mount instead of overwriting /system.
#
# Injection is `patchelf --add-needed` on the surfaceflinger binary rather than
# ptrace: the normal linker then loads us at process start, which means no
# ptrace SELinux rules, no startup race, and automatic re-injection whenever
# SurfaceFlinger restarts.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PATCHELF="$HERE/vendor/patchelf/bin/patchelf"
WORK="${TMPDIR:-/tmp}/glass-install"
LIB=libglassengine.so

# With more than one device attached every adb call below fails with an
# unhelpful "more than one device/emulator" halfway through the install, which
# on a script that swaps out /system/bin/surfaceflinger is a bad time to find
# out. Say so up front instead.
if [ -z "${ANDROID_SERIAL:-}" ] && [ "$(adb devices | grep -c 'device$')" -gt 1 ]; then
  echo "more than one device attached; set ANDROID_SERIAL to pick one:" >&2
  adb devices | sed -n '2,$p' | sed 's/^/    /' >&2
  exit 1
fi

abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
case "$abi" in
  arm64-v8a|x86_64) ;;
  *) echo "unsupported abi: $abi" >&2; exit 1 ;;
esac
SO="$HERE/out/$abi/$LIB"
[ -f "$SO" ] || { echo "not built for $abi — run ./build.sh" >&2; exit 1; }

adb root >/dev/null 2>&1 || true; sleep 2; adb wait-for-device
adb remount >/dev/null 2>&1 || true

rm -rf "$WORK"; mkdir -p "$WORK"

# Keep a pristine copy on-device. Recovery is a single mv, which matters when
# the thing you just broke is the compositor.
if ! adb shell '[ -f /system/bin/surfaceflinger.orig ]'; then
  adb shell cp /system/bin/surfaceflinger /system/bin/surfaceflinger.orig
  echo "backed up -> /system/bin/surfaceflinger.orig"
fi

adb pull /system/bin/surfaceflinger.orig "$WORK/sf" >/dev/null
cp "$WORK/sf" "$WORK/sf.patched"
"$PATCHELF" --add-needed "$LIB" "$WORK/sf.patched"
# Capture first, then test: `... | grep -q` exits early, SIGPIPEs patchelf, and
# `pipefail` reports that as a failed pipeline even though it succeeded.
needed=$("$PATCHELF" --print-needed "$WORK/sf.patched")
grep -qx "$LIB" <<<"$needed" \
  || { echo "patchelf did not add the DT_NEEDED entry" >&2; exit 1; }
echo "added DT_NEEDED $LIB ($(stat -c%s "$WORK/sf.patched") bytes)"

adb push "$SO" "/system/lib64/$LIB" >/dev/null
adb shell "chmod 644 /system/lib64/$LIB && chcon u:object_r:system_lib_file:s0 /system/lib64/$LIB"

# Cannot overwrite a running executable's inode (ETXTBSY), but replacing the
# directory entry is fine — the live process keeps the old inode until restart.
adb push "$WORK/sf.patched" /system/bin/surfaceflinger.new >/dev/null
adb shell 'chmod 755 /system/bin/surfaceflinger.new &&
           chcon u:object_r:surfaceflinger_exec:s0 /system/bin/surfaceflinger.new &&
           mv /system/bin/surfaceflinger.new /system/bin/surfaceflinger'

echo "installed for $abi"
adb shell 'ls -Z /system/bin/surfaceflinger /system/lib64/libglassengine.so'
