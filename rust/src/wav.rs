//! Minimal 16-bit PCM WAV reader/writer.
//!
//! The pipeline only ever produces mono 16 kHz signed 16-bit, and the vendor
//! APIs are told exactly that, so the reader is deliberately narrow: it
//! accepts the canonical 44-byte PCM header and rejects anything else loudly.

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use crate::error::{Error, Result};

pub const SAMPLE_RATE: u32 = 16_000;
pub const CHANNELS: u16 = 1;
pub const BITS_PER_SAMPLE: u16 = 16;

/// Writes `samples` as a canonical PCM WAV file.
pub fn write(path: &Path, samples: &[i16]) -> Result<()> {
    let data_bytes = (samples.len() * 2) as u32;
    let mut header = [0u8; 44];
    header[0..4].copy_from_slice(b"RIFF");
    header[4..8].copy_from_slice(&(36 + data_bytes).to_le_bytes());
    header[8..12].copy_from_slice(b"WAVE");
    header[12..16].copy_from_slice(b"fmt ");
    header[16..20].copy_from_slice(&16u32.to_le_bytes());
    header[20..22].copy_from_slice(&1u16.to_le_bytes()); // PCM
    header[22..24].copy_from_slice(&CHANNELS.to_le_bytes());
    header[24..28].copy_from_slice(&SAMPLE_RATE.to_le_bytes());
    header[28..32].copy_from_slice(&(SAMPLE_RATE * 2).to_le_bytes()); // byte rate
    header[32..34].copy_from_slice(&2u16.to_le_bytes()); // block align
    header[34..36].copy_from_slice(&BITS_PER_SAMPLE.to_le_bytes());
    header[36..40].copy_from_slice(b"data");
    header[40..44].copy_from_slice(&data_bytes.to_le_bytes());

    // 0600: the intermediate WAV contains the user's speech. File::create
    // would give it 0644 and would also follow a symlink planted at the path.
    let mut file = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(path)
        .map_err(|e| {
        Error::audio_data(format!("cannot create {}: {e}", path.display()))
    })?;
    file.write_all(&header)
        .and_then(|_| {
            let mut bytes = Vec::with_capacity(samples.len() * 2);
            for sample in samples {
                bytes.extend_from_slice(&sample.to_le_bytes());
            }
            file.write_all(&bytes)
        })
        .map_err(|e| Error::audio_data(format!("cannot write {}: {e}", path.display())))
}

/// Reads a canonical PCM WAV file produced by `write` or by the capture path.
pub fn read(path: &Path) -> Result<Vec<i16>> {
    let mut file = File::open(path).map_err(|e| {
        Error::audio_data(format!("cannot open {}: {e}", path.display()))
    })?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)
        .map_err(|e| Error::audio_data(format!("cannot read {}: {e}", path.display())))?;

    if bytes.len() < 44 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err(Error::audio_data(format!(
            "{} is not a RIFF/WAVE file",
            path.display()
        )));
    }
    let channels = u16::from_le_bytes([bytes[22], bytes[23]]);
    let rate = u32::from_le_bytes([bytes[24], bytes[25], bytes[26], bytes[27]]);
    let bits = u16::from_le_bytes([bytes[34], bytes[35]]);
    if channels != CHANNELS || rate != SAMPLE_RATE || bits != BITS_PER_SAMPLE {
        return Err(Error::audio_data(format!(
            "{} is {channels}ch/{rate}Hz/{bits}bit, expected mono 16 kHz 16-bit",
            path.display()
        )));
    }
    let data = &bytes[44..];
    Ok(data
        .chunks(2)
        .map(|pair| i16::from_le_bytes([pair[0], pair[1]]))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let dir = std::env::temp_dir().join("ainput-wav-test");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("x.wav");
        let samples: Vec<i16> = (0..1000).map(|i| (i % 100) as i16).collect();
        write(&path, &samples).unwrap();
        assert_eq!(read(&path).unwrap(), samples);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn wav_files_are_not_world_readable() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join("ainput-wav-test");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("private.wav");
        let _ = std::fs::remove_file(&path);
        write(&path, &[1i16, -2, 3]).unwrap();
        let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        let _ = std::fs::remove_file(&path);
        assert_eq!(mode, 0o600, "recorded speech must not be world readable");
    }

    #[test]
    fn rejects_non_wav() {
        let dir = std::env::temp_dir().join("ainput-wav-test");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("bad.wav");
        std::fs::write(&path, b"not a wav at all").unwrap();
        assert!(read(&path).is_err());
        let _ = std::fs::remove_file(&path);
    }
}
