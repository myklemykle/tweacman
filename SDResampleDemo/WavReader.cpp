#include "WavReader.h"

#include <Arduino.h>
#include <cstring>

WavReader &WavReader::operator=(WavReader &&other) noexcept {
  if (this == &other) return *this;
  close();
  file_ = other.file_;
  sampleRate_ = other.sampleRate_;
  dataStart_ = other.dataStart_;
  dataBytes_ = other.dataBytes_;
  encoding_ = other.encoding_;
  bytesPerSample_ = other.bytesPerSample_;
  channels_ = other.channels_;
  memcpy(lastError_, other.lastError_, sizeof(lastError_));
  other.file_ = nullptr;
  return *this;
}

void WavReader::close() {
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
}

bool WavReader::fail(const char *msg) {
  strncpy(lastError_, msg, sizeof(lastError_) - 1);
  lastError_[sizeof(lastError_) - 1] = '\0';
  Serial.print("WavReader: ");
  Serial.println(msg);
  close();
  return false;
}

bool WavReader::open(const char *path) {
  file_ = fopen(path, "rb");
  if (!file_) {
    char msg[64];
    snprintf(msg, sizeof(msg), "couldn't open %s", path);
    return fail(msg);
  }
  // No stdio buffering: it would chop every large read into 1KB pieces
  // before FatFs saw it, and a large read is what lets the SD driver fetch
  // many sectors in one command (see RA6M5FastSD.h).
  setvbuf(file_, nullptr, _IONBF, 0);

  char riff[4], wave[4];
  uint32_t riffSize;
  if (fread(riff, 1, 4, file_) != 4 || fread(&riffSize, 4, 1, file_) != 1 ||
      fread(wave, 1, 4, file_) != 4 ||
      memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
    return fail("not a RIFF/WAVE file");
  }

  bool haveFmt = false;
  uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;

  while (true) {
    char chunkId[4];
    uint32_t chunkSize;
    if (fread(chunkId, 1, 4, file_) != 4 || fread(&chunkSize, 4, 1, file_) != 1) {
      return fail("hit EOF before finding a data chunk");
    }
    long chunkEnd = ftell(file_) + chunkSize + (chunkSize & 1);  // RIFF pads odd chunks to even

    if (memcmp(chunkId, "fmt ", 4) == 0) {
      uint32_t byteRate;
      uint16_t blockAlign;
      fread(&audioFormat, 2, 1, file_);
      fread(&numChannels, 2, 1, file_);
      fread(&sampleRate_, 4, 1, file_);
      fread(&byteRate, 4, 1, file_);
      fread(&blockAlign, 2, 1, file_);
      fread(&bitsPerSample, 2, 1, file_);
      // WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes of
      // a GUID further on (the rest of the GUID is the same for every
      // standard format). Before it: the extension's size, the valid
      // bits, and which speakers the channels map to.
      if (audioFormat == 0xFFFE && chunkSize >= 40) {
        uint16_t extensionSize, validBits;
        uint32_t channelMask;
        fread(&extensionSize, 2, 1, file_);
        fread(&validBits, 2, 1, file_);
        fread(&channelMask, 4, 1, file_);
        fread(&audioFormat, 2, 1, file_);
      }
      haveFmt = true;
    } else if (memcmp(chunkId, "data", 4) == 0) {
      dataStart_ = ftell(file_);
      dataBytes_ = chunkSize;
      break;
    }
    fseek(file_, chunkEnd, SEEK_SET);  // skip whatever's left of this chunk
  }

  if (!haveFmt) {
    return fail("no fmt chunk found");
  }

  // Some recorders write a data size larger than what's actually there
  // (or a placeholder, if they were interrupted) - trust the file instead.
  fseek(file_, 0, SEEK_END);
  long available = ftell(file_) - dataStart_;
  if (available >= 0 && (long)dataBytes_ > available) dataBytes_ = (uint32_t)available;

  char msg[64];
  if (numChannels != 1 && numChannels != 2) {
    snprintf(msg, sizeof(msg), "%u-channel files aren't supported - mono and stereo only",
             numChannels);
    return fail(msg);
  }
  channels_ = (uint8_t)numChannels;
  if (audioFormat == 1 && bitsPerSample == 8) encoding_ = PCM8;
  else if (audioFormat == 1 && bitsPerSample == 16) encoding_ = PCM16;
  else if (audioFormat == 1 && bitsPerSample == 24) encoding_ = PCM24;
  else if (audioFormat == 1 && bitsPerSample == 32) encoding_ = PCM32;
  else if (audioFormat == 3 && bitsPerSample == 32) encoding_ = FLOAT32;
  else if (audioFormat == 3 && bitsPerSample == 64) encoding_ = FLOAT64;
  else if (audioFormat == 1 || audioFormat == 3) {
    snprintf(msg, sizeof(msg), "%u-bit %s isn't supported", bitsPerSample,
             audioFormat == 1 ? "PCM" : "float");
    return fail(msg);
  } else {
    // ADPCM, A-law, mu-law, MP3-in-WAV...
    snprintf(msg, sizeof(msg), "compressed WAV (format 0x%04X) isn't supported", audioFormat);
    return fail(msg);
  }
  bytesPerSample_ = bitsPerSample / 8;
  if (dataBytes_ < (uint32_t)bytesPerSample_ * channels_) return fail("no sample data");
  return true;
}

const char *WavReader::formatName() const {
  switch (encoding_) {
    case PCM8: return "8-bit PCM";
    case PCM16: return "16-bit PCM";
    case PCM24: return "24-bit PCM";
    case PCM32: return "32-bit PCM";
    case FLOAT32: return "32-bit float";
    case FLOAT64: return "64-bit float";
  }
  return "?";
}

bool WavReader::openRaw(const char *path, uint32_t sampleRate, uint8_t channels) {
  close();
  file_ = fopen(path, "rb");
  if (!file_) {
    char msg[64];
    snprintf(msg, sizeof(msg), "couldn't open %s", path);
    return fail(msg);
  }
  setvbuf(file_, nullptr, _IONBF, 0);  // see open()
  fseek(file_, 0, SEEK_END);
  dataBytes_ = (uint32_t)ftell(file_);
  dataStart_ = 0;
  sampleRate_ = sampleRate;
  encoding_ = PCM16;
  bytesPerSample_ = 2;
  channels_ = channels;
  if (dataBytes_ < 2u * channels) return fail("cache file is empty");
  return true;
}

size_t WavReader::readFramesAt(uint32_t frameOffset, int16_t *dst, size_t maxFrames) {
  uint32_t total = totalFrames();
  uint32_t pos = frameOffset;
  size_t framesRead = 0;

  while (framesRead < maxFrames) {
    // Hit the end of the data chunk - loop back to the start for seamless
    // continuous playback; callers never see this boundary.
    if (pos >= total) pos = 0;
    size_t n = maxFrames - framesRead;
    if (n > total - pos) n = total - pos;
    size_t got = readRun(pos, dst + framesRead * channels_, n);
    if (got == 0) break;  // shouldn't happen, but avoid spinning forever
    framesRead += got;
    pos += got;
  }
  return framesRead;
}

// Where non-16-bit samples land before conversion. Shared by every reader:
// reads only ever happen from loop(), one at a time.
static uint8_t convertBuf[2048];

static inline int16_t floatToSample(float f) {
  f *= 32768.0f;
  if (f >= 32767.0f) return 32767;
  if (f <= -32768.0f) return -32768;
  return (int16_t)(f + (f >= 0.0f ? 0.5f : -0.5f));
}

size_t WavReader::readRun(uint32_t frameOffset, int16_t *dst, size_t frames) {
  size_t bytesPerFrame = (size_t)bytesPerSample_ * channels_;
  fseek(file_, dataStart_ + (long)frameOffset * bytesPerFrame, SEEK_SET);
  if (encoding_ == PCM16) return fread(dst, bytesPerFrame, frames, file_);

  // Everything else is converted sample by sample - for stereo, left and
  // right samples alternate exactly as they do in the file, so a frame
  // count just becomes twice as many samples.
  size_t n = frames * channels_;
  size_t done = 0;
  size_t perPiece = sizeof(convertBuf) / bytesPerFrame * channels_;
  while (done < n) {
    size_t piece = n - done < perPiece ? n - done : perPiece;
    size_t got = fread(convertBuf, bytesPerSample_, piece, file_);
    const uint8_t *b = convertBuf;
    int16_t *out = dst + done;
    // Little-endian throughout, like the file - so the top 16 bits of a
    // wider sample are simply its last two bytes.
    for (size_t i = 0; i < got; i++) {
      switch (encoding_) {
        case PCM8:
          out[i] = (int16_t)((b[i] - 128) * 256);
          break;
        case PCM24:
          out[i] = (int16_t)(b[3 * i + 1] | (b[3 * i + 2] << 8));
          break;
        case PCM32:
          out[i] = (int16_t)(b[4 * i + 2] | (b[4 * i + 3] << 8));
          break;
        case FLOAT32: {
          float f;
          memcpy(&f, b + 4 * i, 4);
          out[i] = floatToSample(f);
          break;
        }
        case FLOAT64: {
          double d;
          memcpy(&d, b + 8 * i, 8);
          out[i] = floatToSample((float)d);
          break;
        }
        case PCM16:
          break;  // handled above
      }
    }
    done += got;
    if (got < piece) break;
  }
  return done / channels_;
}
