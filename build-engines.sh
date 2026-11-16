#!/usr/bin/env bash
# Build mkxp-z engines into engines/<target>/ ahead of time. The patcher also
# builds missing engines by itself when you patch a game; this is a shortcut.
#
#   ./build-engines.sh                      # Linux 64-bit + 32-bit, Windows 32-bit + 64-bit
#   ./build-engines.sh windows,linux32      # some (names as in the patcher's --platform)
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
PATCHER="$ROOT/dist/mkxpz-patcher-linux-$(uname -m)"
[[ -x "$PATCHER" ]] || PATCHER="$ROOT/patcher-rs/target/release/mkxpz-patcher"
if [[ ! -x "$PATCHER" ]]; then
    echo "Patcher not built yet; building it first ..." >&2
    "$ROOT/build-patcher.sh" "linux-$(uname -m)"
    PATCHER="$ROOT/dist/mkxpz-patcher-linux-$(uname -m)"
fi
exec "$PATCHER" --build-engines "${1:-linux,linux32,windows,windows64}"
