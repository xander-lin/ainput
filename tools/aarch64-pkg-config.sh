#!/bin/sh
# pkg-config for the aarch64 cross build.
#
# Two separate problems, both solved here:
#  - the *-sys crates call pkg-config directly and would otherwise resolve
#    x86_64 libraries from the host;
#  - meson derives its install prefix from these variables, so they must be
#    set for compiler lookups but not leak into the install step (the release
#    script unsets them before `meson install`).
: "${AINPUT_SYSROOT:?set AINPUT_SYSROOT to the aarch64 sysroot}"
export PKG_CONFIG_SYSROOT_DIR="$AINPUT_SYSROOT"
export PKG_CONFIG_LIBDIR="$AINPUT_SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_PATH=
exec pkg-config "$@"
