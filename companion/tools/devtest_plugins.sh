#!/usr/bin/env bash
# devtest_plugins.sh: a test-only variant of the module for the userdebug AVD,
# out/devtest/stockglass-companion-devtest.apk. It is the build in out/ plus
# tools/devtest/PluginGrant.java (classes2.dex, a second xposed_init line),
# which lets crDroid's clock plugins (SystemUIClocks-*) load on the AVD. Never
# install it anywhere else. Run ./build.sh first; then
#   APK=out/devtest/stockglass-companion-devtest.apk ./dev_attach.sh systemui
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BT="$HOME/Android/Sdk/build-tools/36.0.0"
ANDROID_JAR="$HOME/Android/sdk/platforms/android-33/android.jar"
KEYSTORE="$HOME/.android/debug.keystore"
OUT="$HERE/out/devtest"
[ -f "$HERE/out/base.apk" ] || { echo "no out/base.apk; run ./build.sh" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/assets"
javac -source 17 -target 17 -nowarn -classpath "$ANDROID_JAR:$HERE/out/api" -d "$OUT/classes" \
  "$HERE/tools/devtest/PluginGrant.java" 2>&1 | grep -vE "bootstrap class path|source value|target value|To suppress" || true
find "$OUT/classes" -name '*.class' > "$OUT/classes.txt"
"$BT/d8" --min-api 33 --lib "$ANDROID_JAR" --classpath "$HERE/out/api.jar" --output "$OUT/dex" @"$OUT/classes.txt"
cp "$HERE/out/base.apk" "$OUT/base.apk"
mv "$OUT/dex/classes.dex" "$OUT/classes2.dex"
{ cat "$HERE/assets/xposed_init"; echo "dev.stockglass.devtest.PluginGrant"; } > "$OUT/assets/xposed_init"
(cd "$OUT" && zip -q -u base.apk classes2.dex assets/xposed_init)
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/stockglass-companion-devtest.apk" "$OUT/aligned.apk"
rm -f "$OUT/stockglass-companion-devtest.apk.idsig"
echo "OK: $OUT/stockglass-companion-devtest.apk"
