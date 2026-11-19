#!/usr/bin/env bash
# Vendors Skia PUBLIC HEADERS ONLY, pinned to an AOSP release branch.
#
#   ./fetch_skia_headers.sh [branch]     default: android16-release
#
# Why headers at all: SkPaint(), SkPaint::setShader and
# SkRuntimeEffect::uniforms() are header-inline and have no out-of-line symbol
# in /system/bin/surfaceflinger on any build measured, so there is nothing to
# call. Their bodies and the object layouts they assume must come from the same
# source the target was built from. See docs/ARCHITECTURE.md.
#
# The pin is per Android release AND per QPR — these are different trees:
#   android16-release        Android 16 QPR0   (LineageOS 23.0)
#   android16-qpr1-release   Android 16 QPR1
#   android16-qpr2-release   Android 16 QPR2   (LineageOS 23.2, crDroid v12.7)
#   android17-release        Android 17        (crDroid 13.1) — layout-identical to
#                            android16-release, see engine/test/layout/layout_check.sh
#
# Development AVD is BE2A.250530.026.D1 -> android16-release.
# beryllium on crDroid v12.7 -> android16-qpr2-release.
#
# No sources are vendored and nothing is linked: every out-of-line Skia call is
# routed through a pointer resolved from the target's own MiniDebugInfo.
set -euo pipefail

BRANCH="${1:-android16-release}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/skia"
REMOTE=https://android.googlesource.com/platform/external/skia

# The commit of each branch whose layout was checked against a real
# SurfaceFlinger. A release branch can still move, and a build must not then
# compile against headers nobody has looked at: a pinned branch is fetched at
# its pin. SKIA_COMMIT overrides; a branch with no pin is taken at its tip.
declare -A PIN=([android16-release]=bcb0f77c44783b1800ba37641ba7ecab04f05e07)
COMMIT="${SKIA_COMMIT:-${PIN[$BRANCH]:-}}"

rm -rf "$DEST" "$HERE/.skia-tmp"
# blobless + sparse: the full tree is ~1 GB of sources we do not want.
if [ -n "$COMMIT" ]; then
  git init -q "$HERE/.skia-tmp"
  git -C "$HERE/.skia-tmp" remote add origin "$REMOTE"
  git -C "$HERE/.skia-tmp" config remote.origin.promisor true
  git -C "$HERE/.skia-tmp" config remote.origin.partialclonefilter blob:none
  git -C "$HERE/.skia-tmp" sparse-checkout init --cone
  git -C "$HERE/.skia-tmp" sparse-checkout set include modules/skcms android/include
  git -C "$HERE/.skia-tmp" fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
  git -C "$HERE/.skia-tmp" checkout -q FETCH_HEAD
else
  echo "no pinned commit for $BRANCH: taking its tip"
  git clone --depth 1 --filter=blob:none --sparse -b "$BRANCH" "$REMOTE" "$HERE/.skia-tmp"
fi
# android/include/config/SkUserConfig.h is AOSP's Skia build config. It is NOT
# optional decoration: it defines SK_GANESH, SK_GL, SK_VULKAN,
# SK_ENABLE_ANDROID_UTILS, SK_SUPPORT_PDF and friends, and those change which
# fields exist in Skia objects. Using anything other than the target's real
# config is how you get a layout mismatch and a soft reboot.
git -C "$HERE/.skia-tmp" sparse-checkout set include modules/skcms android/include

mkdir -p "$DEST"
cp -r "$HERE/.skia-tmp/include" "$DEST/"
[ -d "$HERE/.skia-tmp/modules/skcms" ] && mkdir -p "$DEST/modules" && \
  cp -r "$HERE/.skia-tmp/modules/skcms" "$DEST/modules/"
cp -r "$HERE/.skia-tmp/android" "$DEST/"
test -f "$DEST/android/include/config/SkUserConfig.h" || {
  echo "FATAL: AOSP SkUserConfig.h missing from $BRANCH" >&2; exit 1; }

git -C "$HERE/.skia-tmp" rev-parse HEAD > "$DEST/PINNED_COMMIT"
echo "$BRANCH" > "$DEST/PINNED_BRANCH"
rm -rf "$HERE/.skia-tmp"

echo "branch : $BRANCH"
echo "commit : $(cat "$DEST/PINNED_COMMIT")"
echo "headers: $(find "$DEST" -name '*.h' | wc -l) files, $(du -sh "$DEST" | cut -f1)"
