#!/usr/bin/env bash
# Download MSYS2 MinGW packages (and everything they depend on) and unpack them
# into a folder, without MSYS2 or Windows: a "sysroot" to cross-link Windows
# programs on Linux (headers, import libraries, DLLs).
#
#   tools/msys2-sysroot.sh DEST REPO PACKAGE...
#   tools/msys2-sysroot.sh ~/sysroot mingw64 mingw-w64-x86_64-gtk3 mingw-w64-x86_64-crt
#
# REPO is an MSYS2 repository: mingw64 (x86_64, msvcrt) or mingw32 (i686).
# Packages are cached in DEST/.pkgcache; files land in DEST/<repo>/... like in MSYS2.
set -euo pipefail

dest="$1"; repo="$2"; shift 2
mirror="${MSYS2_MIRROR:-https://repo.msys2.org/mingw}"
cache="$dest/.pkgcache"
mkdir -p "$cache" "$dest"

db="$cache/$repo.db"
if [[ ! -s "$db" || -n "$(find "$db" -mmin +720 2>/dev/null)" ]]; then
    echo "Fetching package list $repo.db ..." >&2
    curl -fsSL --retry 3 -o "$db.part" "$mirror/$repo/$repo.db"
    mv "$db.part" "$db"
fi
index="$cache/$repo.index"
if [[ ! -s "$index" || "$db" -nt "$index" ]]; then
    # one line per package: NAME FILENAME DEPENDS(comma-separated) PROVIDES(comma-separated)
    tmp="$(mktemp -d)"
    tar -xf "$db" -C "$tmp"
    for d in "$tmp"/*/; do
        awk '
            /^%[A-Z]+%$/ { sec = $0; next }
            /^$/ { sec = ""; next }
            sec == "%NAME%" { name = $0 }
            sec == "%FILENAME%" { file = $0 }
            sec == "%DEPENDS%" { sub(/[<>=].*/, ""); deps = deps (deps ? "," : "") $0 }
            sec == "%PROVIDES%" { sub(/[<>=].*/, ""); prov = prov (prov ? "," : "") $0 }
            END { printf "%s %s %s %s\n", name, file, (deps ? deps : "-"), (prov ? prov : "-") }
        ' "$d/desc"
    done > "$index"
    rm -rf "$tmp"
fi

# resolve dependencies (breadth first, following "provides")
declare -A want=()
queue=("$@")
while [[ ${#queue[@]} -gt 0 ]]; do
    p="${queue[0]}"; queue=("${queue[@]:1}")
    [[ -n "${want[$p]:-}" ]] && continue
    line="$(awk -v p="$p" '$1 == p { print; exit }' "$index")"
    if [[ -z "$line" ]]; then
        line="$(awk -v p="$p" '{ n = split($4, a, ","); for (i = 1; i <= n; i++) if (a[i] == p) { print; exit } }' "$index")"
    fi
    if [[ -z "$line" ]]; then
        echo "warning: no package provides '$p'" >&2
        want[$p]=missing
        continue
    fi
    read -r name file deps _ <<< "$line"
    want[$p]=1
    [[ "$name" != "$p" ]] && want[$name]=1
    echo "$name $file" >> "$cache/.resolved.$$"
    [[ "$deps" == "-" ]] && continue
    IFS=',' read -ra ds <<< "$deps"
    queue+=("${ds[@]}")
done

sort -u "$cache/.resolved.$$" > "$cache/.resolved.sorted.$$"
rm -f "$cache/.resolved.$$"
n=$(wc -l < "$cache/.resolved.sorted.$$")
echo "$n packages" >&2
while read -r name file; do
    if [[ ! -s "$cache/$file" ]]; then
        echo "  downloading $file" >&2
        curl -fsSL --retry 3 -o "$cache/$file.part" "$mirror/$repo/$file"
        mv "$cache/$file.part" "$cache/$file"
    fi
    stamp="$dest/.installed/$file"
    if [[ ! -e "$stamp" ]]; then
        tar -xf "$cache/$file" -C "$dest" --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.PKGINFO' --exclude='.INSTALL'
        mkdir -p "$dest/.installed" && touch "$stamp"
    fi
done < "$cache/.resolved.sorted.$$"
rm -f "$cache/.resolved.sorted.$$"
echo "sysroot ready: $dest/$repo" >&2
