#!/usr/bin/env bash
# Linker for Windows targets without MinGW-GCC or clang: Rust's bundled rust-lld
# in MinGW mode against an MSYS2 sysroot (see msys2-sysroot.sh). Puts the startup
# object crt2.o first, like the GCC/clang driver does (else atexit is linked twice).
#
#   x86_64-pc-windows-gnu (MSYS2 mingw64):
#     MINGW_SYSROOT=<dest>/mingw64 RUSTFLAGS="-C linker=tools/mingw-lld.sh -C linker-flavor=ld.lld" \
#       cargo build --target x86_64-pc-windows-gnu
#   aarch64-pc-windows-gnullvm (MSYS2 clangarm64): the same with
#     MINGW_SYSROOT=<dest>/clangarm64
#   i686-pc-windows-gnu (MSYS2 mingw32): MINGW_SYSROOT=<dest>/mingw32
#   (the CPU is taken from the sysroot: clangarm64 = ARM64, mingw32 = x86, else x86_64)
set -euo pipefail
: "${MINGW_SYSROOT:?set MINGW_SYSROOT to the mingw64 or clangarm64 folder of an MSYS2 sysroot}"
lld="$(rustc --print sysroot)/lib/rustlib/$(rustc -vV | sed -n 's/^host: //p')/bin/rust-lld"
[[ "${1:-}" == "-flavor" ]] && shift 2   # rustc passes "-flavor gnu"; we set it ourselves
case "$(basename "$MINGW_SYSROOT")" in
    clangarm64)
        # LLVM toolchain: compiler-rt instead of libgcc
        builtins="$(ls "$MINGW_SYSROOT"/lib/clang/*/lib/windows/libclang_rt.builtins-aarch64.a | sort -V | tail -1)"
        # Rust's std wants libunwind: link the static one (searched first from
        # a folder of its own), not libunwind.dll, which Windows doesn't have
        static="$MINGW_SYSROOT/../static-libs"
        mkdir -p "$static"
        ln -sf "$MINGW_SYSROOT/lib/libunwind.a" "$static/libunwind.a"
        exec "$lld" -flavor gnu -m arm64pe "$MINGW_SYSROOT/lib/crt2.o" -L "$static" -L "$MINGW_SYSROOT/lib" "$@" "$builtins"
        ;;
    mingw32)
        # 32-bit x86 (i686-pc-windows-gnu)
        gcclib="$(ls -d "$MINGW_SYSROOT"/lib/gcc/i686-w64-mingw32/* | sort -V | tail -1)"
        exec "$lld" -flavor gnu -m i386pe "$MINGW_SYSROOT/lib/crt2.o" -L "$MINGW_SYSROOT/lib" -L "$gcclib" "$@"
        ;;
    *)
        gcclib="$(ls -d "$MINGW_SYSROOT"/lib/gcc/x86_64-w64-mingw32/* | sort -V | tail -1)"
        exec "$lld" -flavor gnu -m i386pep "$MINGW_SYSROOT/lib/crt2.o" -L "$MINGW_SYSROOT/lib" -L "$gcclib" "$@"
        ;;
esac
