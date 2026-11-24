#!/bin/sh
# Cross-builds the addon for aarch64 and produces the release artifact.
#
#   tools/cross-aarch64.sh [sysroot] [version]
#
# The sysroot comes from tools/make-sysroot.sh: the target's headers,
# link-time .so files and pkg-config metadata.
#
# NOTHING HERE IS VERIFIED BY RUNNING. There is no aarch64 machine to test on,
# and emulating one only proves the loader works. What the checks below do
# prove is that the artifact is aarch64, links the same libraries as the
# x86_64 build, exports the symbol fcitx5 loads, and imports only symbols
# those libraries define on the target.
set -eu

sysroot=${1:?usage: cross-aarch64.sh <sysroot> [version]}
version=${2:?usage: cross-aarch64.sh <sysroot> [version]}
root=$(cd "$(dirname "$0")/.." && pwd)

[ -d "$sysroot/usr/include/Fcitx5" ] || {
    echo "error: $sysroot does not look like an aarch64 sysroot with fcitx5" >&2
    echo "       build one with tools/make-sysroot.sh" >&2
    exit 1
}
[ -d "$sysroot/target-runtime" ] || {
    echo "error: $sysroot has no target-runtime; it predates tools/make-sysroot.sh" >&2
    exit 1
}

export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/pkgconfig
export PKG_CONFIG_PATH=

rm -rf "$root/build-aarch64"
meson setup "$root/build-aarch64" \
    --cross-file "$root/tools/cross-aarch64.ini" \
    --prefix=/usr --buildtype=release
ninja -C "$root/build-aarch64"

so="$root/build-aarch64/ainput.so"
file -b "$so" | grep -q aarch64 || { echo "error: not an aarch64 object" >&2; exit 1; }

# fcitx5 dlopen()s the addon and looks up this one symbol (found.md #14).
readelf -W --dyn-syms "$so" | awk '$7 != "UND" && $8 == "fcitx_addon_factory_instance"' | grep -q . || {
    echo "error: $so does not export fcitx_addon_factory_instance" >&2
    exit 1
}

# Compare the NEEDED list with the native build: a cross build that silently
# pulls in host-only libraries would fail on the user's machine, not here.
if [ -f "$root/build/ainput.so" ]; then
    # The dynamic loader differs by architecture by definition; everything
    # else must match, because the addon should use the same stack on both.
    aarch64-linux-gnu-objdump -p "$so" | sed -n 's/^ *NEEDED *//p' \
        | grep -v '^ld-linux' | sort > /tmp/ainput-needed-aarch64
    objdump -p "$root/build/ainput.so" | sed -n 's/^ *NEEDED *//p' \
        | grep -v '^ld-linux' | sort > /tmp/ainput-needed-x86_64
    if ! diff -u /tmp/ainput-needed-x86_64 /tmp/ainput-needed-aarch64; then
        echo "error: dependency lists differ between architectures" >&2
        exit 1
    fi
    echo "dependencies match the x86_64 build"
fi

# The link fails on an unresolved strong reference, but it resolves them
# against the cross toolchain's own glibc and libstdc++, which follow Arch and
# can be newer than what Arch Linux ARM ships. This check uses the target's
# libraries instead: the closest static answer to "does it load on an
# up-to-date aarch64 system".
"$root/tools/check-symbols.sh" "$so" "$sysroot/usr/lib" "$sysroot/target-runtime/usr/lib"

"$root/tools/make-release.sh" "$version" aarch64
