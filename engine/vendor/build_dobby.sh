#!/usr/bin/env bash
# Builds Dobby as a static library for the ABIs the engine ships.
#
# Dobby rather than ShadowHook: ShadowHook is armeabi-v7a/arm64-v8a only, and
# the whole development loop runs on an x86_64 AVD (an arm64 AVD on an x86_64
# host has no KVM and is unusably slow). Dobby covers x86_64 and arm64, so the
# same hook code is exercised on the emulator and on the phone.
#
# ByteHook was not a candidate at all: it is a PLT hook library, and
# BlurFilter::drawBlurRegion is an internal call inside the surfaceflinger
# binary that never goes through the PLT.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}"
CMAKE="${CMAKE_BIN:-$HOME/Android/Sdk/cmake/3.31.6/bin/cmake}"
NINJA="$HOME/Android/Sdk/cmake/3.31.6/bin/ninja"
API=30

# Upstream bit-rot in Dobby master's Linux backend (ProcessRuntime.cc): it still
# uses RuntimeModule::load_address, renamed to ::base, and treats MemRange::start
# as a field when it is now a method. Nothing we use touches this file — we do
# our own /proc/self/maps parsing in elf_syms.cpp — but it is in the core build,
# so it has to compile. Fixups are idempotent and verified below.
PR="$HERE/dobby/source/Backend/UserMode/PlatformUtil/Linux/ProcessRuntime.cc"
sed -i -e 's/module\.load_address/module.base/g' \
       -e 's/(a\.start < b\.start)/(a.start() < b.start())/' "$PR"
if grep -q "load_address" "$PR"; then
  echo "FATAL: Dobby fixup did not apply cleanly; upstream changed" >&2; exit 1
fi

for abi in arm64-v8a x86_64; do
  build="$HERE/.dobby-build/$abi"
  rm -rf "$build"; mkdir -p "$build"
  "$CMAKE" -S "$HERE/dobby" -B "$build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDOBBY_GENERATE_SHARED=OFF \
    -DDOBBY_BUILD_EXAMPLE=OFF -DDOBBY_BUILD_TEST=OFF \
    -DPlugin.SymbolResolver=OFF \
    > "$build/configure.log" 2>&1 || { echo "$abi: configure FAILED"; tail -5 "$build/configure.log"; continue; }
  "$CMAKE" --build "$build" > "$build/build.log" 2>&1 || {
    echo "$abi: build FAILED"
    grep -E "error:" "$build/build.log" | sed 's/.*error: //' | sort -u | sed 's/^/    /' | head -6
    continue; }

  lib=$(find "$build" -name 'libdobby.a' | head -1)
  [ -n "$lib" ] || { echo "$abi: no libdobby.a produced"; continue; }
  mkdir -p "$HERE/dobby-prebuilt/$abi"
  cp "$lib" "$HERE/dobby-prebuilt/$abi/"
  printf "%-10s %s\n" "$abi" "$(du -h "$HERE/dobby-prebuilt/$abi/libdobby.a" | cut -f1)"
done
