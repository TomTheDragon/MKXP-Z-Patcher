#!/usr/bin/env bash
# RUSTC_WRAPPER for building with an older rustc (e.g. 1.77, the last one for
# Windows 7) from a current cargo: drops the --check-cfg options that newer
# cargo always passes and older rustc doesn't know.
#   RUSTC=<old toolchain>/bin/rustc RUSTC_WRAPPER=tools/rustc-old.sh cargo build ...
set -euo pipefail
rustc="$1"; shift
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --check-cfg) shift 2 ;;
        --check-cfg=*) shift ;;
        *) args+=("$1"); shift ;;
    esac
done
exec "$rustc" "${args[@]}"
