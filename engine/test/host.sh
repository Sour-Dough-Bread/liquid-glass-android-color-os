#!/usr/bin/env bash
# The engine's tests that need neither a device nor a SurfaceFlinger binary:
# the per-frame arithmetic (what to copy, what to merge, which tint a region
# carries), compiled for the host and run.
#
#   engine/test/host.sh
#
# run.sh, abi_test and xz_test need a ROM's surfaceflinger and are not run
# here. The release workflow runs this before it builds anything.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT

for t in composite sharp_bounds merge tint; do
  echo "==> ${t}_test"
  g++ -std=c++17 -O2 -Wall -Wextra -I "$HERE/../src" "$HERE/${t}_test.cpp" -o "$OUT/${t}_test"
  "$OUT/${t}_test"
done
