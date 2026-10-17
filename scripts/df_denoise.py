#!/usr/bin/env python3
"""
DeepFilterNet3 denoise wrapper.
Reads 16kHz mono WAV, resamples to 48kHz, runs deep-filter binary,
resamples back to 16kHz, writes output.
"""
import subprocess, sys, struct, os, shutil

DF_BIN = os.path.expanduser("~/.local/share/ainput/bin/deep-filter")

def wav_read(path):
    with open(path, "rb") as f:
        assert f.read(4) == b"RIFF"
        f.read(4)
        assert f.read(4) == b"WAVE"
        while True:
            ckid = f.read(4)
            cksize = struct.unpack("<I", f.read(4))[0]
            if ckid == b"fmt ":
                fmt_data = f.read(cksize)
                tag, ch = struct.unpack_from("<HH", fmt_data, 0)
                sr = struct.unpack_from("<I", fmt_data, 4)[0]
            elif ckid == b"data":
                raw = f.read(cksize)
                break
            else:
                f.seek(cksize, 1)
        f.seek(0)  # not used
    samples = struct.unpack(f"<{len(raw)//2}h", raw)
    return list(samples), sr, ch

def wav_write(path, samples, sr, ch=1):
    raw = struct.pack(f"<{len(samples)}h", *samples)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(raw)))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))
        f.write(struct.pack("<HHIIHH", 1, ch, sr, sr * ch * 2, ch * 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(raw)))
        f.write(raw)

def resample(samples, src_sr, dst_sr):
    import torch, torchaudio
    t = torch.tensor(samples, dtype=torch.float32).unsqueeze(0) / 32768.0
    if src_sr != dst_sr:
        t = torchaudio.functional.resample(t, src_sr, dst_sr)
    return (t.squeeze(0) * 32768.0).clamp(-32768, 32767).to(torch.int16).tolist()

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.wav> <output.wav>", file=sys.stderr)
        sys.exit(1)

    in_path, out_path = sys.argv[1], sys.argv[2]

    # Read 16kHz WAV
    samples, sr, ch = wav_read(in_path)
    print(f"Read {len(samples)} samples @ {sr}Hz", file=sys.stderr)

    # Resample to 48kHz
    samples_48k = resample(samples, sr, 48000)
    tmp_48k = f"/tmp/ainput_df_{os.getpid()}_48k.wav"
    wav_write(tmp_48k, samples_48k, 48000)

    # Run deep-filter — with -o to same dir, file is modified in-place
    out_dir = os.path.dirname(tmp_48k)
    subprocess.run([DF_BIN, "-D", "-o", out_dir, tmp_48k], check=True, capture_output=True)

    # Read denoised 48kHz (modified in-place)
    samples_df, _, _ = wav_read(tmp_48k)

    # Resample back to 16kHz
    samples_out = resample(samples_df, 48000, 16000)
    wav_write(out_path, samples_out, 16000)

    os.unlink(tmp_48k)
    print(f"Written {len(samples_out)} samples to {out_path}", file=sys.stderr)

if __name__ == "__main__":
    main()
