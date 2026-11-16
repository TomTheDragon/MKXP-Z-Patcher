#!/usr/bin/env bash
# Linker for i686-unknown-linux-gnu on a 64-bit system without 32-bit -devel
# packages: the C compiler in 32-bit mode, against the link folder made by
# linux32-sysroot.sh (LINUX32_SYSROOT) and the system's 32-bit startup files.
#
# Use:  LINUX32_SYSROOT=<dir> CARGO_TARGET_I686_UNKNOWN_LINUX_GNU_LINKER=tools/linux32-link.sh \
#       cargo build --target i686-unknown-linux-gnu
set -euo pipefail
: "${LINUX32_SYSROOT:?set LINUX32_SYSROOT to a folder made by linux32-sysroot.sh}"
sr="$LINUX32_SYSROOT"
root="$(cat "$sr/root")"
lib32="${root%/}/usr/lib"
gcc32="$sr/gcc32"
# --allow-shlib-undefined: the GTK libraries' own dependencies (X11, Wayland,
# fontconfig, ...) needn't be found at link time; they're there at run time.
exec "${CC:-cc}" -m32 --sysroot="$root" -nostartfiles \
    -B"$gcc32" -L"$sr/lib" -L"$gcc32" -L"$lib32" \
    "$lib32/Scrt1.o" "$lib32/crti.o" "$gcc32/crtbeginS.o" \
    "$@" \
    -Wl,--allow-shlib-undefined -Wl,-dynamic-linker,/lib/ld-linux.so.2 \
    "$gcc32/crtendS.o" "$lib32/crtn.o"
