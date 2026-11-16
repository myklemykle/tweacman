// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  filter_render - runs SDResampleDemo's filter mode (BandFilter.h) on a
  computer, so it can be heard and measured without the board. The filter
  code is the sketch's own, unchanged.

  Build and run through the Makefile (`make filter-render`), or by hand:

    c++ -O2 -std=c++17 -I SDResampleDemo tools/filter_render.cpp -o tools/filter_render

  Three jobs:

    filter_render response <low Hz> <high Hz>
        prints the gain at a spread of frequencies, measured with steady
        sine tones - the filter's frequency response at that setting.

    filter_render scream <low Hz> <high Hz>
        feeds it silence and reports what it does by itself: whether it
        oscillates, at what pitch and level, and whether it falls silent
        when faded out as the sketch does when the transport stops (over
        three echo delays - 0.9 seconds at the default delay).

    filter_render wav <in.wav> <out.wav> <low Hz> <high Hz | sweep>
        filters a 16-bit PCM WAV (mono or stereo) for listening. With
        "sweep", the high cutoff glides from 20kHz down to the low one and
        back across the file, as turning the Traveler's top knob would.
*/

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "BandFilter.h"

const int RATE = 48000;
const float CONTROL_SECONDS = 0.01f;  // how often loop() calls update() on the board
const int CONTROL_SAMPLES = (int)(RATE * CONTROL_SECONDS);

// Runs `samples` mono samples through the filter (the same signal in both
// channels), calling update() every CONTROL_SAMPLES as loop() would.
template <typename Source, typename Sink>
void run(BandFilter &filter, int samples, Source source, Sink sink) {
  for (int i = 0; i < samples; i++) {
    if (i % CONTROL_SAMPLES == 0) filter.update(CONTROL_SECONDS);
    float frame[2];
    frame[0] = frame[1] = source(i);
    filter.process(frame);
    sink(i, frame[0]);
  }
}

void response(float low, float high) {
  printf("Response from %.0fHz to %.0fHz (gain in dB):\n", low, high);
  for (float hz = 20.0f; hz <= 20000.0f; hz *= powf(2.0f, 1.0f / 3.0f)) {
    BandFilter filter;
    filter.begin(RATE);
    filter.setCutoffs(low, high);
    filter.setLevel(1.0f);
    const float amplitude = 1000.0f;  // well clear of the clipping in the resonance loop
    int settle = RATE / 2, measure = RATE / 2;
    double sum = 0.0;
    run(filter, settle + measure,
        [&](int i) { return amplitude * sinf(2.0f * (float)M_PI * hz * i / RATE); },
        [&](int i, float y) { if (i >= settle) sum += (double)y * y; });
    double rms = sqrt(sum / measure);
    double gain = 20.0 * log10(rms / (amplitude / sqrt(2.0)) + 1e-12);
    printf("  %7.0fHz %6.1f %s\n", hz, gain,
           std::string(gain > -40.0 ? (int)((gain + 40.0) / 1.5) : 0, '#').c_str());
  }
}

void scream(float low, float high) {
  BandFilter filter;
  filter.begin(RATE);
  filter.setCutoffs(low, high);
  filter.setLevel(1.0f);

  // Two seconds of silence; look at the last half.
  int samples = 2 * RATE, from = samples - RATE / 2;
  float peak = 0.0f, last = 0.0f;
  int crossings = 0;
  run(filter, samples, [](int) { return 0.0f; }, [&](int i, float y) {
    if (i < from) { last = y; return; }
    peak = fmaxf(peak, fabsf(y));
    if ((last < 0.0f) != (y < 0.0f)) crossings++;
    last = y;
  });
  float hz = crossings / 2.0f / 0.5f;
  printf("%.0fHz to %.0fHz (%.2f octaves): ", low, high, filter.widthOctaves());
  if (peak < 100.0f) {
    printf("no oscillation (peak %.1f of 32767)\n", peak);
    return;
  }
  printf("oscillates at about %.0fHz, peak %.0f of 32767 (%.1fdB)\n", hz, peak,
         20.0f * log10f(peak / 32767.0f));

  // Fade it out, as a stop does, and see that it stays silent after.
  const float FADE_SECONDS = 0.9f;
  int fadeSamples = (int)(FADE_SECONDS * RATE), fadedAt = -1, block = RATE / 100;
  float blockPeak = 0.0f, after = 0.0f;
  double tenth = 0.0;
  for (int i = 0; i < 3 * RATE; i++) {
    if (i % CONTROL_SAMPLES == 0) {
      float left = fmaxf(0.0f, 1.0f - (float)i / fadeSamples);
      filter.setLevel(left * left * left);  // the sketch's curve - see "Echo" there
      filter.update(CONTROL_SECONDS);
    }
    float frame[2] = {0.0f, 0.0f};
    filter.process(frame);
    float y = fabsf(frame[0]);
    blockPeak = fmaxf(blockPeak, y);
    if (i >= fadeSamples) after = fmaxf(after, y);
    // The level through each tenth of the fade, relative to the scream's.
    tenth += (double)y * y;
    if (i % (fadeSamples / 10) == fadeSamples / 10 - 1 && i < fadeSamples) {
      double rms = sqrt(tenth / (fadeSamples / 10)) * sqrt(2.0);
      printf("  %3d%%-%3d%% of the fade: %6.1fdB\n", (i + 1) * 100 / fadeSamples - 10,
             (i + 1) * 100 / fadeSamples, 20.0 * log10(rms / peak + 1e-9));
      tenth = 0.0;
    }
    if (i % block == block - 1) {
      if (fadedAt < 0 && blockPeak < peak * 0.0316f) fadedAt = i;  // 30dB down
      blockPeak = 0.0f;
    }
  }
  printf("  faded over %.1fs: 30dB down after %.2fs, peak %.1f once faded\n", FADE_SECONDS,
         fadedAt / (float)RATE, after);
}

// -- Just enough WAV to read and write 16-bit PCM ---------------------------

struct Wav {
  int channels = 0, rate = 0;
  std::vector<int16_t> samples;  // interleaved
};

bool readWav(const char *path, Wav &wav) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char id[4];
  uint32_t size;
  fread(id, 1, 4, f); fread(&size, 4, 1, f); fread(id, 1, 4, f);
  bool ok = false;
  while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
    if (!memcmp(id, "fmt ", 4)) {
      uint16_t format, channels, bits;
      uint32_t rate, byteRate;
      uint16_t align;
      fread(&format, 2, 1, f); fread(&channels, 2, 1, f); fread(&rate, 4, 1, f);
      fread(&byteRate, 4, 1, f); fread(&align, 2, 1, f); fread(&bits, 2, 1, f);
      fseek(f, size - 16, SEEK_CUR);
      if (bits != 16 || channels > 2) { fprintf(stderr, "only 16-bit mono or stereo, please\n"); break; }
      wav.channels = channels;
      wav.rate = rate;
    } else if (!memcmp(id, "data", 4) && wav.channels) {
      wav.samples.resize(size / 2);
      ok = fread(wav.samples.data(), 2, wav.samples.size(), f) == wav.samples.size();
      break;
    } else {
      fseek(f, size + (size & 1), SEEK_CUR);
    }
  }
  fclose(f);
  return ok;
}

bool writeWav(const char *path, const Wav &wav) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  uint32_t data = wav.samples.size() * 2, riff = 36 + data, fmtSize = 16;
  uint32_t rate = wav.rate, byteRate = wav.rate * wav.channels * 2;
  uint16_t format = 1, channels = wav.channels, align = wav.channels * 2, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f); fwrite(&format, 2, 1, f);
  fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  fwrite(wav.samples.data(), 2, wav.samples.size(), f);
  fclose(f);
  return true;
}

int renderWav(const char *in, const char *out, float low, const char *highArg) {
  Wav wav;
  if (!readWav(in, wav)) { fprintf(stderr, "can't read %s\n", in); return 1; }
  if (wav.rate != RATE) fprintf(stderr, "note: %s is %dHz, not 48000 - filtered at its own rate\n", in, wav.rate);
  BandFilter filter;
  filter.begin(wav.rate);
  filter.setLevel(1.0f);
  bool sweep = !strcmp(highArg, "sweep");
  float high = sweep ? FILTER_MAX_HZ : (float)atof(highArg);
  size_t frames = wav.samples.size() / wav.channels;
  int control = (int)(wav.rate * CONTROL_SECONDS);
  for (size_t i = 0; i < frames; i++) {
    if (i % control == 0) {
      if (sweep) {  // 50Hz up to 10kHz and back, evenly in octaves
        float t = (float)i / frames;
        float up = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
        high = FILTER_MAX_HZ * exp2f(-up * log2f(FILTER_MAX_HZ / low));
      }
      filter.setCutoffs(low, high);
      filter.update(CONTROL_SECONDS);
    }
    float frame[2];
    frame[0] = wav.samples[i * wav.channels];
    frame[1] = wav.samples[i * wav.channels + wav.channels - 1];
    filter.process(frame);
    for (int c = 0; c < wav.channels; c++) {
      float y = fmaxf(-32768.0f, fminf(32767.0f, frame[c]));
      wav.samples[i * wav.channels + c] = (int16_t)lrintf(y);
    }
  }
  if (!writeWav(out, wav)) { fprintf(stderr, "can't write %s\n", out); return 1; }
  printf("wrote %s\n", out);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "response")) {
    response(atof(argv[2]), atof(argv[3]));
  } else if (argc == 4 && !strcmp(argv[1], "scream")) {
    scream(atof(argv[2]), atof(argv[3]));
  } else if (argc == 6 && !strcmp(argv[1], "wav")) {
    return renderWav(argv[2], argv[3], atof(argv[4]), argv[5]);
  } else {
    fprintf(stderr, "usage: %s response <low Hz> <high Hz>\n"
                    "       %s scream <low Hz> <high Hz>\n"
                    "       %s wav <in.wav> <out.wav> <low Hz> <high Hz | sweep>\n",
            argv[0], argv[0], argv[0]);
    return 1;
  }
  return 0;
}
