#!/bin/sh
# Builds the release artifact consumed by the -bin package.
#
#   tools/make-release.sh <version> [arch]
#
# Produces dist/fcitx5-ainput-<version>-<arch>.tar.zst and its .sha256:
# a `meson install` DESTDIR image of exactly the files the addon installs, so
# the binary package ships the same tree that was tested rather than an
# ad-hoc copy of ainput.so.
#
# This links against the build host's libraries. `objdump -T` on the result
# records the minimum glibc the artifact needs — check it before publishing,
# because a binary built on Arch testing will not start on an older release.
set -eu

version=${1:?usage: make-release.sh <version> [arch]}
arch=${2:-$(uname -m)}

root=$(cd "$(dirname "$0")/.." && pwd)
# x86_64 uses the native build tree; aarch64 uses the cross build tree, so the
# two artifacts can be produced on one machine without rebuilding in place.
case "$arch" in
    x86_64)  build="$root/build" ;;
    aarch64) build="$root/build-aarch64" ;;
    *)       build="$root/build-$arch" ;;
esac
dist="$root/dist"
stage="$dist/stage-$arch"
name="fcitx5-ainput-$version-$arch"

if [ ! -f "$build/ainput.so" ]; then
    echo "error: $build/ainput.so is missing; build first" >&2
    echo "       x86_64 : meson setup build --buildtype=release && ninja -C build" >&2
    echo "       aarch64: tools/cross-aarch64.sh" >&2
    exit 1
fi

# The binary must actually be for the requested arch; a mixed-up build tree
# would publish a working x86_64 file under an aarch64 name.
actual=$(file -b "$build/ainput.so")
case "$arch:$actual" in
    x86_64:*x86-64*) ;;
    aarch64:*aarch64*) ;;
    *) echo "error: $build/ainput.so is not $arch: $actual" >&2; exit 1 ;;
esac

# The addon descriptor embeds the version, so a stale build tree would publish
# the wrong one.
recorded=$(sed -n 's/^Version=//p' "$build/ainput.conf" 2>/dev/null || true)
if [ -n "$recorded" ] && [ "$recorded" != "$version" ]; then
    echo "error: build/ainput.conf says Version=$recorded but you asked for $version" >&2
    echo "       reconfigure and rebuild: rm -rf build && meson setup build && ninja -C build" >&2
    exit 1
fi

rm -rf "$stage"
# PKG_CONFIG_SYSROOT_DIR is deliberately NOT unset for a cross build: doing so
# breaks the *-sys crates' pkg-config lookups and the install step re-links.
# The install-prefix side effect is instead avoided by staging relative to the
# build directory's own prefix, which the cross file sets to /usr.
DESTDIR="$stage" meson install -C "$build" >/dev/null

# meson prepends PKG_CONFIG_SYSROOT_DIR to install paths when it is set (which a
# cross build needs for its dependency lookups). Move anything it buried under
# <stage><sysroot> back to where pacman expects it.
if [ -n "${PKG_CONFIG_SYSROOT_DIR:-}" ] && [ -d "$stage$PKG_CONFIG_SYSROOT_DIR" ]; then
    cp -a "$stage$PKG_CONFIG_SYSROOT_DIR/." "$stage/"
    rm -rf "$stage$PKG_CONFIG_SYSROOT_DIR"
fi

# Catch the case above and any future one: a release without the addon is
# useless, but the tarball would still be created and published.
if [ ! -f "$stage/usr/lib/fcitx5/ainput.so" ]; then
    echo "error: ainput.so was not staged; refusing to publish an empty artifact" >&2
    find "$stage" -name 'ainput.so' -o -name '*.so' | head >&2
    exit 1
fi

# Nothing compiled should be left in the image; a stray object or archive here
# would silently make the package grow without being used.
find "$stage" -type f | sort

mkdir -p "$dist"
tar --zstd -cf "$dist/$name.tar.zst" -C "$stage" usr
rm -rf "$stage"
(cd "$dist" && sha256sum "$name.tar.zst" | tee "$name.tar.zst.sha256")

echo
echo "artifact: $dist/$name.tar.zst"
echo "minimum glibc required by the binary:"
objdump -T "$build/ainput.so" 2>/dev/null | grep -o 'GLIBC_2\.[0-9]\+' | sort -Vu | tail -1
