#!/usr/bin/env bash
# Fetches prebuilt frida-gum devkits (static lib + single header) for the ABIs
# the engine ships.
#
# Chosen over Dobby after Dobby master proved unbuildable: its CMakeLists has a
# single architecture guard in the entire file, so every build compiles every
# architecture's assembly (an x86_64 build fails on `blr`/`br` mnemonics), and
# its arm64 assembly separately fails to assemble under clang 21 with
# "invalid symbol kind for ADRP relocation". Two independent upstream breakages.
#
# Chosen over ShadowHook because ShadowHook is armeabi-v7a/arm64-v8a only, and
# the whole development loop runs on an x86_64 AVD.
#
# ByteHook was never a candidate: it is a PLT hook library, and
# BlurFilter::drawBlurRegion is an internal call that never goes through a PLT.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VER="${FRIDA_VERSION:-17.17.0}"
DEST="$HERE/frida-gum"

# sha256 of the 17.17.0 archives. The static library in them is linked into
# the engine, so a release asset that changed under the same name stops the
# build. Another FRIDA_VERSION is taken as it comes.
declare -A SHA=(
  [android-arm64]=4a79e1e765a8b536070ffde66e2f6bd96da0cb66f0bce4b683c88eb0eec67c01
  [android-x86_64]=b39f2840ca056334774c0cf9e8f694901b77aeb897679c3962752e2cf0d2024e)

rm -rf "$DEST"; mkdir -p "$DEST"
for pair in "arm64-v8a:android-arm64" "x86_64:android-x86_64"; do
  abi="${pair%%:*}"; fabi="${pair##*:}"
  url="https://github.com/frida/frida/releases/download/$VER/frida-gum-devkit-$VER-$fabi.tar.xz"
  mkdir -p "$DEST/$abi"
  echo "fetching $fabi"
  curl -fsSL "$url" -o "$DEST/$abi/devkit.tar.xz"
  if [ "$VER" = 17.17.0 ]; then
    echo "${SHA[$fabi]}  $DEST/$abi/devkit.tar.xz" | sha256sum -c --status - || {
      echo "FATAL: the $fabi devkit is not the archive this was pinned to" >&2; exit 1; }
  fi
  tar -xJf "$DEST/$abi/devkit.tar.xz" -C "$DEST/$abi"
  rm -f "$DEST/$abi/devkit.tar.xz"
  test -f "$DEST/$abi/libfrida-gum.a" || { echo "FATAL: no libfrida-gum.a for $abi" >&2; exit 1; }
  test -f "$DEST/$abi/frida-gum.h"   || { echo "FATAL: no frida-gum.h for $abi" >&2; exit 1; }
  printf "  %-10s %s\n" "$abi" "$(du -h "$DEST/$abi/libfrida-gum.a" | cut -f1)"
done
echo "$VER" > "$DEST/PINNED_VERSION"
