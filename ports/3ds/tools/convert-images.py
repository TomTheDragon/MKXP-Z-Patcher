#!/usr/bin/env python3
"""MKXP-Z 3DS: convert a game's images ahead of time (PNG -> .mkxi).

The 3DS needs about 200 ms to unpack a 768x512 PNG (zlib) and much less
for LZ4. For each PNG in the game's Graphics folder this writes a .mkxi
next to it: "MKXI", version 1, width, height (32-bit little endian each),
then the RGBA pixels as an LZ4 frame. The engine prefers .mkxi files.

Big images (4 MB of pixels and more, e.g. 3072x2048 character sheets) are
written as version 2, in 256x256 tiles, so the engine can unpack one
character cell without the rest: "MKXI", 2, width, height, tile size
(256), number of tiles; then per tile (row by row) its offset in the file
and its length (0: fully transparent, not stored); then the tiles, each
an LZ4 frame of its RGBA pixels (edge tiles: only the part inside).

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


TILE = 256
TILED_MIN = 4 * 1024 * 1024


def up_to_date(path, w_h_big):
    """an .mkxi in the current form: tiled (version 2) for big images, else
    version 1 with 64 KB LZ4 blocks (older ones had 4 MB)"""
    with open(path, "rb") as f:
        head = f.read(22)
    if len(head) < 8:
        return False
    version = struct.unpack_from("<I", head, 4)[0]
    if w_h_big:
        return version == 2
    return version == 1 and len(head) == 22 and (head[21] >> 4) & 7 == 4


def tiled(raw, w, h, lz4):
    """version 2: 256x256 tiles, each its own LZ4 frame"""
    tx, ty = (w + TILE - 1) // TILE, (h + TILE - 1) // TILE
    head = b"MKXI" + struct.pack("<IIIII", 2, w, h, TILE, tx * ty)
    at = len(head) + 8 * tx * ty
    table, data = [], []
    for j in range(ty):
        for i in range(tx):
            x0, y0 = i * TILE, j * TILE
            tw, th = min(TILE, w - x0), min(TILE, h - y0)
            rows = b"".join(raw[((y0 + y) * w + x0) * 4:((y0 + y) * w + x0 + tw) * 4] for y in range(th))
            if not any(rows[3::4]):
                table.append((0, 0))
                continue
            packed = run(lz4 + ["-12", "-B4", "-q", "-c", "-"], rows)
            table.append((at, len(packed)))
            data.append(packed)
            at += len(packed)
    return head + b"".join(struct.pack("<II", o, n) for o, n in table) + b"".join(data)


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
            w, h = map(int, run(magick + ["identify", "-format", "%w %h\n", src]).split()[:2])
            big = w * h * 4 >= TILED_MIN
            if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src) and up_to_date(dst, big):
                continue
            raw = run(magick + [src, "-depth", "8", "rgba:-"])
            if len(raw) != w * h * 4:
                print(f"skipped {src}: unexpected size", file=sys.stderr)
                continue
            # 64 KB blocks: unpacking then needs no big extra buffer (the
            # default 4 MB blocks don't fit into an original 3DS's memory)
            with open(dst, "wb") as f:
                if big:
                    f.write(tiled(raw, w, h, lz4))
                else:
                    packed = run(lz4 + ["-12", "-B4", "-q", "-c", "-"], raw)
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
