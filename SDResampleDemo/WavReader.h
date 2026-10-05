/*
  WavReader - opens a mono WAV file on the mounted SD card and reads
  samples from any starting position, looping seamlessly at end-of-file.

  Whatever the file's own sample format, samples come out as 16-bit
  signed integers - the one format everything downstream (ring buffers,
  cached copies, the interpolator) deals in. Read as:
    - 16-bit PCM: as-is, straight off the card.
    - 8-bit PCM (unsigned, centred on 128): shifted up to 16 bits.
    - 24- and 32-bit PCM: the top 16 bits. The DAC only has 12, so the
      bits dropped were never going to be heard.
    - 32- and 64-bit IEEE float: scaled from [-1, 1) and clipped.
  Any of those can also come in a WAVE_FORMAT_EXTENSIBLE header, which
  many editors write for anything beyond plain 16-bit; its "valid bits"
  field needs no special handling, since valid bits are always the top
  ones of their container. Conversion happens in small pieces as samples
  are read, so it costs a little time in loop() and no extra memory per
  file. Stereo and wider files are refused, for now.

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

  // Opens path (e.g. "/sd/AUDIO.WAV") and parses its header. Returns false
  // if the file can't be opened or isn't a format this reads (see above) -
  // see lastError() for why, which is also printed to Serial as it happens.
  bool open(const char *path);

  // Opens a headerless file of raw 16-bit mono samples - one of the
  // filtered copies MipCache.h keeps in the card's cache/ folder - at the
  // given sample rate. Everything else (readSamplesAt()'s seamless looping
  // included) works exactly as for a WAV.
  bool openRaw(const char *path, uint32_t sampleRate);

  bool isOpen() const { return file_ != nullptr; }

  // Where sampleOffset falls within a 4-byte word of the file (0 or 2).
  // FatFs reads whole sectors straight into the destination by DMA, which
  // only takes RA6M5FastSD's fast multi-block path if they land 4-byte
  // aligned - true exactly when the destination's address matches the file
  // position mod 4. So for a large read, pass readSamplesAt() a 4-byte
  // aligned buffer plus this many bytes.
  // (Only 16-bit files are read straight into the caller's buffer -
  // anything else goes through a conversion buffer first - so for those
  // this is 0.)
  uint32_t alignmentOf(uint32_t sampleOffset) const {
    if (encoding_ != PCM16) return 0;
    return (uint32_t)(dataStart_ + sampleOffset * 2) & 3;
  }

  // Closes the underlying file, if any. Safe to call when already closed.
  void close();

  uint32_t nativeSampleRate() const { return sampleRate_; }
  uint32_t totalSamples() const { return dataBytes_ / bytesPerSample_; }

  // The file's own sample format, e.g. "24-bit PCM" or "32-bit float".
  const char *formatName() const;
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
  enum Encoding : uint8_t { PCM8, PCM16, PCM24, PCM32, FLOAT32, FLOAT64 };

  // Reads n samples from sampleOffset onward, all before the end of the
  // data - readSamplesAt()'s looping is built on this.
  size_t readRun(uint32_t sampleOffset, int16_t *dst, size_t n);

  // Records msg as the reason open() failed (for lastError()/the BLE
  // ERR: reply), prints it to Serial the same way every failure path
  // here always has, closes any partially-opened file, and returns
  // false - so every failure in open() is one line: `return fail(...)`.
  bool fail(const char *msg);

  FILE *file_ = nullptr;
  uint32_t sampleRate_ = 0;
  long dataStart_ = 0;
  uint32_t dataBytes_ = 0;
  Encoding encoding_ = PCM16;
  uint8_t bytesPerSample_ = 2;
  char lastError_[64] = "";
};
