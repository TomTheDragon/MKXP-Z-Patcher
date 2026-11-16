#!/usr/bin/env bash
# Build release binaries of the MKXP-Z Patcher:
#   dist/mkxpz-patcher-linux-x86_64         (needs GTK 3 dev files: Fedora gtk3-devel)
#   dist/mkxpz-patcher-linux-i686           (needs 32-bit GTK 3 and glibc: Fedora gtk3.i686 glibc-devel.i686;
#                                            the -devel packages of GTK aren't needed, see tools/linux32-sysroot.sh)
#   dist/mkxpz-patcher-windows-x86_64.exe   a single file: native Windows window, no DLLs needed
#   dist/mkxpz-patcher-windows-aarch64.exe  the same for Windows on ARM
#   dist/mkxpz-patcher-linux-aarch64        ARM Linux, 64/32-bit: command line only (no window),
#   dist/mkxpz-patcher-linux-armhf          statically linked (musl), runs on any ARM Linux
#
#   ./build-patcher.sh                  # all
#   ./build-patcher.sh windows-x86_64   # just some: linux-x86_64 linux-i686 windows-x86_64
#                                       #   windows-aarch64 linux-aarch64 linux-armhf
#
# The Windows build needs no MinGW or Windows: the MinGW C runtime comes from
# MSYS2's package repository (tools/msys2-sysroot.sh, cached in
# patcher-rs/target/msys2-sysroot), and Rust's bundled rust-lld links
# (tools/mingw-lld.sh). The patcher is 64-bit on Windows (the portable MSYS2 it
# uses to build engines needs 64-bit Windows); games still get 32-bit engines.
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
CRATE="$ROOT/patcher-rs"
DIST="$ROOT/dist"
ALL=(linux-x86_64 linux-i686 windows-x86_64 windows-aarch64 linux-aarch64 linux-armhf)
targets=("$@")
[[ ${#targets[@]} -gt 0 ]] || targets=("${ALL[@]}")
command -v cargo >/dev/null || { echo "error: cargo not found (install Rust via rustup)" >&2; exit 1; }

need_target() {
    if command -v rustup >/dev/null && ! rustup target list --installed | grep -qx "$1"; then
        rustup target add "$1"
    fi
}

# copy + rename: works even while the old binary is running ("text file busy")
install_bin() {
    mkdir -p "$(dirname "$2")"
    cp "$1" "$2.new" && mv -f "$2.new" "$2"
}

# The tiny Windows game launcher ("Game (mkxp-z).exe") gets embedded into every
# patcher build. It needs no Windows SDK or MinGW: linked with rust-lld (see launcher/.cargo).
echo "==> launcher (i686-pc-windows-msvc)"
need_target i686-pc-windows-msvc
(cd "$ROOT/launcher" && cargo build --release)
cp "$ROOT/launcher/target/i686-pc-windows-msvc/release/mkxpz-launcher.exe" "$CRATE/assets/mkxpz-launcher.exe"
echo "    ok ($(stat -c %s "$CRATE/assets/mkxpz-launcher.exe") bytes)"

failed=()
for t in "${targets[@]}"; do
    echo "==> $t"
    case "$t" in
        linux-x86_64|linux-i686)
            tr="${t#linux-}-unknown-linux-gnu"
            need_target "$tr"
            env=()
            ok=1
            if [[ "$t" == linux-i686 ]]; then
                # Link against the system's 32-bit runtime libraries through a
                # small link folder (no 32-bit -devel packages needed). Inside a
                # Flatpak sandbox the host system is at /run/host.
                root=/
                [[ ! -e /usr/lib/ld-linux.so.2 && -e /run/host/usr/lib/ld-linux.so.2 ]] && root=/run/host
                sysroot="$CRATE/target/linux32-sysroot"
                if "$ROOT/tools/linux32-sysroot.sh" "$sysroot" "$root"; then
                    env=(PKG_CONFIG_ALLOW_CROSS=1 PKG_CONFIG_LIBDIR="$sysroot/pkgconfig" PKG_CONFIG_PATH=
                         LINUX32_SYSROOT="$sysroot"
                         CARGO_TARGET_I686_UNKNOWN_LINUX_GNU_LINKER="$ROOT/tools/linux32-link.sh")
                else
                    ok=0
                fi
            fi
            if [[ $ok == 1 ]] && (cd "$CRATE" && env "${env[@]}" cargo build --release --target "$tr" --bin mkxpz-patcher); then
                install_bin "$CRATE/target/$tr/release/mkxpz-patcher" "$DIST/mkxpz-patcher-$t"
                # glibc functions Rust only uses when present mustn't make the
                # program require the newest glibc
                if command -v python3 >/dev/null; then
                    python3 -I "$ROOT/tools/elf-weak-versions.py" "$DIST/mkxpz-patcher-$t" | sed 's/^/    /'
                fi
                echo "    ok -> dist/mkxpz-patcher-$t ($(stat -c %s "$DIST/mkxpz-patcher-$t") bytes)"
            else
                [[ "$t" == linux-i686 ]] && echo "    (32-bit Linux needs: sudo dnf install gtk3.i686 glibc-devel.i686 libgcc.i686)"
                failed+=("$t")
            fi
            ;;
        windows-x86_64)
            tr=x86_64-pc-windows-gnu
            need_target "$tr"
            sysroot="$CRATE/target/msys2-sysroot"
            "$ROOT/tools/msys2-sysroot.sh" "$sysroot" mingw64 mingw-w64-x86_64-crt mingw-w64-x86_64-gcc >/dev/null
            if (cd "$CRATE" && \
                MINGW_SYSROOT="$sysroot/mingw64" \
                CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="$ROOT/tools/mingw-lld.sh" \
                CARGO_TARGET_X86_64_PC_WINDOWS_GNU_RUSTFLAGS="-C linker-flavor=ld.lld" \
                cargo build --release --target "$tr" --bin mkxpz-patcher); then
                install_bin "$CRATE/target/$tr/release/mkxpz-patcher.exe" "$DIST/mkxpz-patcher-windows-x86_64.exe"
                echo "    ok -> dist/mkxpz-patcher-windows-x86_64.exe ($(stat -c %s "$DIST/mkxpz-patcher-windows-x86_64.exe") bytes)"
            else
                failed+=("$t")
            fi
            ;;
        windows-aarch64)
            # Windows on ARM: LLVM-style MinGW (MSYS2's clangarm64 C runtime),
            # also linked by rust-lld
            tr=aarch64-pc-windows-gnullvm
            need_target "$tr"
            sysroot="$CRATE/target/msys2-sysroot"
            "$ROOT/tools/msys2-sysroot.sh" "$sysroot" clangarm64 mingw-w64-clang-aarch64-crt \
                mingw-w64-clang-aarch64-libunwind mingw-w64-clang-aarch64-compiler-rt >/dev/null
            if (cd "$CRATE" && \
                MINGW_SYSROOT="$sysroot/clangarm64" \
                CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER="$ROOT/tools/mingw-lld.sh" \
                CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_RUSTFLAGS="-C linker-flavor=ld.lld" \
                cargo build --release --target "$tr" --bin mkxpz-patcher); then
                install_bin "$CRATE/target/$tr/release/mkxpz-patcher.exe" "$DIST/mkxpz-patcher-windows-aarch64.exe"
                echo "    ok -> dist/mkxpz-patcher-windows-aarch64.exe ($(stat -c %s "$DIST/mkxpz-patcher-windows-aarch64.exe") bytes)"
            else
                failed+=("$t")
            fi
            ;;
        linux-aarch64|linux-armhf)
            # static musl build with Rust's bundled musl and rust-lld: no ARM
            # toolchain or libraries needed, runs on every ARM Linux. Without GTK
            # (its ARM libraries aren't available here), so command line only.
            [[ "$t" == linux-aarch64 ]] && tr=aarch64-unknown-linux-musl || tr=armv7-unknown-linux-musleabihf
            need_target "$tr"
            v="$(echo "$tr" | tr 'a-z-' 'A-Z_')"
            if (cd "$CRATE" && env "CARGO_TARGET_${v}_LINKER=rust-lld" \
                    "CARGO_TARGET_${v}_RUSTFLAGS=-C linker-flavor=ld.lld -C link-self-contained=yes -C target-feature=+crt-static" \
                    cargo build --release --no-default-features --target "$tr" --bin mkxpz-patcher); then
                install_bin "$CRATE/target/$tr/release/mkxpz-patcher" "$DIST/mkxpz-patcher-$t"
                echo "    ok -> dist/mkxpz-patcher-$t ($(stat -c %s "$DIST/mkxpz-patcher-$t") bytes, command line only)"
            else
                failed+=("$t")
            fi
            ;;
        *) echo "    unknown target (known: ${ALL[*]})"; failed+=("$t") ;;
    esac
done
if [[ ${#failed[@]} -gt 0 ]]; then
    echo "Failed: ${failed[*]}" >&2
    exit 1
fi
echo "Done."
