#!/bin/sh
# Builds the aarch64 sysroot that tools/cross-aarch64.sh compiles and links
# against.
#
#   tools/make-sysroot.sh <dest> [mirror]
#
#   <dest>/sysroot/usr/             headers, link-time .so files and pkg-config
#                                   metadata of the Arch Linux ARM packages the
#                                   addon depends on
#   <dest>/sysroot/target-runtime/  the target's own glibc, libstdc++ and
#                                   libgcc; not on any link path, read only by
#                                   the symbol check in cross-aarch64.sh
#   <dest>/cache/, <dest>/db/       downloaded packages and sync databases,
#                                   reused by the next run
#
# Every dependency is linked dynamically, so this is what pkg-config, the
# compiler and the linker read, not a bootable system. pacman resolves the
# run-time dependency closure of the six packages the addon links, which
# includes every private pkg-config requirement of libcurl. No root is needed:
# pacman only prints the list and curl downloads what the cache lacks.
#
# glibc, libstdc++ and libgcc are kept out of usr/lib on purpose. The cross
# compiler compiles against its own copies of those headers and must link the
# matching libraries; given a second glibc, it would pick up that one's libc.so
# script, whose absolute paths point at the build host. The two sets can
# differ, because the cross toolchain follows Arch while Arch Linux ARM is
# rebuilt on its own schedule, so cross-aarch64.sh checks the finished binary
# against target-runtime instead.
#
# An existing <dest>/sysroot is not overwritten: a re-run would leave files of
# packages that dropped out of the closure behind. Move it away first.
#
# <dest> must survive a reboot. /tmp is tmpfs on Arch, and the sysroot for the
# 0.1.1 release was lost that way (found.md #44).
set -eu

dest=${1:?usage: make-sysroot.sh <dest> [mirror]}
mirror=${2:-http://os.archlinuxarm.org/aarch64}
# What the addon links. Anything a .pc file requires privately is a run-time
# dependency of one of them, so the closure covers it.
packages="fcitx5 libpulse curl libebur128 speexdsp libsoxr"
# Arch Linux ARM's aarch64 repositories.
repos="core extra alarm"

sysroot="$dest/sysroot"
cache="$dest/cache"
[ ! -e "$sysroot" ] || {
    echo "error: $sysroot exists; move it away and re-run" >&2
    exit 1
}
mkdir -p "$dest/db/sync" "$cache"

cat > "$dest/pacman.conf" <<EOF
[options]
Architecture = aarch64
# Packages are only extracted, never installed.
SigLevel = Never
[core]
Server = $mirror/core
[extra]
Server = $mirror/extra
[alarm]
Server = $mirror/alarm
EOF

for repo in $repos; do
    curl -fsSL --retry 3 -o "$dest/db/sync/$repo.db" "$mirror/$repo/$repo.db"
done

# Name and URL of every package in the closure, one per line.
pacman -Sp --config "$dest/pacman.conf" --dbpath "$dest/db" --noconfirm \
    --print-format '%n %l' $packages > "$dest/packages.txt"

# Versioned file names never change, so a cached file is a finished download.
# Eight at a time; the closure is about 175 packages.
xargs -d '\n' -P 8 -I{} sh -c '
    file="$1/$(basename "${2#* }")"
    [ -f "$file" ] && exit 0
    curl -fsSL --retry 3 -o "$file.part" "${2#* }" && mv "$file.part" "$file"
' sh "$cache" {} < "$dest/packages.txt"
# xargs stops at its own error count, so ask for every package again.
while read -r name url; do
    file="$cache/$(basename "$url")"
    [ -s "$file" ] || { echo "error: $file was not downloaded" >&2; exit 1; }
done < "$dest/packages.txt"

# Only what pkg-config, the compiler and the linker read. tar extracts a
# directory with everything below it, so the two directory entries are the
# whole selection. Names are taken literally: a member with "*" or "[" in its
# name is not a pattern, and a member outside the selection is not read.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# File capabilities in a package are stored in headers tar does not read.
taropts='--warning=no-unknown-keyword --quoting-style=literal'
listopts='--no-wildcards --verbatim-files-from'
mkdir -p "$sysroot/target-runtime"
while read -r name url; do
    case "$name" in
        glibc|libstdc++|libgcc) target="$sysroot/target-runtime" ;;
        *) target="$sysroot" ;;
    esac
    archive="$cache/$(basename "$url")"
    tar $taropts -tf "$archive" > "$work/members"
    # A hard link below them can point elsewhere in the package (glibc's
    # getconf profiles link to usr/bin/getconf); extract its target as well.
    # tar reads the archive once, so the list keeps archive order.
    : > "$work/wanted"
    tar $taropts -tvf "$archive" | awk '
        /^h/ && $6 ~ /^usr\/(include|lib)\// { sub(/^.* link to /, ""); print }' \
        | grep -v -E '^usr/(include|lib)/' >> "$work/wanted" || true
    grep -E '^usr/(include|lib)/$' "$work/members" >> "$work/wanted" || true
    awk 'NR == FNR { want[$0] = 1; next } $0 in want' \
        "$work/wanted" "$work/members" > "$work/list"
    [ -s "$work/list" ] || continue
    # shellcheck disable=SC2086 # the options are words
    tar -xpf "$archive" -C "$target" $taropts $listopts -T "$work/list"
done < "$dest/packages.txt"

[ -f "$sysroot/usr/lib/pkgconfig/libcurl.pc" ] &&
    [ -f "$sysroot/target-runtime/usr/lib/libc.so.6" ] || {
    echo "error: $sysroot is incomplete" >&2
    exit 1
}

echo "sysroot: $sysroot"
for name in $packages glibc libstdc++; do
    file=$(awk -v n="$name" '$1==n{print $2; exit}' "$dest/packages.txt" | sed 's|.*/||')
    printf '  %-10s %s\n' "$name" "$(printf '%s' "$file" | sed -e "s/^$name-//" -e 's/-[a-z0-9_]*\.pkg\.tar\..*$//')"
done
