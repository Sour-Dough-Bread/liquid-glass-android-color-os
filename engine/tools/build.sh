#!/usr/bin/env bash
# Builds glasscheck, the module's install-time preflight, for both ABIs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
API=30
OUT="$HERE/out"
XZ_C=(xz_crc32 xz_dec_lzma2 xz_dec_stream xz_dec_bcj)
XZ_FLAGS=(-DXZ_DEC_BCJ -DXZ_DEC_X86 -DXZ_DEC_ARM -DXZ_DEC_ARM64)
declare -A TRIPLE=([arm64-v8a]=aarch64-linux-android [x86_64]=x86_64-linux-android)

for abi in arm64-v8a x86_64; do
  t="${TRIPLE[$abi]}"
  mkdir -p "$OUT/$abi/obj"
  for c in "${XZ_C[@]}"; do
    "$TC/clang" --target="$t$API" -std=c11 -O2 -w -I "$HERE/../vendor/xz" "${XZ_FLAGS[@]}" \
      -c "$HERE/../vendor/xz/$c.c" -o "$OUT/$abi/obj/$c.o"
  done
  "$TC/clang++" --target="$t$API" -std=c++17 -O2 -s -static-libstdc++ \
    -I "$HERE/../vendor/xz" \
    -o "$OUT/$abi/glasscheck" \
    "$HERE/glasscheck.cpp" "$HERE/../src/abi_check.cpp" "$HERE/../src/elf_syms.cpp" "$HERE/../src/sf_symbols.cpp" \
    "$HERE/../src/xz_inflate.cpp" "$HERE/../src/glass_params.cpp" "$OUT/$abi/obj"/*.o
  printf "%-10s %s (%s bytes)\n" "$abi" "$OUT/$abi/glasscheck" "$(stat -c%s "$OUT/$abi/glasscheck")"
done
