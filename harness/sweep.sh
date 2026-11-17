#!/usr/bin/env bash
# Sweeps a single shader parameter and builds a labelled contact sheet.
#
#   ./sweep.sh blurFalloff 0.5 1.0 1.5 2.5 4.0
#   ./sweep.sh blurMix 0.6 0.8 1.0 -- blurFalloff=2.0
#
# Anything after `--` is extra `key=value` held fixed across the sweep. Every
# frame is written from scratch, so no value leaks between cells (the params
# file resets to defaults on each reload).
set -euo pipefail

KEY="$1"; shift
VALUES=(); FIXED=()
seen_sep=0
for a in "$@"; do
  if [ "$a" = "--" ]; then seen_sep=1; continue; fi
  if [ $seen_sep -eq 1 ]; then FIXED+=("$a"); else VALUES+=("$a"); fi
done

OUT="${SWEEP_OUT:-/tmp/glass-sweep}"
# Which pane to crop, as fractions of the screen. Pane 1 by default.
CX0=${CX0:-0.05} CY0=${CY0:-0.08} CX1=${CX1:-0.97} CY1=${CY1:-0.27}

rm -rf "$OUT"; mkdir -p "$OUT"
adb shell am force-stop dev.stockglass.harness >/dev/null 2>&1 || true
adb shell am start -n dev.stockglass.harness/.GlassActivity >/dev/null 2>&1
sleep 3

for v in "${VALUES[@]}"; do
  {
    echo "# sweep $KEY=$v"
    for f in "${FIXED[@]}"; do echo "$f"; done
    echo "$KEY=$v"
  } > "$OUT/p.txt"
  adb push "$OUT/p.txt" /data/local/tmp/glass_params.txt >/dev/null 2>&1
  sleep 1.2
  adb exec-out screencap -p > "$OUT/raw_$v.png" 2>/dev/null
  python3 - "$OUT/raw_$v.png" "$OUT/cell_$v.png" "$KEY=$v" "$CX0" "$CY0" "$CX1" "$CY1" <<'PY'
import sys
from PIL import Image, ImageDraw
src, dst, label, x0, y0, x1, y1 = sys.argv[1], sys.argv[2], sys.argv[3], *map(float, sys.argv[4:8])
im = Image.open(src).convert("RGB"); W, H = im.size
c = im.crop((int(x0*W), int(y0*H), int(x1*W), int(y1*H)))
out = Image.new("RGB", (c.width, c.height + 34), (16, 16, 18))
out.paste(c, (0, 34))
ImageDraw.Draw(out).text((10, 10), label, fill=(235, 235, 240))
out.save(dst)
PY
  echo "  $KEY=$v"
done

CELLS=(); for v in "${VALUES[@]}"; do CELLS+=("$OUT/cell_$v.png"); done
montage "${CELLS[@]}" -tile 1x -geometry +0+4 -background '#0a0a0c' "$OUT/sheet_$KEY.png"
echo "sheet: $OUT/sheet_$KEY.png"
