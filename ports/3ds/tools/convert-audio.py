#!/usr/bin/env python3
"""MKXP-Z 3DS: convert a game's sound ahead of time (-> .mkxa, DSP-ADPCM).

    convert-audio.py [--soundfont <file.sf2>] <game folder>

For each file in the game's Audio folder (Ogg, WAV, MP3, MIDI...) this
writes a .mkxa next to it (tools/mkxa.c has the format): a quarter of
16-bit PCM, decoded by the 3DS's sound chip (sound effects) or with
hardly any work (music), instead of Vorbis or MP3 decoding on the 3DS's
slow processor. Music above 32728 Hz (the 3DS's own output rate) is
resampled to it; sound effects keep their rate. RPG Maker's loop points
are kept: the Ogg tags LOOPSTART / LOOPLENGTH, and MIDI's controller 111.

MIDI is rendered here with a SoundFont (the 3DS is too slow to synthesize
it while playing): --soundfont, or one installed (/usr/share/soundfonts:
FluidR3_GM.sf2 first). Without one the MIDI files stay as they are (the
engine plays them with soundfont.sf2 in sdmc:/3ds/mkxpz/, slowly).

The engine prefers .mkxa files; make-3ds-package.py leaves out the
originals that have one. Needs ffmpeg / ffprobe and a C compiler ("cc",
or CC) for tools/mkxa.c. Files already converted (newer than their
original) are skipped.
"""
import concurrent.futures
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DSP_RATE = 32728
AUDIO_EXT = (".ogg", ".oga", ".wav", ".mp3", ".mid", ".midi", ".wma", ".flac", ".m4a")


def build_mkxa(work):
    """tools/mkxa.c compiled for this PC"""
    exe = os.path.join(work, "mkxa")
    cc = shlex.split(os.environ.get("CC", "cc"))
    r = subprocess.run(cc + ["-O2", "-o", exe, os.path.join(HERE, "mkxa.c"), "-lm"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        sys.exit("can't compile mkxa.c:\n" + r.stderr.decode(errors="replace"))
    return exe


def find_soundfont():
    home = os.path.expanduser("~")
    roots = ["/usr/share/soundfonts", "/usr/share/sounds/sf2", "/usr/local/share/soundfonts",
             os.path.join(home, ".local/share/soundfonts"), "/run/host/usr/share/soundfonts"]
    preferred = ["FluidR3_GM.sf2", "default-GM.sf2", "default.sf2", "TimGM6mb.sf2", "GeneralUser GS.sf2"]
    for r in roots:
        for n in preferred:
            p = os.path.join(r, n)
            if os.path.isfile(p):
                return os.path.realpath(p)
    return None


def probe(path):
    """(rate, channels, loop start, loop length) of a file (ffprobe)"""
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                        "stream=sample_rate,channels:format_tags:stream_tags", "-of", "default=nw=1", path],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    info = {}
    for line in r.stdout.decode(errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            info[k.strip().lower().replace("tag:", "")] = v.strip()
    def num(k):
        try:
            return int(info.get(k, "") or -1)
        except ValueError:
            return -1
    return num("sample_rate"), num("channels"), num("loopstart"), num("looplength")


def convert_one(mkxa, path, music, soundfont):
    """the .mkxa for one file; (path, message or None)"""
    dst = os.path.splitext(path)[0] + ".mkxa"
    low = path.lower()
    with tempfile.TemporaryDirectory(prefix="mkxa-") as tmp:
        pcm = os.path.join(tmp, "pcm")
        loop_start, loop_end = 0, 0
        if low.endswith((".mid", ".midi")):
            if not soundfont:
                return path, "MIDI kept (no SoundFont)"
            with open(pcm, "wb") as f:
                r = subprocess.run([mkxa, "midi", soundfont, path, str(DSP_RATE)], stdout=f, stderr=subprocess.PIPE)
            if r.returncode:
                return path, "MIDI not rendered: " + r.stderr.decode(errors="replace").strip()
            for line in r.stderr.decode(errors="replace").splitlines():
                if line.startswith("loop "):
                    loop_start = int(line.split()[1])
            rate, channels = DSP_RATE, 2
        else:
            src_rate, channels, ls, ll = probe(path)
            if src_rate <= 0 or channels <= 0:
                return path, "not a sound file (kept)"
            channels = min(channels, 2)
            rate = DSP_RATE if music and src_rate > DSP_RATE else src_rate
            if ls >= 0 and ll > 0:
                loop_start = ls * rate // src_rate
                loop_end = (ls + ll) * rate // src_rate
            r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-ac", str(channels), "-ar", str(rate),
                                "-f", "s16le", pcm], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode:
                return path, "not decoded: " + r.stderr.decode(errors="replace").strip()[:200]
        with open(pcm, "rb") as f:
            r = subprocess.run([mkxa, "encode", str(rate), str(channels), str(loop_start), str(loop_end), dst],
                               stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode:
            return path, "not encoded: " + r.stderr.decode(errors="replace").strip()
    return path, None


def main(game, soundfont=None):
    root = os.path.join(game, "Audio")
    if not os.path.isdir(root):
        return
    if soundfont is None:
        soundfont = find_soundfont()
    todo = []
    for dirpath, _, files in os.walk(root):
        music = os.path.basename(dirpath).lower() in ("bgm", "bgs", "me")
        for name in sorted(files):
            if not name.lower().endswith(AUDIO_EXT):
                continue
            src = os.path.join(dirpath, name)
            dst = os.path.splitext(src)[0] + ".mkxa"
            if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
                continue
            todo.append((src, music))
    if not todo:
        print("audio: nothing to convert")
        return
    with tempfile.TemporaryDirectory(prefix="mkxa-build-") as work:
        mkxa = build_mkxa(work)
        before = after = 0
        notes = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for path, msg in pool.map(lambda t: convert_one(mkxa, t[0], t[1], soundfont), todo):
                if msg:
                    notes.append(f"{os.path.relpath(path, game)}: {msg}")
                    continue
                before += os.path.getsize(path)
                after += os.path.getsize(os.path.splitext(path)[0] + ".mkxa")
        for n in notes[:20]:
            print("audio:", n, file=sys.stderr)
        print(f"converted {len(todo) - len(notes)} sounds: {before // 1024} KB -> {after // 1024} KB .mkxa"
              + (f" (MIDI with {os.path.basename(soundfont)})" if soundfont else ""))


if __name__ == "__main__":
    args = sys.argv[1:]
    sf = None
    if len(args) >= 2 and args[0] == "--soundfont":
        sf, args = args[1], args[2:]
    if len(args) != 1:
        sys.exit(__doc__)
    main(args[0], sf)
