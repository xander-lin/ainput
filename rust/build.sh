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

if [ "$profile" = "release" ]; then
    set -- build --release
    artifact="$target_dir/release/libainput_core.a"
else
    set -- build
    artifact="$target_dir/debug/libainput_core.a"
fi

if [ -n "$features" ]; then
    set -- "$@" --features "$features"
fi

"$cargo" "$@" --manifest-path "$manifest" --target-dir "$target_dir"
cp "$artifact" "$out"
