#!/usr/bin/env bash
# Vendors the minimal XZ Embedded decoder.
#
# Android has no xz/xzcat/unxz (checked on an A16 image: toybox ships neither),
# and .gnu_debugdata is xz-compressed, so the engine has to carry its own
# decoder to reach the symbol table at runtime.
#
# Only the CRC32 path is needed: the .gnu_debugdata streams produced by the
# Android build use check type 1 (CRC32), verified by reading the stream flags
# byte. crc64 and sha256 are deliberately not vendored. The BCJ filter decoder
# IS included — xz commonly applies an architecture BCJ filter, and omitting it
# would fail only on some targets, which is the worst kind of bug.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/xz"
TMP="$HERE/.xz-tmp"

[ -d "$TMP" ] || git clone --depth 1 https://github.com/tukaani-project/xz-embedded.git "$TMP"

rm -rf "$DEST"; mkdir -p "$DEST"
for f in xz_crc32.c xz_dec_lzma2.c xz_dec_stream.c xz_dec_bcj.c \
         xz_private.h xz_lzma2.h xz_stream.h; do
  cp "$TMP/linux/lib/xz/$f" "$DEST/"
done
cp "$TMP/linux/include/linux/xz.h" "$DEST/"
cp "$TMP/userspace/xz_config.h" "$DEST/"
git -C "$TMP" rev-parse HEAD > "$DEST/PINNED_COMMIT"
rm -rf "$TMP"

echo "vendored $(ls "$DEST"/*.c "$DEST"/*.h | wc -l) files, $(du -sh "$DEST" | cut -f1)"
echo "commit: $(cat "$DEST/PINNED_COMMIT")"
