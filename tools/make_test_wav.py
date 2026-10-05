#!/usr/bin/env python3
"""
make_test_wav.py - generates mono 16-bit PCM test files for SDResampleDemo.

By default, AUDIO.WAV: a sine sweep. A sweep (not music or a fixed tone)
makes speed changes and any resampling artifacts obvious by ear: its
instantaneous pitch traces a known, smooth curve, so speeding up/slowing
down or any zippering in the resampler shows up immediately as a kink in
that curve.

With --sine HZ, a steady tone instead - e.g. --sine 20 makes SINE20.WAV, a
20Hz tone that sweeps right up through the audible range as the demo's
speed knob goes from 1x to 1000x. The length is rounded to a whole number
of cycles, so the file loops seamlessly: at 1000x a 10-second file loops
100 times a second, and any seam would be loud.

Usage:
    python3 make_test_wav.py [output_path]
    python3 make_test_wav.py --sine HZ [output_path]

Copy the resulting file to the SD card's root.
"""
import argparse
import math
import struct
import wave

SAMPLE_RATE = 44100
DURATION_S = 10.0
START_HZ = 200.0
END_HZ = 2000.0
AMPLITUDE = 0.8  # fraction of full scale, leaving headroom


def sweep_samples():
    n_samples = int(SAMPLE_RATE * DURATION_S)
    phase = 0.0
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        # Linear frequency sweep: instantaneous frequency rises from
        # START_HZ to END_HZ over DURATION_S; phase is its integral,
        # not just 2*pi*freq(t)*t (that would give the wrong curve).
        freq = START_HZ + (END_HZ - START_HZ) * (t / DURATION_S)
        phase += 2 * math.pi * freq / SAMPLE_RATE
        yield AMPLITUDE * math.sin(phase)


def sine_samples(hz):
    # A whole number of cycles, as close to DURATION_S as possible, so the
    # last sample leads straight back into the first.
    cycles = max(1, round(DURATION_S * hz))
    n_samples = round(cycles * SAMPLE_RATE / hz)
    for i in range(n_samples):
        yield AMPLITUDE * math.sin(2 * math.pi * cycles * i / n_samples)


def main():
    parser = argparse.ArgumentParser(description="Make a test WAV for SDResampleDemo.")
    parser.add_argument("--sine", type=float, metavar="HZ",
                        help="a steady tone at HZ instead of the default sweep")
    parser.add_argument("output_path", nargs="?")
    args = parser.parse_args()

    if args.sine:
        out_path = args.output_path or f"SINE{args.sine:g}.WAV"
        samples = list(sine_samples(args.sine))
    else:
        out_path = args.output_path or "AUDIO.WAV"
        samples = list(sweep_samples())

    with wave.open(out_path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)  # 16-bit
        f.setframerate(SAMPLE_RATE)
        f.writeframes(b"".join(struct.pack("<h", int(s * 32767)) for s in samples))

    print(f"Wrote {len(samples)} samples ({len(samples) / SAMPLE_RATE:.3f}s @ {SAMPLE_RATE}Hz) "
          f"to {out_path}")


if __name__ == "__main__":
    main()
