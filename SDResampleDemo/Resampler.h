/*
  Resampler - a fractional-position phase accumulator plus a 4-point
  Catmull-Rom interpolation kernel, used to play a source sample stream
  at any continuously-variable speed while always emitting output at a
  fixed rate (see SDResampleDemo.ino).

  advance()'s job is pure bookkeeping: given how many source samples
  should be consumed for one output tick (step - which can be less
  than, equal to, or much greater than 1), it returns the integer
  source-stream position the *next* interpolation window starts at, and
  keeps the leftover fractional phase for that window's `t`.

  The same mechanism handles speeding up and slowing down with no
  branch: step > 1 skips whole source samples between output ticks
  (interpolating between whichever two samples straddle the new
  fractional position, not just picking the nearest one - a nicer
  emergent property than naive nearest-neighbor decimation); step < 1
  keeps landing inside the same 4-sample window across several output
  ticks, each time at a different fractional position - genuinely new
  interpolated values, not repeats.

  No anti-aliasing filter is applied on top of this. Past step ~= 2,
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
  uint32_t index = 0;  // source-stream position of y0 in the current interpolation window
  float frac = 0.0f;   // fractional position in [0,1) between index and index+1

  // step = desired_speed_ratio * (source_file_rate / output_rate).
  // Returns the new index to hand to RingBuffer::release().
  uint32_t advance(float step) {
    float total = frac + step;
    float whole = floorf(total);
    frac = total - whole;
    index += (uint32_t)whole;
    return index;
  }
};

// Cubic Hermite (Catmull-Rom) interpolation through 4 evenly-spaced
// points, evaluated at fractional position t in [0,1) between y0 and
// y1 (ym1 and y2 are the one-before and one-after neighbors that shape
// the curve's slope through y0/y1).
inline float catmullRom(float ym1, float y0, float y1, float y2, float t) {
  float a0 = -0.5f * ym1 + 1.5f * y0 - 1.5f * y1 + 0.5f * y2;
  float a1 =  ym1        - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  float a2 = -0.5f * ym1 + 0.5f * y1;
  float a3 = y0;
  return ((a0 * t + a1) * t + a2) * t + a3;
}
