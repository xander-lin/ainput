#!/bin/sh
# Builds the Rust core for meson.
#
# Arguments: source root, output archive path, cargo path, profile, feature list,
#            cargo target directory.
#
# Deliberately does not chdir: meson hands us a path relative to the build
# directory, which must stay valid when the archive is copied out.
set -eu

src=$1
out=$2
cargo=$3
profile=$4
features=$5
target_dir=$6

manifest="$src/rust/Cargo.toml"

# Cross builds are selected by the same variable cargo itself uses, so meson's
# cross file and a manual build cannot disagree. Without this the archive would
# be built for the host and the linker would reject it ("Relocations in
# generic ELF (EM: 62)"), which is a confusing way to learn the flag was lost.
target=${CARGO_BUILD_TARGET:-}

if [ "$profile" = "release" ]; then
    set -- build --release
    subdir="release"
else
    set -- build
    subdir="debug"
fi

if [ -n "$features" ]; then
    set -- "$@" --features "$features"
fi

if [ -n "$target" ]; then
    set -- "$@" --target "$target"
    artifact="$target_dir/$target/$subdir/libainput_core.a"
else
    artifact="$target_dir/$subdir/libainput_core.a"
fi

"$cargo" "$@" --manifest-path "$manifest" --target-dir "$target_dir"
cp "$artifact" "$out"
