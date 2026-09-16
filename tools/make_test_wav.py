#!/usr/bin/env python3
"""
make_test_wav.py - generates AUDIO.WAV, a mono 16-bit PCM sine sweep, for
testing SDResampleDemo.

A sweep (not music or a fixed tone) makes speed changes and any
resampling artifacts obvious by ear: its instantaneous pitch traces a
known, smooth curve, so speeding up/slowing down or any zippering in the
resampler shows up immediately as a kink in that curve.

Usage:
    python3 make_test_wav.py [output_path]

Copy the resulting file to the SD card's root as AUDIO.WAV.
"""
import math
import struct
import sys
import wave

SAMPLE_RATE = 44100
DURATION_S = 10.0
START_HZ = 200.0
END_HZ = 2000.0
AMPLITUDE = 0.8  # fraction of full scale, leaving headroom


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "AUDIO.WAV"
    n_samples = int(SAMPLE_RATE * DURATION_S)

    with wave.open(out_path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)  # 16-bit
        f.setframerate(SAMPLE_RATE)

        frames = bytearray()
        phase = 0.0
        for i in range(n_samples):
            t = i / SAMPLE_RATE
            # Linear frequency sweep: instantaneous frequency rises from
            # START_HZ to END_HZ over DURATION_S; phase is its integral,
            # not just 2*pi*freq(t)*t (that would give the wrong curve).
            freq = START_HZ + (END_HZ - START_HZ) * (t / DURATION_S)
            phase += 2 * math.pi * freq / SAMPLE_RATE
            sample = AMPLITUDE * math.sin(phase)
            frames += struct.pack("<h", int(sample * 32767))
        f.writeframes(bytes(frames))

    print(f"Wrote {n_samples} samples ({DURATION_S}s @ {SAMPLE_RATE}Hz) to {out_path}")


if __name__ == "__main__":
    main()
