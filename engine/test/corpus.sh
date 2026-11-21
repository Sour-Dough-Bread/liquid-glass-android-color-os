#!/usr/bin/env bash
# The resolver against every surfaceflinger in a firmware corpus, one line each.
#
#   engine/test/corpus.sh [corpus-dir]      default: captures/sf-corpus
#
# A corpus is a tree with one directory per build holding the binary as
# `surfaceflinger` (and, optionally, info.json and build.prop files), the way
# captures/sf-corpus/<family>/<build>/ is laid out. The binaries are ROM files
# and stay out of git; this is how a change to the entry-point table or the
# judging rules is checked against all of them at once. Same resolver, same
# rules as glasscheck and the engine (src/sf_resolve.h).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CORPUS="${1:-$HERE/../../captures/sf-corpus}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
BIN="${GLASS_TEST_BIN:-$WORK/resolve_test}"
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -include cstdarg \
  -o "$BIN" "$HERE/resolve_test.cpp" "$HERE/../src/elf_syms.cpp" "$HERE/../src/sf_symbols.cpp" \
  "$HERE/../src/sf_resolve.cpp" "$HERE/../src/sf_cache.cpp" "$HERE/../src/abi_check.cpp"

pass=0 fail=0
while IFS= read -r sf; do
  build="${sf#"$CORPUS"/}"; build="${build%/surfaceflinger}"
  if line=$("$BIN" --brief "$sf" 2>&1); then pass=$((pass + 1)); else fail=$((fail + 1)); fi
  printf '%-58s %s\n' "$build" "$line"
done < <(find "$CORPUS" -name surfaceflinger -type f | sort)
echo
echo "$pass pass, $fail fail"

# The blur scale each build maps Android's blur with (src/blur_scale.h),
# against what the survey found per family.
echo
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -include cstdarg \
  -o "$WORK/blur_scale_test" "$HERE/blur_scale_test.cpp" "$HERE/../src/blur_scale.cpp" \
  "$HERE/../src/elf_syms.cpp" "$HERE/../src/sf_symbols.cpp" "$HERE/../src/sf_resolve.cpp" \
  "$HERE/../src/sf_cache.cpp" "$HERE/../src/abi_check.cpp"
scale=0
"$WORK/blur_scale_test" --corpus "$CORPUS" | tail -n 1 || scale=1
[ "$fail" -eq 0 ] && [ "$scale" -eq 0 ]
