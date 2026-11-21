#!/usr/bin/env bash
# stage.sh: put live mode's test setup on an emulator. Emulator only.
#
#   harness/live/stage.sh            unpacked module in /data/local/tmp/stockglass
#   harness/live/stage.sh --policy   ...and the policy-tool test double in
#                                    /data/local/tmp/glass-live-test
#
# The module directory is what a temporary-root user would have after
# unpacking the release zip: live.sh, glassctl, sepolicy.rule and the
# x86_64 engine, glasscheck and patchelf, from the current builds.
#
# --policy builds harness/live/sepatch (host libsepol, static), pulls the
# emulator's loaded policy, adds module/sepolicy.rule to it, and pushes the
# result with fake-magiskpolicy, which loads it when live.sh asks
# "magiskpolicy --live --apply" for exactly that rules file. Put
# /data/local/tmp/glass-live-test/bin first in PATH for live.sh to find it.
set -euo pipefail
SERIAL="${ANDROID_SERIAL:-emulator-5564}"
A="adb -s $SERIAL"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
WORK="${GLASS_LIVE_WORK:-$ROOT/captures/sf-corpus/work/rootless/stage}"
[ "$($A shell getprop ro.kernel.qemu | tr -d '\r')" = 1 ] || { echo "refusing: $SERIAL is not an emulator" >&2; exit 1; }

D=/data/local/tmp/stockglass
$A shell "rm -rf $D && mkdir -p $D/libs/x86_64 $D/bin/x86_64"
$A push "$ROOT/module/live.sh" "$ROOT/module/glassctl" "$ROOT/module/meta.sh" \
        "$ROOT/module/sepolicy.rule" "$D/" >/dev/null
$A push "$ROOT/engine/out/x86_64/libglassengine.so" "$D/libs/x86_64/" >/dev/null
$A push "$ROOT/engine/tools/out/x86_64/glasscheck" "$D/bin/x86_64/" >/dev/null
$A push "$ROOT/engine/vendor/patchelf-android/x86_64/patchelf" "$D/bin/x86_64/" >/dev/null
echo "staged $D:"
$A shell "cd $D && ls -l live.sh glassctl sepolicy.rule libs/x86_64 bin/x86_64 | grep -v '^total'"

[ "${1:-}" = --policy ] || exit 0
mkdir -p "$WORK"
gcc -O2 -Wall -o "$WORK/sepatch" "$HERE/sepatch.c" -l:libsepol.a
H=/data/local/tmp/glass-live-test
$A shell "rm -rf $H && mkdir -p $H/bin && cat /sys/fs/selinux/policy > $H/policy.live"
$A pull "$H/policy.live" "$WORK/policy.live" >/dev/null
"$WORK/sepatch" "$WORK/policy.live" "$WORK/policy.patched" "$ROOT/module/sepolicy.rule"
sha256sum < "$ROOT/module/sepolicy.rule" | cut -d' ' -f1 > "$WORK/rules.sha256"
$A push "$WORK/policy.patched" "$WORK/rules.sha256" "$H/" >/dev/null
$A push "$HERE/fake-magiskpolicy" "$H/bin/magiskpolicy" >/dev/null
$A shell "chmod 755 $H/bin/magiskpolicy"
echo "test double: PATH=$H/bin:\$PATH finds magiskpolicy"
