#!/usr/bin/env bash
# Builds patchelf for Android, for the KernelSU module to run ON DEVICE.
#
# The module cannot ship a pre-patched surfaceflinger: every ROM has its own,
# and it is 8 MB. So it copies the device's binary and injects the DT_NEEDED
# entry there, which needs an ELF patcher that runs on the device.
#
# Why not LD_PRELOAD, which would need none of this: init starts surfaceflinger
# with `capabilities SYS_NICE`, so the kernel sets AT_SECURE=1, and bionic's
# linker then ignores LD_PRELOAD *and strips it from the environment*. Measured:
# /proc/<sf>/auxv reports AT_SECURE=1 and LD_PRELOAD never appears in
# /proc/<sf>/environ. See docs/VERIFIED.md.
#
# patchelf has no dependencies beyond a C++17 compiler, so its one translation
# unit is compiled directly and autotools is skipped entirely.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VER="${1:-0.18.0}"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
API=30
WORK="$HERE/.patchelf-build"
OUT="$HERE/patchelf-android"

rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"
curl -fsSL "https://github.com/NixOS/patchelf/releases/download/$VER/patchelf-$VER.tar.gz" \
  -o "$WORK/patchelf.tar.gz"
# The module runs this on the device as root: the 0.18.0 tarball is checked
# against the one it was first built from. Another version is taken as it comes.
if [ "$VER" = 0.18.0 ]; then
  echo "64de10e4c6b8b8379db7e87f58030f336ea747c0515f381132e810dbf84a86e7  $WORK/patchelf.tar.gz" \
    | sha256sum -c --status - || { echo "FATAL: patchelf-$VER.tar.gz is not the pinned tarball" >&2; exit 1; }
fi
tar xzf "$WORK/patchelf.tar.gz" -C "$WORK" --strip-components=1

declare -A TRIPLE=([arm64-v8a]=aarch64-linux-android [x86_64]=x86_64-linux-android)
for abi in arm64-v8a x86_64; do
  mkdir -p "$OUT/$abi"
  # -static-libstdc++ for the same reason the engine needs it: libc++_shared.so
  # does not exist on a device.
  "$TC/clang++" --target="${TRIPLE[$abi]}$API" -std=c++17 -O2 -static-libstdc++ -s \
    -DPACKAGE_STRING="\"patchelf $VER\"" -D_FILE_OFFSET_BITS=64 \
    -I "$WORK/src" "$WORK/src/patchelf.cc" -o "$OUT/$abi/patchelf"
  printf "%-10s %s (%s bytes)\n" "$abi" "$OUT/$abi/patchelf" "$(stat -c%s "$OUT/$abi/patchelf")"
done
rm -rf "$WORK"
echo "patchelf $VER built for Android"
