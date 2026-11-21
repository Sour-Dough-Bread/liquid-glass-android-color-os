#!/usr/bin/env bash
# Generates the frameworks/native patch that builds StockGlass into a ROM.
#
#   rom/make_patch.sh <branch> [--tree <frameworks/native checkout>] [--engine <dir>]
#
# <branch> is the frameworks/native branch to patch:
#   android17-release, android16-qpr2-release, ...   AOSP, from android.googlesource.com
#   lineage-24.0, lineage-23.2, ...                    LineageOS, from github.com/LineageOS
#   evox-cnb                                           Evolution X, branch cnb
#   crdroid-17.0                                       crDroid, branch 17.0
# With --tree, the three files the patch edits come from that checkout instead
# (any fork; <branch> then only names the output). --engine takes the engine
# sources from a copy of engine/src instead of engine/src itself.
#
# Writes rom/out/<branch>/0001-renderengine-stockglass-liquid-glass.patch, for
# `git am` in frameworks/native, and leaves the patched files in
# rom/out/<branch>/tree/ for a look or a compile check. rom/README.md says
# what the patch does and what a ROM needs besides.
#
# Everything in the patch comes from this repository as it is now: the
# drawing code, shader and parameter tables from engine/src, the glue from
# rom/glue/. Generated patches are not committed; generate one for the tree
# you are patching.
set -euo pipefail

ROM=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$ROM/.." && pwd)
SRC=$REPO/engine/src

usage() { sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ $# -ge 1 ] || usage
branch=$1; shift
tree=""
while [ $# -gt 0 ]; do
  case "$1" in
    --tree) tree=$(cd "$2" && pwd); shift 2 ;;
    --engine) SRC=$(cd "$2" && pwd); shift 2 ;;
    *) usage ;;
  esac
done
case "$branch" in */*|.*|"") echo "make_patch: odd branch name: $branch" >&2; exit 2 ;; esac

OUT=$ROM/out/$branch
PATCH=$OUT/0001-renderengine-stockglass-liquid-glass.patch
EDITED=(
  libs/renderengine/Android.bp
  libs/renderengine/skia/SkiaRenderEngine.cpp
  libs/renderengine/skia/filters/BlurFilter.h
)

# Where <branch> lives, and its name there.
ref=$branch
case "$branch" in
  lineage-*) remote=https://github.com/LineageOS/android_frameworks_native ;;
  evox-*)    remote=https://github.com/Evolution-X/frameworks_native; ref=${branch#evox-} ;;
  crdroid-*) remote=https://github.com/crdroidandroid/android_frameworks_native
             ref=${branch#crdroid-} ;;
  *)         remote=https://android.googlesource.com/platform/frameworks/native ;;
esac

rm -rf "$OUT"
mkdir -p "$OUT"
work=$OUT/work
git init -q "$work"
g() { git -C "$work" -c user.name="${GIT_AUTHOR_NAME:-StockGlass}" \
        -c user.email="${GIT_AUTHOR_EMAIL:-stockglass@users.noreply.github.com}" "$@"; }

# ---- the base: the three files as the branch has them
if [ -n "$tree" ]; then
  base="$(git -C "$tree" rev-parse HEAD 2>/dev/null || echo unknown) ($tree)"
  for f in "${EDITED[@]}"; do
    mkdir -p "$work/$(dirname "$f")"
    cp "$tree/$f" "$work/$f"
  done
else
  sha=$(git ls-remote "$remote" "refs/heads/$ref" | cut -f1)
  [ -n "$sha" ] || { echo "make_patch: $remote has no branch $ref" >&2; exit 1; }
  base="$sha ($remote $ref)"
  for f in "${EDITED[@]}"; do
    mkdir -p "$work/$(dirname "$f")"
    case "$remote" in
      *github.com*)
        url=${remote/github.com/raw.githubusercontent.com}/$sha/$f
        curl -fsSL --retry 3 -o "$work/$f" "$url" ;;
      *)
        curl -fsSL --retry 3 "$remote/+/$sha/$f?format=TEXT" | base64 -d > "$work/$f" ;;
    esac
    [ -s "$work/$f" ] || { echo "make_patch: could not fetch $f" >&2; exit 1; }
  done
fi
g add -A
g commit -q -m base

# ---- which BlurFilter this tree has (GlassFilterApi.h), and whether
# GlassFilter can stand in for it: Android 17's generateTemporaryImage() or
# Android 16 QPR2's generate(), and a fork's material, setDisplay(),
# lensReach()
api_what=$(python3 -I "$ROM/tools/blur_api.py" "$work")

# ---- the edits to existing files
python3 -I "$ROM/tools/edit_tree.py" "$work"

# ---- the new files
FILTERS=$work/libs/renderengine/skia/filters
cp "$ROM/glue/GlassFilter.h" "$ROM/glue/GlassFilter.cpp" "$FILTERS/"
python3 -I "$ROM/tools/stage_glass.py" "$SRC" "$FILTERS/glass"

# The embedded shader is the engine's, byte for byte.
cmp "$SRC/generated/glass_shader_src.h" "$FILTERS/glass/generated/glass_shader_src.h"

# ---- the commit
src_rev=$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
dirty=""
[ -z "$(git -C "$REPO" status --porcelain -- engine/src rom 2>/dev/null)" ] ||
  dirty=" plus uncommitted changes"
[ "$SRC" = "$REPO/engine/src" ] || dirty="$dirty (engine sources: a copy of engine/src)"
when=$(git -C "$REPO" log -1 --format=%cI 2>/dev/null || date -u +%FT%TZ)
g add -A
GIT_AUTHOR_DATE=$when GIT_COMMITTER_DATE=$when g commit -q -F - <<EOF
renderengine: draw blur regions as Liquid Glass (StockGlass)

GlassFilter wraps the blur filter SkiaRenderEngine picks. While
persist.sys.glass.mode=glass it draws each blur region with the StockGlass
Liquid Glass shader; anything else, including no property at all, leaves
the region to the wrapped filter, which draws Android's blur as before.
With persist.sys.glass.enabled=0 at boot the filter is not wrapped.

The drawing code, the shader, its presets and the persist.sys.glass.*
properties are the StockGlass module's own, so glassctl, its WebUI and its
LSPosed companion drive a ROM that carries this patch the way they drive
the module. No sepolicy change is needed: the properties are system_prop,
which SurfaceFlinger reads and sets and Settings can set.

Existing files: BlurFilter::drawBlurRegion becomes virtual (and so do
setDisplay and lensReach where the tree has them, which GlassFilter passes
on to the filter it wraps), SkiaRenderEngine wraps the filter it picked,
Android.bp builds the new files. Where drawBlurRegion takes a region's
material, the regions the glass does not draw reach the wrapped filter with
it.

$(printf "This tree's BlurFilter: %s." "$api_what" | fold -s -w 72 | sed 's/ *$//')

Generated by rom/make_patch.sh $branch from
https://github.com/capthndsme/liquid-glass-android $src_rev$dirty.
Base: $base
EOF
g format-patch -q -1 --stdout > "$PATCH"

# ---- check: it applies to the base it was made from
check=$OUT/check
git init -q "$check"
for f in "${EDITED[@]}"; do
  mkdir -p "$check/$(dirname "$f")"
  git -C "$work" show "HEAD~1:$f" > "$check/$f"
done
git -C "$check" apply --check "$PATCH"

mv "$work" "$OUT/tree"
rm -rf "$check"
echo "make_patch: $PATCH"
git -C "$OUT/tree" show --stat --format= HEAD | sed 's/^/  /'
