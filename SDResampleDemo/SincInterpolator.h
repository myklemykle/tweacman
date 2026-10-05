/*
  SincInterpolator - the band-limited interpolation kernel SDResampleDemo
  reads every output sample through.

  Reading a stream at s source samples per output sample (s > 1 is faster
  than real time) squeezes its spectrum up by s. Anything that lands above
  the DAC's Nyquist limit (24kHz at 48kHz output) doesn't disappear - it
  folds back down as aliasing, tones moving the wrong way. The fix is to
  low-pass the source at 1/s of its own Nyquist limit *while* interpolating,
  so nothing is left up there to fold.

  That's what a windowed sinc does: an ideal low-pass filter's impulse
  response (sinc), tapered to a finite length by a window (Kaiser here), and
  evaluated at the fractional position being read. Stretching it by s
  lowers its cutoff by s, at the cost of s times as many taps - which is why
  SDResampleDemo switches to a lower-rate cached copy (MipCache.h) rather
  than reading any copy much faster than 2 samples per tick.

  It also makes those switches smooth. With a fixed kernel, a copy read at
  1.9 samples per tick aliases and the next copy read at 0.95 doesn't, so
  the aliasing vanished abruptly at each switch. With this one, both sides
  of a switch are band-limited the same way and sound (almost) the same.

  The kernel is tabulated once at startup and looked up with linear
  interpolation - computing sin() per tap at 48kHz would be far too slow.

  Numbers: 6 samples of reach either side at full bandwidth (so 14 taps at
  s <= 1, 26 at s = 2), cutoff at 0.45 of the sample rate, Kaiser beta 5.
  That's flat to about 15kHz at 48kHz output, rolling off above, and what
  little does alias lands above 20kHz. More reach would sharpen that, at
  proportionally more CPU in the audio interrupt - 8 was tried, and a
  crossfade (two voices at once) then took nearly the whole of a tick.
*/

#pragma once

#include <cmath>
#include <cstdint>

// Speed matters here - this runs for every output sample, sometimes four
// times over (two voices crossfading, each with two pitch-shifter taps).
// Two things this core's build settings would otherwise undo:
//   - It compiles with -fno-builtin, which turns fabsf() and floorf() into
//     real library calls rather than single FPU instructions - one per tap
//     adds up fast - so the hot path uses the __builtin_ versions directly.
//   - It optimises for size (-Os); AUDIO_HOT asks for speed instead on the
//     handful of functions the audio interrupt spends its time in.
#define AUDIO_HOT __attribute__((optimize("O2")))

const int SINC_HALF_WIDTH = 6;      // samples of reach either side, at full bandwidth
const int SINC_RES = 128;           // table entries per sample of distance
const float SINC_CUTOFF = 0.45f;    // cycles per sample (0.5 is Nyquist)
const float SINC_KAISER_BETA = 5.0f;

// Padded with zeros a sample's worth past the kernel's end, so the tap loop
// below never needs a bounds check: a tap can land at most one sample
// beyond the kernel's reach.
const int SINC_TABLE_LEN = (SINC_HALF_WIDTH + 1) * SINC_RES + 2;
extern float sincTable[SINC_TABLE_LEN];

// Fills sincTable. Call once from setup().
inline void initSincTable() {
  // Modified Bessel function of the first kind, order 0 - the Kaiser
  // window's shape - by its power series, which converges fast here.
  auto besselI0 = [](float x) {
    float sum = 1.0f, term = 1.0f;
    for (int k = 1; k < 25; k++) {
      float t = x / (2.0f * k);
      term *= t * t;
      sum += term;
    }
    return sum;
  };
  float i0Beta = besselI0(SINC_KAISER_BETA);
  for (int i = 0; i < SINC_TABLE_LEN; i++) {
    float u = (float)i / SINC_RES;  // distance in samples
    if (u >= SINC_HALF_WIDTH) {
      sincTable[i] = 0.0f;
      continue;
    }
    float x = 2.0f * SINC_CUTOFF * u;
    float sinc = (x == 0.0f) ? 1.0f : sinf(M_PI * x) / (M_PI * x);
    float r = u / SINC_HALF_WIDTH;
    float window = besselI0(SINC_KAISER_BETA * sqrtf(1.0f - r * r)) / i0Beta;
    sincTable[i] = sinc * window;
  }
}

// Reads one sample at position base + frac (frac in [0,1)) from anything
// with contains(first, last) and at(pos) - an AudioRingBuffer - low-passed
// at `bandwidth` (1 for the full band, 1/s when reading s samples per
// output sample). Returns false if the samples it needs aren't all there.
template <typename Ring>
AUDIO_HOT inline bool readSinc(const Ring &ring, int32_t base, float frac, float bandwidth, float &out) {
  float reach = SINC_HALF_WIDTH / bandwidth;
  int32_t first = base - (int32_t)reach;
  int32_t last = base + (int32_t)reach + 1;
  if (!ring.contains(first, last)) return false;

  // Walk the taps, stepping through the table by `bandwidth` samples'
  // worth each time - that's what stretches the kernel. Weights are summed
  // and divided out at the end, so the gain is exactly 1 at every
  // fractional position, whatever the table's own rounding.
  float tableStep = bandwidth * SINC_RES;
  float u = ((float)(first - base) - frac) * tableStep;
  float acc = 0.0f, weightSum = 0.0f;
  for (int32_t i = first; i <= last; i++, u += tableStep) {
    float d = __builtin_fabsf(u);
    int k = (int)d;
    float w = sincTable[k] + (d - (float)k) * (sincTable[k + 1] - sincTable[k]);
    acc += w * (float)ring.at(i);
    weightSum += w;
  }
  out = acc / weightSum;
  return true;
}
