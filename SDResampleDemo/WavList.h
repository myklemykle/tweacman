// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  WavList - the WAV files on the card, in the one order everything uses.

  A FAT folder keeps its entries in roughly the order they were created, so
  readdir() hands names back in an order nobody chose. The web page's list,
  the file knob (knob2 with touch1 held) and the file cued when nothing else
  is all go through sortedWavNames() instead, so they agree.

  The order is "natural": case is ignored, and runs of digits compare as
  numbers, so TRACK2.WAV comes before TRACK10.WAV. Names are plain ASCII in
  practice - this core's FatFs converts long filenames through code page
  437, not Unicode - so nothing fancier than that is needed.
*/

#pragma once

#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include "FATFileSystem.h"  // opendir()/readdir() on the card, as in the sketch
#include <algorithm>
#include <vector>

// Whether a name on the card is one of the WAVs this demo lists and plays.
// Not a name starting with a dot: macOS leaves a hidden "._name.wav" beside
// every file it copies onto a FAT card (see "USB disk mode"), which ends in
// .wav but holds Finder metadata, not audio.
inline bool isWavName(const String &name) {
  if (name.startsWith(".")) return false;
  String upper = name;
  upper.toUpperCase();
  return upper.endsWith(".WAV");
}

// Negative, zero or positive as `a` sorts before, with or after `b`:
// ignoring case, with digit runs compared by value ("2" < "10").
inline int naturalCompare(const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      while (*a == '0') a++;              // leading zeros don't count
      while (*b == '0') b++;
      const char *endA = a, *endB = b;
      while (isdigit((unsigned char)*endA)) endA++;
      while (isdigit((unsigned char)*endB)) endB++;
      // More digits is a bigger number; the same count compares digit by digit.
      if (endA - a != endB - b) return (endA - a) < (endB - b) ? -1 : 1;
      int c = strncmp(a, b, endA - a);
      if (c != 0) return c;
      a = endA;
      b = endB;
      continue;
    }
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb) return ca < cb ? -1 : 1;
    a++;
    b++;
  }
  return (*a != 0) - (*b != 0);  // a name that runs out first sorts first
}

// Every WAV in `folder`, sorted. Empty if it can't be opened.
inline std::vector<String> sortedWavNames(const char *folder = "/sd") {
  std::vector<String> names;
  DIR *dir = opendir(folder);
  if (!dir) return names;
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_type != DT_REG) continue;
    String name(ent->d_name);
    if (isWavName(name)) names.push_back(name);
  }
  closedir(dir);
  std::sort(names.begin(), names.end(), [](const String &a, const String &b) {
    int c = naturalCompare(a.c_str(), b.c_str());
    // "TRACK01" and "TRACK1" compare equal above; settle it so the order
    // never depends on where the folder happened to put them.
    return c != 0 ? c < 0 : strcmp(a.c_str(), b.c_str()) < 0;
  });
  return names;
}
