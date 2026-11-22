#!/usr/bin/env python3
"""MKXP-Z 3DS: the engine as a game's own homebrew app.

    make-package.py [--icon <picture>] <game folder> <mkxpz.3dsx> [<output folder>]

Writes <output folder>/<Title>.3dsx (default output: the game folder): the
engine with the game's title (Game.ini) and icon (Game.exe's, 48x48) in its
SMDH, which the Homebrew Launcher shows. The engine runs the game next to
the .3dsx, so each game gets its own folder on the SD card, e.g.
sdmc:/3ds/mkxpz/Dazed/Dazed.3dsx with the game's files beside it.

Needs smdhtool (devkitPro; or set SMDHTOOL) and ImageMagick ("magick", or
set MAGICK) for the icon. Without an icon in Game.exe the 3DS's default
homebrew icon is used. --icon: any picture ImageMagick reads instead
(squared and scaled to 48x48), e.g. for games that kept RPG Maker's
standard icon.
"""
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEVKITPRO = os.environ.get("DEVKITPRO", os.path.join(HERE, "..", "..", "..", "dist", "devkitpro"))


def game_title(game):
    """Game.ini's Title (Windows code page or UTF-8)"""
    for name in os.listdir(game):
        if name.lower() == "game.ini":
            raw = open(os.path.join(game, name), "rb").read()
            for enc in ("utf-8-sig", "cp1252", "cp932"):
                try:
                    text = raw.decode(enc)
                    break
                except UnicodeDecodeError:
                    continue
            m = re.search(r"^\s*Title\s*=\s*(.*?)\s*$", text, re.M | re.I)
            if m and m.group(1):
                return m.group(1)
    return os.path.basename(os.path.abspath(game))


def file_name(title):
    """a file name for the SD card (FAT): no reserved characters"""
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "", title).strip(" .")
    return name or "game"


def exe_icon(exe):
    """the main icon of a Windows .exe as an .ico file (bytes), or None"""
    d = open(exe, "rb").read()
    u16 = lambda o: struct.unpack_from("<H", d, o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    try:
        pe = u32(0x3C)
        if d[pe:pe + 4] != b"PE\0\0":
            return None
        nsec, optsize = u16(pe + 6), u16(pe + 20)
        opt = pe + 24
        dirs = opt + (96 if u16(opt) == 0x10B else 112)
        rsrc_rva = u32(dirs + 2 * 8)
        if not rsrc_rva:
            return None
        sections = [(u32(s + 12), max(u32(s + 8), u32(s + 16)), u32(s + 20))
                    for s in (opt + optsize + 40 * i for i in range(nsec))]

        def off(rva):
            for va, size, raw in sections:
                if va <= rva < va + size:
                    return rva - va + raw
            raise ValueError("rva")

        base = off(rsrc_rva)

        def entries(o):
            n = u16(base + o + 12) + u16(base + o + 14)
            return [(u32(base + o + 16 + 8 * i), u32(base + o + 20 + 8 * i)) for i in range(n)]

        def first_data(o):
            while o & 0x80000000:
                o = entries(o & 0x7FFFFFFF)[0][1]
            rva, size = u32(base + o), u32(base + o + 4)
            return d[off(rva):off(rva) + size]

        types = dict(entries(0))
        groups, icons = types.get(14), types.get(3)
        if groups is None or icons is None:
            return None
        group = first_data(entries(groups & 0x7FFFFFFF)[0][1])
        by_id = {i: o for i, o in entries(icons & 0x7FFFFFFF)}
        count = struct.unpack_from("<H", group, 4)[0]
        images = []
        for k in range(count):
            e = group[6 + 14 * k:6 + 14 * (k + 1)]
            ident = struct.unpack_from("<H", e, 12)[0]
            if ident in by_id:
                images.append((e[:12], first_data(by_id[ident])))
        if not images:
            return None
        head = struct.pack("<HHH", 0, 1, len(images))
        at = 6 + 16 * len(images)
        dirent, data = b"", b""
        for e, img in images:
            dirent += e[:8] + struct.pack("<II", len(img), at + len(data))
            data += img
        return head + dirent + data
    except (struct.error, ValueError, IndexError, KeyError):
        return None


def icon_png(game, tmp):
    """a 48x48 PNG of the game's icon, or None"""
    magick = shlex.split(os.environ.get("MAGICK", "magick"))
    for name in os.listdir(game):
        if name.lower() == "game.exe":
            ico = exe_icon(os.path.join(game, name))
            if not ico:
                return None
            ico = largest_last(ico)
            path = os.path.join(tmp, "icon.ico")
            open(path, "wb").write(ico)
            out = os.path.join(tmp, "icon.png")
            # the largest image of the icon (the last one now), scaled to 48x48
            last = struct.unpack_from("<H", ico, 4)[0] - 1
            r = subprocess.run(magick + [f"{path}[{last}]", "-alpha", "on", "-resize", "48x48", "-background",
                                         "none", "-gravity", "center", "-extent", "48x48", out],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode == 0 and os.path.exists(out):
                return out
            print("icon:", r.stderr.decode(errors="replace").strip(), file=sys.stderr)
    return None


def largest_last(ico):
    """the .ico's images sorted by size, the largest last (for -delete)"""
    n = struct.unpack_from("<H", ico, 4)[0]
    ents = [ico[6 + 16 * i:6 + 16 * (i + 1)] for i in range(n)]
    size = lambda e: (e[0] or 256) * (e[1] or 256) * 1000 + struct.unpack_from("<H", e, 6)[0]
    order = sorted(range(n), key=lambda i: size(ents[i]))
    head = struct.pack("<HHH", 0, 1, n)
    data_at = 6 + 16 * n
    dirent, data = b"", b""
    for i in order:
        e = ents[i]
        length, start = struct.unpack_from("<II", e, 8)
        dirent += e[:8] + struct.pack("<II", length, data_at + len(data))
        data += ico[start:start + length]
    return head + dirent + data


def set_smdh(path, smdh):
    """put an SMDH into a .3dsx (its extended header says where it is)"""
    with open(path, "r+b") as f:
        head = f.read(44)
        if head[:4] != b"3DSX" or struct.unpack_from("<H", head, 4)[0] < 44:
            sys.exit(f"{path}: not a .3dsx with an extended header")
        at, size = struct.unpack_from("<II", head, 32)
        if size != len(smdh):
            sys.exit(f"{path}: SMDH of {size} bytes, the new one has {len(smdh)}")
        f.seek(at)
        f.write(smdh)


def main():
    args = sys.argv[1:]
    own_icon = None
    if len(args) >= 2 and args[0] == "--icon":
        own_icon, args = args[1], args[2:]
    if len(args) not in (2, 3):
        sys.exit(__doc__.strip())
    game, engine = args[0], args[1]
    out_dir = args[2] if len(args) == 3 else game
    title = game_title(game)
    smdhtool = os.environ.get("SMDHTOOL") or shutil.which("smdhtool") or os.path.join(DEVKITPRO, "tools", "bin", "smdhtool")
    with tempfile.TemporaryDirectory() as tmp:
        if own_icon:
            icon = os.path.join(tmp, "own.png")
            subprocess.run(shlex.split(os.environ.get("MAGICK", "magick")) +
                           [own_icon + "[0]", "-alpha", "on", "-resize", "48x48", "-background", "none",
                            "-gravity", "center", "-extent", "48x48", icon], check=True)
        else:
            icon = icon_png(game, tmp)
        if not icon:
            print("no icon in Game.exe: the default homebrew icon", file=sys.stderr)
            icon = os.path.join(DEVKITPRO, "libctru", "default_icon.png")
        smdh = os.path.join(tmp, "game.smdh")
        subprocess.run([smdhtool, "--create", title[:63], "RPG Maker game (MKXP-Z 3DS)", "MKXP-Z Patcher", icon, smdh],
                       check=True, stdout=subprocess.DEVNULL)
        os.makedirs(out_dir, exist_ok=True)
        target = os.path.join(out_dir, file_name(title) + ".3dsx")
        shutil.copyfile(engine, target)
        set_smdh(target, open(smdh, "rb").read())
    print(f"{target}: \"{title}\"")


if __name__ == "__main__":
    main()
