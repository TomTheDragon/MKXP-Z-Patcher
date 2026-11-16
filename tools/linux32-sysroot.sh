#!/usr/bin/env bash
# Link folder for building the 32-bit Linux patcher (i686-unknown-linux-gnu)
# against the 32-bit libraries already installed on this system, without the
# 32-bit -devel packages: only their runtime .so files are needed (Fedora:
# gtk3.i686 and glibc.i686, also pulled in by Steam), plus glibc-devel.i686
# for the startup files (crt1.o, libc_nonshared.a) and GCC's 32-bit files.
#
#   tools/linux32-sysroot.sh <out dir> [<root>]
#
# <root> is where the system's /usr is (default /; /run/host inside a Flatpak).
# Creates <out>/lib (unversioned .so links) and <out>/pkgconfig (minimal .pc
# files for the GTK crates' build scripts). See linux32-link.sh.
set -euo pipefail
out="${1:?usage: linux32-sysroot.sh <out dir> [<root>]}"
root="${2:-/}"
root="${root%/}"
lib32="$root/usr/lib"
[[ -e "$lib32/ld-linux.so.2" ]] || { echo "error: no 32-bit glibc in $lib32 (Fedora: glibc.i686)" >&2; exit 1; }
[[ -e "$lib32/Scrt1.o" ]] || { echo "error: no 32-bit startup files in $lib32 (Fedora: glibc-devel.i686)" >&2; exit 1; }

mkdir -p "$out/lib" "$out/pkgconfig"
out="$(cd "$out" && pwd)"

# pkg-config module, library name (lib<name>.so), runtime file
modules=(
    "glib-2.0 glib-2.0 libglib-2.0.so.0"
    "gobject-2.0 gobject-2.0 libgobject-2.0.so.0"
    "gio-2.0 gio-2.0 libgio-2.0.so.0"
    "gio-unix-2.0 gio-2.0 libgio-2.0.so.0"
    "gmodule-2.0 gmodule-2.0 libgmodule-2.0.so.0"
    "cairo cairo libcairo.so.2"
    "cairo-gobject cairo-gobject libcairo-gobject.so.2"
    "pango pango-1.0 libpango-1.0.so.0"
    "pangocairo pangocairo-1.0 libpangocairo-1.0.so.0"
    "gdk-pixbuf-2.0 gdk_pixbuf-2.0 libgdk_pixbuf-2.0.so.0"
    "atk atk-1.0 libatk-1.0.so.0"
    "gdk-3.0 gdk-3 libgdk-3.so.0"
    "gtk+-3.0 gtk-3 libgtk-3.so.0"
)
missing=()
for m in "${modules[@]}"; do
    read -r mod name file <<<"$m"
    if [[ ! -e "$lib32/$file" ]]; then
        missing+=("$file")
        continue
    fi
    ln -sfn "$lib32/$file" "$out/lib/lib$name.so"
    # the version the build scripts check: the 64-bit one's if known (same
    # package), else high enough for any minimum
    ver="$(env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_PATH pkg-config --modversion "$mod" 2>/dev/null || echo 99.0)"
    cat > "$out/pkgconfig/$mod.pc" <<EOF
Name: $mod
Description: $mod (32-bit, links to $lib32/$file)
Version: $ver
Libs: -L$out/lib -l$name
Cflags:
EOF
done
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: 32-bit libraries missing in $lib32: ${missing[*]} (Fedora: gtk3.i686)" >&2
    exit 1
fi
# glibc's small libraries that Rust's std links by name
for l in m pthread dl rt util; do
    f="$(ls "$lib32"/lib$l.so.[0-9]* 2>/dev/null | head -1 || true)"
    [[ -n "$f" ]] && ln -sfn "$f" "$out/lib/lib$l.so"
done
f="$(ls "$lib32"/libgcc_s.so.1 2>/dev/null || true)"
[[ -n "$f" ]] && ln -sfn "$f" "$out/lib/libgcc_s.so"
gcc32="$(ls -d "$root"/usr/lib/gcc/x86_64-*-linux*/*/32 2>/dev/null | sort -V | tail -1 || true)"
[[ -n "$gcc32" && -e "$gcc32/crtbeginS.o" ]] || { echo "error: no 32-bit GCC files (crtbeginS.o) under $root/usr/lib/gcc" >&2; exit 1; }
ln -sfn "$gcc32" "$out/gcc32"
echo "$root" > "$out/root"
