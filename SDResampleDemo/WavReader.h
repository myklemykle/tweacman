/*
  WavReader - opens a mono 16-bit PCM WAV file on the mounted SD card
  and reads raw samples from any starting position, looping seamlessly
  at end-of-file.

  Parses the RIFF chunk structure properly (skipping any chunk it
  doesn't need) rather than assuming a fixed 44-byte header - real WAV
  files, especially anything exported by Audacity or ffmpeg with
  metadata, routinely have extra chunks (LIST, fact, JUNK...) between
  "fmt " and "data". Multi-byte fields are read directly into place
  with no manual byte-swapping, since both the RA6M5 and the WAV format
  itself are little-endian.

  Only ever reads forward - backward playback (see SDResampleDemo.ino)
  doesn't need this class to read backward at all, only to start a
  forward read from an earlier offset than the last one. There's no
  persistent read cursor for this reason: every call is given its own
  starting sample offset, computed by the caller.

  Movable but not copyable: an instance owns a FILE*, and
  SDResampleDemo.ino's file-switch logic needs to build a fresh instance,
  validate it completely, and only then move it into place over the one
  currently playing (see loadFile() there) - copying a FILE* handle
  wouldn't make sense, but moving ownership does.
*/

#pragma once

#include <cstdint>
#include <cstdio>

class WavReader {
 public:
  WavReader() = default;
  ~WavReader() { close(); }
  WavReader(const WavReader &) = delete;
  WavReader &operator=(const WavReader &) = delete;
  WavReader(WavReader &&other) noexcept { *this = static_cast<WavReader &&>(other); }
  WavReader &operator=(WavReader &&other) noexcept;

  // Opens path (e.g. "/sd/AUDIO.WAV"), parses its header, and leaves
  // the file positioned at the start of its sample data. Returns false
  // if the file can't be opened or isn't mono 16-bit PCM - see
  // lastError() for why, which is also printed to Serial as it happens.
  bool open(const char *path);

  // Closes the underlying file, if any. Safe to call when already closed.
  void close();

  uint32_t nativeSampleRate() const { return sampleRate_; }
  uint32_t totalSamples() const { return dataBytes_ / 2; }
  const char *lastError() const { return lastError_; }

  // Seeks to sampleOffset (must be < totalSamples() - callers wrap
  // arbitrary signed/virtual positions into range themselves, see
  // fileOffsetFor() in SDResampleDemo.ino) and reads up to maxSamples
  // forward from there, looping back to the start of the data chunk
  // (and continuing to fill) if end-of-file is hit partway through -
  // callers never see the file's own boundary at all. Returns the
  // number of samples actually written; only less than maxSamples if
  // the data chunk itself is pathologically short.
  size_t readSamplesAt(uint32_t sampleOffset, int16_t *dst, size_t maxSamples);

 private:
  // Records msg as the reason open() failed (for lastError()/the BLE
  // ERR: reply), prints it to Serial the same way every failure path
  // here always has, closes any partially-opened file, and returns
  // false - so every failure in open() is one line: `return fail(...)`.
  bool fail(const char *msg);

  FILE *file_ = nullptr;
  uint32_t sampleRate_ = 0;
  long dataStart_ = 0;
  uint32_t dataBytes_ = 0;
  char lastError_[64] = "";
};
