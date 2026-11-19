#!/usr/bin/env python3
"""An old glibc to link the Linux patcher against, so it runs on older systems.

Downloads Ubuntu 20.04's glibc packages (glibc 2.31: Ubuntu 20.04, Debian 11
and newer; checksums pinned below) and unpacks them into a folder, with
absolute symbolic links made relative so they stay inside it. Only the C
library comes from there; GTK is linked from the build system (its own
version requirements count at run time on the user's system, not here).
See tools/oldglibc-link.sh.

    tools/glibc-sysroot.py <out dir> amd64|i386

Mirror: UBUNTU_MIRROR (default http://archive.ubuntu.com/ubuntu; the
checksums make plain http safe).
"""
import hashlib
import io
import lzma
import os
import sys
import tarfile
import urllib.request

VERSION = "2.31-0ubuntu9.18"
PACKAGES = {
    "amd64": {
        "libc6": "1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0",
        "libc6-dev": "11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a",
    },
    "i386": {
        "libc6": "635b0dd37b832ed3852ea1885d7edc56b73a8f13d9fc474a03af88eddbd82974",
        "libc6-dev": "68def210ef1f7b8fbb1a6bd3ab88309a866e5ea57e74365a01fa32d26bc4c595",
    },
}


def ar_members(data):
    """The members of an ar archive (a .deb): name -> bytes."""
    if data[:8] != b"!<arch>\n":
        raise ValueError("not a .deb (ar) file")
    out, pos = {}, 8
    while pos + 60 <= len(data):
        name = data[pos:pos + 16].decode().strip().rstrip("/")
        size = int(data[pos + 48:pos + 58].decode().strip())
        out[name] = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    return out


def unpack(deb, out):
    members = ar_members(deb)
    name = next((n for n in members if n.startswith("data.tar")), None)
    if name is None:
        raise ValueError("no data.tar in the package")
    raw = members[name]
    if name.endswith(".xz"):
        raw = lzma.decompress(raw)
    elif not name.endswith(".tar"):
        raise ValueError(f"unsupported {name}")
    root = os.path.realpath(out)
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for m in tar.getmembers():
            rel = os.path.normpath(m.name.lstrip("./"))
            if rel in (".", "") or rel.startswith(".."):
                continue
            dest = os.path.join(root, rel)
            if not os.path.realpath(os.path.dirname(dest)).startswith(root):
                raise ValueError(f"unsafe path {m.name}")
            if m.isdir():
                os.makedirs(dest, exist_ok=True)
            elif m.issym():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                target = m.linkname
                if target.startswith("/"):
                    # inside the folder, not the build system's own /lib
                    target = os.path.relpath(os.path.join(root, target.lstrip("/")), os.path.dirname(dest))
                if os.path.lexists(dest):
                    os.remove(dest)
                os.symlink(target, dest)
            elif m.isfile():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with tar.extractfile(m) as f, open(dest, "wb") as o:
                    o.write(f.read())
                os.chmod(dest, m.mode & 0o755)


def main(out, arch):
    if arch not in PACKAGES:
        sys.exit(f"unknown architecture {arch} (amd64 or i386)")
    stamp = os.path.join(out, ".glibc-sysroot")
    want = f"{VERSION} {arch}\n"
    if os.path.isfile(stamp) and open(stamp).read() == want:
        return
    mirror = os.environ.get("UBUNTU_MIRROR", "http://archive.ubuntu.com/ubuntu").rstrip("/")
    cache = os.path.join(out, ".debs")
    os.makedirs(cache, exist_ok=True)
    for pkg, sha in PACKAGES[arch].items():
        file = f"{pkg}_{VERSION}_{arch}.deb"
        path = os.path.join(cache, file)
        data = open(path, "rb").read() if os.path.isfile(path) else None
        if data is None or hashlib.sha256(data).hexdigest() != sha:
            url = f"{mirror}/pool/main/g/glibc/{file}"
            print(f"  downloading {url}", file=sys.stderr)
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            if hashlib.sha256(data).hexdigest() != sha:
                sys.exit(f"{file}: checksum mismatch")
            with open(path, "wb") as f:
                f.write(data)
        unpack(data, out)
    with open(stamp, "w") as f:
        f.write(want)
    print(f"glibc {VERSION} ({arch}) ready: {out}", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
