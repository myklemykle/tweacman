#!/usr/bin/env python3
"""
make_format_test_wavs.py - writes a set of mono WAV files in assorted sample
rates, bit depths and header styles, for testing SDResampleDemo's WAV
decoding.

Every file holds the same short loop - three notes, A4, C#5 and E5, a tenth
of a second each - so played at 1x they should all sound identical: same
pitch, same tempo. A file decoded at the wrong rate comes out transposed and
faster or slower; one decoded with the wrong sample format comes out as
noise. Either is obvious by ear, and SDResampleDemo can measure the pitch
itself too. Names start with t_ so they sort together on the web page:
t_<rate>_<format>.wav, where the format is:

    08 / 16 / 24 / 32i  integer PCM of that many bits (8-bit is unsigned)
    32f / 64f           IEEE float
    x16 / x24 / x32f    the same, in a WAVE_FORMAT_EXTENSIBLE header
    x20                 20 valid bits in a 24-bit container (extensible)

t_44100_08.wav also has an odd-length chunk before its sample data, to
check the reader honours RIFF's pad byte.

Usage:
    python3 make_format_test_wavs.py [output_dir]     (default: test_wavs/)

Then copy them to the card's root - or let tools/ble_upload.py send them.
"""
import math
import os
import struct
import sys

NOTES_HZ = [440.0, 554.37, 659.26]  # A4, C#5, E5
NOTE_S = 0.1
AMPLITUDE = 0.5
FADE_S = 0.005  # per note edge, so the loop has no clicks

# Format codes in a WAV header.
PCM = 1
FLOAT = 3
EXTENSIBLE = 0xFFFE


def melody(rate):
    """The loop, as floats in [-1, 1)."""
    out = []
    per_note = int(rate * NOTE_S)
    fade = max(1, int(rate * FADE_S))
    for hz in NOTES_HZ:
        for i in range(per_note):
            env = min(1.0, i / fade, (per_note - 1 - i) / fade)
            out.append(AMPLITUDE * env * math.sin(2 * math.pi * hz * i / rate))
    return out


def encode(samples, bits, is_float):
    if is_float:
        fmt = "<f" if bits == 32 else "<d"
        return b"".join(struct.pack(fmt, s) for s in samples)
    if bits == 8:  # unsigned, centred on 128
        return bytes(max(0, min(255, round(128 + s * 127))) for s in samples)
    full = (1 << (bits - 1)) - 1
    nbytes = bits // 8
    return b"".join(
        int(round(s * full)).to_bytes(nbytes, "little", signed=True) for s in samples)


def fmt_chunk(rate, bits, is_float, extensible, valid_bits=None):
    block = bits // 8
    if not extensible:
        body = struct.pack("<HHIIHH", FLOAT if is_float else PCM, 1, rate, rate * block, block, bits)
        return b"fmt " + struct.pack("<I", len(body)) + body
    # WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes of a
    # GUID at the end; the rest of the GUID is fixed.
    sub = FLOAT if is_float else PCM
    guid = struct.pack("<H", sub) + bytes.fromhex("000000001000800000aa00389b71")
    body = struct.pack("<HHIIHH", EXTENSIBLE, 1, rate, rate * block, block, bits)
    body += struct.pack("<HHI", 22, valid_bits or bits, 0x4) + guid  # 0x4 = front centre
    return b"fmt " + struct.pack("<I", len(body)) + body


def write_wav(path, rate, bits, is_float=False, extensible=False, valid_bits=None, odd_chunk=False):
    samples = melody(rate)
    if valid_bits and valid_bits < bits:
        # Valid bits sit at the top of the container; the low ones are zero.
        step = 1 << (bits - valid_bits)
        full = (1 << (bits - 1)) - 1
        data = b"".join(
            ((int(round(s * full)) // step) * step).to_bytes(bits // 8, "little", signed=True)
            for s in samples)
    else:
        data = encode(samples, bits, is_float)

    chunks = fmt_chunk(rate, bits, is_float, extensible, valid_bits)
    if odd_chunk:
        note = b"odd"  # 3 bytes, so a pad byte must follow
        chunks += b"note" + struct.pack("<I", len(note)) + note + b"\0"
    chunks += b"data" + struct.pack("<I", len(data)) + data
    if len(data) & 1:
        chunks += b"\0"
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
    return len(chunks) + 12


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "test_wavs"
    os.makedirs(out_dir, exist_ok=True)
    files = []
    for rate in (8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000):
        files.append((f"t_{rate:05d}_16.wav", dict(rate=rate, bits=16)))
    files += [
        ("t_44100_08.wav", dict(rate=44100, bits=8, odd_chunk=True)),
        ("t_44100_24.wav", dict(rate=44100, bits=24)),
        ("t_44100_32i.wav", dict(rate=44100, bits=32)),
        ("t_44100_32f.wav", dict(rate=44100, bits=32, is_float=True)),
        ("t_44100_64f.wav", dict(rate=44100, bits=64, is_float=True)),
        ("t_44100_x16.wav", dict(rate=44100, bits=16, extensible=True)),
        ("t_44100_x24.wav", dict(rate=44100, bits=24, extensible=True)),
        ("t_44100_x20.wav", dict(rate=44100, bits=24, extensible=True, valid_bits=20)),
        ("t_44100_x32f.wav", dict(rate=44100, bits=32, is_float=True, extensible=True)),
        ("t_96000_24.wav", dict(rate=96000, bits=24)),
    ]
    total = 0
    for name, kw in files:
        size = write_wav(os.path.join(out_dir, name), **kw)
        total += size
        print(f"  {name:20s} {size / 1024:6.1f} KB")
    print(f"Wrote {len(files)} files, {total / 1024:.0f} KB, to {out_dir}/")


if __name__ == "__main__":
    main()
