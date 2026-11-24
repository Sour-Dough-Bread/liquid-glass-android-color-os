#!/system/bin/sh
# The watchdog: Liquid Glass goes back to Android's own blur by itself when
# nobody can say it works, and the module turns itself off when even Android's
# blur does not.
#
#   watchdog.sh confirm [seconds]  glass was just switched on. Keep it only if
#                                  Volume Up is pressed, or `glassctl keep` runs
#                                  (the WebUI's Keep button), within the time;
#                                  otherwise switch back to Android's blur. Then
#                                  watch the UI for a while (health).
#   watchdog.sh health [seconds]   watch SurfaceFlinger and System UI. Either
#                                  restarting over and over with glass on means
#                                  glass goes off; SurfaceFlinger still
#                                  restarting with glass off, while the engine
#                                  is in it, means the module turns itself off
#                                  and the phone reboots without it.
#   watchdog.sh keep               answer a pending confirm: keep the glass
#   watchdog.sh cancel             drop a pending confirm (glass switched off)
#   watchdog.sh pending            seconds left on a pending confirm, or nothing
#
# Why each layer, and what is already covered elsewhere:
#   - a boot that never completes: post-fs-data.sh's boot guard disables the
#     module on the next boot;
#   - SurfaceFlinger crashing while it draws glass: the engine's crash guard
#     (engine/src/crash_guard.h) brings it back stock, once;
#   - a boot that completes into a screen nobody can use (glass drawn wrong,
#     black or garbled, a System UI that keeps dying): nothing could tell, so
#     glass is kept only when someone confirms it, the way a display settings
#     change is. A broken screen cannot confirm, so it reverts.
#
# The timeout is persist.sys.glass.confirm, in seconds (default 20; 0 turns the
# question off). Everything here fails towards Android's blur: no answer, no
# key events, no notification service, no clock — each one means revert.
MODDIR=${0%/*}
[ "$MODDIR" = "$0" ] && MODDIR=.
P=persist.sys.glass
STATE=${GLASS_STATE:-/data/adb/glass}
LOG="$STATE/watchdog.log"
TOKEN="$STATE/confirm"
DEFAULT_CONFIRM=20

mkdir -p "$STATE" 2>/dev/null

log() {
  echo "$(date '+%m-%d %H:%M:%S' 2>/dev/null) $*" >> "$LOG"
  # A log that could grow every boot, kept short.
  if [ "$(wc -l < "$LOG" 2>/dev/null)" -gt 400 ] 2>/dev/null; then
    tail -n 200 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
  fi
}

# One notification, replaced each time (same tag). A device without the
# notification shell command still gets the log and the property.
notify() {
  cmd notification post -S bigtext -t "Stock Glass" stockglass "$1" >/dev/null 2>&1
}

now() { date +%s 2>/dev/null || echo 0; }

mode() {
  case "$(getprop $P.mode)" in
    glass|on) [ "$(getprop $P.bypass)" = "1" ] && echo stock || echo glass ;;
    *)        echo stock ;;
  esac
}

# Back to Android's blur, live: the engine reads the mode within half a
# second, and the companion's Gate switches its surfaces off with it.
revert() {
  setprop $P.mode stock
  setprop sys.glass.reverted "$(now) $1"
  log "reverted to Android's blur: $1"
  notify "Switched back to Android's blur: $1. Turn Liquid Glass on again from the module's Action button, its WebUI or 'glassctl glass'."
}

confirm_secs() {
  s=${1:-$(getprop $P.confirm)}
  case "$s" in
    ''|*[!0-9]*) s=$DEFAULT_CONFIRM ;;
  esac
  echo "$s"
}

# The pending confirm belongs to the process named in the token; a newer
# `glassctl glass` takes it over, and keep/cancel remove it.
still_mine() { [ "$(cut -d' ' -f1 "$TOKEN" 2>/dev/null)" = "$$" ]; }

clear_token() {
  rm -f "$TOKEN"
  setprop sys.glass.confirm ""
}

# Volume Up pressed within $1 seconds. getevent prints one line per input
# event (-l: with names); the first key-down of Volume Up ends the wait. With
# no getevent or no timeout, this is never true, and the glass reverts.
volume_up_within() {
  command -v getevent >/dev/null 2>&1 || return 1
  command -v timeout >/dev/null 2>&1 || return 1
  timeout "$1" getevent -lq 2>/dev/null | grep -q 'KEY_VOLUMEUP.*DOWN'
}

pid_of() { pidof "$1" 2>/dev/null | cut -d' ' -f1; }

# Whether the engine is inside SurfaceFlinger from the module's own mount (not
# live mode, which restarts SurfaceFlinger on purpose and has its own checks).
engine_mounted() {
  _p=$(pid_of surfaceflinger)
  [ -n "$_p" ] && grep -q /system/lib64/libglassengine.so "${GLASS_PROC:-/proc}/$_p/maps" 2>/dev/null
}

# Turn the module off for the next boot and reboot into it: the only way to
# take the engine out of a SurfaceFlinger that keeps dying with glass off.
disable_and_reboot() {
  [ -f "$STATE/disabled" ] && return
  echo "watchdog: $1" > "$STATE/disabled"
  log "disabling the module and rebooting: $1"
  notify "Stock Glass turned itself off ($1) and is rebooting the phone without it."
  sync
  sleep 3
  # By path: module scripts run in busybox's standalone shell, where a bare
  # `svc` is busybox's runit tool and `reboot` its own. Android's svc asks
  # PowerManager for an orderly reboot; the others are fallbacks.
  /system/bin/svc power reboot stockglass 2>/dev/null ||
    /system/bin/reboot stockglass 2>/dev/null ||
    setprop sys.powerctl reboot,stockglass
}

# ------------------------------------------------------------------- health
health() {
  window=${1:-180}
  t=0
  sf_prev=$(pid_of surfaceflinger)
  ui_prev=$(pid_of com.android.systemui)
  sf_n=0 ui_n=0 sf_stock_n=0
  trip_prev=$(getprop sys.glass.tripped)
  log "health: watching SurfaceFlinger and System UI for ${window} s"
  while [ "$t" -lt "$window" ]; do
    sleep 2
    t=$((t + 2))
    sf=$(pid_of surfaceflinger)
    ui=$(pid_of com.android.systemui)
    # Live mode restarts SurfaceFlinger on purpose: not a crash.
    if [ -n "$(getprop sys.glass.live)" ]; then
      sf_prev=$sf
    elif [ -n "$sf" ] && [ -n "$sf_prev" ] && [ "$sf" != "$sf_prev" ]; then
      trip=$(getprop sys.glass.tripped)
      if [ -n "$trip" ] && [ "$trip" != "$trip_prev" ]; then
        # It died drawing glass, and the engine's crash guard has already
        # brought the new one up stock: glass is off; nothing more to do.
        log "health: SurfaceFlinger restarted ($sf_prev -> $sf) drawing glass; the crash guard switched it off"
        setprop sys.glass.reverted "$(now) SurfaceFlinger crashed drawing Liquid Glass"
        notify "SurfaceFlinger crashed while drawing Liquid Glass, so Android's blur is back. Turn it on again from the Action button, the WebUI or 'glassctl glass'."
        trip_prev=$trip
      elif [ "$(mode)" = glass ]; then
        sf_n=$((sf_n + 1))
        log "health: SurfaceFlinger restarted ($sf_prev -> $sf) with glass on"
      else
        sf_stock_n=$((sf_stock_n + 1))
        log "health: SurfaceFlinger restarted ($sf_prev -> $sf) with Android's blur"
      fi
    fi
    [ -n "$sf" ] && sf_prev=$sf
    if [ -n "$ui" ] && [ -n "$ui_prev" ] && [ "$ui" != "$ui_prev" ]; then
      [ "$(mode)" = glass ] && ui_n=$((ui_n + 1))
      log "health: System UI restarted ($ui_prev -> $ui), mode $(mode)"
    fi
    [ -n "$ui" ] && ui_prev=$ui

    # Two restarts with glass on: a crash the engine's guard could not see
    # (outside its draw), so glass goes off from out here.
    if [ "$(mode)" = glass ] && [ "$sf_n" -ge 2 ]; then
      revert "SurfaceFlinger restarted $sf_n times with Liquid Glass on"
      sf_n=0 ui_n=0 t=0   # and watch Android's blur for a full window
    elif [ "$(mode)" = glass ] && [ "$ui_n" -ge 3 ]; then
      revert "System UI restarted $ui_n times with Liquid Glass on"
      sf_n=0 ui_n=0 t=0
    elif [ "$sf_stock_n" -ge 3 ] && engine_mounted; then
      disable_and_reboot "SurfaceFlinger restarted $sf_stock_n times with Android's blur"
      return
    fi
  done
  log "health: steady"
}

# ------------------------------------------------------------------ confirm
confirm() {
  secs=$(confirm_secs "$1")
  if [ "$secs" -eq 0 ]; then
    health 120
    return
  fi
  deadline=$(( $(now) + secs ))
  echo "$$ $deadline" > "$TOKEN"
  setprop sys.glass.confirm "$deadline"
  log "confirm: Liquid Glass on; keeping it only if confirmed within $secs s"
  notify "Liquid Glass is on. Press Volume Up within $secs seconds to keep it; otherwise Android's blur comes back by itself."
  left=$secs
  while [ "$left" -gt 0 ]; do
    if ! still_mine; then
      log "confirm: answered elsewhere"
      [ "$(mode)" = glass ] && health 120
      return
    fi
    if [ "$(mode)" != glass ]; then
      clear_token
      log "confirm: glass was switched off meanwhile"
      return
    fi
    if volume_up_within 1; then
      still_mine || return
      clear_token
      log "confirm: kept (Volume Up)"
      notify "Liquid Glass kept."
      health 120
      return
    fi
    left=$((left - 1))
  done
  still_mine || return
  clear_token
  [ "$(mode)" = glass ] && revert "it was not confirmed within $secs seconds"
}

case "$1" in
  confirm) shift; confirm "$@" ;;
  health)  shift; health "$@" ;;
  keep)
    if [ -f "$TOKEN" ]; then
      clear_token
      log "confirm: kept (glassctl keep)"
      notify "Liquid Glass kept."
      echo "Liquid Glass kept"
    else
      echo "nothing to confirm"
    fi
    ;;
  cancel) [ -f "$TOKEN" ] && { clear_token; log "confirm: cancelled"; } ;;
  pending)
    d=$(cut -d' ' -f2 "$TOKEN" 2>/dev/null)
    case "$d" in
      ''|*[!0-9]*) ;;
      *) l=$((d - $(now))); [ "$l" -gt 0 ] && echo "$l" ;;
    esac
    ;;
  *) echo "usage: watchdog.sh confirm|health [seconds] | keep | cancel | pending" >&2; exit 2 ;;
esac
