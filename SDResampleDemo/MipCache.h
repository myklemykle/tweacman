/*
  MipCache - builds and maintains filtered, lower-rate copies of a WAV file
  on the SD card, so SDResampleDemo can play it at very high speeds.

  Why: playing a file at 1000x straight off the card would mean reading 1000
  seconds of audio every second - far beyond what the card can deliver - and
  most of what it read would be thrown away, or worse, alias: content pushed
  above what the DAC can reproduce folds back down as tones that move the
  wrong way (the "ring modulator" sound of the raw high-speed path). So
  instead, each WAV gets a ladder of copies at 1/2, 1/4, 1/8 ... 1/1024 of
  its sample rate, each low-pass filtered so it holds only what will still
  be audible when played that much faster. At any speed, playback reads the
  copy that keeps it stepping through about one sample per output sample -
  never more than a few times real-time from the card, and no aliasing to
  speak of. It's the audio version of a graphics "mipmap", hence the name.

  The copies halve in size at every step, so the whole ladder takes about as
  much space as the original file.

  On the card: /sd/cache/<wav name>/ holds L01.raw ... L10.raw (raw 16-bit
  mono samples, no header) and info.txt, a one-line description of the WAV
  they were made from - its length, sample rate, and a hash of its first and
  last 4KB. This core can't read file timestamps, so content is the only way
  to notice a WAV has changed underneath its cache. info.txt is written
  before the copies, and each copy is written under a .tmp name and renamed
  when complete, so a reset mid-build loses at most the copy in progress -
  the next build resumes from there.

  Building: each copy is made from the one before it (L01 from the WAV, L02
  from L01, ...) in small blocks, one per step() call, so loop() can keep
  playback fed while it runs. A copy becomes usable the moment it's done -
  readyLevels() - so higher speeds open up progressively.

  The filter is a 63-tap windowed-sinc half-band (Blackman window), applied
  centred on each output sample, so copies line up in time with each other
  and the original - which is what lets the sketch crossfade between them.
  It also treats each file as a loop (the first block reaches back around to
  the end of the file), matching how the sketch plays it.
*/

#pragma once

#include <Arduino.h>
#include "WavReader.h"

class MipCache {
 public:
  static const uint8_t MAX_LEVELS = 10;   // copies down to 1/1024 the rate
  static const uint32_t MIN_LEVEL_SAMPLES = 64;  // a shorter copy isn't worth making

  // Starts maintaining the cache for /sd/<wavName>: checks any existing
  // cache still matches the WAV, discards it if not, and sets up to build
  // whatever copies are missing. Stops any build already in progress.
  void start(const String &wavName);

  // Abandons any build in progress, closing its files. A partly written
  // copy stays on the card as .tmp and is simply overwritten next time.
  void stop();

  // Does one block of building work (a few milliseconds of SD reading,
  // filtering and writing). Returns whether there was any work to do.
  bool step();

  bool building() const { return active_; }
  const String &name() const { return name_; }

  // Copies 1..readyLevels() are complete and safe to play.
  uint8_t readyLevels() const { return ready_; }

  // How many copies this file gets in all - fewer than MAX_LEVELS for short
  // files, since each must hold at least MIN_LEVEL_SAMPLES.
  uint8_t levelCount() const { return count_; }

  // Overall build progress, 0-100.
  int percentDone() const;

  // -- Card housekeeping, for when the sketch changes a WAV --------------
  static String dirFor(const String &wavName) { return "/sd/cache/" + wavName; }
  static String levelPath(const String &wavName, uint8_t level, bool temp = false);
  static void removeFor(const String &wavName);
  static void renameFor(const String &from, const String &to);
  // Removes cache folders whose WAV no longer exists - e.g. one deleted
  // from a computer rather than through the sketch.
  static void removeOrphans();

 private:
  bool openNextLevel();
  void finishLevel();

  String name_;
  bool active_ = false;
  uint8_t count_ = 0;
  uint8_t ready_ = 0;
  uint32_t levelLen_[MAX_LEVELS + 1] = {};  // [0] is the WAV itself
  uint32_t sampleRate_ = 0;

  WavReader src_;       // the copy being read from: the WAV, or the copy before
  FILE *out_ = nullptr; // the copy being written
  uint32_t outDone_ = 0;  // samples of it filtered so far
  uint32_t pending_ = 0;  // ...of which this many are waiting in the write buffer
  uint32_t inFirst_ = 0;    // the first output the input buffer currently covers
  uint32_t inCovered_ = 0;  // ...and how many outputs it covers

  uint32_t workTotal_ = 0;  // samples to write across all copies
  uint32_t workDone_ = 0;
  unsigned long startMillis_ = 0;
};
