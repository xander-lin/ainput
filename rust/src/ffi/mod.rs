//! Hand-written FFI for the C libraries that have no stable Rust crate:
//! libebur128, speexdsp and libsoxr.
//!
//! PulseAudio and libcurl use the maintained `libpulse-binding` and `curl`
//! crates instead; both link the system libraries, so the dependency set stays
//! the same as before.

pub mod dsp;
