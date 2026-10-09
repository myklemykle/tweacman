#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mykle Hansen
# Specified by Mykle Hansen, implemented by Claude (Anthropic).

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
100 times a second, and any seam would be loud. --saw makes a sawtooth
instead - bright, with harmonics up to Nyquist - and --exact cuts a tone at
exactly --seconds, mid-cycle if need be: the worst case for SDResampleDemo's
cached copies, whose loop points only line up exactly when the file's length
divides evenly.

Usage:
    python3 make_test_wav.py [output_path]
    python3 make_test_wav.py --sine HZ [--seconds S] [--exact] [output_path]
    python3 make_test_wav.py --saw HZ [--seconds S] [--exact] [output_path]

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


def tone_samples(hz, seconds, wave_fn, exact=False):
    # Normally a whole number of cycles, as close to `seconds` as possible,
    # so the last sample leads straight back into the first. With exact,
    # precisely `seconds` long instead, wherever in a cycle that ends - a
    # deliberately broken loop point.
    if exact:
        n_samples = round(seconds * SAMPLE_RATE)
        step = hz / SAMPLE_RATE
    else:
        cycles = max(1, round(seconds * hz))
        n_samples = round(cycles * SAMPLE_RATE / hz)
        step = cycles / n_samples
    for i in range(n_samples):
        yield AMPLITUDE * wave_fn((i * step) % 1.0)


def sine(phase):
    return math.sin(2 * math.pi * phase)


def saw(phase):
    # Rising ramp - every harmonic, at full strength right up to Nyquist.
    return 2.0 * phase - 1.0


def main():
    parser = argparse.ArgumentParser(description="Make a test WAV for SDResampleDemo.")
    parser.add_argument("--sine", type=float, metavar="HZ",
                        help="a steady tone at HZ instead of the default sweep")
    parser.add_argument("--saw", type=float, metavar="HZ",
                        help="a sawtooth at HZ - bright, with harmonics all the way up")
    parser.add_argument("--seconds", type=float, default=DURATION_S,
                        help=f"length of a --sine/--saw tone (default {DURATION_S:g}) - shorter "
                             "uploads faster over BLE, and loops just as seamlessly")
    parser.add_argument("--exact", action="store_true",
                        help="make a tone exactly --seconds long rather than a whole number "
                             "of cycles, so its loop point jumps")
    parser.add_argument("output_path", nargs="?")
    args = parser.parse_args()

    if args.sine or args.saw:
        hz = args.sine or args.saw
        name = "SINE" if args.sine else "SAW"
        out_path = args.output_path or f"{name}{hz:g}{'X' if args.exact else ''}.WAV"
        samples = list(tone_samples(hz, args.seconds, sine if args.sine else saw, args.exact))
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
