// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  FileSettings - each WAV's own playback settings, kept on the SD card so
  they come back when the file is picked again, and after a reboot.

  One small text file per WAV, /sd/settings/<wav name>.txt - or
  /sd/settings/<set>/<wav name>.txt for one in a set (see WavList.h) - one
  setting per line:

    speed=1.000000
    pitch=0.500000
    window_start=11025
    window_end=0
    echo_time=0.300000
    echo_amount=0.500000
    filter_centre=640.000000
    filter_width=10.000000

  Plain text so it can be read or edited from a computer. Unknown lines are
  ignored and missing ones keep their defaults, so adding a setting later
  never breaks the files already on a card.

  They live in their own folder rather than next to the WAV's cached copies
  in /sd/cache, because deleting cache/ is the way to force those to be
  rebuilt, and that shouldn't wipe anyone's settings. The sketch keeps them
  in step with the WAVs the same way it does the cache: renamed with the
  file, removed with it, and orphans cleared at boot.

  The folder also holds current.txt, the path of the file that was cued
  last, so the board comes back to it after a reboot, and set.txt, the
  default set's own settings - just the gap a set leaves between its
  tracks when it plays through, gap=10.000000. Each set's folder in there
  has a set.txt of its own.

  Each file is written to a .tmp first and then renamed into place, so a
  power cut mid-write leaves the old settings rather than half of new ones.
*/

#pragma once

#include <Arduino.h>

struct FileSettings {
  float speed = 1.0f;  // requested playback speed, negative for backward
  float pitch = 1.0f;  // pitch ratio, 1 for unshifted
  uint32_t windowStart = 0;  // the stretch of the file that plays, in frames -
  uint32_t windowEnd = 0;    // its end being 0 for "the end of the file"
  float echoTime = 0.3f;    // the echo's delay, in seconds
  float echoAmount = 0.0f;  // its level and feedback, 0 for no echo
  float filterCentre = 640.0f;  // the filter's centre, in Hz...
  float filterWidth = 10.0f;    // ...and its width in octaves; 10 is wide open, no filter

  // Close enough counts as equal: the knobs recompute these through the
  // dial's own mapping, which doesn't land back on exactly the value that
  // was loaded.
  bool operator==(const FileSettings &other) const;
  bool operator!=(const FileSettings &other) const { return !(*this == other); }

  // The settings saved for a WAV, or the defaults if there are none.
  static FileSettings load(const String &wavName);
  bool save(const String &wavName) const;

  // Card housekeeping, called alongside the WAV's own.
  static void removeFor(const String &wavName);
  static void renameFor(const String &from, const String &to);
  static void removeOrphans();

  // Which file was cued last - empty if none is recorded.
  static String loadCurrentName();
  static void saveCurrentName(const String &wavName);

  // The silence a set leaves between its tracks, in seconds, when it plays
  // through - see "Sets" in the sketch.
  static const float DEFAULT_GAP_SECONDS;
  static float loadGap(const String &set);
  static void saveGap(const String &set, float seconds);
};
