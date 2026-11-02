// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

#include "MipCache.h"

#include "FATFileSystem.h"  // opendir()/mkdir() on the card
#include "WavList.h"        // setOf(), for files in a set

#include <sys/stat.h>
#include <cmath>
#include <cstring>

// Bump whenever the filter or file layout changes, so caches built by an
// older version of this code are recognised as stale and rebuilt. (2: caches
// built while reads into misaligned buffers came back garbled - see
// RA6M5FastSD.cpp - can have corrupt stretches near their loop points.)
static const char *CACHE_FORMAT = "MIPCACHE 3";  // 3: copies can be stereo

// -- The half-band filter --------------------------------------------------
// A half-band low-pass cuts at a quarter of the input rate - exactly the
// output rate's Nyquist limit after halving - and has the handy property
// that every even-numbered tap except the centre one is zero, so only the
// centre and the odd taps need multiplying. Symmetric, so each odd tap is
// applied to a pair of samples equidistant from the centre.
static const int FILTER_HALF = 31;                      // 63 taps: -31..+31
static const int ODD_TAPS = (FILTER_HALF + 1) / 2;      // taps 1, 3, ... 31
static float oddTap[ODD_TAPS];                          // oddTap[i] is tap 2i+1
static bool filterReady = false;

static void initFilter() {
  if (filterReady) return;
  float sum = 0.0f;
  for (int i = 0; i < ODD_TAPS; i++) {
    int n = 2 * i + 1;
    // Ideal half-band response (a sinc cut at a quarter of the sample
    // rate), shaped by a Blackman window to tame its ripple.
    float ideal = sinf(PI * n / 2.0f) / (PI * n);
    float w = 0.42f + 0.5f * cosf(PI * n / FILTER_HALF) + 0.08f * cosf(2.0f * PI * n / FILTER_HALF);
    oddTap[i] = ideal * w;
    sum += oddTap[i];
  }
  // Unity gain at DC: the centre tap is 0.5, so the odd taps on both sides
  // must contribute the other 0.5 - i.e. 0.25 per side.
  for (int i = 0; i < ODD_TAPS; i++) oddTap[i] *= 0.25f / sum;
  filterReady = true;
}

// One building step filters OUT_BLOCK output frames - few enough that each
// step is quick. The card, though, is happiest with fewer, larger transfers
// (see RA6M5FastSD.h), so input is read READ_OUTPUTS mono outputs' worth at
// a time - the 2 * READ_OUTPUTS input frames they're centred on, plus
// FILTER_HALF either side - and output collects in writeBuf until there are
// WRITE_BLOCK samples (8KB) to write in one go. Stereo frames are twice the
// size, so a stereo copy is read and written half as many frames at a time,
// in the same buffers. WRITE_BLOCK stays a multiple of 256 samples (one
// sector), so every write but a copy's last is sector-aligned in the file,
// which multi-block writing needs.
//
// Writes are still the slow part, and the stalls a build causes in loop()
// come from them: an 8KB write takes ~2.7ms on the bench card (~3MB/s,
// worst ~8ms), measured with RA6M5FastSD's SDWriteBench. (It was ~25ms
// until RA6M5FastSD stopped FatFs erasing every sector before writing it.)
static const uint32_t OUT_BLOCK = 256;
static const uint32_t READ_OUTPUTS = 2048;
static const uint32_t IN_CAPACITY = 2 * READ_OUTPUTS + 4 * FILTER_HALF;  // samples - room
                                                                         // for stereo's margins
static const uint32_t WRITE_BLOCK = 4096;
// Both 4-byte aligned for the SD host's DMA. inRaw has a spare sample so a
// read can start one sample in, to match the file position - see
// WavReader::alignmentOf(). Both hold interleaved frames.
alignas(4) static int16_t inRaw[IN_CAPACITY + 1];
alignas(4) static int16_t writeBuf[WRITE_BLOCK];
static int16_t *inData = inRaw;  // where the current input window starts in inRaw

// Same wraparound as SDResampleDemo.ino's fileOffsetFor(): maps a possibly
// negative position onto a file treated as a loop.
static uint32_t wrapOffset(int32_t pos, uint32_t total) {
  int32_t m = pos % (int32_t)total;
  if (m < 0) m += (int32_t)total;
  return (uint32_t)m;
}

// -- Fingerprint ----------------------------------------------------------

// FNV-1a: a tiny, adequate hash for "has this file changed?"
static uint32_t fnv1a(uint32_t h, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

// Length, rate, channels, and the first and last 4KB of sample data.
static uint32_t fingerprint(WavReader &wav) {
  uint32_t h = 2166136261u;
  uint32_t total = wav.totalFrames();
  uint32_t rate = wav.nativeSampleRate();
  uint8_t channels = wav.channels();
  h = fnv1a(h, &total, sizeof(total));
  h = fnv1a(h, &rate, sizeof(rate));
  h = fnv1a(h, &channels, sizeof(channels));
  uint32_t span = 2048 / channels;  // frames, i.e. 4KB
  uint32_t n = total < span ? total : span;
  uint32_t perPiece = IN_CAPACITY / channels;
  // Hashed a buffer's worth at a time, borrowing the builder's input
  // buffer, which is sized for building, not for this.
  for (uint32_t start : {(uint32_t)0, total - n}) {
    for (uint32_t done = 0; done < n;) {
      uint32_t piece = n - done < perPiece ? n - done : perPiece;
      size_t got = wav.readFramesAt(start + done, inRaw, piece);
      if (got == 0) break;
      h = fnv1a(h, inRaw, got * 2 * channels);
      done += got;
    }
  }
  return h;
}

// -- Paths and housekeeping -------------------------------------------------

// A WAV's cache folder is named by a hash of its path, ignoring case as
// FAT does - see "Why such cryptic names" in MipCache.h.
static uint32_t pathHash(const String &wavName) {
  String lower = wavName;
  lower.toLowerCase();
  return fnv1a(2166136261u, lower.c_str(), lower.length());
}

static String folderName(uint32_t hash) {
  char name[9];
  snprintf(name, sizeof(name), "%08lX", (unsigned long)hash);
  return String(name);
}

String MipCache::dirFor(const String &wavName) {
  return "/sd/cache/" + folderName(pathHash(wavName));
}

String MipCache::levelPath(const String &wavName, uint8_t level) {
  char leaf[16];
  snprintf(leaf, sizeof(leaf), "/L%02u.RAW", level);
  return dirFor(wavName) + leaf;
}

static String infoPathFor(const String &wavName) { return MipCache::dirFor(wavName) + "/INFO.TXT"; }

// Deletes everything in a cache folder but the folder itself.
static void emptyDir(const String &wavName) {
  for (uint8_t k = 1; k <= MipCache::MAX_LEVELS; k++) remove(MipCache::levelPath(wavName, k).c_str());
  remove(infoPathFor(wavName).c_str());
}

void MipCache::removeFor(const String &wavName) {
  emptyDir(wavName);
  remove(dirFor(wavName).c_str());  // FatFs only removes an empty folder
}

void MipCache::renameFor(const String &from, const String &to) {
  // A change of case alone hashes the same, so the folder stays put;
  // anything else must not land on top of a stale cache for the new name.
  if (pathHash(from) == pathHash(to)) return;
  removeFor(to);
  if (rename(dirFor(from).c_str(), dirFor(to).c_str()) != 0) {
    removeFor(from);  // couldn't move it, so don't leave it orphaned - it gets rebuilt
  }
}

// Deletes a file, or a folder and everything in it.
static void removeTree(const String &path) {
  // Collected a batch at a time and deleted after, rather than deleting
  // while the folder is still being read.
  const int BATCH = 16;
  while (true) {
    String names[BATCH];
    bool isDir[BATCH];
    int found = 0;
    DIR *dir = opendir(path.c_str());
    if (!dir) break;  // a file, not a folder
    struct dirent *ent;
    while (found < BATCH && (ent = readdir(dir)) != nullptr) {
      String name(ent->d_name);
      if (name == "." || name == "..") continue;
      names[found] = name;
      isDir[found++] = ent->d_type == DT_DIR;
    }
    closedir(dir);
    if (found == 0) break;
    for (int i = 0; i < found; i++) {
      String child = path + "/" + names[i];
      if (isDir[i]) {
        removeTree(child);
      } else {
        remove(child.c_str());
      }
    }
  }
  remove(path.c_str());
}

void MipCache::removeOrphans() {
  // The folder name every WAV on the card would have...
  std::vector<String> keep;
  for (const String &set : sortedSetNames()) {
    for (const String &path : sortedWavPaths(set)) keep.push_back(folderName(pathHash(path)));
  }
  // ...and anything else in /sd/cache goes, a batch at a time.
  const int BATCH = 16;
  int removed = 0;
  while (true) {
    String orphans[BATCH];
    int found = 0;
    DIR *dir = opendir("/sd/cache");
    if (!dir) return;
    struct dirent *ent;
    while (found < BATCH && (ent = readdir(dir)) != nullptr) {
      String name(ent->d_name);
      if (name == "." || name == "..") continue;
      bool wanted = false;
      for (const String &k : keep) {
        if (name.equalsIgnoreCase(k)) {
          wanted = true;
          break;
        }
      }
      if (!wanted) orphans[found++] = name;
    }
    closedir(dir);
    for (int i = 0; i < found; i++) removeTree("/sd/cache/" + orphans[i]);
    removed += found;
    if (found < BATCH) break;
  }
  if (removed) {
    Serial.print("Cache: removed ");
    Serial.print(removed);
    Serial.println(" folder(s) of copies whose WAV is gone.");
  }
}

// Whether a copy on the card is finished: written under its own name from
// the start, it only reaches its full length once it is.
bool MipCache::levelComplete(uint8_t level) const {
  struct stat st;
  if (stat(levelPath(name_, level).c_str(), &st) != 0) return false;
  return (uint32_t)st.st_size == levelLen_[level] * 2 * channels_;
}

// -- Building ---------------------------------------------------------------

void MipCache::start(const String &wavName) {
  stop();
  initFilter();
  name_ = wavName;
  ready_ = 0;
  wipeLevel_ = 0;
  count_ = 0;
  workTotal_ = 0;
  workDone_ = 0;

  WavReader wav;
  if (!wav.open(("/sd/" + wavName).c_str())) return;
  sampleRate_ = wav.nativeSampleRate();
  channels_ = wav.channels();
  levelLen_[0] = wav.totalFrames();
  while (count_ < MAX_LEVELS && levelLen_[count_] / 2 >= MIN_LEVEL_SAMPLES) {
    levelLen_[count_ + 1] = levelLen_[count_] / 2;
    count_++;
  }
  if (count_ == 0) return;  // too short to be worth caching

  char expected[64];
  snprintf(expected, sizeof(expected), "%s %lu %lu %u %08lx", CACHE_FORMAT,
           (unsigned long)levelLen_[0], (unsigned long)sampleRate_, channels_,
           (unsigned long)fingerprint(wav));
  wav.close();

  char found[64] = "";
  FILE *info = fopen(infoPathFor(wavName).c_str(), "r");
  if (info) {
    if (!fgets(found, sizeof(found), info)) found[0] = '\0';
    fclose(info);
    found[strcspn(found, "\r\n")] = '\0';
  } else {
    mkdir("/sd/cache", 0777);  // either may exist already, which is fine
    mkdir(dirFor(wavName).c_str(), 0777);
  }
  if (strcmp(found, expected) != 0) {
    // Missing, stale, or from an older format: start over. The old copies
    // are deleted a couple per step() rather than all at once here - a
    // card can take a few hundred milliseconds to delete twenty files,
    // and the file that's just been loaded is already playing. INFO.TXT is
    // only rewritten once they're gone, so a reset mid-wipe just finds the
    // cache still stale next time, and wipes again.
    if (found[0]) {
      Serial.print("Cache: ");
      Serial.print(wavName);
      Serial.println(" has changed since its copies were made - rebuilding.");
    }
    wipeLevel_ = MAX_LEVELS;
    strncpy(info_, expected, sizeof(info_) - 1);
  } else {
    while (ready_ < count_ && levelComplete(ready_ + 1)) ready_++;
  }

  workTotal_ = 0;
  workDone_ = 0;
  for (uint8_t k = 1; k <= count_; k++) {
    workTotal_ += levelLen_[k];
    if (k <= ready_) workDone_ += levelLen_[k];
  }
  active_ = ready_ < count_;
  startMillis_ = millis();
  if (active_) {
    Serial.print("Cache: building ");
    Serial.print(count_ - ready_);
    Serial.print(" copies of ");
    Serial.println(wavName);
  }
}

void MipCache::stop() {
  if (out_) {
    fclose(out_);
    out_ = nullptr;
  }
  src_.close();
  outDone_ = 0;
  active_ = false;
}

int MipCache::percentDone() const {
  if (workTotal_ == 0) return 100;
  return (int)((uint64_t)workDone_ * 100 / workTotal_);
}

bool MipCache::openNextLevel() {
  uint8_t k = ready_ + 1;
  bool ok = (k == 1) ? src_.open(("/sd/" + name_).c_str())
                     : src_.openRaw(levelPath(name_, k - 1).c_str(), sampleRate_ >> (k - 1),
                                    channels_);
  if (!ok) return false;
  out_ = fopen(levelPath(name_, k).c_str(), "wb");
  if (!out_) {
    src_.close();
    return false;
  }
  // Unbuffered: the C library's buffering would split every write into 1KB
  // pieces, defeating the point of collecting 8KB in writeBuf.
  setvbuf(out_, nullptr, _IONBF, 0);
  outDone_ = 0;
  pending_ = 0;
  inFirst_ = 0;
  inCovered_ = 0;
  return true;
}

void MipCache::finishLevel() {
  // Closing it is all it takes: it's under its own name already, and now
  // at full length - see levelComplete().
  if (fclose(out_) != 0) {
    out_ = nullptr;
    src_.close();
    Serial.println("Cache: couldn't finish a copy - stopping.");
    active_ = false;
    return;
  }
  out_ = nullptr;
  src_.close();
  ready_++;
  if (ready_ == count_) {
    active_ = false;
    Serial.print("Cache: ");
    Serial.print(name_);
    Serial.print(" ready, ");
    Serial.print(count_);
    Serial.print(" copies built in ");
    Serial.print(millis() - startMillis_);
    Serial.println("ms");
  }
}

bool MipCache::step() {
  if (!active_) return false;

  // Clearing out a stale cache first, one level's files per step - see
  // start(). Then the new description, and building can begin.
  if (wipeLevel_ > 0) {
    remove(levelPath(name_, wipeLevel_).c_str());
    if (--wipeLevel_ == 0) {
      FILE *info = fopen(infoPathFor(name_).c_str(), "w");
      if (!info) {
        Serial.println("Cache: couldn't write to the card - playing without copies.");
        stop();
        return false;
      }
      fprintf(info, "%s\n", info_);
      fclose(info);
    }
    return true;
  }

  // Opening and finishing a copy each get a step to themselves - creating
  // and closing a file are some of the slowest things FatFs does, and
  // stacking them on top of a block of filtering would double the stall.
  if (!out_) {
    if (!openNextLevel()) {
      Serial.println("Cache: couldn't open a copy for building - stopping.");
      stop();
      return false;
    }
    return true;
  }
  uint32_t srcLen = levelLen_[ready_];
  uint32_t outLen = levelLen_[ready_ + 1];
  if (outDone_ == outLen) {
    finishLevel();
    return true;
  }
  // Read the next stretch of input if this step's outputs aren't covered by
  // the last one. Output frame m is centred on input frame 2m, so outputs
  // from outDone_ need input from 2 * outDone_ - FILTER_HALF onward.
  // readFramesAt() loops past the end of the file, which is what makes the
  // filter wrap around the loop point the same way playback does.
  const uint8_t ch = channels_;
  if (outDone_ >= inFirst_ + inCovered_) {
    inFirst_ = outDone_;
    inCovered_ = outLen - outDone_;
    if (inCovered_ > READ_OUTPUTS / ch) inCovered_ = READ_OUTPUTS / ch;
    uint32_t offset = wrapOffset(2 * (int32_t)outDone_ - FILTER_HALF, srcLen);
    inData = inRaw + src_.alignmentOf(offset) / 2;
    src_.readFramesAt(offset, inData, 2 * inCovered_ + 2 * FILTER_HALF);
  }

  const uint32_t writeBlockFrames = WRITE_BLOCK / ch;
  uint32_t n = inFirst_ + inCovered_ - outDone_;
  if (n > OUT_BLOCK) n = OUT_BLOCK;
  if (n > writeBlockFrames - pending_) n = writeBlockFrames - pending_;

  // Each channel is filtered on its own: in interleaved data, a channel's
  // neighbouring samples are `ch` apart rather than adjacent.
  const int16_t *first = inData + (2 * (outDone_ - inFirst_) + FILTER_HALF) * ch;
  for (uint32_t i = 0; i < n; i++) {
    for (uint8_t c = 0; c < ch; c++) {
      const int16_t *centre = first + 2 * i * ch + c;
      float acc = 0.5f * centre[0];
      for (int t = 0; t < ODD_TAPS; t++) {
        int d = (2 * t + 1) * ch;
        acc += oddTap[t] * (float)(centre[d] + centre[-d]);
      }
      // Round to nearest by hand - lroundf() was a large share of the time.
      int32_t v = (int32_t)(acc + (acc >= 0.0f ? 0.5f : -0.5f));
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      writeBuf[(pending_ + i) * ch + c] = (int16_t)v;
    }
  }
  pending_ += n;
  outDone_ += n;
  workDone_ += n;

  if (pending_ == writeBlockFrames || outDone_ == outLen) {
    if (fwrite(writeBuf, 2 * ch, pending_, out_) != pending_) {
      Serial.println("Cache: write failed - is the card full? Stopping.");
      stop();
      return false;
    }
    pending_ = 0;
  }
  return true;
}
