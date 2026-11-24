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

# The x8 walk on hand-assembled code: the patterns the firmware survey found.
echo "==> abi_walk_test"
g++ -std=c++17 -O2 -Wall -Wextra -I "$HERE/../src" "$HERE/abi_walk_test.cpp" \
  "$HERE/../src/abi_check.cpp" "$HERE/../src/elf_syms.cpp" -o "$OUT/abi_walk_test"
"$OUT/abi_walk_test"

# The note readers on hand-made ELF files, malformed ones included.
echo "==> elf_note_test"
g++ -std=c++17 -O2 -Wall -Wextra -I "$HERE/../src" "$HERE/elf_note_test.cpp" \
  "$HERE/../src/elf_syms.cpp" -o "$OUT/elf_note_test"
"$OUT/elf_note_test"

# Skia from a library the binary loads (ColorOS 17's libhwui.so), on hand-made
# ELF files: imports, precedence, refusals, and what may never come from one.
echo "==> library_test"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -include cstdarg -I "$HERE/../src" \
  "$HERE/library_test.cpp" "$HERE/../src/elf_syms.cpp" "$HERE/../src/sf_symbols.cpp" \
  "$HERE/../src/sf_resolve.cpp" "$HERE/../src/abi_check.cpp" -o "$OUT/library_test"
"$OUT/library_test"

# The blur-scale probe on hand-assembled code: the constants and calls the
# firmware survey found, and readers that refuse, truncate or lie.
echo "==> blur_scale_test"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -include cstdarg -I "$HERE/../src" \
  "$HERE/blur_scale_test.cpp" "$HERE/../src/blur_scale.cpp" "$HERE/../src/elf_syms.cpp" \
  "$HERE/../src/sf_symbols.cpp" "$HERE/../src/sf_resolve.cpp" "$HERE/../src/sf_cache.cpp" \
  "$HERE/../src/abi_check.cpp" -o "$OUT/blur_scale_test"
"$OUT/blur_scale_test"
