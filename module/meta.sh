# Sourced by customize.sh, post-fs-data.sh and glassctl: will the root manager
# mount this module's overlay at all?
#
# Magisk, APatch, KernelSU-Next and KernelSU before 3.0 mount modules
# themselves. KernelSU 3.0 and later mount nothing: a metamodule does, and
# KernelSU only runs one that /data/adb/metamodule points at, whose module.prop
# still says metamodule=1, and that is not disabled or marked for removal.
#
# That module.prop is a weak point. Some metamodules rewrite it with `sed -i`
# at every boot to show a status line, and sed's rename is not synced: a hard
# reset soon after leaves a file of the right size full of zero bytes. KernelSU
# then no longer sees a metamodule, mounts nothing from any module, and says
# nothing about it (a Poco F1 with meta-overlayfsx, 2026-11-18). Once zeroed,
# the next boot's sed finds no line to change and writes the zeros back.
#
# mount_state prints one word and leaves the details in MOUNT_WHY and META_NAME:
#   self    the manager mounts modules itself
#   none    KernelSU 3+ with no metamodule installed
#   off     a metamodule is installed but disabled or marked for removal
#   broken  a metamodule is installed but KernelSU will not run it
#   ok      a metamodule KernelSU will run
mount_state() {
  MOUNT_WHY="" META_NAME=""
  if [ ! -x /data/adb/ksud ] || ! grep -q metamodule /data/adb/ksud 2>/dev/null; then
    echo self
    return
  fi
  if [ ! -e /data/adb/metamodule ]; then
    MOUNT_WHY="no metamodule is installed"
    echo none
    return
  fi
  _meta=$(readlink -f /data/adb/metamodule 2>/dev/null)
  [ -n "$_meta" ] || _meta=/data/adb/metamodule
  _prop="$_meta/module.prop"
  META_NAME=$(sed -n 's/^name=//p' "$_prop" 2>/dev/null | head -n 1)
  [ -n "$META_NAME" ] || META_NAME=${_meta##*/}
  if [ ! -s "$_prop" ]; then
    MOUNT_WHY="its module.prop is missing or empty"
    echo broken
    return
  fi
  # tr drops the NULs; a file that was nothing but NULs leaves nothing behind.
  if [ "$(tr -d '\000' < "$_prop" | wc -c)" -eq 0 ]; then
    MOUNT_WHY="its module.prop is all zero bytes, as a hard reset leaves it when it was being rewritten"
    echo broken
    return
  fi
  if ! grep -qiE '^metamodule=(1|true)' "$_prop"; then
    MOUNT_WHY="its module.prop no longer says metamodule=1"
    echo broken
    return
  fi
  if [ -e "$_meta/disable" ] || [ -e "$_meta/remove" ]; then
    MOUNT_WHY="it is disabled or marked for removal in the manager"
    echo off
    return
  fi
  echo ok
}
