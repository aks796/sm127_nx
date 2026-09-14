#!/usr/bin/env python3
"""make_pck.py -- pack a Godot 3 Android assets/ tree into one .pck.

The APK stores the game as ~8500 loose files. On the Switch each existence
check and open of one of them is a FAT path walk on the SD card, and a scene
load does hundreds. Mounted as a main pack, every lookup becomes an in-memory
PackedData hit and every read a seek within one already-open file.

Format: Godot 3.x PACK_FORMAT_VERSION 1, as read by 3.6's
PackedSourcePCK::try_open_pack:

  u32 magic 'GDPC'   u32 format 1   u32 major, minor, patch
  u32 reserved[16]   u32 file_count
  per file: u32 path_len, path ("res://...", NUL-padded to 4), u64 offset,
            u64 size, u8 md5[16]
  file data (offsets are absolute)

The file is re-read after writing and every entry checked against the source
(bytes and MD5), the same way the engine walks the directory.

usage: make_pck.py <assets dir> <out.pck> [--exclude NAME ...] [--version 3.6.0]
"""

import argparse
import hashlib
import os
import struct
import sys

MAGIC = 0x43504447
ALIGN = 16   # data alignment; the reader takes absolute offsets, any works


def collect(root, exclude):
    files = []
    for dp, dirs, fs in os.walk(root):
        dirs.sort()
        for f in sorted(fs):
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel in exclude or f == ".DS_Store":
                continue
            files.append((rel, full))
    return files


def path_entry(rel):
    b = ("res://" + rel).encode("utf8")
    pad = (4 - len(b) % 4) % 4
    return b + b"\0" * pad


def write(root, out, exclude, version):
    files = collect(root, exclude)
    major, minor, patch = (int(x) for x in version.split("."))

    header = struct.pack("<5I", MAGIC, 1, major, minor, patch) + b"\0" * 64
    header += struct.pack("<I", len(files))
    dir_size = sum(4 + len(path_entry(r)) + 8 + 8 + 16 for r, _ in files)

    ofs = len(header) + dir_size
    entries, blobs = [], []
    for rel, full in files:
        data = open(full, "rb").read()
        ofs = (ofs + ALIGN - 1) & ~(ALIGN - 1)
        entries.append((rel, ofs, len(data), hashlib.md5(data).digest()))
        blobs.append((ofs, data))
        ofs += len(data)

    with open(out + ".part", "wb") as f:
        f.write(header)
        for rel, o, size, md5 in entries:
            p = path_entry(rel)
            f.write(struct.pack("<I", len(p)) + p + struct.pack("<QQ", o, size) + md5)
        for o, data in blobs:
            f.write(b"\0" * (o - f.tell()))
            f.write(data)
    os.replace(out + ".part", out)
    return files


def verify(out, files):
    d = open(out, "rb").read()
    magic, fmt, major, minor, patch = struct.unpack_from("<5I", d, 0)
    assert magic == MAGIC and fmt == 1, "bad header"
    o = 20 + 64
    count = struct.unpack_from("<I", d, o)[0]; o += 4
    assert count == len(files), f"count {count} != {len(files)}"
    src = dict(files)
    for _ in range(count):
        sl = struct.unpack_from("<I", d, o)[0]; o += 4
        path = d[o:o + sl].split(b"\0")[0].decode("utf8"); o += sl
        fo, size = struct.unpack_from("<QQ", d, o); o += 16
        md5 = d[o:o + 16]; o += 16
        assert path.startswith("res://"), path
        data = open(src[path[6:]], "rb").read()
        assert d[fo:fo + size] == data, f"data mismatch: {path}"
        assert hashlib.md5(data).digest() == md5, f"md5 mismatch: {path}"
    return major, minor, patch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("assets")
    ap.add_argument("out")
    ap.add_argument("--exclude", action="append", default=[])
    ap.add_argument("--version", default="3.6.0")
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.assets, "project.binary")):
        sys.exit(f"{a.assets}: no project.binary")
    files = write(a.assets, a.out, set(a.exclude), a.version)
    ver = verify(a.out, files)
    print(f"  {a.out}: {len(files)} files, {os.path.getsize(a.out) >> 20} MB, "
          f"Godot {'.'.join(map(str, ver))} pack format 1 -- verified")


if __name__ == "__main__":
    main()
