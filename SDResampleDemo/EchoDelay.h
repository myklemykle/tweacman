// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  EchoDelay - a feedback echo on SDResampleDemo's output: what was played a
  moment ago comes back, quieter, and that comes back again quieter still.

  It works the way every digital echo does. Each output sample is written
  into a long circular buffer in RAM (the "delay line"), and the echo is
  simply that buffer read back from some way behind the point being written.
  Mixing what is read back into what gets written is the feedback that makes
  one echo into a decaying train of them.

    in --+---------------------------------------(+)--> out
         |                                        ^
         +-> DC blocker -(+)-> low-pass -> line   | x amount
                          ^                 |     |
                          | x amount        v     |
                          +----------- low-pass --+

  One number, the amount, sets both how loud the echo is and how much of it
  is fed back: the first repeat comes back at `amount`, the second at amount
  squared, and so on. At zero there is no echo at all.

  The delay line is deliberately thinner than the output, to make it longer.
  At the output's own format (stereo, 48kHz, 16 bits) this chip could spare
  about 0.7 seconds. So the line is mono and runs at half the output rate,
  24kHz, which makes the same 128KB last 2.7 seconds:

    - Mono: left and right are averaged on the way in, and the echo is added
      equally to both on the way out. The sound itself stays stereo.
    - Half rate: a sample is written every other tick. A 24kHz line can only
      hold frequencies below 12kHz, so the signal is low-passed before it's
      written (or anything higher would fold back down as aliasing), and
      again after it's read (which smooths the steps between samples back
      out). Both are gentle second-order filters at ECHO_CUTOFF_HZ.

  The price is that echoes are duller than the original, and each repeat has
  been round through both filters once more, so they get duller as they
  fade. Tape echoes do the same, for much the same reason, and it's a large
  part of why they sit behind the sound instead of cluttering it.

  Changing the delay time works like a tape echo too: the point being read
  slides along the line to its new distance rather than jumping there. While
  it slides it's reading faster or slower than real time, so the echoes
  bend up in pitch as the delay shortens and down as it lengthens. A jump
  would click. The slide is limited to reading between a quarter and four
  times real speed (two octaves either way), so a big change takes a moment
  to arrive.

  The DC blocker is there because of how this sketch stops: a stopped
  transport holds one sample, a constant level that is rarely zero. Fed
  round a feedback loop a constant builds up to 1/(1 - amount) times itself,
  enough to push the output hard against its limit. Blocking it on the way
  in means only sound gets echoed - and lets the echo run all the time,
  whether or not anything is playing.

  Left to itself, then, the tail would ring on long after a stop - for many
  seconds, at a long delay with a lot of feedback. setDamped(true) is for
  that: it takes the feedback away at once, so nothing repeats again, and
  fades out the echoes already on their way over about half a second.
  (SDResampleDemo fades the echo out more slowly with setFade() first - see
  below - and damps it once that's done.)
  Cutting the echo dead instead would sound as abrupt as it is. The line
  keeps running underneath, so setDamped(false) brings the echo straight
  back.

  That makes two gains out of the one amount: how much of the echo is fed
  back, and how loud it is heard. Undamped, both simply follow the amount.

  setFade() is the slow way out, for a fade of any length and shape: it
  scales how loud the echo is heard, and nothing else, so the repeats carry
  on underneath exactly as they were while the caller turns them down.

  process() runs in the audio interrupt, once per output sample; setTime(),
  setAmount(), setFade() and setDamped() are called from loop(). The cost is a few dozen floating
  point operations a tick whatever the settings.
*/

#pragma once

#include <cmath>
#include <cstdint>

#include "SincInterpolator.h"  // for AUDIO_HOT

const float ECHO_OUTPUT_RATE = 48000.0f;            // the audio interrupt's rate
const float ECHO_LINE_RATE = ECHO_OUTPUT_RATE / 2;  // the delay line's - see above
const uint32_t ECHO_LINE_SAMPLES = 65536;           // a power of two, so positions wrap with a mask

// The shortest and longest delay. Short ones aren't heard as echoes but as
// a pitched, metallic ring (a comb filter) - worth having. The longest is
// the whole line, less the few samples the read needs clear of the write.
const float ECHO_MIN_SECONDS = 0.001f;
const float ECHO_MAX_SECONDS = (float)(ECHO_LINE_SAMPLES - 4) / ECHO_LINE_RATE;

// Feedback of 1 would never die away, and more than that grows without
// limit, so the amount stops short of it.
const float ECHO_MAX_AMOUNT = 0.95f;

const float ECHO_CUTOFF_HZ = 7500.0f;  // of the filters either side of the line

// How the delay time slides to a new setting: each tick it covers this
// fraction of what's left (about a tenth of a second to mostly arrive)...
const float ECHO_TIME_GLIDE = 1.0f / (0.1f * ECHO_OUTPUT_RATE);
// ...but no faster than these, in line samples per tick. The line itself
// moves on half a sample per tick, so lengthening the delay by 0.375 a tick
// reads it at a quarter speed, and shortening it by 1.5 reads at four times.
const float ECHO_MAX_GROW = 0.375f;
const float ECHO_MAX_SHRINK = 1.5f;
// ...and no slower than this, so it does finally get there.
const float ECHO_MIN_SLIDE = 0.01f;

// The amount is smoothed as well, so turning its knob doesn't zipper.
const float ECHO_AMOUNT_GLIDE = 1.0f / (0.02f * ECHO_OUTPUT_RATE);

// Damped, the echo's level falls away this fast instead: each tick it loses
// this fraction of itself, which is 30dB down - as good as gone - in half a
// second. (The feedback goes at the amount's own, much quicker, rate.)
const float ECHO_DAMP_GLIDE = 1.0f / (0.15f * ECHO_OUTPUT_RATE);

const float ECHO_DC_BLOCK = 0.995f;  // blocks below about 40Hz

// A second-order low-pass filter (a "biquad", Butterworth response - flat,
// then falling away at 12dB per octave above its cutoff).
struct EchoLowpass {
  float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;  // coefficients - see setCutoff()
  float z1 = 0.0f, z2 = 0.0f;                                   // its memory

  // The standard formulas (Robert Bristow-Johnson's "Audio EQ Cookbook").
  void setCutoff(float hz, float sampleRate) {
    float w = 2.0f * 3.14159265f * hz / sampleRate;
    float alpha = sinf(w) / (2.0f * 0.70711f);
    float a0 = 1.0f + alpha;
    b1 = (1.0f - cosf(w)) / a0;
    b0 = b2 = b1 * 0.5f;
    a1 = -2.0f * cosf(w) / a0;
    a2 = (1.0f - alpha) / a0;
  }

  AUDIO_HOT inline float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

class EchoDelay {
 public:
  // Call once from setup(), before the audio interrupt starts, and before
  // setTime(). (Everything here starts out as zeros and is set up in this
  // function rather than where it's declared, so that the 128KB line is
  // just reserved in RAM instead of being stored in the program as well.)
  void begin() {
    inputFilter_.setCutoff(ECHO_CUTOFF_HZ, ECHO_OUTPUT_RATE);
    outputFilter_.setCutoff(ECHO_CUTOFF_HZ, ECHO_OUTPUT_RATE);
    targetDelay_ = delay_ = ECHO_MIN_SECONDS * ECHO_LINE_RATE;
    fade_ = 1.0f;
  }

  // The delay time, in seconds. The echo slides there - see above.
  void setTime(float seconds) {
    if (seconds < ECHO_MIN_SECONDS) seconds = ECHO_MIN_SECONDS;
    if (seconds > ECHO_MAX_SECONDS) seconds = ECHO_MAX_SECONDS;
    targetDelay_ = seconds * ECHO_LINE_RATE;
  }

  // The echo's level and feedback, 0 (none) to ECHO_MAX_AMOUNT.
  void setAmount(float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > ECHO_MAX_AMOUNT) amount = ECHO_MAX_AMOUNT;
    targetAmount_ = amount;
  }

  // Turns the echo down without changing it: 1 is its full level, 0
  // silent. The feedback isn't touched. Smoothed like the amount, so it can
  // be stepped down from loop() without zippering.
  void setFade(float fade) {
    if (fade < 0.0f) fade = 0.0f;
    if (fade > 1.0f) fade = 1.0f;
    fade_ = fade;
  }

  // Damped, the echo dies away quickly whatever its amount: no feedback,
  // and what's already in the line fades out - see above. For when the
  // sound that was being echoed has stopped.
  void setDamped(bool damped) { damped_ = damped; }

  // Adds the echo to one output frame (left, right) in place, and records
  // the frame for echoes to come. Call exactly once per output sample.
  AUDIO_HOT inline void process(float frame[2]) {
    // The two gains, each gliding to the amount - or, damped, to nothing,
    // the feedback quickly and the level more gently.
    bool damped = damped_;
    float amount = damped ? 0.0f : targetAmount_;
    float feedback = feedback_ + (amount - feedback_) * ECHO_AMOUNT_GLIDE;
    feedback_ = feedback;
    float level = level_ + (amount * fade_ - level_) * (damped ? ECHO_DAMP_GLIDE : ECHO_AMOUNT_GLIDE);
    level_ = level;

    // Slide the delay toward where it's been set.
    float delay = delay_;
    float remaining = targetDelay_ - delay;
    if (remaining != 0.0f) {
      float slide = remaining * ECHO_TIME_GLIDE;
      if (slide > ECHO_MAX_GROW) slide = ECHO_MAX_GROW;
      else if (slide < -ECHO_MAX_SHRINK) slide = -ECHO_MAX_SHRINK;
      else if (__builtin_fabsf(slide) < ECHO_MIN_SLIDE) {
        slide = (__builtin_fabsf(remaining) < ECHO_MIN_SLIDE)
                    ? remaining
                    : __builtin_copysignf(ECHO_MIN_SLIDE, remaining);
      }
      delay += slide;
      delay_ = delay;
    }

    // Read the line `delay` samples behind "now". Now is the next sample
    // to be written, plus half a sample on the ticks in between writes.
    // The position falls between two samples; take a straight line
    // between them.
    float behind = delay - (betweenWrites_ ? 0.5f : 0.0f);
    uint32_t whole = (uint32_t)behind;
    float frac = behind - (float)whole;
    uint32_t at = writeIndex_ - whole;
    float newer = line_[at & MASK];
    float older = line_[(at - 1) & MASK];
    float echo = outputFilter_.process(newer + (older - newer) * frac);

    // What goes into the line: this frame as mono, without its DC, plus the
    // echo again for the feedback.
    float in = (frame[0] + frame[1]) * 0.5f;
    float sound = in - dcLastIn_ + ECHO_DC_BLOCK * dcLastOut_;
    dcLastIn_ = in;
    dcLastOut_ = sound;
    float record = inputFilter_.process(sound + feedback * echo);

    // Written every other tick - the filter above runs on every tick, so
    // it has already removed what the samples skipped would have carried.
    if (betweenWrites_) {
      if (record > 32767.0f) record = 32767.0f;
      else if (record < -32768.0f) record = -32768.0f;
      line_[writeIndex_ & MASK] = (int16_t)record;
      writeIndex_++;
    }
    betweenWrites_ = !betweenWrites_;

    frame[0] += level * echo;
    frame[1] += level * echo;
  }

 private:
  static const uint32_t MASK = ECHO_LINE_SAMPLES - 1;
  static_assert((ECHO_LINE_SAMPLES & (ECHO_LINE_SAMPLES - 1)) == 0,
                "ECHO_LINE_SAMPLES must be a power of two");

  int16_t line_[ECHO_LINE_SAMPLES] = {};  // 128KB
  uint32_t writeIndex_ = 0;     // the next sample of the line to be written
  bool betweenWrites_ = false;  // true on the ticks that end with a write

  // Set by loop(), followed by the interrupt. The delay is in line samples.
  volatile float targetDelay_ = 0.0f;
  volatile float targetAmount_ = 0.0f;
  volatile float fade_ = 0.0f;  // 1 once begin() has run
  volatile bool damped_ = false;
  float delay_ = 0.0f;
  float feedback_ = 0.0f;  // how much of the echo goes back into the line
  float level_ = 0.0f;     // how much of it is heard

  EchoLowpass inputFilter_;
  EchoLowpass outputFilter_;
  float dcLastIn_ = 0.0f;
  float dcLastOut_ = 0.0f;
};
