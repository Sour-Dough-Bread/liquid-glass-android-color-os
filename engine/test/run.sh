#!/usr/bin/env bash
# Host tests for the symbol resolver. No device, no risk — this is the same
# parsing that later runs inside SurfaceFlinger.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${GLASS_TEST_BIN:-/tmp/glass_resolve_test}"
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -include cstdarg \
  -o "$BIN" "$HERE/resolve_test.cpp" "$HERE/../src/elf_syms.cpp" "$HERE/../src/sf_symbols.cpp" \
  "$HERE/../src/sf_resolve.cpp" "$HERE/../src/sf_cache.cpp" "$HERE/../src/abi_check.cpp"
fail=0
for spec in "$@"; do
  IFS=: read -r bin addr <<< "$spec"
  "$BIN" "$bin" "${addr:-}" || fail=1
done
exit $fail
