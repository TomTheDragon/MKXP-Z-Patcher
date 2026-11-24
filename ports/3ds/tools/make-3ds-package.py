#!/usr/bin/env python3
"""MKXP-Z 3DS: a game's folder for the SD card, in one go.

    make-3ds-package.py [--icon <picture>] [--soundfont <file.sf2>] [--keep-audio]
                        [--app <mkxpz.3dsx>] <game folder> <output folder>

The output folder (made new; copy it into sdmc:/mkxpz/, where the one app
"MKXP-Z 3DS.3dsx" lists the games) gets:
  game.smdh     the game's title and icon for that list (make-package.py);
                with --app also <Title>.3dsx, the game's own app (the
                engine with that title and icon, started from the folder)
  Game.data     the game's files in one pack, read by the engine directly:
                 Data, Graphics (images as .mkxi, convert-images.py; no
                 PNGs), Audio (as .mkxa, DSP-ADPCM, convert-audio.py: MIDI
                 rendered with --soundfont or an installed SoundFont; not
                 with --keep-audio) and, if the game has an RGSS archive
                 (Game.rgssad / .rgss2a / .rgss3a), its contents
                 (extract-archive.py): nothing is decrypted on the 3DS
  Game.ini, Fonts/ and text files (.txt, .ini: games write some) as they are
Left out: Game.exe and other programs, DLLs, the archive itself, editor
project files, saves, zip files, hidden files, the PC patcher's mkxp-z
folder, Photoshop files, manuals
(PDF, HTML), Thumbs.db / desktop.ini, uninstallers. Other files in the
game's folder itself stay plain (games write some).

Pack format (engine/pack.c): "MKXPZPK1", u32 count, u32 index length;
per file (sorted by the lowercase name) u32 offset, u32 size, u16 name
length, the name (as the game has it, "/", relative to the game folder);
then the files.

Needs ImageMagick ("magick", or MAGICK), lz4 ("lz4", or LZ4), smdhtool
(devkitPro, or SMDHTOOL), and for the sound ffmpeg and a C compiler.
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


SKIP_EXT = {".exe", ".dll", ".rgssad", ".rgss2a", ".rgss3a", ".rxproj", ".rvproj", ".rvproj2", ".lnk", ".url",
            ".ppdb", ".zip", ".7z", ".rar", ".psd", ".pdf", ".html", ".htm", ".ifl"}
SKIP_NAMES = {"thumbs.db", "desktop.ini"}
SKIP_DIRS_ANYWHERE = set()
PLAIN_EXT = {".txt", ".ini", ".cfg", ".json"}
SKIP_DIRS = {"mkxp-z"}


def is_save(name):
    low = name.lower()
    return low.startswith("save") and (low.endswith(".rxdata") or low.endswith(".rvdata") or low.endswith(".rvdata2"))


def main():
    app = []
    args = sys.argv[1:]
    icon = []
    soundfont = None
    keep_audio = False
    while args and args[0].startswith("--"):
        if args[0] == "--icon" and len(args) >= 2:
            icon, args = ["--icon", args[1]], args[2:]
        elif args[0] == "--soundfont" and len(args) >= 2:
            soundfont, args = args[1], args[2:]
        elif args[0] == "--keep-audio":
            keep_audio, args = True, args[1:]
        elif args[0] == "--app" and len(args) >= 2:
            app, args = ["--app", os.path.abspath(args[1])], args[2:]
        else:
            sys.exit(__doc__.strip())
    if len(args) == 3 and args[1].lower().endswith(".3dsx"):
        args = [args[0], args[2]]  # (the earlier form: the engine in the middle, no longer needed)
    if len(args) != 2:
        sys.exit(__doc__.strip())
    game, out = (os.path.abspath(a) for a in args)
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
            if name.startswith("."):
                continue
            if os.path.isdir(src):
                if low not in SKIP_DIRS:
                    shutil.copytree(src, os.path.join(stage, name))
            elif os.path.splitext(low)[1] not in SKIP_EXT and not is_save(name):
                shutil.copy2(src, os.path.join(stage, name))
        for name in os.listdir(game):
            if os.path.splitext(name.lower())[1] in (".rgssad", ".rgss2a", ".rgss3a"):
                sys.argv = ["extract-archive.py", os.path.join(game, name), stage]
                extract.main()
        # (a game whose .exe has another name, e.g. "Pocket Mirror.exe": its
        # .ini is the engine's Game.ini)
        base = tool("make-package").game_base(game)
        if base != "game":
            for name in os.listdir(stage):
                if name.lower() == base + ".ini":
                    os.rename(os.path.join(stage, name), os.path.join(stage, "Game.ini"))
        # 2. images converted (.mkxi next to each PNG), sounds (.mkxa)
        if os.path.isdir(os.path.join(stage, "Graphics")):
            convert.main(stage)
        if not keep_audio:
            tool("convert-audio").main(stage, soundfont)
        # 3. plain files and the pack
        os.makedirs(out)
        files = []
        for dirpath, dirnames, filenames in os.walk(stage):
            dirnames[:] = sorted(d for d in dirnames if d.lower() not in SKIP_DIRS_ANYWHERE)
            rel_dir = os.path.relpath(dirpath, stage)
            for name in sorted(filenames):
                rel = os.path.normpath(os.path.join(rel_dir, name))
                low = rel.lower().replace(os.sep, "/")
                ext = os.path.splitext(low)[1]
                src = os.path.join(dirpath, name)
                top = low.split("/")[0]
                if os.path.basename(low) in SKIP_NAMES or ext in SKIP_EXT or os.path.basename(low).startswith("uninstall"):
                    continue
                # (files in the game folder itself stay plain: games write
                # some, e.g. Heartache 101's "records", opened with "r+")
                if top == "fonts" or ext in PLAIN_EXT or "/" not in low:
                    dst = os.path.join(out, rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
                    continue
                if top == "graphics" and ext in (".png", ".jpg", ".jpeg", ".bmp") and \
                        os.path.exists(os.path.splitext(src)[0] + ".mkxi"):
                    continue  # (the .mkxi is used)
                if top == "audio" and ext != ".mkxa" and os.path.exists(os.path.splitext(src)[0] + ".mkxa"):
                    continue  # (the .mkxa is used)
                files.append((low, rel.replace(os.sep, "/"), src))
        files.sort()
        index = b""
        sizes = [os.path.getsize(src) for _, _, src in files]
        # (sorted by the lowercase name, which the engine looks up; stored
        # as the game names them, for folder listings)
        names = [shown.encode("utf-8") for _, shown, _ in files]
        ilen = sum(10 + len(n) for n in names)
        at = 16 + ilen
        for n, size in zip(names, sizes):
            index += struct.pack("<IIH", at, size, len(n)) + n
            at += size
        with open(os.path.join(out, "Game.data"), "wb") as f:
            f.write(b"MKXPZPK1" + struct.pack("<II", len(files), ilen) + index)
            for _, _, src in files:
                with open(src, "rb") as s:
                    shutil.copyfileobj(s, f)
        # 4. game.smdh (title and icon from the original folder: Game.exe)
        sys.argv = ["make-package.py"] + icon + app + [game, out]
        tool("make-package").main()
        print(f"{out}: Game.data with {len(files)} files ({at // 1024 // 1024} MB)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
