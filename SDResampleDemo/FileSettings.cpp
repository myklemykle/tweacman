// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

#include "FileSettings.h"

#include "FATFileSystem.h"  // opendir()/mkdir() on the card
#include "WavList.h"        // setOf(), for files in a set

#include <cmath>

static const char *const SETTINGS_DIR = "/sd/settings";
static const char *const SETTINGS_SUFFIX = ".txt";
static const char *const CURRENT_PATH = "/sd/settings/current.txt";
static const char *const SET_LEAF = "set.txt";

const float FileSettings::DEFAULT_GAP_SECONDS = 10.0f;

static String pathFor(const String &wavName) {
  return String(SETTINGS_DIR) + "/" + wavName + SETTINGS_SUFFIX;
}

static bool exists(const String &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

// The folder for a set's settings, "" being the root's.
static String folderFor(const String &set) {
  return set.length() ? String(SETTINGS_DIR) + "/" + set : String(SETTINGS_DIR);
}

// Writes `text` to `path` through a .tmp file - see the header comment.
// `set` is the set it belongs to, whose folder may need making first.
static bool writeReplacing(const String &path, const String &text, const String &set = "") {
  mkdir(SETTINGS_DIR, 0777);  // fails harmlessly when it already exists
  if (set.length()) mkdir(folderFor(set).c_str(), 0777);
  String temp = path + ".tmp";
  FILE *f = fopen(temp.c_str(), "w");
  if (!f) return false;
  bool ok = fputs(text.c_str(), f) >= 0;
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    remove(temp.c_str());
    return false;
  }
  remove(path.c_str());  // FatFs won't rename onto an existing file
  return rename(temp.c_str(), path.c_str()) == 0;
}

static bool nearlyEqual(float a, float b) {
  return fabsf(a - b) <= 1e-4f * fmaxf(1.0f, fabsf(a));
}

bool FileSettings::operator==(const FileSettings &other) const {
  return nearlyEqual(speed, other.speed) && nearlyEqual(pitch, other.pitch) &&
         windowStart == other.windowStart && windowEnd == other.windowEnd &&
         nearlyEqual(echoTime, other.echoTime) && nearlyEqual(echoAmount, other.echoAmount) &&
         nearlyEqual(filterCentre, other.filterCentre) && nearlyEqual(filterWidth, other.filterWidth);
}

FileSettings FileSettings::load(const String &wavName) {
  FileSettings s;
  FILE *f = fopen(pathFor(wavName).c_str(), "r");
  if (!f) return s;
  // Files saved before the filter was set by centre and width have its
  // bottom and top cutoffs instead: those are turned into a centre and width.
  float low = 0.0f, high = 0.0f;
  bool haveBand = false;
  char line[64];
  while (fgets(line, sizeof(line), f)) {
    String text(line);
    text.trim();
    int eq = text.indexOf('=');
    if (eq == -1) continue;
    String key = text.substring(0, eq);
    String number = text.substring(eq + 1);
    float value = number.toFloat();
    if (key == "speed") s.speed = value;
    else if (key == "pitch" && value > 0.0f) s.pitch = value;
    // Whole frames, read as integers: a float can't hold a long file's.
    else if (key == "window_start") s.windowStart = strtoul(number.c_str(), nullptr, 10);
    else if (key == "window_end") s.windowEnd = strtoul(number.c_str(), nullptr, 10);
    else if (key == "echo_time" && value > 0.0f) s.echoTime = value;
    else if (key == "echo_amount" && value >= 0.0f) s.echoAmount = value;
    else if (key == "filter_centre" && value > 0.0f) s.filterCentre = value, haveBand = true;
    else if (key == "filter_width" && value >= 0.0f) s.filterWidth = value, haveBand = true;
    else if (key == "filter_low" && value > 0.0f) low = value;
    else if (key == "filter_high" && value > 0.0f) high = value;
  }
  fclose(f);
  if (!haveBand && low > 0.0f && high >= low) {
    s.filterCentre = sqrtf(low * high);
    s.filterWidth = fminf(log2f(high / low), 10.0f);
  }
  return s;
}

bool FileSettings::save(const String &wavName) const {
  String text = "speed=" + String(speed, 6) + "\n" +
                "pitch=" + String(pitch, 6) + "\n" +
                "window_start=" + String(windowStart) + "\n" +
                "window_end=" + String(windowEnd) + "\n" +
                "echo_time=" + String(echoTime, 6) + "\n" +
                "echo_amount=" + String(echoAmount, 6) + "\n" +
                "filter_centre=" + String(filterCentre, 6) + "\n" +
                "filter_width=" + String(filterWidth, 6) + "\n";
  return writeReplacing(pathFor(wavName), text, setOf(wavName));
}

void FileSettings::removeFor(const String &wavName) {
  remove(pathFor(wavName).c_str());
}

void FileSettings::renameFor(const String &from, const String &to) {
  // A change of case alone is the same file as far as FAT is concerned.
  if (!to.equalsIgnoreCase(from)) removeFor(to);
  String set = setOf(to);
  if (set.length()) {
    mkdir(SETTINGS_DIR, 0777);
    mkdir(folderFor(set).c_str(), 0777);
  }
  rename(pathFor(from).c_str(), pathFor(to).c_str());
}

// Removes the settings in one set's folder (the root's, for set "") whose
// WAV is gone. The root's pass also lists the sets' folders it finds, in
// setFolders, to be gone through next - the same scheme as MipCache's.
static void removeOrphansIn(const String &set, std::vector<String> *setFolders) {
  // Collected first and removed after, rather than deleting while the
  // folder is still being read - same as MipCache::removeOrphans().
  const int BATCH = 16;
  const String suffix = String(".wav") + SETTINGS_SUFFIX;
  while (true) {
    String orphans[BATCH];
    int found = 0;
    DIR *dir = opendir(folderFor(set).c_str());
    if (!dir) return;
    struct dirent *ent;
    while (found < BATCH && (ent = readdir(dir)) != nullptr) {
      String name(ent->d_name);
      if (ent->d_type == DT_DIR) {
        if (setFolders && name != "." && name != "..") setFolders->push_back(name);
        continue;
      }
      if (ent->d_type != DT_REG) continue;
      String lower = name;
      lower.toLowerCase();
      if (!lower.endsWith(suffix)) continue;  // current.txt, set.txt, or a stray .tmp
      String wavName = pathInSet(set, name.substring(0, name.length() - strlen(SETTINGS_SUFFIX)));
      if (!exists("/sd/" + wavName)) orphans[found++] = wavName;
    }
    closedir(dir);
    if (setFolders && found == BATCH) setFolders->clear();  // listed again next time round
    for (int i = 0; i < found; i++) {
      Serial.print("Settings: removing orphaned settings for ");
      Serial.println(orphans[i]);
      FileSettings::removeFor(orphans[i]);
    }
    if (found < BATCH) return;
  }
}

void FileSettings::removeOrphans() {
  std::vector<String> sets;
  removeOrphansIn("", &sets);
  for (const String &set : sets) {
    removeOrphansIn(set, nullptr);
    // A set whose folder is gone from the card altogether loses its gap too,
    // and then its folder here, now empty.
    DIR *dir = opendir(("/sd/" + set).c_str());
    if (dir) {
      closedir(dir);
    } else {
      remove((folderFor(set) + "/" + SET_LEAF).c_str());
      remove(folderFor(set).c_str());
    }
  }
}

String FileSettings::loadCurrentName() {
  FILE *f = fopen(CURRENT_PATH, "r");
  if (!f) return "";
  char line[320];  // a path: a set's name, and a long filename in it
  String name = fgets(line, sizeof(line), f) ? String(line) : String();
  fclose(f);
  name.trim();
  return name;
}

void FileSettings::saveCurrentName(const String &wavName) {
  writeReplacing(CURRENT_PATH, wavName + "\n");
}

float FileSettings::loadGap(const String &set) {
  float gap = DEFAULT_GAP_SECONDS;
  FILE *f = fopen((folderFor(set) + "/" + SET_LEAF).c_str(), "r");
  if (!f) return gap;
  char line[64];
  while (fgets(line, sizeof(line), f)) {
    String text(line);
    text.trim();
    if (text.startsWith("gap=")) {
      float value = text.substring(4).toFloat();
      if (value >= 0.0f) gap = value;
    }
  }
  fclose(f);
  return gap;
}

void FileSettings::saveGap(const String &set, float seconds) {
  writeReplacing(folderFor(set) + "/" + SET_LEAF, "gap=" + String(seconds, 6) + "\n", set);
}
