// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

#include "FileSettings.h"

#include "FATFileSystem.h"  // opendir()/mkdir() on the card

#include <cmath>

static const char *const SETTINGS_DIR = "/sd/settings";
static const char *const SETTINGS_SUFFIX = ".txt";
static const char *const CURRENT_PATH = "/sd/settings/current.txt";

static String pathFor(const String &wavName) {
  return String(SETTINGS_DIR) + "/" + wavName + SETTINGS_SUFFIX;
}

static bool exists(const String &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

// Writes `text` to `path` through a .tmp file - see the header comment.
static bool writeReplacing(const String &path, const String &text) {
  mkdir(SETTINGS_DIR, 0777);  // fails harmlessly when it already exists
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
         windowStart == other.windowStart && windowEnd == other.windowEnd;
}

FileSettings FileSettings::load(const String &wavName) {
  FileSettings s;
  FILE *f = fopen(pathFor(wavName).c_str(), "r");
  if (!f) return s;
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
  }
  fclose(f);
  return s;
}

bool FileSettings::save(const String &wavName) const {
  String text = "speed=" + String(speed, 6) + "\n" +
                "pitch=" + String(pitch, 6) + "\n" +
                "window_start=" + String(windowStart) + "\n" +
                "window_end=" + String(windowEnd) + "\n";
  return writeReplacing(pathFor(wavName), text);
}

void FileSettings::removeFor(const String &wavName) {
  remove(pathFor(wavName).c_str());
}

void FileSettings::renameFor(const String &from, const String &to) {
  // A change of case alone is the same file as far as FAT is concerned.
  if (!to.equalsIgnoreCase(from)) removeFor(to);
  rename(pathFor(from).c_str(), pathFor(to).c_str());
}

void FileSettings::removeOrphans() {
  // Collected first and removed after, rather than deleting while the
  // folder is still being read - same as MipCache::removeOrphans().
  const int BATCH = 16;
  const String suffix = String(".wav") + SETTINGS_SUFFIX;
  while (true) {
    String orphans[BATCH];
    int found = 0;
    DIR *dir = opendir(SETTINGS_DIR);
    if (!dir) return;
    struct dirent *ent;
    while (found < BATCH && (ent = readdir(dir)) != nullptr) {
      if (ent->d_type != DT_REG) continue;
      String name(ent->d_name);
      String lower = name;
      lower.toLowerCase();
      if (!lower.endsWith(suffix)) continue;  // current.txt, or a stray .tmp
      String wavName = name.substring(0, name.length() - strlen(SETTINGS_SUFFIX));
      if (!exists("/sd/" + wavName)) orphans[found++] = wavName;
    }
    closedir(dir);
    for (int i = 0; i < found; i++) {
      Serial.print("Settings: removing orphaned settings for ");
      Serial.println(orphans[i]);
      removeFor(orphans[i]);
    }
    if (found < BATCH) return;
  }
}

String FileSettings::loadCurrentName() {
  FILE *f = fopen(CURRENT_PATH, "r");
  if (!f) return "";
  char line[128];
  String name = fgets(line, sizeof(line), f) ? String(line) : String();
  fclose(f);
  name.trim();
  return name;
}

void FileSettings::saveCurrentName(const String &wavName) {
  writeReplacing(CURRENT_PATH, wavName + "\n");
}
