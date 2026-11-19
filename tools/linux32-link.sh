#!/usr/bin/env bash
# Linker for i686-unknown-linux-gnu on a 64-bit system without 32-bit -devel
# packages: the C compiler in 32-bit mode, against the link folder made by
# linux32-sysroot.sh (LINUX32_SYSROOT) and the system's 32-bit startup files.
#
# Use:  LINUX32_SYSROOT=<dir> CARGO_TARGET_I686_UNKNOWN_LINUX_GNU_LINKER=tools/linux32-link.sh \
#       cargo build --target i686-unknown-linux-gnu
set -euo pipefail
: "${LINUX32_SYSROOT:?set LINUX32_SYSROOT to a folder made by linux32-sysroot.sh}"
# With GLIBC_SYSROOT (glibc-sysroot.py ... i386), the C library and its startup
# files come from there instead (an old glibc, so the patcher runs on older
# systems too); GTK still comes from this system.
sr="$LINUX32_SYSROOT"
root="$(cat "$sr/root")"
lib32="${root%/}/usr/lib"
gcc32="$sr/gcc32"
if [[ -n "${GLIBC_SYSROOT:-}" ]]; then
    sysroot="$GLIBC_SYSROOT"
    crt="$GLIBC_SYSROOT/usr/lib/i386-linux-gnu"
    first=(-L"$GLIBC_SYSROOT/lib/i386-linux-gnu" -L"$crt")
else
    sysroot="$root"
    crt="$lib32"
    first=()
fi
# --allow-shlib-undefined: the GTK libraries' own dependencies (X11, Wayland,
# fontconfig, ...) needn't be found at link time; they're there at run time.
exec "${CC:-cc}" -m32 --sysroot="$sysroot" -nostartfiles \
    -B"$gcc32" "${first[@]}" -L"$sr/lib" -L"$gcc32" -L"$lib32" \
    "$crt/Scrt1.o" "$crt/crti.o" "$gcc32/crtbeginS.o" \
    "$@" \
    -Wl,--allow-shlib-undefined -Wl,-dynamic-linker,/lib/ld-linux.so.2 \
    "$gcc32/crtendS.o" "$crt/crtn.o"
