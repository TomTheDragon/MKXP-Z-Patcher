#!/usr/bin/env python3
"""MKXP-Z 3DS: a game's folder for the SD card, in one go.

    make-3ds-package.py [--icon <picture>] <game folder> <mkxpz.3dsx> <output folder>

The output folder (made new; copy it into sdmc:/3ds/) gets:
  <Title>.3dsx   the engine with the game's title and icon (make-package.py)
  Game.data     the game's files in one pack, read by the engine directly:
                 Data, Graphics (images as .mkxi, convert-images.py; no
                 PNGs), Audio, Movies... and, if the game has an RGSS archive
                 (Game.rgssad / .rgss2a / .rgss3a), its contents
                 (extract-archive.py): nothing is decrypted on the 3DS
  Game.ini, Fonts/ and text files (.txt, .ini: games write some) as they are
Left out: Game.exe and other programs, DLLs, the archive itself, editor
project files, saves, the PC patcher's mkxp-z folder.

Pack format (engine/pack.c): "MKXPZPK1", u32 count, u32 index length;
per file (sorted by name) u32 offset, u32 size, u16 name length, the name
(lowercase, "/", relative to the game folder); then the files.

Needs ImageMagick ("magick", or MAGICK), lz4 ("lz4", or LZ4) and smdhtool
(devkitPro, or SMDHTOOL).
"""
import importlib.util
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def tool(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


SKIP_EXT = {".exe", ".dll", ".rgssad", ".rgss2a", ".rgss3a", ".rxproj", ".rvproj", ".rvproj2", ".lnk", ".url"}
PLAIN_EXT = {".txt", ".ini", ".cfg", ".json"}
SKIP_DIRS = {"mkxp-z"}


def is_save(name):
    low = name.lower()
    return low.startswith("save") and (low.endswith(".rxdata") or low.endswith(".rvdata") or low.endswith(".rvdata2"))


def main():
    args = sys.argv[1:]
    icon = []
    if len(args) >= 2 and args[0] == "--icon":
        icon, args = ["--icon", args[1]], args[2:]
    if len(args) != 3:
        sys.exit(__doc__.strip())
    game, engine, out = (os.path.abspath(a) for a in args)
    if os.path.exists(out):
        sys.exit(f"{out} exists already (a new folder is made)")
    extract = tool("extract-archive")
    convert = tool("convert-images")
    work = tempfile.mkdtemp(prefix="mkxpz-pkg-")
    try:
        # 1. everything of the game in one staging tree (the archive unpacked)
        stage = os.path.join(work, "game")
        os.makedirs(stage)
        for name in sorted(os.listdir(game)):
            src = os.path.join(game, name)
            low = name.lower()
            if os.path.isdir(src):
                if low not in SKIP_DIRS:
                    shutil.copytree(src, os.path.join(stage, name))
            elif os.path.splitext(low)[1] not in SKIP_EXT and not is_save(name):
                shutil.copy2(src, os.path.join(stage, name))
        for name in os.listdir(game):
            if os.path.splitext(name.lower())[1] in (".rgssad", ".rgss2a", ".rgss3a"):
                sys.argv = ["extract-archive.py", os.path.join(game, name), stage]
                extract.main()
        # 2. images converted (.mkxi next to each PNG)
        if os.path.isdir(os.path.join(stage, "Graphics")):
            convert.main(stage)
        # 3. plain files and the pack
        os.makedirs(out)
        files = []
        for dirpath, dirnames, filenames in os.walk(stage):
            dirnames.sort()
            rel_dir = os.path.relpath(dirpath, stage)
            for name in sorted(filenames):
                rel = os.path.normpath(os.path.join(rel_dir, name))
                low = rel.lower().replace(os.sep, "/")
                ext = os.path.splitext(low)[1]
                src = os.path.join(dirpath, name)
                top = low.split("/")[0]
                if top == "fonts" or ext in PLAIN_EXT or "/" not in low:
                    dst = os.path.join(out, rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
                    continue
                if top == "graphics" and ext in (".png", ".jpg", ".bmp") and \
                        os.path.exists(os.path.splitext(src)[0] + ".mkxi"):
                    continue  # (the .mkxi is used)
                files.append((low, src))
        files.sort()
        index = b""
        sizes = [os.path.getsize(src) for _, src in files]
        names = [low.encode("utf-8") for low, _ in files]
        ilen = sum(10 + len(n) for n in names)
        at = 16 + ilen
        for n, size in zip(names, sizes):
            index += struct.pack("<IIH", at, size, len(n)) + n
            at += size
        with open(os.path.join(out, "Game.data"), "wb") as f:
            f.write(b"MKXPZPK1" + struct.pack("<II", len(files), ilen) + index)
            for _, src in files:
                with open(src, "rb") as s:
                    shutil.copyfileobj(s, f)
        # 4. the .3dsx (title and icon from the original folder: Game.exe)
        sys.argv = ["make-package.py"] + icon + [game, engine, out]
        tool("make-package").main()
        print(f"{out}: Game.data with {len(files)} files ({at // 1024 // 1024} MB)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
