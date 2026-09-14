// synth.dsp - two sine oscillators, mixed to mono
//
// freq1/freq2/gain1/gain2 are meant to be driven directly from C++ (see
// CaptureUI in AudioKnobsDemo.ino), not from a real user interface - the
// Arduino sketch maps each RV112FF pot's angle to a frequency and writes
// it straight into the slider's memory address.
//
// Regenerate synth.h after editing this file:
//   make faust
import("stdfaust.lib");

// os.osc's default sine table is 65536 entries (256KB as a float array -
// too much of this MCU's ~512KB total RAM for a lookup table alone).
// 4096 matches the RA6M5 DAC's own 12-bit resolution (4096 distinct
// output levels): a finer table can't produce anything the DAC could
// actually represent, so this is the natural ceiling, at a much more
// reasonable 16KB.
TABLESIZE = 4096;
mysine(freq) = rdtable(TABLESIZE, os.sinwaveform(TABLESIZE), int(os.phasor(TABLESIZE, freq)));

// si.smoo (a one-pole lowpass) smooths each control value to avoid
// zipper noise/clicks from the sketch updating these in discrete
// control-rate steps - particularly gain1/gain2, which jump instantly
// between 0 and 1 the moment a pot's calibration completes.
freq1 = hslider("freq1", 220, 50, 10000, 0.01) : si.smoo;
freq2 = hslider("freq2", 220, 50, 10000, 0.01) : si.smoo;
gain1 = hslider("gain1", 0, 0, 1, 0.01) : si.smoo;
gain2 = hslider("gain2", 0, 0, 1, 0.01) : si.smoo;

process = (mysine(freq1) * gain1 + mysine(freq2) * gain2) * 0.5;
