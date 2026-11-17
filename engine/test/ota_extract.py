#!/usr/bin/env python3
"""Pulls one partition image out of a full A/B OTA zip. Stdlib only (plus the
zstd CLI for REPLACE_ZSTD operations).

    engine/test/ota_extract.py <ota.zip> system system.img
    debugfs -R "dump /system/bin/surfaceflinger sf" system.img     # ext4 images
    engine/test/run.sh sf

This is how a new ROM is checked without the device: crDroid 13.1 (Android 17)
for alioth was brought up this way. payload.bin is read straight out of the zip
(it is stored, not deflated), and just enough of the DeltaArchiveManifest
protobuf is decoded by hand to replay the partition's REPLACE* and ZERO
operations. Incremental OTAs need a source image and are refused.

EROFS system images need erofs-utils (fsck.erofs --extract) instead of debugfs.
"""
import bz2
import hashlib
import lzma
import struct
import subprocess
import sys
import zipfile


def varint(b, i):
    shift = 0
    out = 0
    while True:
        c = b[i]
        i += 1
        out |= (c & 0x7F) << shift
        if not c & 0x80:
            return out, i
        shift += 7


def fields(b):
    """Yield (field_number, wire_type, value) for a protobuf message."""
    i = 0
    n = len(b)
    while i < n:
        key, i = varint(b, i)
        fn, wt = key >> 3, key & 7
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 1:
            v = struct.unpack_from("<Q", b, i)[0]
            i += 8
        elif wt == 2:
            ln, i = varint(b, i)
            v = b[i:i + ln]
            i += ln
        elif wt == 5:
            v = struct.unpack_from("<I", b, i)[0]
            i += 4
        else:
            raise ValueError(f"wire type {wt}")
        yield fn, wt, v


def parse_extent(b):
    start = num = 0
    for fn, _, v in fields(b):
        if fn == 1:
            start = v
        elif fn == 2:
            num = v
    return start, num


def parse_op(b):
    op = {"type": 0, "off": 0, "len": 0, "dst": [], "sha": None}
    for fn, _, v in fields(b):
        if fn == 1:
            op["type"] = v
        elif fn == 2:
            op["off"] = v
        elif fn == 3:
            op["len"] = v
        elif fn == 6:
            op["dst"].append(parse_extent(v))
        elif fn == 8:
            op["sha"] = bytes(v)
    return op


def parse_partition(b):
    p = {"name": None, "ops": [], "size": None}
    for fn, _, v in fields(b):
        if fn == 1:
            p["name"] = bytes(v).decode()
        elif fn == 8:
            p["ops"].append(parse_op(v))
        elif fn == 7:
            for f2, _, v2 in fields(v):
                if f2 == 1:
                    p["size"] = v2
    return p


def stored_member_offset(zpath, name):
    with zipfile.ZipFile(zpath) as z:
        zi = z.getinfo(name)
        if zi.compress_type != zipfile.ZIP_STORED:
            raise SystemExit(f"{name} is compressed in the zip; extract it first")
    with open(zpath, "rb") as f:
        f.seek(zi.header_offset)
        h = f.read(30)
        if h[:4] != b"PK\x03\x04":
            raise SystemExit("bad local header")
        fnl, exl = struct.unpack_from("<HH", h, 26)
        return zi.header_offset + 30 + fnl + exl


def main():
    zpath, want, out = sys.argv[1:4]
    base = stored_member_offset(zpath, "payload.bin")
    f = open(zpath, "rb")
    f.seek(base)
    hdr = f.read(24)
    if hdr[:4] != b"CrAU":
        raise SystemExit("not a payload")
    ver, msize = struct.unpack_from(">QQ", hdr, 4)
    sigsize = struct.unpack_from(">I", hdr, 20)[0] if ver >= 2 else 0
    hlen = 24 if ver >= 2 else 20
    f.seek(base + hlen)
    manifest = f.read(msize)
    data_base = base + hlen + msize + sigsize

    block = 4096
    parts = []
    for fn, _, v in fields(manifest):
        if fn == 3:
            block = v
        elif fn == 13:
            parts.append(parse_partition(v))
    print("partitions:", ", ".join(f"{p['name']}({len(p['ops'])} ops)" for p in parts))
    part = next((p for p in parts if p["name"] == want), None)
    if part is None:
        raise SystemExit(f"no partition {want}")

    with open(out, "wb") as o:
        if part["size"]:
            o.truncate(part["size"])
        for k, op in enumerate(part["ops"]):
            t = op["type"]
            f.seek(data_base + op["off"])
            raw = f.read(op["len"]) if op["len"] else b""
            if op["sha"] and raw and hashlib.sha256(raw).digest() != op["sha"]:
                raise SystemExit(f"op {k}: sha256 mismatch")
            if t == 0:      # REPLACE
                data = raw
            elif t == 1:    # REPLACE_BZ
                data = bz2.decompress(raw)
            elif t == 8:    # REPLACE_XZ
                data = lzma.decompress(raw)
            elif t == 14:   # REPLACE_ZSTD
                data = subprocess.run(["zstd", "-dc"], input=raw, capture_output=True,
                                      check=True).stdout
            elif t in (6, 7):  # ZERO / DISCARD
                for s, n in op["dst"]:
                    o.seek(s * block)
                    o.write(b"\0" * (n * block))
                continue
            else:
                raise SystemExit(f"op {k}: type {t} needs a source image (incremental OTA?)")
            pos = 0
            for s, n in op["dst"]:
                o.seek(s * block)
                o.write(data[pos:pos + n * block])
                pos += n * block
            if k % 200 == 0:
                print(f"  op {k}/{len(part['ops'])}", flush=True)
    print(f"wrote {out} ({part['size']} bytes)")


if __name__ == "__main__":
    main()
