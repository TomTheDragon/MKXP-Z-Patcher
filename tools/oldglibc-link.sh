#!/usr/bin/env bash
# Linker for the 64-bit Linux patcher (x86_64-unknown-linux-gnu) against the
# old glibc from glibc-sysroot.py, so it runs on systems from 2020 on (glibc
# 2.31) and not just on ones as new as the build system. The C library and its
# startup files come from GLIBC_SYSROOT, everything else (GTK, libgcc) from the
# build system. GTK's own needs are checked at run time on the user's system.
#
# Use:  GLIBC_SYSROOT=<dir> CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=tools/oldglibc-link.sh \
#       cargo build --target x86_64-unknown-linux-gnu
set -euo pipefail
: "${GLIBC_SYSROOT:?set GLIBC_SYSROOT to a folder made by glibc-sysroot.py (amd64)}"
sr="$GLIBC_SYSROOT"
cc="${CC:-cc}"
lib="$sr/usr/lib/x86_64-linux-gnu"
# build scripts and proc-macro libraries run here, at build time: link them normally
for a in "$@"; do
    [[ "$a" == -shared ]] && exec "$cc" "$@"
done
[[ "${CARGO_CRATE_NAME:-}" == build_script_* ]] && exec "$cc" "$@"
# where the build system's GTK is (searched after the old glibc)
gtk="$(pkg-config --variable=libdir gtk+-3.0 2>/dev/null || true)"
# --allow-shlib-undefined: GTK's libraries were made with the build system's
# newer glibc; they're linked against the user's at run time.
exec "$cc" --sysroot="$sr" -nostartfiles \
    -L"$sr/lib/x86_64-linux-gnu" -L"$lib" \
    "$lib/Scrt1.o" "$lib/crti.o" "$("$cc" -print-file-name=crtbeginS.o)" \
    "$@" \
    ${gtk:+-L"$gtk"} -Wl,--allow-shlib-undefined -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
    "$("$cc" -print-file-name=crtendS.o)" "$lib/crtn.o"
