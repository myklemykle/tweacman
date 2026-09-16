/*
  WavReader - opens a mono 16-bit PCM WAV file on the mounted SD card
  and streams raw samples from it, looping seamlessly at end-of-file.

  Parses the RIFF chunk structure properly (skipping any chunk it
  doesn't need) rather than assuming a fixed 44-byte header - real WAV
  files, especially anything exported by Audacity or ffmpeg with
  metadata, routinely have extra chunks (LIST, fact, JUNK...) between
  "fmt " and "data". Multi-byte fields are read directly into place
  with no manual byte-swapping, since both the RA6M5 and the WAV format
  itself are little-endian.
*/

#pragma once

#include <cstdint>
#include <cstdio>

class WavReader {
 public:
  // Opens path (e.g. "/sd/AUDIO.WAV"), parses its header, and leaves
  // the file positioned at the start of its sample data. Returns false
  // (after printing why to Serial) if the file can't be opened or
  // isn't mono 16-bit PCM.
  bool open(const char *path);

  uint32_t nativeSampleRate() const { return sampleRate_; }

  // Fills dst with up to maxSamples samples, wrapping back to the start
  // of the data chunk (and continuing to fill) if end-of-file is hit
  // partway through - callers never see the file's boundary at all.
  // Returns the number of samples actually written; only less than
  // maxSamples if the data chunk itself is pathologically short.
  size_t readSamples(int16_t *dst, size_t maxSamples);

 private:
  FILE *file_ = nullptr;
  uint32_t sampleRate_ = 0;
  long dataStart_ = 0;
  uint32_t dataBytes_ = 0;
  uint32_t bytesRemaining_ = 0;  // left in the current pass through the data chunk
};
