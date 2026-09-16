#include "WavReader.h"

#include <Arduino.h>
#include <cstring>

bool WavReader::open(const char *path) {
  file_ = fopen(path, "rb");
  if (!file_) {
    Serial.print("WavReader: couldn't open ");
    Serial.println(path);
    return false;
  }

  char riff[4], wave[4];
  uint32_t riffSize;
  if (fread(riff, 1, 4, file_) != 4 || fread(&riffSize, 4, 1, file_) != 1 ||
      fread(wave, 1, 4, file_) != 4 ||
      memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
    Serial.println("WavReader: not a RIFF/WAVE file");
    return false;
  }

  bool haveFmt = false;
  uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;

  while (true) {
    char chunkId[4];
    uint32_t chunkSize;
    if (fread(chunkId, 1, 4, file_) != 4 || fread(&chunkSize, 4, 1, file_) != 1) {
      Serial.println("WavReader: hit EOF before finding a data chunk");
      return false;
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
    Serial.println("WavReader: no fmt chunk found");
    return false;
  }
  if (audioFormat != 1 || numChannels != 1 || bitsPerSample != 16) {
    Serial.println("WavReader: this demo only plays mono 16-bit PCM WAV files");
    return false;
  }

  bytesRemaining_ = dataBytes_;
  return true;
}

size_t WavReader::readSamples(int16_t *dst, size_t maxSamples) {
  size_t samplesRead = 0;

  while (samplesRead < maxSamples) {
    if (bytesRemaining_ < 2) {
      // Hit the end of the data chunk - loop back to the start for
      // seamless continuous playback; callers never see this boundary.
      fseek(file_, dataStart_, SEEK_SET);
      bytesRemaining_ = dataBytes_;
      if (dataBytes_ < 2) break;  // pathological empty file - avoid spinning forever
    }
    size_t samplesLeftHere = maxSamples - samplesRead;
    size_t samplesAvailableHere = bytesRemaining_ / 2;
    size_t chunk = samplesLeftHere < samplesAvailableHere ? samplesLeftHere : samplesAvailableHere;
    size_t got = fread(dst + samplesRead, 2, chunk, file_);
    samplesRead += got;
    bytesRemaining_ -= got * 2;
    if (got == 0) break;  // shouldn't happen, but avoid spinning forever
  }
  return samplesRead;
}
