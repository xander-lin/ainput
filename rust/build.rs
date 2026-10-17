//! Links the system C libraries the FFI modules declare.
//!
//! Kept explicit instead of relying on `-sys` crates so the build uses exactly
//! the libraries named in PKGBUILD.

fn main() {
    for lib in ["ebur128", "speexdsp", "soxr", "m", "dl"] {
        println!("cargo:rustc-link-lib={lib}");
    }
    println!("cargo:rerun-if-changed=build.rs");
}
