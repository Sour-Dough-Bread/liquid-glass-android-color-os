#!/usr/bin/env bash
# Screenshot of the car screen as the head unit decodes it (the lab's
# aa_lab.sh shot), plus a 2x crop of the dashboard media card.
#
#   tools/card_shot.sh <out.png>     -> <out.png> and <out>-card.png
set -euo pipefail
# The lab is in captures/, which is not in the repository.
LAB="${LAB:-$(cd "$(dirname "$0")/../../.." && pwd)/captures/2026-11-22-android-auto/lab}"
out="$(realpath -m "${1:?usage: card_shot.sh <out.png>}")"
"$LAB/aa_lab.sh" shot "$out" >/dev/null
# The card: Dashboard window at (860,120) in the 1280x720 head unit frame, 410x480.
python3 -I - "$out" <<'PY'
import sys
from PIL import Image
p = sys.argv[1]
im = Image.open(p)
im.crop((860, 120, 1270, 600)).resize((820, 960), Image.LANCZOS).save(p[:-4] + "-card.png")
PY
echo "$out"
