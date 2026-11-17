#!/usr/bin/env bash
# Builds + installs the glass harness APK without Gradle.
#
# Gradle is avoided deliberately: this box has JDK 25, and AGP's supported-JDK
# matrix is a rabbit hole for what is a two-class app with zero dependencies.
# aapt2 -> javac -> d8 -> zipalign -> apksigner is five commands and has no
# version negotiation to lose.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# build-tools 36: 33.0.2's d8 (R8 3.3.20, 2022) NPEs on JDK 25 class files, and
# its aapt2 cannot parse a modern android.jar resource table.
BT="$HOME/Android/Sdk/build-tools/36.0.0"
# android-33, not android-37: aapt2 33.0.2 cannot parse the newer resource
# table, and API 33 is exactly where RuntimeShader lands anyway. targetSdk in
# the manifest is just an integer attribute, so it is unaffected.
ANDROID_JAR="$HOME/Android/sdk/platforms/android-33/android.jar"
KEYSTORE="$HOME/.android/debug.keystore"
OUT="$HERE/out"
PKG="dev.stockglass.harness"

rm -rf "$OUT"; mkdir -p "$OUT/res" "$OUT/classes" "$OUT/assets"

# The generated shaders are the single source of truth; never hand-edit assets.
python3 "$HERE/../shader/gen_glass_sksl.py" >/dev/null
cp "$HERE/../shader/build/"*.sksl "$OUT/assets/"

if [ ! -f "$KEYSTORE" ]; then
  echo "==> creating debug keystore"
  keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

echo "==> aapt2 link"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$ANDROID_JAR" \
  --manifest "$HERE/AndroidManifest.xml" \
  -A "$OUT/assets" \
  --java "$OUT" --auto-add-overlay

echo "==> javac"
find "$HERE/java" -name '*.java' > "$OUT/sources.txt"
# No -bootclasspath: JDK 25 rejects it alongside -source/-target, and d8
# desugars whatever java.* we touch anyway. android.jar on the classpath is
# what makes android.* resolve.
javac -source 17 -target 17 -nowarn -classpath "$ANDROID_JAR" \
  -d "$OUT/classes" @"$OUT/sources.txt" 2>&1 \
  | grep -vE "bootstrap class path|source value|target value" || true

if [ -z "$(find "$OUT/classes" -name '*.class' -print -quit)" ]; then
  echo "FATAL: javac produced no classes" >&2; exit 1
fi

echo "==> d8"
find "$OUT/classes" -name '*.class' > "$OUT/classes.txt"
"$BT/d8" --min-api 33 --lib "$ANDROID_JAR" --output "$OUT" @"$OUT/classes.txt"

echo "==> package"
cd "$OUT" && zip -q -u base.apk classes.dex && cd - >/dev/null
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/harness-aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/harness.apk" "$OUT/harness-aligned.apk"

# Same trap as engine/install_dev.sh: with two devices attached adb fails
# halfway through with an unhelpful message. Say so before doing any work.
if [ -z "${ANDROID_SERIAL:-}" ] && [ "$(adb devices | grep -c 'device$')" -gt 1 ]; then
  echo "more than one device attached; set ANDROID_SERIAL to pick one:" >&2
  adb devices | sed -n '2,$p' | sed 's/^/    /' >&2
  exit 1
fi

echo "==> install"
adb install -r "$OUT/harness.apk"
echo "OK: $OUT/harness.apk"
