#!/usr/bin/env bash
# Assembles the flashable KernelSU / Magisk module.
#
#   ./pack.sh
#
# Builds nothing itself except by delegation, so the zip can never contain a
# stale engine: it rebuilds the engine, the preflight checker and patchelf, then
# packs exactly what those produced.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
OUT="$HERE/out"
STAGE="$OUT/stage"

echo "==> engine"
"$ROOT/engine/build.sh" >/dev/null
echo "==> preflight checker"
"$ROOT/engine/tools/build.sh" >/dev/null
[ -d "$ROOT/engine/vendor/patchelf-android" ] || {
  echo "==> patchelf for Android"; "$ROOT/engine/vendor/fetch_patchelf_android.sh" >/dev/null; }

rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$HERE"/module.prop "$HERE"/customize.sh "$HERE"/post-fs-data.sh \
   "$HERE"/service.sh "$HERE"/post-mount.sh "$HERE"/meta.sh "$HERE"/glass-theme.sh "$HERE"/build-overlay.sh \
   "$HERE"/uninstall.sh "$HERE"/glassctl "$HERE"/live.sh "$HERE"/action.sh "$HERE"/sepolicy.rule "$STAGE/"
[ -f "$HERE/README.md" ] && cp "$HERE/README.md" "$STAGE/"
# The WebUI (KernelSU, KernelSU-Next, APatch). Magisk ignores it and has the
# Action button instead.
cp -r "$HERE/webroot" "$STAGE/"

for abi in arm64-v8a x86_64; do
  mkdir -p "$STAGE/libs/$abi" "$STAGE/bin/$abi"
  cp "$ROOT/engine/out/$abi/libglassengine.so"        "$STAGE/libs/$abi/"
  cp "$ROOT/engine/tools/out/$abi/glasscheck"         "$STAGE/bin/$abi/"
  cp "$ROOT/engine/vendor/patchelf-android/$abi/patchelf" "$STAGE/bin/$abi/"
done

# Scripts must keep unix line endings or /system/bin/sh chokes on the shebang.
find "$STAGE" \( -name '*.sh' -o -name glassctl \) -exec sed -i 's/\r$//' {} +

ZIP="$OUT/stockglass-$(sed -n 's/^version=//p' "$HERE/module.prop").zip"
rm -f "$ZIP"
( cd "$STAGE" && zip -qr9 "$ZIP" . -x '.*' )
echo
echo "==> $ZIP  ($(stat -c%s "$ZIP") bytes)"
unzip -l "$ZIP" | tail -n +4 | head -20
