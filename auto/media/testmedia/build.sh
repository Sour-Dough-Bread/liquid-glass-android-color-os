#!/usr/bin/env bash
# Builds the test media app (dev.stockglass.auto.testmedia) without Gradle:
# aapt2 -> javac -> d8 -> zip -> zipalign -> apksigner, like companion/build.sh.
#
#   ./build.sh   -> out/testmedia.apk
#   adb -s emulator-5564 install -r out/testmedia.apk
#   adb -s emulator-5564 shell am start -n dev.stockglass.auto.testmedia/.Start
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BT="${ANDROID_BUILD_TOOLS:-$HOME/Android/Sdk/build-tools/36.0.0}"
ANDROID_JAR="${ANDROID_JAR:-$HOME/Android/sdk/platforms/android-33/android.jar}"
KEYSTORE="${KEYSTORE:-$HOME/.android/debug.keystore}"
export KS_PASS="${KEYSTORE_PASS:-android}"
OUT="$HERE/out"
rm -rf "$OUT"; mkdir -p "$OUT/res" "$OUT/classes" "$OUT/dex" "$OUT/gen"

"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/res/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$ANDROID_JAR" --manifest "$HERE/AndroidManifest.xml" \
  --java "$OUT/gen" "$OUT/res/res.zip"
find "$HERE/java" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
javac -encoding UTF-8 -source 17 -target 17 -nowarn -classpath "$ANDROID_JAR" -d "$OUT/classes" @"$OUT/sources.txt" 2>&1 \
  | grep -vE "bootstrap class path|source value|target value|To suppress warnings" || true
[ -f "$OUT/classes/dev/stockglass/auto/testmedia/Player.class" ] || { echo "FATAL: javac failed" >&2; exit 1; }
find "$OUT/classes" -name '*.class' > "$OUT/classes.txt"
"$BT/d8" --min-api 33 --lib "$ANDROID_JAR" --output "$OUT/dex" @"$OUT/classes.txt"
cp "$OUT/dex/classes.dex" "$OUT/classes.dex"
(cd "$OUT" && zip -q -u base.apk classes.dex)
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass env:KS_PASS --key-pass env:KS_PASS \
  --out "$OUT/testmedia.apk" "$OUT/aligned.apk"
rm -f "$OUT/testmedia.apk.idsig"
echo "OK: $OUT/testmedia.apk"
