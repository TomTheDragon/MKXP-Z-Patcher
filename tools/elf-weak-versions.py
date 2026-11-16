#!/usr/bin/env python3
"""Mark symbol version requirements as weak when only weak symbols use them.

Rust's standard library links some newer glibc functions weakly (e.g.
pidfd_spawnp from glibc 2.39) and falls back when they are missing. The linker
still records "needs GLIBC_2.39" as a hard requirement, so the program refuses
to start on older systems. glibc's loader skips missing versions flagged
VER_FLG_WEAK; this sets that flag on versions whose symbols are all weak.

    tools/elf-weak-versions.py <ELF file>
"""
import struct
import sys

VER_FLG_WEAK = 0x2
STB_WEAK = 2
SHT_DYNSYM = 11
SHT_GNU_VERSYM = 0x6FFFFFFF
SHT_GNU_VERNEED = 0x6FFFFFFE


def main(path):
    d = bytearray(open(path, "rb").read())
    if d[:4] != b"\x7fELF":
        sys.exit(f"{path}: not an ELF file")
    is64 = d[4] == 2
    e = "<" if d[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(e + "Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", d, 0x3A)
    else:
        shoff, = struct.unpack_from(e + "I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", d, 0x2E)

    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64:
            name, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from(e + "IIQQQQIIQQ", d, o)
        else:
            name, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from(e + "IIIIIIIIII", d, o)
        secs.append(dict(type=typ, off=off, size=size, link=link, entsize=entsize))

    def find(t):
        return next((s for s in secs if s["type"] == t), None)

    dynsym, versym, verneed = find(SHT_DYNSYM), find(SHT_GNU_VERSYM), find(SHT_GNU_VERNEED)
    if not (dynsym and versym and verneed):
        print(f"{path}: no symbol versions, nothing to do")
        return
    strtab = secs[verneed["link"]]["off"]

    def cstr(o):
        return bytes(d[o:d.index(0, o)]).decode()

    # version index -> [weak, strong] counts over the dynamic symbols
    use = {}
    n = dynsym["size"] // dynsym["entsize"]
    for i in range(1, n):
        so = dynsym["off"] + i * dynsym["entsize"]
        info = d[so + (4 if is64 else 12)]
        shndx, = struct.unpack_from(e + "H", d, so + (6 if is64 else 14))
        if shndx != 0:
            continue  # defined here, not a requirement
        ver, = struct.unpack_from(e + "H", d, versym["off"] + i * 2)
        ver &= 0x7FFF
        if ver < 2:
            continue
        c = use.setdefault(ver, [0, 0])
        c[0 if (info >> 4) == STB_WEAK else 1] += 1

    changed = []
    o = verneed["off"]
    while True:
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from(e + "HHIII", d, o)
        a = o + vn_aux
        for _ in range(vn_cnt):
            vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from(e + "IHHII", d, a)
            weak, strong = use.get(vna_other, [0, 0])
            if weak and not strong and not vna_flags & VER_FLG_WEAK:
                struct.pack_into(e + "H", d, a + 4, vna_flags | VER_FLG_WEAK)
                changed.append(f"{cstr(strtab + vn_file)}:{cstr(strtab + vna_name)}")
            if not vna_next:
                break
            a += vna_next
        if not vn_next:
            break
        o += vn_next

    if changed:
        open(path, "wb").write(d)
        print(f"{path}: optional now: {', '.join(changed)}")
    else:
        print(f"{path}: nothing to change")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
