#!/usr/bin/env bash
# Compares the Skia object layouts the engine depends on across header trees.
#
#   engine/test/layout/layout_check.sh <skia-tree> [<skia-tree> ...]
#
# Each tree is a fetch_skia_headers.sh-style checkout (include/, android/).
# Prints one column per tree and ABI and flags any row that differs. The engine
# is compiled against ONE pin; a new Android release is safe to drive with it
# only if every row here matches, which is how android17-release was cleared
# against the android16-release pin. Nothing is linked or run: the values are
# read back out of the compiler's assembly, so this works for arm64 on an x86
# host.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

cols=()
for tree in "$@"; do
  name=$(cat "$tree/PINNED_BRANCH" 2>/dev/null || basename "$tree")
  # SkData's storage changed spelling in android17-release; probe whichever exists.
  def=(); grep -q 'SkSpan<std::byte>   fSpan' "$tree/include/core/SkData.h" && def=(-DSKDATA_SPAN)
  for t in aarch64 x86_64; do
    out="$WORK/$name-$t"
    "$TC/clang++" --target=$t-linux-android30 -std=c++17 -O0 -S "${def[@]}" -o "$out.s" \
      -I "$tree/android" -I "$tree" -I "$tree/include/config" "$HERE/probe.cpp" 2>/dev/null
    python3 -I -c '
import re, sys
s = open(sys.argv[1]).read()
for m in re.finditer(r"^(\w+):\s*\n\s*\.(?:xword|quad)\s+(\d+)", s, re.M):
    print(m.group(1), m.group(2))' "$out.s" > "$out.txt"
    cols+=("$out.txt")
  done
done

python3 -I - "${cols[@]}" <<'PY'
import sys, os
cols = sys.argv[1:]
tables = [dict(l.split() for l in open(c)) for c in cols]
names = [os.path.basename(c)[:-4] for c in cols]
print(f"{'':20s}" + "".join(f"{n[-24:]:>26s}" for n in names))
bad = 0
info = 0
for k in tables[0]:
    vals = [t.get(k, "?") for t in tables]
    same = len(set(vals)) == 1
    # info_ rows (probe.cpp, I()) are facts worth seeing that the engine does
    # not depend on; they never fail the check.
    informational = k.startswith("info_")
    if not same:
        if informational:
            info += 1
        else:
            bad += 1
    flag = "" if same else ("   (differs; informational)" if informational else "   <-- DIFFERS")
    print(f"{k:20s}" + "".join(f"{v:>26s}" for v in vals) + flag)
if info:
    print(f"\n{info} informational row(s) differ: the engine does not depend on them (probe.cpp says why)")
print("\nall layouts identical" if not bad else f"\n{bad} layout(s) differ: do not drive this tree with the current pin")
sys.exit(1 if bad else 0)
PY
