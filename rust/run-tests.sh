#!/bin/sh
# Runs the Rust unit tests from `meson test`.
set -eu

src=$1
features=$2

cd "$src/rust"
if [ -n "$features" ]; then
    exec cargo test --features "$features"
fi
exec cargo test
