// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  BandFilter - a resonant band-pass filter on SDResampleDemo's output, in
  the spirit of the Korg MS-20's (and before it, the MiniKorg 700's
  "Traveler"): a high-pass filter and a low-pass filter in series. Only what
  lies above one cutoff and below the other gets through.

    in --+--------------------------------------- x dry ---(+)--> out
         |                                                  ^
         +--> high-pass --> low-pass --> limit --- x wet ---+
              (cutoff A)    (cutoff B, resonant)

  It's played with its two cutoffs, the bottom and the top of the band,
  as the Traveler is: one knob for each. How it sounds depends on the
  band's width - the distance between them, in octaves.

    - Wide open (FILTER_MIN_HZ to FILTER_MAX_HZ, ten octaves, 20Hz to
      20kHz) the band covers everything you can hear. Over the last octave
      before that the filtered sound is crossfaded into the untouched one,
      so wide open means exactly no filter at all.
    - Narrowing it, the low-pass filter's resonance rises - first a peak at
      the top of the band, then, over the last stretch, enough to take it
      into oscillation: a sine-ish scream at the low-pass cutoff, which
      plays like a pitch control. Both filters keep a little resonance
      throughout - the "bump" just before the cutoff that the MS-20 is
      known for.

  The cutoffs can meet but not cross: setCutoffs() keeps the top at or
  above the bottom. (The sketch makes one push the other along - see its
  turnFilterLow() and turnFilterHigh().)

  Each filter is a 12dB-per-octave stage, the MS-20's slope: a state
  variable filter in its "zero-delay feedback" form - two integrators in a
  loop, solved each sample so the response stays true right up to the top
  of the audio range and doesn't misbehave while the cutoff moves. Its
  damping, 2 - k, sets the resonance: at k = 0 the stage is two gentle
  poles; as k nears 2 a peak grows at the cutoff; past 2 it oscillates.

  The MS-20 flavour is in what keeps that oscillation in hand. Its circuit
  clips the resonance signal with diodes, which is why it can scream
  without running away, and why a loud input drowns the resonance out.
  Here k is divided down by the size of the stage's band-pass state,
  k / (1 + |s1|): small signals see all of k; a loud one, or the
  oscillation once it has grown, sees less - so the scream settles at a
  level instead of growing without end. It's not a component-by-component
  model of the MS-20's circuit; it's the behaviour that gives it its sound,
  at a price this chip can pay 48000 times a second.

  That price is still the biggest in the audio interrupt: about 570
  processor cycles per stereo sample, 14% of the 4167 there are between
  ticks. It's written directly in C++ rather than in Faust, as this
  project's synth is (see AudioKnobsDemo): the same filter written in Faust
  measured 925 cycles. The Faust compiler's code is good, but it's written
  for blocks of samples, and called one sample at a time from an interrupt
  it re-reads and re-checks everything on every call.

  When the sound stops, so does the scream: an oscillating filter would
  otherwise carry on by itself, and be hard to silence for anyone not in
  filter mode. setLevel() is for that: the sketch fades the filtered sound
  out with it, along with the echo's tail, and once it's down to nothing the
  resonance is taken down out of oscillation. Turned back up, playing
  again, the scream starts again.

  Sitting wide open, as it does until the width knob is turned, the filter
  isn't run at all, so it costs nothing in the other modes.

  update() and the setters are called from loop(); process() from the audio
  interrupt, once per output sample.
*/

#pragma once

#include <cmath>
#include <cstdint>

#include "SincInterpolator.h"  // for AUDIO_HOT

// The cutoffs' range: the audible range, ten octaves exactly. With the
// cutoffs at its ends the band is wide open, and the filter is out of the
// circuit entirely.
const float FILTER_MIN_HZ = 20.0f;
const float FILTER_MAX_HZ = 20480.0f;
const float FILTER_OPEN_OCTAVES = 10.0f;  // log2(FILTER_MAX_HZ / FILTER_MIN_HZ)
// The filtered sound fades in over this much narrowing from open.
const float FILTER_DRY_OCTAVES = 1.0f;
// The low-pass resonance climbs from FILTER_BUMP to FILTER_SCREAM over this
// last stretch of narrowing; the scream starts most of the way in.
const float FILTER_RESONANCE_OCTAVES = 2.0f;

// Resonance, as the stages' k: 0 is none, 2 the edge of oscillation (the
// clipping means it takes a little over 2 to get it going).
const float FILTER_BUMP = 1.5f;    // both filters, always - the peak before the cutoff
const float FILTER_SCREAM = 2.5f;  // the low-pass, fully narrowed

// The highest a cutoff can go at any sample rate: short of the 24kHz
// Nyquist limit at 48kHz, where a coefficient runs off to infinity.
const float FILTER_HIGHEST_HZ = 21000.0f;

// Every setting glides to a new value over a few milliseconds, so a knob
// stepping it at loop()'s control rate doesn't zipper.
const float FILTER_GLIDE_SECONDS = 0.004f;

// Faded below this level the filter counts as silent, and its resonance is
// taken out of oscillation.
const float FILTER_SILENT = 0.01f;

// Wide open for this long, the filter stops running: by then the glide of
// the mix has long since arrived.
const float FILTER_BYPASS_SECONDS = 0.05f;

class BandFilter {
 public:
  // Call once from setup(), before the audio interrupt starts.
  void begin(int sampleRate = 48000) {
    sampleRate_ = (float)sampleRate;
    glide_ = 1.0f - expf(-1.0f / (FILTER_GLIDE_SECONDS * sampleRate_));
    changed_ = true;
    update(0.0f);
    now_ = target_;  // start where the settings are, with nothing to glide
  }

  // The band: its bottom and top cutoffs in Hz, each held within
  // FILTER_MIN_HZ to FILTER_MAX_HZ, and the top no lower than the bottom.
  // Takes effect at the next update().
  void setCutoffs(float lowHz, float highHz) {
    lowHz_ = clampf(lowHz, FILTER_MIN_HZ, FILTER_MAX_HZ);
    highHz_ = clampf(highHz, lowHz_, FILTER_MAX_HZ);
    width_ = log2f(highHz_ / lowHz_);
    changed_ = true;
  }

  // How loud the filtered sound is heard, from 0 (silent - and with its
  // resonance taken out of oscillation) to 1 (full). For fading it out when
  // the sound being filtered has stopped - see above. Takes effect at the
  // next update().
  void setLevel(float level) { level_ = clampf(level, 0.0f, 1.0f); }

  // Call every control tick from loop(), with the seconds since the last
  // call: passes anything that's changed on to the audio interrupt. (Each setting is a single float, which the
  // interrupt reads whole; if it catches some updated and some not, the
  // glide smooths that over.)
  void update(float seconds) {
    // How much of the sound is filtered, and how much isn't.
    float filtered = clampf((FILTER_OPEN_OCTAVES - width_) / FILTER_DRY_OCTAVES, 0.0f, 1.0f);
    target_.dry = 1.0f - filtered;
    target_.wet = filtered * level_;

    // Out of the circuit while wide open - see FILTER_BYPASS_SECONDS.
    if (filtered > 0.0f) {
      openSeconds_ = 0.0f;
      bypassed_ = false;
    } else if (!bypassed_) {
      openSeconds_ += seconds;
      bypassed_ = openSeconds_ >= FILTER_BYPASS_SECONDS;
    }

    // The low-pass resonance: rising over the last of the narrowing, as the
    // square of how far in, so it creeps up and then arrives - unless the
    // filter has been faded to silence.
    float in = clampf(1.0f - width_ / FILTER_RESONANCE_OCTAVES, 0.0f, 1.0f);
    bool silenced = level_ < FILTER_SILENT;
    target_.kHP = FILTER_BUMP;
    target_.kLP = silenced ? FILTER_BUMP : FILTER_BUMP + (FILTER_SCREAM - FILTER_BUMP) * in * in;

    // The stages' coefficients - the one bit of real maths, done only when
    // a knob has moved.
    if (changed_) {
      changed_ = false;
      target_.gHP = coefficient(lowHz_);
      target_.gLP = coefficient(highHz_);
    }
  }

  // Filters one output frame (left, right) in place. Call exactly once per
  // output sample.
  AUDIO_HOT inline void process(float frame[2]) {
    if (bypassed_) return;

    // Each setting a step closer to where update() last put it.
    Settings &s = now_;
    const Settings &t = target_;
    float a = glide_;
    s.gHP += (t.gHP - s.gHP) * a;
    s.gLP += (t.gLP - s.gLP) * a;
    s.kHP += (t.kHP - s.kHP) * a;
    s.kLP += (t.kLP - s.kLP) * a;
    s.dry += (t.dry - s.dry) * a;
    s.wet += (t.wet - s.wet) * a;
    Stage hp(s.gHP, s.kHP), lp(s.gLP, s.kLP);

    // A whisper of noise, about 100dB down, so the filter can break into
    // oscillation from silence, as an analog one does from its own hiss. A
    // fast pseudo-random number: each one from the last, by a multiply and
    // an add.
    seed_ = seed_ * 1103515245u + 12345u;
    float hiss = (float)(int32_t)seed_ * (0.00001f / 2147483648.0f);

    for (int c = 0; c < 2; c++) {
      // Samples arrive at 16-bit scale (+-32768); the stages work at +-1.
      float x = frame[c] * (1.0f / 32768.0f) + hiss;
      float band = lp.lowPass(state_[c][1], hp.highPass(state_[c][0], x));
      frame[c] = s.dry * frame[c] + s.wet * 32768.0f * saturate(band);
    }
  }

  float lowHz() const { return lowHz_; }
  float highHz() const { return highHz_; }
  float widthOctaves() const { return width_; }
  bool isOpen() const { return width_ >= FILTER_OPEN_OCTAVES; }

 private:
  // What the audio interrupt filters with: each stage's coefficient g and
  // resonance k, and the levels of the unfiltered and filtered sound.
  struct Settings {
    float gHP = 0.0f, gLP = 0.0f, kHP = 0.0f, kLP = 0.0f, dry = 1.0f, wet = 0.0f;
  };

  // One filter's memory: its two integrators.
  struct State {
    float s1 = 0.0f, s2 = 0.0f;
  };

  // One resonant stage, for a given g and k - see the top of this file.
  struct Stage {
    float g, k, c1, c2, gk;

    Stage(float g, float k) : g(g), k(k), c1(2.0f + g), c2(1.0f + g * (2.0f + g)), gk(g * k) {}

    // Advances the stage by one sample of x, returning its high-pass
    // output and setting lp to its low-pass. Written plainly, the solve is
    //   hp = (x - (d + g) * s1 - s2) / (1 + g * (d + g)),  d = 2 - k / a
    // with a = 1 + |s1| clipping the resonance - two divisions, this chip's
    // slowest arithmetic (14 cycles each). Multiplying top and bottom by a
    // makes it one. Using the state from the sample before for the clipping
    // keeps it a straight-line sum, with no solving for it.
    inline float step(State &st, float x, float &lp) const {
      float s1 = st.s1, s2 = st.s2;
      float a = 1.0f + __builtin_fabsf(s1);
      float hp = ((x - c1 * s1 - s2) * a + k * s1) / (a * c2 - gk);
      float v1 = g * hp;
      float bp = s1 + v1;
      float v2 = g * bp;
      lp = s2 + v2;
      st.s1 = bp + v1;
      st.s2 = lp + v2;
      return hp;
    }
    inline float highPass(State &st, float x) const { float lp; return step(st, x, lp); }
    inline float lowPass(State &st, float x) const { float lp; step(st, x, lp); return lp; }
  };

  // A soft limit on what comes out. Filtering moves a sound's peaks about -
  // a sawtooth with its fundamental taken away by the high-pass grows sharp
  // spikes - and with the bumps at the band's edges a loud sound can come out
  // louder than full scale, which would otherwise be clipped hard on the way
  // to the DAC. This rounds those peaks off instead, like an analog stage
  // running out of headroom: the curve x - 4x^3/27 leaves quiet sounds alone,
  // squeezes loud ones (full scale comes out at 85%), and levels off at full
  // scale for anything 1.5 times that or more, where it's held.
  static inline float saturate(float x) {
    float y = __builtin_fmaxf(-1.5f, __builtin_fminf(1.5f, x));
    return y - (4.0f / 27.0f) * y * y * y;
  }

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

  // A stage's coefficient for a cutoff: tan(pi * cutoff / sample rate),
  // which "pre-warps" the cutoff so the digital filter's lands exactly
  // where the analog one's would.
  float coefficient(float hz) const {
    if (hz > FILTER_HIGHEST_HZ * sampleRate_ / 48000.0f) hz = FILTER_HIGHEST_HZ * sampleRate_ / 48000.0f;
    return tanf(3.14159265f * hz / sampleRate_);
  }

  float sampleRate_ = 48000.0f;
  float glide_ = 0.0f;   // how far each setting moves toward its target, per sample
  Settings target_;      // where update() wants the settings
  Settings now_;         // where the interrupt has them, gliding toward target_
  State state_[2][2];    // [left/right][high-pass/low-pass]
  uint32_t seed_ = 12345;

  float lowHz_ = FILTER_MIN_HZ;
  float highHz_ = FILTER_MAX_HZ;
  float width_ = FILTER_OPEN_OCTAVES;
  bool changed_ = false;
  float level_ = 0.0f;     // the fade: 0 silent, 1 full; silent until loop() says otherwise
  float openSeconds_ = 0.0f;
  volatile bool bypassed_ = true;
};
