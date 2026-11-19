#!/usr/bin/env bash
# Builds the companion LSPosed module without Gradle, the same way as
# harness/build.sh: aapt2 -> javac -> d8 -> zip -> zipalign -> apksigner, plus
# the NDK for the dev loader's JVMTI agent.
#
#   ./build.sh        -> out/stockglass-companion.apk
#
# The tool paths and the signing key are this machine's unless the environment
# says otherwise (the release workflow does): ANDROID_BUILD_TOOLS, ANDROID_JAR,
# ANDROID_NDK, and COMPANION_KEYSTORE with COMPANION_KEYSTORE_PASS. An update
# installs over an earlier APK only when both carry the same key's signature.
#
# Outputs:
#   classes.dex                 the module. Compiled against the Xposed API in
#                               devloader/java but never containing it, so
#                               under LSPosed the framework's classes resolve.
#   assets/devloader.dex        that API's dev implementation, in the APK, and
#   out/lib/<abi>/libglassagent.so  the JVMTI agent that loads it. Only
#                               dev_attach.sh on the userdebug AVD uses these
#                               two; the agent is not in the APK at all.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BT="${ANDROID_BUILD_TOOLS:-$HOME/Android/Sdk/build-tools/36.0.0}"
# android-33 for the same reason as the harness: the code only needs public
# View/Drawable/Canvas API, and everything hidden is reached by reflection.
ANDROID_JAR="${ANDROID_JAR:-$HOME/Android/sdk/platforms/android-33/android.jar}"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
KEYSTORE="${COMPANION_KEYSTORE:-$HOME/.android/debug.keystore}"
# Exported: apksigner reads the password from the environment (env:KS_PASS), so
# it is on no command line.
export KS_PASS="${COMPANION_KEYSTORE_PASS:-android}"
OUT="$HERE/out"
# jvmti.h comes from the local JDK (the NDK has none). Only that one header is
# copied, so Android's own jni.h is the one it includes.
JDK_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"

rm -rf "$OUT"
mkdir -p "$OUT/api" "$OUT/module" "$OUT/dex-api" "$OUT/dex-module" "$OUT/assets" "$OUT/include"

if [ ! -f "$KEYSTORE" ]; then
  echo "==> creating debug keystore"
  keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

javac_() {
  # Same flags as the harness: JDK 25 rejects -bootclasspath with -source.
  # The encoding is said outright: before JDK 18 javac reads sources in the
  # locale's, and under a C locale a comment's arrow stops the build.
  javac -encoding UTF-8 -source 17 -target 17 -nowarn "$@" 2>&1 \
    | grep -vE "bootstrap class path|source value|target value|To suppress warnings" || true
}

echo "==> Xposed API and dev loader"
find "$HERE/devloader/java" -name '*.java' > "$OUT/api-sources.txt"
javac_ -classpath "$ANDROID_JAR" -d "$OUT/api" @"$OUT/api-sources.txt"
[ -f "$OUT/api/dev/stockglass/devloader/Loader.class" ] || { echo "FATAL: API javac failed" >&2; exit 1; }
find "$OUT/api" -name '*.class' > "$OUT/api-classes.txt"
"$BT/d8" --min-api 33 --lib "$ANDROID_JAR" --output "$OUT/dex-api" @"$OUT/api-classes.txt"
cp "$OUT/dex-api/classes.dex" "$OUT/assets/devloader.dex"
(cd "$OUT/api" && jar cf "$OUT/api.jar" .)

echo "==> module"
find "$HERE/java" -name '*.java' > "$OUT/module-sources.txt"
javac_ -classpath "$ANDROID_JAR:$OUT/api" -d "$OUT/module" @"$OUT/module-sources.txt"
[ -f "$OUT/module/dev/stockglass/companion/Entry.class" ] || { echo "FATAL: module javac failed" >&2; exit 1; }
if find "$OUT/module" -path '*de/robv*' | grep -q .; then
  echo "FATAL: Xposed API classes ended up in the module" >&2; exit 1
fi
find "$OUT/module" -name '*.class' > "$OUT/module-classes.txt"
"$BT/d8" --min-api 33 --lib "$ANDROID_JAR" --classpath "$OUT/api.jar" \
  --output "$OUT/dex-module" @"$OUT/module-classes.txt"
cp "$HERE/assets/xposed_init" "$OUT/assets/"

echo "==> agent"
cp "$JDK_HOME/include/jvmti.h" "$OUT/include/"
for abi in x86_64 arm64-v8a; do
  case "$abi" in
    x86_64)    triple=x86_64-linux-android30 ;;
    arm64-v8a) triple=aarch64-linux-android30 ;;
  esac
  mkdir -p "$OUT/lib/$abi"
  # No C++ runtime is used, so none is linked. 16 KB pages: Android 15+
  # devices may run them, and the alignment costs nothing on 4 KB ones.
  "$TC/clang++" --target="$triple" -std=c++17 -O2 -fPIC -shared -fno-exceptions -fno-rtti \
    -nostdlib++ -Wall -Werror -I "$OUT/include" -Wl,-z,max-page-size=16384 \
    -o "$OUT/lib/$abi/libglassagent.so" "$HERE/devloader/agent.cpp" -llog
done

echo "==> package"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$ANDROID_JAR" \
  --manifest "$HERE/AndroidManifest.xml" -A "$OUT/assets"
cp "$OUT/dex-module/classes.dex" "$OUT/classes.dex"
(cd "$OUT" && zip -q -u base.apk classes.dex)
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass env:KS_PASS --key-pass env:KS_PASS \
  --out "$OUT/stockglass-companion.apk" "$OUT/aligned.apk"
rm -f "$OUT/stockglass-companion.apk.idsig"
echo "OK: $OUT/stockglass-companion.apk ($(stat -c%s "$OUT/stockglass-companion.apk") bytes)"
