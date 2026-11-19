#!/usr/bin/env bash
# What the Release workflow publishes, and under which name.
#
#   release.sh name                    decide this build's name and kind
#   release.sh notes TAG KIND FILE...  the release's text, to stdout
#
# The name comes from module/module.prop's version= (say v0.4.20) and the
# repository's tags:
#
#   release     no tag v0.4.20 yet. This push is that version's release.
#   prerelease  the tag is on an earlier commit. The build is named as
#               `git describe` would name it, v0.4.20-3-g1a2b3c4 (three commits
#               after v0.4.20, at 1a2b3c4), and `name` writes that into
#               module.prop, so the zip and the module inside it say so too.
#               Two different builds are then never called the same.
#   same        the tag is this very commit. It is published already.
#
# `name` prints tag= and kind= lines, into $GITHUB_OUTPUT as well when a
# workflow runs it. It edits module.prop for a prerelease: outside a workflow,
# run it in a checkout whose module.prop may change.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PROP="$ROOT/module/module.prop"
g() { git -C "$ROOT" "$@"; }

name() {
  local version tag kind n
  version=$(sed -n 's/^version=//p' "$PROP")
  [ -n "$version" ] || { echo "no version= in $PROP" >&2; exit 1; }
  if g rev-parse -q --verify "refs/tags/$version^{commit}" >/dev/null; then
    if [ "$(g rev-parse "refs/tags/$version^{commit}")" = "$(g rev-parse HEAD)" ]; then
      tag=$version kind=same
    else
      n=$(g rev-list --count "refs/tags/$version..HEAD")
      tag="$version-$n-g$(g rev-parse --short=7 HEAD)"
      kind=prerelease
      sed -i "s/^version=.*/version=$tag/" "$PROP"
    fi
  else
    tag=$version kind=release
  fi
  printf 'tag=%s\nkind=%s\n' "$tag" "$kind"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    printf 'tag=%s\nkind=%s\n' "$tag" "$kind" >> "$GITHUB_OUTPUT"
  fi
}

notes() {
  local tag=$1 kind=$2; shift 2
  local version=${tag%%-*} since="" head have_apk=0 f
  head=$(g rev-parse HEAD)
  case "$kind" in
    prerelease)
      since="refs/tags/$version"
      echo "A build between versions: $(g rev-list --count "$since..HEAD") commit(s) after $version. The module in it reports its version as \`$tag\`."
      ;;
    *)
      # The release before this one: the nearest tag below this commit.
      # Version tags only, not the builds between them.
      since=$(g describe --tags --abbrev=0 --match 'v[0-9]*' --exclude '*-g*' "HEAD^" 2>/dev/null || true)
      [ -n "$since" ] && since="refs/tags/$since"
      echo "Stock Glass $tag."
      ;;
  esac
  echo
  echo "Flash the zip in KernelSU, KernelSU-Next, APatch or Magisk and reboot. Glass stays off until you switch it on (the module's Action button, its WebUI, or \`glassctl glass\`). The companion APK is an LSPosed module. The [README](https://github.com/${GITHUB_REPOSITORY:-capthndsme/liquid-glass-android}#readme) has the rest."
  echo
  if [ -n "$since" ]; then
    echo "### Changes since ${since#refs/tags/}"
    echo
    g log --no-merges --format='- %s (%h)' "$since..HEAD"
  else
    echo "### Latest changes"
    echo
    g log --no-merges --format='- %s (%h)' -n 15 HEAD
  fi
  echo
  echo "### Files"
  echo
  echo "| file | sha256 |"
  echo "|---|---|"
  for f in "$@"; do
    case "$f" in *.apk) have_apk=1 ;; esac
    echo "| \`$(basename "$f")\` | \`$(sha256sum "$f" | cut -d' ' -f1)\` |"
  done
  echo
  if [ "$have_apk" = 0 ]; then
    echo "No companion APK in this build: the repository holds no signing key for it (the \`COMPANION_KEYSTORE_B64\` secret), and an APK signed with a fresh key could not update an installed one."
    echo
  fi
  # The engine is what runs inside SurfaceFlinger, and docs/VERIFIED.md names
  # the engines that ran on real phones by these hashes. The same NDK compiles
  # the same sources to the same bytes, so a reader can tell whether this
  # build's engine is one of them.
  local so
  for so in "$ROOT"/engine/out/*/libglassengine.so; do
    [ -f "$so" ] || continue
    echo "- engine, $(basename "$(dirname "$so")"): \`$(sha256sum "$so" | cut -d' ' -f1)\`"
  done
  echo
  echo "Built by GitHub Actions from \`$head\`, after the host tests passed. This zip was not installed anywhere before it was published. [docs/VERIFIED.md](https://github.com/${GITHUB_REPOSITORY:-capthndsme/liquid-glass-android}/blob/$head/docs/VERIFIED.md) says which engines, by the hashes above, ran on which phones."
}

cmd=${1:-}; [ $# -gt 0 ] && shift
case "$cmd" in
  name)  name ;;
  notes) [ $# -ge 3 ] || { echo "usage: release.sh notes TAG KIND FILE..." >&2; exit 1; }
         notes "$@" ;;
  *)     echo "usage: release.sh name | notes TAG KIND FILE..." >&2; exit 1 ;;
esac
