// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  WavList - the WAV files on the card, in the one order everything uses.

  The card is organised into sets: each folder at its root is one, and the
  WAVs at the root itself are one more, the default set. Only one level
  deep - a folder inside a set is ignored. A file is named everywhere by
  its path from the card's root: "HAL/001 - Open the doors.wav" for one in
  a set, plain "boom.wav" for one at the root. Set names are folder names,
  "" for the root.

  A FAT folder keeps its entries in roughly the order they were created, so
  readdir() hands names back in an order nobody chose. The web page's list,
  the file knob (knob2 with touch1 held), a set playing through, and the
  file cued when nothing else is all go through sortedWavNames() and
  sortedSetNames() instead, so they agree.

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

// Sorts names in place, naturally (see above).
inline void sortNaturally(std::vector<String> &names) {
  std::sort(names.begin(), names.end(), [](const String &a, const String &b) {
    int c = naturalCompare(a.c_str(), b.c_str());
    // "TRACK01" and "TRACK1" compare equal above; settle it so the order
    // never depends on where the folder happened to put them.
    return c != 0 ? c < 0 : strcmp(a.c_str(), b.c_str()) < 0;
  });
}

// -- Paths and sets ------------------------------------------------------------

// The set a file's path is in ("" for the root), and its name within it.
inline String setOf(const String &path) {
  int slash = path.lastIndexOf('/');
  return slash == -1 ? String() : path.substring(0, slash);
}
inline String leafOf(const String &path) {
  return path.substring(path.lastIndexOf('/') + 1);
}
inline String pathInSet(const String &set, const String &leaf) {
  return set.length() ? set + "/" + leaf : leaf;
}

// Whether a folder at the card's root is a set. Not the sketch's own
// folders, nor hidden ones (macOS leaves .Spotlight-V100 and .fseventsd on
// every card it mounts), nor the one Windows keeps.
inline bool isSetFolderName(const String &name) {
  if (name.length() == 0 || name.startsWith(".")) return false;
  return !name.equalsIgnoreCase("cache") && !name.equalsIgnoreCase("settings") &&
         !name.equalsIgnoreCase("System Volume Information");
}

// Whether a path names a file the sketch could play: a WAV at the root or
// in a set, no deeper.
inline bool isTrackPath(const String &path) {
  String set = setOf(path);
  if (set.indexOf('/') != -1) return false;
  if (set.length() && !isSetFolderName(set)) return false;
  return isWavName(leafOf(path));
}

// Every WAV in a set, by name within it, sorted. Empty if there's no such
// set.
inline std::vector<String> sortedWavNames(const String &set = "") {
  std::vector<String> names;
  DIR *dir = opendir(set.length() ? ("/sd/" + set).c_str() : "/sd");
  if (!dir) return names;
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_type != DT_REG) continue;
    String name(ent->d_name);
    if (isWavName(name)) names.push_back(name);
  }
  closedir(dir);
  sortNaturally(names);
  return names;
}

// The same, as paths from the card's root.
inline std::vector<String> sortedWavPaths(const String &set) {
  std::vector<String> paths = sortedWavNames(set);
  for (String &p : paths) p = pathInSet(set, p);
  return paths;
}

// Every set with a WAV in it: the root first, if it has any, then the
// folders, sorted.
inline std::vector<String> sortedSetNames() {
  std::vector<String> folders;
  DIR *dir = opendir("/sd");
  if (!dir) return folders;
  bool rootHasWavs = false;
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    String name(ent->d_name);
    if (ent->d_type == DT_DIR && isSetFolderName(name)) folders.push_back(name);
    if (ent->d_type == DT_REG && isWavName(name)) rootHasWavs = true;
  }
  closedir(dir);
  sortNaturally(folders);

  std::vector<String> sets;
  if (rootHasWavs) sets.push_back("");
  for (const String &folder : folders) {
    if (!sortedWavNames(folder).empty()) sets.push_back(folder);
  }
  return sets;
}
