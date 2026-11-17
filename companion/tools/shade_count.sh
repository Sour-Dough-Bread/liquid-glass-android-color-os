#!/usr/bin/env bash
# shade_count.sh: blur regions on the NotificationShade layer only (0 when it has none).
adb -s ${ANDROID_SERIAL:-emulator-5564} shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | awk '
  /Layer \[/ {name=$0}
  /blurRegions.size\(\)=/ && name ~ /VRI-NotificationShade/ {s=$0; sub(/.*blurRegions.size\(\)=/, "", s); print s+0; f=1; exit}
  END {if (!f) print 0}'
