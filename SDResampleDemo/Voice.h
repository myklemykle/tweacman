// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  Voice - one playback stream in SDResampleDemo: a copy of the current file
  (the WAV itself, or one of MipCache's filtered lower-rate copies), the ring
  buffer it's read into, and a position within it. See the "Playback
  voices" section of SDResampleDemo.ino for how two of these take turns.

  In a header of its own only because Arduino generates prototypes for the
  sketch's functions near the top of the .ino, ahead of anything declared
  there - so a type those functions take has to come from an #include.
*/

#pragma once

#include "RingBuffer.h"
#include "Resampler.h"
#include "WavReader.h"

const uint32_t RING_CAPACITY = 32768;  // per voice, in samples: 16384 stereo frames or
                                        // 32768 mono ones - ~370ms of stereo at 1x on a
                                        // 44.1kHz source. A voice never reads its copy much
                                        // faster than 2x (see LEVEL_UP_STEP in the sketch).
                                        // Part of this is kept as trailing-edge headroom
                                        // (REVERSAL_MARGIN).

// The sketch's window (see "The window" there), fitted to one copy of the
// file, in that copy's frames.
struct CopyWindow {
  uint32_t start = 0;
  uint32_t length = 1;
  uint32_t fade = 0;       // length of the crossfade joining its end to its start
  bool fadeAtEnd = true;   // whether that's over its last frames, or its first
};

struct Voice {
  WavReader reader;                     // the WAV itself, or one of its cached copies
  AudioRingBuffer<RING_CAPACITY> ring;
  Resampler resampler;                  // position, in this copy's own samples
  uint8_t level = 0;                    // 0 = the WAV, k = the copy at 1/2^k its rate
  float levelScale = 1.0f;              // 1/2^level: converts a step through the
                                        // original file into a step through this copy

  // The stretch of this copy that plays, as a loop - "The window" in the
  // sketch. The voice's position runs on and on, and maps onto the copy
  // round and round the window, with position windowBase at its start.
  CopyWindow window;
  int32_t windowBase = 0;
};
