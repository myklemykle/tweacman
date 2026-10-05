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

    if (memcmp(chunkId, "fmt ", 4) == 0) {
      uint32_t byteRate;
      uint16_t blockAlign;
      fread(&audioFormat, 2, 1, file_);
      fread(&numChannels, 2, 1, file_);
      fread(&sampleRate_, 4, 1, file_);
      fread(&byteRate, 4, 1, file_);
      fread(&blockAlign, 2, 1, file_);
      fread(&bitsPerSample, 2, 1, file_);
      // fmt chunks are sometimes larger than the 16 bytes just read
      // (extensible-format fields) - skip whatever's left of it.
      if (chunkSize > 16) fseek(file_, chunkSize - 16, SEEK_CUR);
      haveFmt = true;
    } else if (memcmp(chunkId, "data", 4) == 0) {
      dataStart_ = ftell(file_);
      dataBytes_ = chunkSize;
      break;
    } else {
      fseek(file_, chunkSize + (chunkSize & 1), SEEK_CUR);  // RIFF pads odd chunks to even
    }
  }

  if (!haveFmt) {
    return fail("no fmt chunk found");
  }
  if (audioFormat != 1 || numChannels != 1 || bitsPerSample != 16) {
    return fail("this demo only plays mono 16-bit PCM WAV files");
  }

  return true;
}

bool WavReader::openRaw(const char *path, uint32_t sampleRate) {
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
  if (dataBytes_ < 2) return fail("cache file is empty");
  return true;
}

size_t WavReader::readSamplesAt(uint32_t sampleOffset, int16_t *dst, size_t maxSamples) {
  fseek(file_, dataStart_ + (long)sampleOffset * 2, SEEK_SET);
  uint32_t bytesRemaining = dataBytes_ - sampleOffset * 2;
  size_t samplesRead = 0;

  while (samplesRead < maxSamples) {
    if (bytesRemaining < 2) {
      // Hit the end of the data chunk - loop back to the start for
      // seamless continuous playback; callers never see this boundary.
      fseek(file_, dataStart_, SEEK_SET);
      bytesRemaining = dataBytes_;
      if (dataBytes_ < 2) break;  // pathological empty file - avoid spinning forever
    }
    size_t samplesLeftHere = maxSamples - samplesRead;
    size_t samplesAvailableHere = bytesRemaining / 2;
    size_t chunk = samplesLeftHere < samplesAvailableHere ? samplesLeftHere : samplesAvailableHere;
    size_t got = fread(dst + samplesRead, 2, chunk, file_);
    samplesRead += got;
    bytesRemaining -= got * 2;
    if (got == 0) break;  // shouldn't happen, but avoid spinning forever
  }
  return samplesRead;
}
