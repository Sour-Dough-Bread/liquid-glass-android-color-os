#!/usr/bin/env bash
# Host test for the boot guard (post-fs-data.sh + post-mount.sh) and the
# metamodule check (meta.sh). No device: each scenario runs the real scripts
# under busybox ash with /data/adb, /system/lib64 and the property tools
# redirected into a scratch tree.
#
#   module/tools/guard_test.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SH="busybox ash"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail=0

# A copy of each script with its absolute paths pointed into $root.
stage() {
  local root=$1
  mkdir -p "$root/adb/modules/stockglass" "$root/system/lib64" "$root/bin"
  for f in post-fs-data.sh post-mount.sh meta.sh; do
    sed -e "s#/data/adb#$root/adb#g" -e "s#/system/lib64#$root/system/lib64#g" \
        -e "s#/system/bin/surfaceflinger#$root/system/bin/surfaceflinger#g" \
        "$HERE/$f" > "$root/adb/modules/stockglass/$f"
  done
  # Property tools and sync are not the host's business.
  printf '#!/bin/sh\nexit 0\n' > "$root/bin/setprop"
  printf '#!/bin/sh\necho ""\n' > "$root/bin/getprop"
  printf '#!/bin/sh\nexit 0\n' > "$root/bin/sync"
  chmod +x "$root/bin/"*
}

run() {  # root script
  PATH="$1/bin:$PATH" $SH "$1/adb/modules/stockglass/$2" >/dev/null 2>&1 || true
}

check() {  # name condition...
  local name=$1; shift
  if "$@"; then echo "  ok    $name"; else echo "  FAIL  $name"; fail=1; fi
}

echo "boot guard"
# The module dir has no abi and no patchelf, so post-fs-data.sh stands down
# right after the guard ("no usable ABI"); that is far enough to see the verdict.
r="$WORK/g1"; stage "$r"; mkdir -p "$r/adb/glass"
: > "$r/adb/glass/boot_pending"; : > "$r/adb/glass/overlay_absent"
run "$r" post-fs-data.sh
check "failed boot without our overlay: not disabled" test ! -e "$r/adb/glass/disabled"
check "  ...and says why" grep -q "overlay was not mounted" "$r/adb/glass/last_boot.log"
check "  ...and the marker is consumed" test ! -e "$r/adb/glass/overlay_absent"

r="$WORK/g2"; stage "$r"; mkdir -p "$r/adb/glass"
: > "$r/adb/glass/boot_pending"
run "$r" post-fs-data.sh
check "failed boot with the overlay mounted (or unknown): disabled" test -e "$r/adb/glass/disabled"

r="$WORK/g3"; stage "$r"; mkdir -p "$r/adb/glass"
run "$r" post-fs-data.sh
check "a clean boot: not disabled" test ! -e "$r/adb/glass/disabled"

r="$WORK/g4"; stage "$r"; mkdir -p "$r/adb/glass"
: > "$r/adb/glass/overlay_absent"   # left by a boot that completed
run "$r" post-fs-data.sh             # the next boot, which is clean
check "every boot consumes an earlier boot's marker" test ! -e "$r/adb/glass/overlay_absent"

echo "post-mount"
r="$WORK/p1"; stage "$r"; mkdir -p "$r/adb/glass"
run "$r" post-mount.sh
check "no overlay: records overlay_absent" test -e "$r/adb/glass/overlay_absent"
r="$WORK/p2"; stage "$r"; mkdir -p "$r/adb/glass"; : > "$r/system/lib64/libglassengine.so"
: > "$r/adb/glass/overlay_absent"
run "$r" post-mount.sh
check "overlay mounted: no overlay_absent" test ! -e "$r/adb/glass/overlay_absent"

echo "mount_state"
state() {  # root -> word and why
  PATH="$1/bin:$PATH" $SH -c ". '$1/adb/modules/stockglass/meta.sh'; w=\$(mount_state); mount_state >/dev/null; echo \"\$w|\$MOUNT_WHY|\$META_NAME\""
}
ksu3() {  # root: a ksud that knows about metamodules, plus a metamodule
  printf 'ksud ... metamodule ...' > "$1/adb/ksud"; chmod +x "$1/adb/ksud"
  mkdir -p "$1/adb/modules/meta-x"
  ln -sfn "$1/adb/modules/meta-x" "$1/adb/metamodule"
  printf 'id=meta-x\nmetamodule=1\nname=Meta X\nversion=1\n' > "$1/adb/modules/meta-x/module.prop"
}
r="$WORK/m1"; stage "$r"
check "no ksud: self" test "$(state "$r" | cut -d'|' -f1)" = self
r="$WORK/m2"; stage "$r"; printf 'ksud metamodule' > "$r/adb/ksud"; chmod +x "$r/adb/ksud"
check "KernelSU 3+ without one: none" test "$(state "$r" | cut -d'|' -f1)" = none
r="$WORK/m3"; stage "$r"; ksu3 "$r"
check "a working metamodule: ok" test "$(state "$r")" = "ok||Meta X"
r="$WORK/m4"; stage "$r"; ksu3 "$r"; head -c 343 /dev/zero > "$r/adb/modules/meta-x/module.prop"
s=$(state "$r")
check "zero-filled module.prop: broken" test "${s%%|*}" = broken
check "  ...says zero bytes" grep -q "zero bytes" <<< "$s"
check "  ...names it by its directory" test "${s##*|}" = meta-x
r="$WORK/m5"; stage "$r"; ksu3 "$r"; printf 'id=meta-x\nname=Meta X\n' > "$r/adb/modules/meta-x/module.prop"
check "no metamodule=1 line: broken" test "$(state "$r" | cut -d'|' -f1)" = broken
r="$WORK/m6"; stage "$r"; ksu3 "$r"; : > "$r/adb/modules/meta-x/disable"
check "disabled in the manager: off" test "$(state "$r" | cut -d'|' -f1)" = off
r="$WORK/m7"; stage "$r"; ksu3 "$r"; : > "$r/adb/modules/meta-x/module.prop"
check "empty module.prop: broken" test "$(state "$r" | cut -d'|' -f1)" = broken

echo "post-fs-data logs a broken metamodule"
r="$WORK/l1"; stage "$r"; ksu3 "$r"; head -c 100 /dev/zero > "$r/adb/modules/meta-x/module.prop"
mkdir -p "$r/adb/glass"; run "$r" post-fs-data.sh
check "last_boot.log names the problem" grep -q "will not run it" "$r/adb/glass/last_boot.log"

[ "$fail" = 0 ] && echo PASS || { echo FAIL; exit 1; }
