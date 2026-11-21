#!/system/bin/sh
# live.sh: Stock Glass without a module mount ("live mode"), for this boot only.
#
#   sh live.sh check     say what live mode needs and whether this device has
#                        it. Changes nothing.
#   sh live.sh on        load the engine into SurfaceFlinger now
#   sh live.sh off       put the ROM's own SurfaceFlinger back
#   sh live.sh status    on, off, idle, failed, mounted or switching, and why
#   sh live.sh mount     prepare and mount without restarting SurfaceFlinger,
#                        for a script that runs before it starts (post-fs-data)
#
#   on and off restart SurfaceFlinger, and init restarts the whole framework
#   with it: the screen goes black for a few seconds and every app starts
#   again. Add --force to `on` to go ahead when SELinux cannot be asked
#   whether the engine's rules are in place.
#
# The module's way in is an overlay of /system/bin/surfaceflinger that the root
# manager mounts at boot. Some roots never mount it: temporary root (KernelSU
# or APatch loaded after boot, an exploit), KernelSU 3 without a metamodule, a
# metamodule that broke. Live mode does the same job by hand, after boot:
#
#   1. A tmpfs at /dev/stockglass gets a copy of the ROM's surfaceflinger with
#      one more DT_NEEDED entry, the engine's absolute path in that tmpfs, plus
#      the engine and its symbol cache, labelled like the overlay's copies.
#   2. The copy is bind-mounted over /system/bin/surfaceflinger in init's mount
#      namespace, the one init starts SurfaceFlinger in.
#   3. SurfaceFlinger is restarted and watched until it is up with the engine.
#      If it does not come up, the mount comes off before init's next try, so
#      that try is the ROM's own compositor.
#
# Nothing survives a reboot: the tmpfs, the mounts, rules added to the live
# SELinux policy and sys.* properties all go, so a reboot always brings back
# the stock system. Live mode writes no file outside /dev and no persistent
# property, with one exception that copies the engine's crash guard: when
# SurfaceFlinger dies drawing glass while live mode is reverting, it sets
# persist.sys.glass.mode=stock, as the engine would have on its next start.
#
# Exit status: 0 done or nothing to do, 1 refused or failed (the device is as
# it was), 2 usage.

MODDIR=${0%/*}
[ "$MODDIR" = "$0" ] && MODDIR=.
MODDIR=$(cd "$MODDIR" 2>/dev/null && pwd) || { echo "live: cannot find my own directory" >&2; exit 1; }
SELF="$MODDIR/${0##*/}"

SF=/system/bin/surfaceflinger
LIVE=/dev/stockglass                  # the tmpfs
LIB=$LIVE/libglassengine.so           # the DT_NEEDED entry the copy carries
CACHE=$LIVE/libglassengine.symcache   # where the engine looks: its .so path, .symcache
COPY=$LIVE/surfaceflinger
LOG=/dev/stockglass.log               # outside the tmpfs, so `off` keeps it
LOCK=/dev/stockglass.lock
WATCH=/dev/stockglass.watch           # pid of the watcher that `on` leaves behind
P=persist.sys.glass
SFD=u:r:surfaceflinger:s0

# toybox rather than whatever a root manager put first in PATH: busybox stat
# may lack %C, and these commands were tested with toybox.
if [ -x /system/bin/toybox ]; then TB=/system/bin/toybox; else TB=; fi
tb() { if [ -n "$TB" ]; then "$TB" "$@"; else "$@"; fi; }

ABI=$(cat "$MODDIR/abi" 2>/dev/null)
[ -n "$ABI" ] || case "$(getprop ro.product.cpu.abi)" in
  arm64-v8a) ABI=arm64-v8a ;;
  x86_64)    ABI=x86_64 ;;
esac
ENGINE="$MODDIR/libs/$ABI/libglassengine.so"
PATCHELF="$MODDIR/bin/$ABI/patchelf"
CHECK="$MODDIR/bin/$ABI/glasscheck"

# ------------------------------------------------------------------ output

now() { date +%H:%M:%S; }
log() { { echo "$(now) $*" >> "$LOG"; } 2>/dev/null; }
say() { echo "$*"; log "$*"; }
warn() { echo "live: $*" >&2; log "warning: $*"; }
die() {
  echo "live: $*" >&2
  log "refused: $*"
  release_lock
  exit 1
}

# ------------------------------------------------------------ small facts

sf_pid() { pidof surfaceflinger 2>/dev/null | cut -d' ' -f1; }
# The engine a process has mapped, by path, or nothing.
engine_of() { [ -n "$1" ] && grep -m1 -o '/[^ ]*libglassengine\.so' "/proc/$1/maps" 2>/dev/null; }
devino() { tb stat -Lc '%d:%i' "$1" 2>/dev/null; }
# Is /system/bin/surfaceflinger showing our copy?
bound() { [ -f "$COPY" ] && [ "$(devino "$SF")" = "$(devino "$COPY")" ]; }
tmpfs_line() { grep " $LIVE " /proc/self/mountinfo 2>/dev/null | head -n 1; }
label() { tb stat -c %C "$1" 2>/dev/null; }
ns_of() { readlink "/proc/$1/ns/mnt" 2>/dev/null; }
my_context() { tr -d '\0' < /proc/self/attr/current 2>/dev/null; }
enforcing() { [ "$(cat /sys/fs/selinux/enforce 2>/dev/null)" = 1 ]; }

# What the loaded SELinux policy decides, asked through selinuxfs's access
# file (libselinux's security_compute_av), so no policy tool is needed to know.
#   sel_allows <source> <target> <class> <perm>
# 0 allowed (or the source domain is permissive), 1 denied, 2 cannot tell.
sel_allows() {
  _c=$(cat "/sys/fs/selinux/class/$3/index" 2>/dev/null) || return 2
  _b=$(cat "/sys/fs/selinux/class/$3/perms/$4" 2>/dev/null) || return 2
  # The answer has no trailing newline, so read reports end of file (1) even
  # though it filled the variables: test what it read, not its status.
  _r=$( { exec 3<>/sys/fs/selinux/access &&
          printf '%s %s %s' "$1" "$2" "$_c" >&3 &&
          { read -r _av _x _aa _ad _sq _fl <&3; [ -n "$_fl" ] && echo "$_av $_fl"; }; } 2>/dev/null )
  [ -n "$_r" ] || return 2
  _av=${_r% *} _fl=${_r#* }
  [ $(( 0x$_fl & 1 )) = 1 ] && return 0
  [ $(( (0x$_av >> (_b - 1)) & 1 )) = 1 ] && return 0
  return 1
}

# The two rules the engine needs and stock policy does not give, from the
# module's sepolicy.rule: frida-gum allocates executable trampolines
# (execmem) and re-protects the patched text page of surfaceflinger itself
# (execmod on surfaceflinger_exec). Prints "none", the missing permissions,
# or "unknown" when SELinux cannot be asked.
missing_rules() {
  _m="" _u=0
  sel_allows $SFD $SFD process execmem
  case $? in 1) _m="$_m execmem" ;; 2) _u=1 ;; esac
  sel_allows $SFD u:object_r:surfaceflinger_exec:s0 file execmod
  case $? in 1) _m="$_m execmod" ;; 2) _u=1 ;; esac
  if [ -n "$_m" ]; then echo "${_m# }"; elif [ $_u = 1 ]; then echo unknown; else echo none; fi
}

# What the mechanism itself needs. Stock policy grants all of it (AOSP
# private/file.te and private/domain.te), so a denial means a ROM whose policy
# differs, and live mode does not try to patch around that.
#   mechanism_denied [label of the tmpfs root]
mechanism_denied() {
  _d="" _root=${1:-u:object_r:tmpfs:s0}
  sel_allows u:object_r:surfaceflinger_exec:s0 u:object_r:tmpfs:s0 filesystem associate
  [ $? = 1 ] && _d="$_d surfaceflinger_exec-on-tmpfs"
  sel_allows u:object_r:system_lib_file:s0 u:object_r:tmpfs:s0 filesystem associate
  [ $? = 1 ] && _d="$_d system_lib_file-on-tmpfs"
  sel_allows $SFD "$_root" dir search
  [ $? = 1 ] && _d="$_d search-$_root"
  sel_allows $SFD u:object_r:device:s0 dir search
  [ $? = 1 ] && _d="$_d search-dev"
  sel_allows $SFD u:object_r:system_lib_file:s0 file execute
  [ $? = 1 ] && _d="$_d execute-system_lib_file"
  echo "${_d# }"
}

# May this shell restart a service? init checks ctl.<service> first, then
# ctl.restart$<service> (init/property_service.cpp).
can_restart() {
  _me=$(my_context)
  [ -n "$_me" ] || return 2
  sel_allows "$_me" u:object_r:ctl_default_prop:s0 property_service set && return 0
  sel_allows "$_me" u:object_r:ctl_restart_prop:s0 property_service set
}

RULES='allow surfaceflinger surfaceflinger process execmem
allow surfaceflinger surfaceflinger_exec file execmod
allow surfaceflinger system_prop property_service set
allow surfaceflinger property_socket sock_file write
allow surfaceflinger init unix_stream_socket connectto'

# Each root manager has its own way to add rules to the live policy, and each
# takes the module's sepolicy.rule as it is. KernelSU and KernelSU-Next: ksud
# sepolicy apply. APatch: apd sepolicy --live --apply. Magisk: magiskpolicy.
find_policy_tool() {
  POLICY_TOOL=""
  if [ -x /data/adb/ksud ]; then POLICY_TOOL="/data/adb/ksud sepolicy apply"; return 0; fi
  if [ -x /data/adb/apd ]; then POLICY_TOOL="/data/adb/apd sepolicy --live --apply"; return 0; fi
  for _t in "$(command -v magiskpolicy 2>/dev/null)" /data/adb/magisk/magiskpolicy \
            /debug_ramdisk/magiskpolicy /sbin/magiskpolicy /data/adb/ap/bin/magiskpolicy; do
    if [ -n "$_t" ] && [ -x "$_t" ]; then POLICY_TOOL="$_t --live --apply"; return 0; fi
  done
  return 1
}

# --------------------------------------------------------------- the lock

HAVE_LOCK=
take_lock() {
  if ! mkdir "$LOCK" 2>/dev/null; then
    _o=$(cat "$LOCK/pid" 2>/dev/null)
    if [ -n "$_o" ] && kill -0 "$_o" 2>/dev/null; then
      die "another live switch is still running (pid $_o); wait for it to finish, or read $LOG"
    fi
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null || die "cannot create $LOCK"
  fi
  echo $$ > "$LOCK/pid"
  HAVE_LOCK=1
}
release_lock() {
  [ "$HAVE_LOCK" = 1 ] && rm -rf "$LOCK"
  HAVE_LOCK=
}

# ---------------------------------------------------- init's mount namespace

# The bind mount has to exist where init looks when it starts the new
# SurfaceFlinger: init's own mount namespace. A root shell may run in a private
# one (KernelSU and Magisk can give each app's su its own), and a mount made
# there is invisible to init. In init's namespace / is a shared mount
# (init/mount_namespace.cpp), so the mount also reaches every namespace that
# init's was cloned into.
enter_init_ns() {
  _init=$(ns_of 1)
  [ -n "$_init" ] || die "cannot read /proc/1/ns/mnt, so init's mount namespace is out of reach: this root is too confined for live mode (SELinux context $(my_context))"
  [ "$(ns_of self)" = "$_init" ] && return 0
  [ "$GLASS_LIVE_NS" = 1 ] && die "entered init's mount namespace but this shell is still outside it"
  for _n in nsenter "/system/bin/toybox nsenter" "/data/adb/ksu/bin/busybox nsenter" \
            "/data/adb/ap/bin/busybox nsenter" "/data/adb/magisk/busybox nsenter"; do
    # shellcheck disable=SC2086  # word splitting of the candidate is wanted
    if $_n -t 1 -m true 2>/dev/null; then
      export GLASS_LIVE_NS=1
      # shellcheck disable=SC2086
      exec $_n -t 1 -m /system/bin/sh "$SELF" "$@"
    fi
  done
  die "this shell runs in a mount namespace of its own and nothing here (nsenter) can leave it. Run it from a root shell in the global namespace: su -M -c 'sh $SELF $*' (KernelSU and Magisk), or adb shell su"
}

# -------------------------------------------------------------- preflight

need_root() {
  [ "$(id -u)" = 0 ] || die "needs root: su -c 'sh $SELF $*'"
}

need_files() {
  [ -n "$ABI" ] || die "$(getprop ro.product.cpu.abi) is not supported: Stock Glass needs arm64-v8a or x86_64"
  for _f in "$ENGINE" "$PATCHELF" "$CHECK"; do
    [ -f "$_f" ] || die "missing $_f: run live.sh from an unpacked Stock Glass module that has $ABI in it"
  done
  for _f in "$PATCHELF" "$CHECK"; do
    [ -x "$_f" ] || chmod 0755 "$_f" 2>/dev/null
  done
  "$PATCHELF" --version >/dev/null 2>&1 ||
    die "cannot run $PATCHELF. If $MODDIR is on shared storage (mounted noexec), unpack the module to /data/local/tmp/stockglass and run it from there"
}

# Refuses when live mode is pointless or would stack on something else.
preflight() {
  _p=$(sf_pid)
  _e=$(engine_of "$_p")
  case "$_e" in
    "") ;;
    "$LIB")
      if [ "$(getprop sys.glass.live)" = idle ]; then
        die "SurfaceFlinger still runs the engine from an earlier try, which did not hook ($(getprop sys.glass.live.why)). sh $SELF off restarts it without; fix the cause before trying again"
      fi
      die "already on: SurfaceFlinger runs with the engine from $LIVE. sh $SELF status says more; sh $SELF off takes it out" ;;
    *) die "the engine is already loaded, from $_e: the module's overlay is mounted, so live mode is not needed" ;;
  esac
  [ -f "$SF" ] || die "$SF not found"
  if ! bound && "$PATCHELF" --print-needed "$SF" 2>/dev/null | grep -q 'libglassengine'; then
    die "$SF already carries the engine (the module's overlay, or a development install), but SurfaceFlinger has not loaded it. Restart it or reboot instead of using live mode"
  fi
  [ "$(getprop $P.enabled)" = 0 ] &&
    die "$P.enabled=0 tells the engine not to start. Clear it first: setprop $P.enabled ''"
  # init reboots into the bootloader when a critical service crashes more than
  # four times in four minutes (init/service.cpp), and a start the engine
  # breaks costs two crashes before the revert. AOSP's surfaceflinger is not
  # critical; a ROM that makes it so is not worth that risk.
  if grep -qsE '^[[:space:]]*critical([[:space:]]|$)' /system/etc/init/surfaceflinger.rc; then
    die "this ROM marks surfaceflinger critical, so a start that fails twice could reboot into the bootloader; live mode stays off here"
  fi
  return 0
}

# ------------------------------------------------------------ the tmpfs

undo_prepare() {
  bound && return 1
  if [ -n "$(tmpfs_line)" ]; then
    tb umount "$LIVE" 2>/dev/null || tb umount -l "$LIVE" 2>/dev/null
  fi
  rmdir "$LIVE" 2>/dev/null
  return 0
}

prep_fail() {
  undo_prepare
  die "$@"
}

prepare() {
  # Leftovers of an earlier run (a `mount` SurfaceFlinger never started from,
  # a switch that did not finish). preflight made sure SurfaceFlinger does not
  # run from them, so they can go.
  if bound; then unbind || die "cannot take an earlier copy off $SF"; fi
  undo_prepare
  mkdir -p "$LIVE" || die "cannot create $LIVE"
  if [ -z "$(tmpfs_line)" ]; then
    # Neither nosuid nor noexec. init enters SurfaceFlinger's SELinux domain
    # when it executes the copy, and from a nosuid mount the kernel allows that
    # transition only with process2 nosuid_transition, which stock policy does
    # not give init. /dev itself is nosuid, so the copy cannot sit in /dev.
    tb mount -t tmpfs -o mode=0755,nodev,size=64m stockglass "$LIVE" ||
      prep_fail "cannot mount a tmpfs at $LIVE"
  fi
  case "$(tmpfs_line)" in
    *nosuid*|*noexec*) prep_fail "the tmpfs at $LIVE came up nosuid or noexec; SurfaceFlinger could not start from it" ;;
  esac
  rm -f "$LIB" "$CACHE" "$COPY"

  cp "$ENGINE" "$LIB" && chmod 0644 "$LIB" && chown 0:0 "$LIB" &&
    tb chcon u:object_r:system_lib_file:s0 "$LIB" || prep_fail "cannot place the engine in $LIVE"

  # The copy: the ROM's binary plus one DT_NEEDED entry naming the engine by
  # absolute path. Bionic opens a needed name that has a slash in it as a path
  # (linker.cpp, open_library), and SurfaceFlinger's linker namespace, which is
  # isolated, does not check a library on tmpfs against its permitted paths
  # (linker.cpp, load_library: "do not check accessibility ... on tmpfs").
  cp "$SF" "$COPY" || prep_fail "cannot copy $SF"
  "$PATCHELF" --add-needed "$LIB" "$COPY" >>"$LOG" 2>&1 ||
    prep_fail "patchelf could not add the engine to the copy of $SF"
  _n=$("$PATCHELF" --print-needed "$COPY" 2>/dev/null | grep -c 'libglassengine')
  [ "$_n" = 1 ] || prep_fail "expected one libglassengine entry in the copy, found $_n"
  "$PATCHELF" --print-needed "$COPY" 2>/dev/null | grep -qx "$LIB" ||
    prep_fail "the copy does not name $LIB"
  chown "$(tb stat -c '%u:%g' "$SF")" "$COPY" && chmod 0755 "$COPY" ||
    prep_fail "cannot set the copy's owner and mode"
  _ctx=$(label "$SF")
  [ -n "$_ctx" ] || _ctx=u:object_r:surfaceflinger_exec:s0
  tb chcon "$_ctx" "$COPY" || prep_fail "cannot label the copy $_ctx"

  # The resolver the engine runs, against the very file SurfaceFlinger will
  # execute, which also writes the install-time symbol cache beside the engine.
  if ! "$CHECK" --cache-out "$CACHE" "$COPY" > "$LIVE/glasscheck.log" 2>&1; then
    sed 's/^/    /' "$LIVE/glasscheck.log" >&2
    cat "$LIVE/glasscheck.log" >> "$LOG"
    prep_fail "this ROM's SurfaceFlinger does not expose what the engine needs (glasscheck above); nothing was changed"
  fi
  cat "$LIVE/glasscheck.log" >> "$LOG"
  if [ -f "$CACHE" ]; then
    chmod 0644 "$CACHE" && chown 0:0 "$CACHE" &&
      tb chcon u:object_r:system_lib_file:s0 "$CACHE" || rm -f "$CACHE"
  fi
  [ -f "$CACHE" ] || log "no symbol cache; the engine resolves symbols when SurfaceFlinger starts"

  [ "$(label "$LIB")" = u:object_r:system_lib_file:s0 ] && [ "$(label "$COPY")" = "$_ctx" ] ||
    prep_fail "the copies in $LIVE did not keep their labels ($(label "$LIB"), $(label "$COPY"))"
}

# ---------------------------------------------------------------- SELinux

# Read-only: can the engine's rules be in place? Leaves POLICY saying how,
# NEED_TOOL=1 when a tool has to add them, or FAIL saying why not.
policy_plan() {
  NEED_TOOL=0
  if ! enforcing; then POLICY="SELinux is permissive"; return 0; fi
  _m=$(missing_rules)
  case "$_m" in
    none) POLICY="the engine's rules are already in the policy"; return 0 ;;
  esac
  if find_policy_tool; then NEED_TOOL=1; return 0; fi
  if [ "$_m" = unknown ] && [ "$FORCE" = 1 ]; then
    POLICY="SELinux could not be asked; going ahead (--force)"
    return 0
  fi
  if [ "$_m" = unknown ]; then
    FAIL="SELinux is enforcing, this shell cannot ask it whether SurfaceFlinger may patch code, and no tool to add the rules was found (ksud, apd, magiskpolicy). on --force tries anyway: if the rules are missing, the engine refuses to hook and live mode takes itself out again"
  else
    FAIL="SELinux is enforcing and does not let SurfaceFlinger use $_m, which the engine needs to hook. A root manager adds that live (ksud sepolicy, apd sepolicy, magiskpolicy --live), and none was found. Use a root that has one, or install the module so its sepolicy.rule is applied at boot. Live mode does not switch SELinux to permissive for you"
  fi
  return 1
}

# Adds the rules when policy_plan said a tool has to. Rules stay in the
# loaded policy until the next reboot; there is no taking them out.
ensure_policy() {
  policy_plan || return 1
  [ "$NEED_TOOL" = 1 ] || return 0
  if [ -f "$MODDIR/sepolicy.rule" ]; then
    cp "$MODDIR/sepolicy.rule" "$LIVE/sepolicy.rule"
  else
    echo "$RULES" > "$LIVE/sepolicy.rule"
  fi
  log "adding the engine's rules: $POLICY_TOOL $LIVE/sepolicy.rule"
  # shellcheck disable=SC2086
  if ! $POLICY_TOOL "$LIVE/sepolicy.rule" >>"$LOG" 2>&1; then
    FAIL="${POLICY_TOOL%% *} failed to add the engine's SELinux rules (see $LOG)"
    return 1
  fi
  _m=$(missing_rules)
  case "$_m" in
    none)    POLICY="added the engine's rules with ${POLICY_TOOL%% *}"; return 0 ;;
    unknown) POLICY="added the engine's rules with ${POLICY_TOOL%% *} (not verified)"; return 0 ;;
  esac
  FAIL="${POLICY_TOOL%% *} ran, but SurfaceFlinger still may not use $_m"
  return 1
}

# ------------------------------------------------------------- switching

unbind() {
  # Lazy, because SurfaceFlinger keeps the file it runs from busy. A lazy
  # unmount still propagates to every namespace the mount reached.
  _i=0
  while bound && [ $_i -lt 3 ]; do
    tb umount -l "$SF" 2>/dev/null
    _i=$((_i + 1))
  done
  ! bound
}

drop_tmpfs() {
  bound && return 1
  [ -n "$(tmpfs_line)" ] && tb umount -l "$LIVE" 2>/dev/null
  rmdir "$LIVE" 2>/dev/null
  return 0
}

# Waits up to $1 seconds for a SurfaceFlinger without our engine that stays
# up for 3 seconds.
wait_stock() {
  _t=0
  while [ $_t -lt $(($1 * 2)) ]; do
    _p=$(sf_pid)
    if [ -n "$_p" ] && [ "$(engine_of "$_p")" != "$LIB" ]; then
      sleep 3
      [ "$(sf_pid)" = "$_p" ] && return 0
    fi
    sleep 0.5
    _t=$((_t + 1))
  done
  return 1
}

# Why a start failed, from what the dynamic linker or a crash left in the
# log, as ": <reason>", or nothing.
start_failure() {
  _l=$(logcat -d -b main,crash ${SWITCH_AT:+-T "$SWITCH_AT"} 2>/dev/null |
       grep -E 'CANNOT LINK EXECUTABLE "?/system/bin/surfaceflinger|Fatal signal.*\(surfaceflinger\)' |
       tail -n 1 | sed 's/^.*\(CANNOT LINK EXECUTABLE\)/\1/; s/^.*\(Fatal signal\)/\1/' | cut -c1-120)
  [ -n "$_l" ] && echo ": $_l"
}

set_state() {
  setprop sys.glass.live "$1" 2>/dev/null
  setprop sys.glass.live.why "$(echo "$2" | cut -c1-90)" 2>/dev/null
  log "state: $1${2:+ ($2)}"
}

# Puts the ROM's compositor back after a failed or crashing switch. Never
# returns with the copy mounted.
revert() {
  log "reverting: $1"
  unbind || log "could not unmount the copy from $SF"
  # The engine's crash guard: a SurfaceFlinger that died drawing glass leaves
  # sys.glass.active=1, and the next one switches the saved mode to stock. The
  # next one is the ROM's own and will not, so do it here.
  if [ "$(getprop sys.glass.active)" = 1 ]; then
    setprop $P.mode stock
    setprop sys.glass.tripped "$(date +%s)"
    log "SurfaceFlinger died drawing glass: $P.mode=stock, as the engine's crash guard does"
  fi
  setprop sys.glass.active 0
  _p=$(sf_pid)
  if [ -n "$_p" ] && [ "$(engine_of "$_p")" = "$LIB" ]; then
    setprop ctl.restart surfaceflinger
  elif [ -z "$_p" ]; then
    case "$(getprop init.svc.surfaceflinger)" in
      running|restarting) ;;   # init starts it again by itself, from the ROM's file now
      *) setprop ctl.start surfaceflinger ;;
    esac
  fi
  if wait_stock 40; then
    log "the ROM's SurfaceFlinger is back (pid $(sf_pid))"
  else
    log "SurfaceFlinger did not come back within 40 s; a reboot restores everything"
  fi
  drop_tmpfs
  set_state failed "$1"
}

switch_on() {
  _old=$(sf_pid)
  set_state switching ""
  # The engine keeps a boot guard of its own for development installs, with
  # state under /data that SurfaceFlinger's domain may not even look at. This
  # property says someone else owns the guard. Here a reboot is the guard.
  [ "$(getprop sys.glass.module)" = 1 ] || setprop sys.glass.module 1
  setprop sys.glass.engine ""
  # Read by the engine's crash guard at start; this restart is deliberate.
  setprop sys.glass.active 0
  if ! tb mount -o bind "$COPY" "$SF" || ! bound; then
    unbind
    drop_tmpfs
    set_state failed "could not bind-mount the copy over $SF"
    return 1
  fi
  log "copy mounted over $SF; restarting SurfaceFlinger (pid $_old)"
  SWITCH_AT=$(date '+%m-%d %H:%M:%S.000')
  if ! setprop ctl.restart surfaceflinger; then
    unbind
    drop_tmpfs
    set_state failed "init refused to restart SurfaceFlinger"
    return 1
  fi

  # The engine sets sys.glass.engine from its constructor, before
  # SurfaceFlinger's main, so a healthy start gives a verdict within a second
  # or two. A start that fails does not: an engine the linker cannot load
  # takes SurfaceFlinger down before any of its code runs, and init tries
  # again every 5 s. So it is a failure when a second new process appears,
  # when the new one disappears, or when nothing has stayed up for 12 s.
  _new="" _e="" _t=0 _starts=0 _seen=""
  while [ $_t -lt 24 ]; do
    sleep 0.5
    _t=$((_t + 1))
    _p=$(sf_pid)
    if [ -n "$_p" ] && [ "$_p" != "$_old" ] && [ "$_p" != "$_seen" ]; then
      _starts=$((_starts + 1)) _seen=$_p
    fi
    if [ $_starts -ge 2 ]; then
      revert "SurfaceFlinger keeps dying as it starts with the engine$(start_failure)"
      return 1
    fi
    if [ -n "$_seen" ] && [ "$_p" != "$_seen" ]; then
      revert "SurfaceFlinger died as it started with the engine$(start_failure)"
      return 1
    fi
    [ -n "$_seen" ] || continue
    _new=$_seen
    _e=$(getprop sys.glass.engine)
    [ -n "$_e" ] && break
  done
  if [ -z "$_new" ]; then
    revert "SurfaceFlinger did not stay up after the restart$(start_failure)"
    return 1
  fi
  if [ "$(engine_of "$_new")" != "$LIB" ]; then
    revert "SurfaceFlinger started without the engine: init did not see the mount"
    return 1
  fi
  if [ -z "$_e" ]; then revert "the engine gave no verdict within 12 s"; return 1; fi

  # It has to stay up: a crash at start shows as another pid.
  _t=0
  while [ $_t -lt 16 ]; do
    sleep 0.5
    if [ "$(sf_pid)" != "$_new" ]; then
      revert "SurfaceFlinger died within seconds of starting with the engine (tombstone in /data/tombstones)"
      return 1
    fi
    _t=$((_t + 1))
  done

  case "$_e" in
    hooked|glass|no-glass)
      set_state on "$_e"
      log "SurfaceFlinger $_new runs with the engine: $_e"
      return 0 ;;
  esac
  # Loaded, but it will not hook here (logcat says why). It is harmless where
  # it is and will do nothing this boot, so take the mount off: whenever
  # SurfaceFlinger next starts, it is the ROM's own again.
  _why=$(logcat -d -s StockGlass 2>/dev/null | grep -E 'refus|SELinux will not|cannot drive|incomplete|failed' | tail -n 1 | sed 's/^.*StockGlass *: //')
  unbind
  drop_tmpfs
  set_state idle "${_why:-the engine declined to hook ($_e)}"
  return 1
}

# Lives until live mode is switched off or the device reboots, waking every
# 2 s for a getprop and a pidof. SurfaceFlinger restarts by itself only when
# it dies. One death while drawing glass is the crash guard's business (the
# next start draws Android's blur); a second within 90 s means it dies with
# glass off as well, and then the mount comes off so that init's next try is
# the ROM's compositor.
watch_sf() {
  echo $$ > "$WATCH"
  _pid=$1 _last=0
  while sleep 2; do
    [ "$(getprop sys.glass.live)" = on ] || break
    _p=$(sf_pid)
    if [ -z "$_p" ] || [ "$_p" = "$_pid" ]; then continue; fi
    _now=$(date +%s)
    if [ $_last -gt 0 ] && [ $((_now - _last)) -lt 90 ]; then
      # Off may have started meanwhile; it holds the lock and owns the revert.
      mkdir "$LOCK" 2>/dev/null || break
      echo $$ > "$LOCK/pid"
      HAVE_LOCK=1
      revert "SurfaceFlinger restarted twice within 90 s with the engine in it"
      release_lock
      break
    fi
    _last=$_now _pid=$_p
    log "SurfaceFlinger restarted (pid $_p); the mount comes off if it happens again within 90 s"
  done
  rm -f "$WATCH"
}

stop_watch() {
  _w=$(cat "$WATCH" 2>/dev/null)
  [ -n "$_w" ] && kill "$_w" 2>/dev/null
  rm -f "$WATCH"
}

switch_off() {
  set_state switching ""
  stop_watch
  _p=$(sf_pid)
  setprop sys.glass.active 0
  if ! unbind; then
    set_state on "could not unmount the copy from $SF; a reboot removes it"
    return 1
  fi
  if [ "$(engine_of "$_p")" = "$LIB" ]; then
    setprop ctl.restart surfaceflinger
    if ! wait_stock 40; then
      drop_tmpfs
      set_state off "SurfaceFlinger did not settle within 40 s after the restart"
      return 1
    fi
  fi
  drop_tmpfs
  set_state off ""
  return 0
}

# A soft reboot ends every app, and a root shell started from one (a terminal
# app, a manager's WebUI) is that app's child. The switch must outlive that:
# it runs in a session of its own with no terminal, ignores SIGHUP, and leaves
# the app's cgroup. APatch's own soft reboot detaches the same way.
worker() {
  trap '' HUP INT
  HAVE_LOCK=1
  echo $$ > "$LOCK/pid"
  echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null
  case "$1" in
    on)
      if switch_on; then
        release_lock
        watch_sf "$(sf_pid)"
      else
        release_lock
      fi ;;
    off)
      switch_off
      release_lock ;;
  esac
}

spawn() {
  log "switching $1 in the background; this terminal may close"
  # An external command, never the tb function: a function sent to the
  # background runs in a subshell that waits for the worker and keeps this
  # shell's stdout open, so the caller's terminal or `adb shell` never closes.
  if [ -n "$TB" ] && "$TB" setsid true 2>/dev/null; then
    "$TB" setsid /system/bin/sh "$SELF" _worker "$1" </dev/null >>"$LOG" 2>&1 &
  elif command -v setsid >/dev/null 2>&1; then
    setsid /system/bin/sh "$SELF" _worker "$1" </dev/null >>"$LOG" 2>&1 &
  else
    nohup /system/bin/sh "$SELF" _worker "$1" </dev/null >>"$LOG" 2>&1 &
  fi
  echo $! > "$LOCK/pid"
  HAVE_LOCK=   # the worker owns the lock now
}

# Waits for the worker's verdict, prints it, and returns 0 if it is $1.
wait_result() {
  _t=0
  while [ $_t -lt 180 ]; do
    _s=$(getprop sys.glass.live)
    case "$_s" in switching|"") ;; *) break ;; esac
    sleep 0.5
    _t=$((_t + 1))
  done
  status
  [ "$(getprop sys.glass.live)" = "$1" ]
}

# --------------------------------------------------------------- commands

status() {
  _s=$(getprop sys.glass.live)
  # A property holds 91 characters; the log has the whole reason.
  _why=$(grep ' state: ' "$LOG" 2>/dev/null | tail -n 1 | sed -n 's/^[0-9:]* state: [a-z]* (\(.*\))$/\1/p')
  [ -n "$_why" ] || _why=$(getprop sys.glass.live.why)
  _p=$(sf_pid)
  _e=$(engine_of "$_p")
  # Facts first: the property is only as good as the last run that set it.
  if [ "$_e" = "$LIB" ] && [ "$_s" != idle ]; then _s=on; fi
  if [ -n "$_e" ] && [ "$_e" != "$LIB" ]; then
    echo "Live mode: not needed. The engine is loaded through the module's overlay ($_e)."
    return 0
  fi
  case "$_s" in
    on)
      echo "Live mode: on. SurfaceFlinger (pid $_p) runs with the engine from $LIVE: $(getprop sys.glass.engine)."
      case "$(getprop $P.mode)" in
        glass|on) echo "Liquid Glass is on. glassctl stock switches back to Android's blur, live." ;;
        *)        echo "Android's blur is drawn until you switch glass on: glassctl glass (or setprop $P.mode glass)." ;;
      esac
      echo "Undo: sh $SELF off, or reboot." ;;
    idle)
      echo "Live mode: idle. The engine loaded but did not hook: $_why"
      if [ "$_e" = "$LIB" ]; then
        echo "The mount is already off; SurfaceFlinger keeps the idle engine until it next restarts (sh $SELF off restarts it now)."
      fi ;;
    failed)
      echo "Live mode: failed. $_why"
      echo "The ROM's own SurfaceFlinger runs. Details: $LOG" ;;
    mounted)
      echo "Live mode: mounted. The copy is in place and SurfaceFlinger loads it at its next start." ;;
    switching)
      echo "Live mode: switching. SurfaceFlinger is restarting; ask again in a few seconds." ;;
    *)
      echo "Live mode: off. SurfaceFlinger is the ROM's own${_why:+ ($_why)}." ;;
  esac
  return 0
}

cmd_check() {
  _bad=0
  row() {
    printf '  %-4s %-11s %s\n' "$1" "$2" "$3"
    [ "$1" = no ] && _bad=1
    return 0
  }
  echo "What live mode needs on this device:"
  if [ "$(id -u)" != 0 ]; then
    row no root "uid $(id -u): run it through su"
    echo "Live mode cannot work from this shell."
    return 1
  fi
  row ok root "uid 0, $(my_context)"
  if [ -z "$ABI" ]; then
    row no files "$(getprop ro.product.cpu.abi) is not supported (needs arm64-v8a or x86_64)"
  elif [ ! -f "$ENGINE" ] || [ ! -f "$PATCHELF" ] || [ ! -f "$CHECK" ]; then
    row no files "$MODDIR lacks the engine, patchelf or glasscheck for $ABI"
  else
    [ -x "$PATCHELF" ] || chmod 0755 "$PATCHELF" "$CHECK" 2>/dev/null
    if "$PATCHELF" --version >/dev/null 2>&1; then
      row ok files "$MODDIR ($ABI)"
    else
      row no files "cannot run binaries from $MODDIR (noexec storage?): unpack it to /data/local/tmp"
    fi
  fi
  _p=$(sf_pid)
  _e=$(engine_of "$_p")
  _have=""
  case "$_e" in
    "")     if "$PATCHELF" --print-needed "$SF" 2>/dev/null | grep -q libglassengine && ! bound; then
              row no compositor "$SF already carries the engine (overlay or development install)"
            else
              row ok compositor "$SF is the ROM's own (SurfaceFlinger pid $_p)"
            fi ;;
    "$LIB") row ok compositor "live mode is already on"; _have=live ;;
    *)      row ok compositor "the engine is already loaded through the module ($_e)"; _have=module ;;
  esac
  if [ "$(getprop $P.enabled)" = 0 ]; then row no engine "$P.enabled=0 keeps the engine from starting"; fi
  if [ -x "$CHECK" ]; then
    _out=$("$CHECK" "$SF" 2>&1)
    if [ $? = 0 ]; then
      row ok resolver "$(echo "$_out" | grep -m1 '^  hook' | sed 's/^ *hook *//')"
    else
      row no resolver "glasscheck: $(echo "$_out" | grep -E 'MISSING|FAIL' | head -n 2 | tr '\n' ' ')"
    fi
  fi
  row ok namespace "init's mount namespace ($(ns_of 1))"
  _probe=/dev/stockglass.probe
  if mkdir -p "$_probe" && tb mount -t tmpfs -o mode=0700,nodev,size=1m probe "$_probe" 2>/dev/null; then
    tb umount "$_probe" 2>/dev/null
    row ok mounting "a tmpfs can be mounted in /dev"
  else
    row no mounting "cannot mount a tmpfs in /dev from this shell"
  fi
  rmdir "$_probe" 2>/dev/null
  can_restart
  case $? in
    0) row ok restart "this shell may restart SurfaceFlinger (ctl.restart)" ;;
    1) row no restart "SELinux does not let $(my_context) restart services" ;;
    *) row ok restart "not verified (cannot ask SELinux); usually fine for a root manager's su" ;;
  esac
  if ! enforcing; then
    row ok selinux "permissive: nothing to add"
  else
    _d=$(mechanism_denied)
    [ -n "$_d" ] && row no selinux "this ROM's policy refuses what live mode itself needs: $_d"
    _m=$(missing_rules)
    case "$_m" in
      none)    row ok selinux "enforcing; the engine's rules are already in the policy" ;;
      unknown) if find_policy_tool; then
                 row ok selinux "enforcing; cannot ask SELinux, ${POLICY_TOOL%% *} will add the rules"
               else
                 row no selinux "enforcing; cannot ask SELinux and no policy tool (on --force tries anyway)"
               fi ;;
      *)       if find_policy_tool; then
                 row ok selinux "enforcing; missing $_m, which ${POLICY_TOOL%% *} adds until reboot"
               else
                 row no selinux "enforcing; missing $_m and no tool to add it (ksud, apd, magiskpolicy)"
               fi ;;
    esac
  fi
  if [ $_bad = 1 ]; then
    echo "Live mode cannot work here until the 'no' lines are fixed."
    return 1
  fi
  case "$_have" in
    module) echo "Live mode is not needed: the module's overlay already loads the engine." ;;
    live)   echo "Live mode is on: sh $SELF status" ;;
    *)      echo "Live mode can work here: sh $SELF on" ;;
  esac
  return 0
}

cmd_on() {
  take_lock
  need_files
  preflight
  policy_plan || die "$FAIL"
  can_restart
  [ $? = 1 ] && die "SELinux does not let this shell ($(my_context)) restart SurfaceFlinger"
  say "Preparing a copy of SurfaceFlinger with the engine in $LIVE"
  prepare
  _hook=$(grep -m1 '^  hook' "$LIVE/glasscheck.log" | sed 's/^ *hook *//')
  say "  hook: $_hook$([ -f "$CACHE" ] && echo ", symbol cache written")"
  if ! ensure_policy; then
    undo_prepare
    die "$FAIL"
  fi
  say "  SELinux: $POLICY"
  _d=$(enforcing && mechanism_denied "$(label "$LIVE")")
  if [ -n "$_d" ]; then
    undo_prepare
    die "this ROM's SELinux policy refuses what live mode needs: $_d"
  fi
  say "Restarting SurfaceFlinger: the screen goes black for a few seconds and apps restart."
  say "If this terminal closes with them, sh $SELF status tells how it went."
  set_state switching ""
  spawn on
  wait_result on
}

cmd_off() {
  take_lock
  _p=$(sf_pid)
  _e=$(engine_of "$_p")
  if [ -n "$_e" ] && [ "$_e" != "$LIB" ]; then
    die "the engine in SurfaceFlinger comes from the module's overlay ($_e), not from live mode; disable or remove the module instead"
  fi
  if [ "$_e" != "$LIB" ] && ! bound && [ -z "$(tmpfs_line)" ]; then
    stop_watch
    set_state off ""
    release_lock
    echo "Live mode is already off."
    return 0
  fi
  if [ "$_e" = "$LIB" ]; then
    say "Restarting SurfaceFlinger without the engine: the screen goes black for a few seconds and apps restart."
  fi
  set_state switching ""
  spawn off
  wait_result off
}

# Prepare and mount, no restart: for a boot script that runs before init
# starts SurfaceFlinger (post-fs-data). The boot guard is the caller's.
cmd_mount() {
  take_lock
  need_files
  preflight
  if bound; then release_lock; echo "already mounted"; return 0; fi
  policy_plan || die "$FAIL"
  prepare
  if ! ensure_policy; then
    undo_prepare
    die "$FAIL"
  fi
  [ "$(getprop sys.glass.module)" = 1 ] || setprop sys.glass.module 1
  setprop sys.glass.active 0
  if ! tb mount -o bind "$COPY" "$SF" || ! bound; then
    unbind
    undo_prepare
    die "could not bind-mount the copy over $SF"
  fi
  set_state mounted ""
  release_lock
  say "mounted: SurfaceFlinger starts with the engine"
}

# ------------------------------------------------------------------- main

CMD=${1:-status}
[ $# -gt 0 ] && shift
if [ "$CMD" = _worker ]; then
  worker "$1"
  exit 0
fi
FORCE=0
for _a in "$@"; do
  case "$_a" in
    --force) FORCE=1 ;;
    *) echo "live: unknown option $_a" >&2; exit 2 ;;
  esac
done

case "$CMD" in
  on|off|mount|check)
    need_root "$CMD"
    enter_init_ns "$CMD" "$@" ;;
  status) ;;   # reads properties and /proc only, from any namespace
  -h|--help|help) sed -n '2,15p' "$SELF" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) echo "live: unknown command '$CMD' (sh $SELF help)" >&2; exit 2 ;;
esac

case "$CMD" in
  check)   cmd_check ;;
  on)      cmd_on ;;
  off)     cmd_off ;;
  mount)   cmd_mount ;;
  status)  status ;;
esac
