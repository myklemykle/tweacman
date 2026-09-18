/*
  Resampler - a fractional-position phase accumulator plus a 4-point
  Catmull-Rom interpolation kernel, used to play a source sample stream
  forward OR backward at any continuously-variable speed while always
  emitting output at a fixed rate (see SDResampleDemo.ino).

  advance()'s job is pure bookkeeping: given how many source samples
  should be consumed for one output tick (step - which can be negative
  for backward playback, zero to pause, or any positive magnitude for
  forward playback, slower or faster than native speed), it returns the
  signed source-stream position the *next* interpolation window starts
  at, and keeps the leftover fractional phase for that window's `t`.

  The same mechanism handles speeding up, slowing down, and reversing
  with no branch: step > 1 skips whole source samples between output
  ticks (interpolating between whichever two samples straddle the new
  fractional position, not just picking the nearest one - a nicer
  emergent property than naive nearest-neighbor decimation); |step| < 1
  keeps landing inside the same 4-sample window across several output
  ticks, each time at a different fractional position - genuinely new
  interpolated values, not repeats; step == 0 leaves index/frac frozen
  exactly (floorf(frac) is always 0 since frac stays in [0,1)), which is
  the entire implementation of "pause" - audioCallback() needs no special
  case for it at all.

  No anti-aliasing filter is applied on top of this. Past |step| ~= 2,
  high source frequencies will audibly fold down and sound "crunchy" -
  that's expected, not a bug: a proper fix needs a filter whose cutoff
  tracks the live ratio, which is real DSP scope this hand-rolled kernel
  deliberately doesn't take on. It's also a useful signpost, since it
  shows up before the harder SD-bandwidth wall this demo is built to find.
*/

#pragma once

#include <cmath>
#include <cstdint>

struct Resampler {
  // volatile: written by the audio ISR (advance(), called from
  // audioCallback()), read directly by loop() (see SDResampleDemo.ino's
  // direction-aware refill logic) with no lock - the same "single word,
  // no torn read on this core" reasoning currentStep already relies on
  // for the opposite direction (loop() writes it, the ISR reads it).
  volatile int32_t index = 0;  // signed source-stream position of y0 in the current window
  volatile float frac = 0.0f;  // fractional position in [0,1) between index and index+1

  // step = desired_speed_ratio * (source_file_rate / output_rate) -
  // negative plays backward, zero pauses. Returns the new index.
  int32_t advance(float step) {
    float total = frac + step;
    float whole = floorf(total);
    frac = total - whole;
    // (int32_t)whole, not (uint32_t)whole: converting a *negative*
    // float to an unsigned type is undefined behavior in C++ (unlike
    // int-to-unsigned conversion, which is well-defined modulo
    // arithmetic) - this was silently fine only as long as step was
    // always positive, so whole was always >= 0. Checked against
    // step=-0.5, frac=0.3: total=-0.2, whole=floorf(-0.2)=-1.0 (floorf
    // already does the right thing), frac becomes 0.8. Checked against
    // step=-1.5, frac=0: whole=-2.0, frac=0.5, net position change
    // -2+0.5=-1.5 - matches step exactly.
    index += (int32_t)whole;
    return index;
  }
};

// Cubic Hermite (Catmull-Rom) interpolation through 4 evenly-spaced
// points, evaluated at fractional position t in [0,1) between y0 and
// y1 (ym1 and y2 are the one-before and one-after neighbors that shape
// the curve's slope through y0/y1). Pure math over 4 already-fetched
// values - direction-agnostic, needs no changes for backward playback.
inline float catmullRom(float ym1, float y0, float y1, float y2, float t) {
  float a0 = -0.5f * ym1 + 1.5f * y0 - 1.5f * y1 + 0.5f * y2;
  float a1 =  ym1        - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  float a2 = -0.5f * ym1 + 0.5f * y1;
  float a3 = y0;
  return ((a0 * t + a1) * t + a2) * t + a3;
}
