#!/usr/bin/env python3
"""MKXP-Z 3DS: convert a game's images ahead of time (PNG -> .mkxi).

The 3DS needs about 200 ms to unpack a 768x512 PNG (zlib) and much less
for LZ4. For each PNG in the game's Graphics folder this writes a .mkxi
next to it: "MKXI", version 1, width, height (32-bit little endian each),
then the RGBA pixels as an LZ4 frame. The engine prefers .mkxi files.

    convert-images.py <game folder>

Needs ImageMagick ("magick", or set MAGICK) and lz4 ("lz4", or set LZ4).
Files already converted (newer than their PNG) are skipped.
"""
import os
import shlex
import struct
import subprocess
import sys


def run(cmd, data=None):
    return subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout


def small_blocks(path):
    """an .mkxi with 64 KB LZ4 blocks (older ones had 4 MB)"""
    with open(path, "rb") as f:
        head = f.read(22)
    return len(head) == 22 and (head[21] >> 4) & 7 == 4


def main(game):
    magick = shlex.split(os.environ.get("MAGICK", "magick"))
    lz4 = shlex.split(os.environ.get("LZ4", "lz4"))
    root = os.path.join(game, "Graphics")
    if not os.path.isdir(root):
        sys.exit(f"{root}: no such folder")
    count = before = after = 0
    for dirpath, _, files in os.walk(root):
        for name in sorted(files):
            if not name.lower().endswith(".png"):
                continue
            src = os.path.join(dirpath, name)
            dst = os.path.splitext(src)[0] + ".mkxi"
            if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src) and small_blocks(dst):
                continue
            w, h = map(int, run(magick + ["identify", "-format", "%w %h\n", src]).split()[:2])
            raw = run(magick + [src, "-depth", "8", "rgba:-"])
            if len(raw) != w * h * 4:
                print(f"skipped {src}: unexpected size", file=sys.stderr)
                continue
            # 64 KB blocks: unpacking then needs no big extra buffer (the
            # default 4 MB blocks don't fit into an original 3DS's memory)
            packed = run(lz4 + ["-12", "-B4", "-q", "-c", "-"], raw)
            with open(dst, "wb") as f:
                f.write(b"MKXI" + struct.pack("<III", 1, w, h) + packed)
            count += 1
            before += os.path.getsize(src)
            after += os.path.getsize(dst)
    if count:
        print(f"converted {count} images: {before // 1024} KB PNG -> {after // 1024} KB .mkxi")
    else:
        print("nothing to convert")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
