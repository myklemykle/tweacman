// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  FileSettings - each WAV's own playback settings, kept on the SD card so
  they come back when the file is picked again, and after a reboot.

  One small text file per WAV, /sd/settings/<wav name>.txt, one setting per
  line:

    speed=1.000000
    pitch=0.500000

  Plain text so it can be read or edited from a computer. Unknown lines are
  ignored and missing ones keep their defaults, so adding a setting later
  never breaks the files already on a card.

  They live in their own folder rather than next to the WAV's cached copies
  in /sd/cache, because deleting cache/ is the way to force those to be
  rebuilt, and that shouldn't wipe anyone's settings. The sketch keeps them
  in step with the WAVs the same way it does the cache: renamed with the
  file, removed with it, and orphans cleared at boot.

  The folder also holds current.txt, the name of the file that was cued
  last, so the board comes back to it after a reboot.

  Each file is written to a .tmp first and then renamed into place, so a
  power cut mid-write leaves the old settings rather than half of new ones.
*/

#pragma once

#include <Arduino.h>

struct FileSettings {
  float speed = 1.0f;  // requested playback speed, negative for backward
  float pitch = 1.0f;  // pitch ratio, 1 for unshifted

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
};
