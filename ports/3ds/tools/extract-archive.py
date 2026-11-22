#!/usr/bin/env python3
"""MKXP-Z 3DS: unpack a game's RGSS archive (Game.rgssad, Game.rgss2a,
Game.rgss3a) into its folder, for the 3DS package.

The 3DS engine reads the game's files directly (and its images converted
to .mkxi: convert-images.py), not from the archive: unpacked once on the
PC, nothing is decrypted while playing.

    extract-archive.py <archive> [<output folder>]

Writes Data/, Graphics/... into the output folder (default: the archive's
folder). Files that are already there aren't overwritten unless they
differ. Same formats as the patcher (patcher-rs/src/archive.rs): RGSSAD
version 1 (XP, VX) and 3 (VX Ace).
"""
import os
import struct
import sys

MASK = 0xFFFFFFFF


def advance(k):
    return (k * 7 + 3) & MASK


def decrypt(data, key):
    """the file's bytes: XOR with a key that advances every 4 bytes"""
    n = len(data)
    pad = (-n) % 4
    words = struct.unpack("<%dI" % ((n + pad) // 4), data + b"\0" * pad)
    out = []
    for w in words:
        out.append(w ^ key)
        key = (key * 7 + 3) & MASK
    return struct.pack("<%dI" % len(out), *out)[:n]


def entries(d):
    """(name bytes, offset, size, key) of each file"""
    if len(d) < 8 or d[:7] != b"RGSSAD\0":
        sys.exit("not an RGSS archive")
    version = d[7]
    if version == 1:
        p, key = 8, 0xDEADCAFE
        while p + 4 <= len(d):
            n = struct.unpack_from("<I", d, p)[0] ^ key
            p += 4
            key = advance(key)
            name = bytearray()
            for b in d[p:p + n]:
                name.append(b ^ (key & 0xFF))
                key = advance(key)
            p += n
            size = struct.unpack_from("<I", d, p)[0] ^ key
            p += 4
            key = advance(key)
            if p + size > len(d):
                sys.exit("damaged archive")
            yield bytes(name), p, size, key
            p += size
    elif version == 3:
        base = (struct.unpack_from("<I", d, 8)[0] * 9 + 3) & MASK
        kb = struct.pack("<I", base)
        p = 12
        while p + 16 <= len(d):
            off, size, fkey, n = (v ^ base for v in struct.unpack_from("<4I", d, p))
            if off == 0:
                break
            name = bytes(b ^ kb[i % 4] for i, b in enumerate(d[p + 16:p + 16 + n]))
            p += 16 + n
            if off + size > len(d):
                sys.exit("damaged archive")
            yield name, off, size, fkey
    else:
        sys.exit(f"unsupported RGSS archive version {version}")


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip())
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.dirname(os.path.abspath(src))
    with open(src, "rb") as f:
        d = f.read()
    count = written = 0
    for name, off, size, key in entries(d):
        parts = [p for p in name.replace(b"\\", b"/").split(b"/") if p not in (b"", b".", b"..")]
        if not parts:
            continue
        path = os.path.join(os.fsencode(out), *parts)
        data = decrypt(d[off:off + size], key)
        count += 1
        if os.path.exists(path) and os.path.getsize(path) == len(data):
            with open(path, "rb") as f:
                if f.read() == data:
                    continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        written += 1
    print(f"{src}: {count} files, {written} written to {out}")


if __name__ == "__main__":
    main()
