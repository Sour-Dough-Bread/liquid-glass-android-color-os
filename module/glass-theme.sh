#!/system/bin/sh
# Keeps the glass following the system theme: frostColor tracks light/dark, and
# accentR/G/B track the Material You palette for Liquid Material.
#
# The shader documents frostColor.rgb as "the system background colour", and the
# engine ships the dark-theme value because SurfaceFlinger has no view of the
# framework's theme — it is a compositor, several layers below anything that
# knows what Material You is doing. So the values have to arrive from outside,
# and a property is the channel the engine already listens on.
#
# `cmd uimode night` rather than `settings get secure ui_night_mode`: the setting
# can be null or "auto", while this reports the state actually in effect after
# schedules and battery-saver overrides are resolved.
#
# The palette comes from `cmd overlay lookup`, which resolves a framework colour
# with the wallpaper's dynamic-colour overlay applied. system_accent1_500 is the
# accent at tone 50, which carries the palette's hue and chroma; the engine
# derives the lighter and darker tones it needs from it.
#
# Polling is unlovely but it is what is available without an app or a framework
# hook. Two binder calls every 15 s are not measurable next to what the
# compositor is doing, and a property is only written when its answer changes,
# so the engine's own re-read stays idle too.
#
# Set persist.sys.glass.follow_theme to 0 to turn this off and manage
# frostR/G/B and accentR/G/B by hand.
INTERVAL=15
last=""
last_accent=""

# 0..255 -> "0.412"
unit() { printf '%d.%03d' $(( $1 * 1000 / 255 / 1000 )) $(( $1 * 1000 / 255 % 1000 )); }

while true; do
  if [ "$(getprop persist.sys.glass.follow_theme)" = "0" ]; then
    last=""          # forget, so re-enabling reapplies immediately
    last_accent=""
    sleep "$INTERVAL"
    continue
  fi

  case "$(cmd uimode night 2>/dev/null)" in
    *yes*) mode=dark  ;;
    *no*)  mode=light ;;
    *)     mode=""    ;;   # unavailable; leave it alone
  esac

  if [ -n "$mode" ] && [ "$mode" != "$last" ]; then
    if [ "$mode" = "dark" ]; then
      setprop persist.sys.glass.frostR 0.08
      setprop persist.sys.glass.frostG 0.08
      setprop persist.sys.glass.frostB 0.09
    else
      setprop persist.sys.glass.frostR 0.95
      setprop persist.sys.glass.frostG 0.95
      setprop persist.sys.glass.frostB 0.97
    fi
    last="$mode"
    echo "$(date) theme -> $mode" >> /data/adb/glass/theme.log
  fi

  # "@17171412 -> #ff69769b"; anything else (no such resource on an older
  # Android) leaves the accent as it was.
  hex=$(cmd overlay lookup android android:color/system_accent1_500 2>/dev/null \
        | sed -n 's/.*#[0-9a-fA-F]\{2\}\([0-9a-fA-F]\{6\}\).*/\1/p' | tail -1)
  if [ -n "$hex" ] && [ "$hex" != "$last_accent" ]; then
    r=$(unit $(( 0x$(echo "$hex" | cut -c1-2) )))
    g=$(unit $(( 0x$(echo "$hex" | cut -c3-4) )))
    b=$(unit $(( 0x$(echo "$hex" | cut -c5-6) )))
    setprop persist.sys.glass.accentR "$r"
    setprop persist.sys.glass.accentG "$g"
    setprop persist.sys.glass.accentB "$b"
    last_accent="$hex"
    echo "$(date) accent -> #$hex" >> /data/adb/glass/theme.log
  fi
  sleep "$INTERVAL"
done
