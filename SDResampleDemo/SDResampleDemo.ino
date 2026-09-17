/*
  SDResampleDemo - SparkFun Thing Plus RA6M5

  Streams a mono 16-bit PCM WAV file off the onboard SD card and plays
  it out the DAC, with a pot controlling a completely smooth, arbitrary
  playback-speed ratio - not discrete multiples, and slowing down
  genuinely interpolates new sample values rather than just repeating
  or stepping through existing ones.

  Wiring: an SD card (FAT32, any SDHC card - see the SD note in
  CLAUDE.md, this board's slot is wired to the RA6M5's native SDHI
  peripheral, not SPI) with a file named AUDIO.WAV at its root - use
  tools/make_test_wav.py to generate one, a slow sine sweep that makes
  speed changes and any resampling artifacts obvious by ear. Otherwise
  same as DACAudioTest (3.5mm jack on A0/A1/GND) plus one pot (RV112FF
  wipers on A2/A3, track ends on 3.3V/GND) and one CapTouch pad (D20,
  with the shared TSCAP capacitor on D6 - see CapTouch.h).

  Why the DAC runs at a fixed 48kHz: an earlier version of this idea
  just changed the DAC's own output clock to change speed, tape-style -
  simple, but it directly couples "how fast can we go" to the DAC
  hardware's own conversion-time ceiling (~333kHz per the RA6M5
  datasheet, Table 53.48), which isn't the interesting limit here. This
  version fixes the DAC at a constant, comfortable 48kHz always, and
  does real-time resampling from whatever rate the SD card is actually
  being asked to supply. That decouples the speed knob from the DAC
  entirely and points it at the thing this demo is actually about: SD
  read throughput (the RA6M5's SDHI peripheral has a theoretical ~25MB/s
  ceiling per the datasheet's Table 53.34). Turn the speed ratio up far
  enough and the SD reader in loop() should eventually fail to keep the
  ring buffer fed before the DAC or CPU ever struggle - see "underrun"
  below.

  Measured on the first bench card (a plain microSD, brand/speed class
  unrecorded): underruns start right around speed ratio ~23-25x with
  this sketch's default settings, regardless of whether SD reads happen
  in 2KB or 32KB chunks and regardless of the ring buffer's size - both
  were tried and neither moved the wall, which rules out per-fread()-
  call overhead as the cause. That points at ~2-2.2MB/s as this card's
  real sustained sequential read speed - a small fraction of the SDHI
  bus's theoretical ceiling above, and apparently the card's own limit,
  not this sketch's. Worth re-measuring with a different/faster card.

  How the resampling works: RingBuffer.h holds raw samples read from SD;
  Resampler.h tracks a fractional read position into that buffer and
  advances it by a continuously-variable amount (`step`) once per
  48kHz tick, interpolating a fresh output value with a 4-point
  Catmull-Rom curve each time. The same mechanism handles speeding up
  (step > 1 - skip some source samples, but still interpolate between
  whichever two straddle the new position) and slowing down (step < 1 -
  the read position stays inside the same 4 samples across several
  output ticks, each time interpolating a genuinely new in-between
  value) with no separate code path for either direction. See both of
  those files' own comments for more detail.

  Aliasing at high speed ratios is expected, not a bug: no filter tracks
  the ratio to prevent high source frequencies folding down as `step`
  grows past ~2. A proper fix needs a cutoff that tracks the live ratio
  - real DSP scope this hand-rolled kernel deliberately skips. It's also
  a useful signpost, since it shows up audibly before the harder
  SD-bandwidth wall.

  Underrun policy: if the ring buffer can't supply what the resampler
  needs, the DAC ISR holds the last good sample rather than dropping to
  silence - that's what a real buffer-starved device actually sounds
  like (a stuck stutter), and it avoids an extra click on both entering
  and leaving the underrun. The resampler's position is frozen too, so
  playback resumes exactly where it left off with no skipped content.
  underrunTicks (printed periodically over Serial, along with live ring
  buffer occupancy) is this demo's actual headline measurement - watch
  it, and the occupancy trending toward zero, as you turn the ratio up.

  How to test: bring this up in stages rather than expecting it to work
  end-to-end on the first try.
    1. Flash the stock Storage/examples/TestSDCARD sketch from the core
       first, unmodified, to confirm the card mounts and lists files at
       all before any of this sketch's code is involved.
    2. With this sketch, watch the startup Serial output for the mount/
       WAV-parse messages. AUDIO.WAV not being exactly 48000Hz already
       means the resampler is doing real, non-trivial interpolation from
       the very first sample, even before the knob is touched.
    3. Turn the pot slowly across its full range; confirm smooth,
       continuous pitch/tempo tracking with no zippering or clicks,
       symmetric behavior above/below 1x, and that touching the pad
       switches between fast/coarse and slow/fine sweep rates.
    4. Push the ratio hard enough to see the ring buffer's occupancy
       fall and underrunTicks start climbing - that onset point is a
       real, card-dependent number (~23-25x measured so far - see
       above) worth re-checking against whatever card is on the bench.

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <CapTouch.h>
#include "SDCardBlockDevice.h"
#include "FATFileSystem.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR
#include "RingBuffer.h"
#include "Resampler.h"
#include "WavReader.h"

// -- Speed range and knob mapping ----------------------------------------
// Same shape as AudioKnobsDemo's angleToFrequency()/knobAngle(), just
// retargeted from a pitch range onto a speed-ratio range - see that
// sketch's header comment for why the exponential mapping and the
// fold-at-180-degrees behavior feel right for a knob like this.

const float MIN_RATIO = 0.1f;
const float MAX_RATIO = 128.0f;  // deliberately probing well past the SD ceiling

float angleToSpeedRatio(float angleDegrees) {
  float foldedAngle = (angleDegrees <= 180.0f) ? angleDegrees : (360.0f - angleDegrees);
  float t = foldedAngle / 180.0f;
  return MIN_RATIO * powf(MAX_RATIO / MIN_RATIO, t);
}

const float TURNS_PER_SWEEP_FAST = 0.5f;
const float TURNS_PER_SWEEP_SLOW = 2.0f;

float knobAngle(float accumulatedDegrees, float turnsPerSweep) {
  if (accumulatedDegrees == -1.0f) return -1;
  float wrapped = fmodf(accumulatedDegrees / (turnsPerSweep * 2.0f), 360.0f);
  if (wrapped < 0) wrapped += 360.0f;
  return wrapped;
}

// -- SD card and WAV source -----------------------------------------------
// Declared at file scope, not inside setup(): FATFileSystem keeps a
// reference to the block device for as long as any file on it stays
// open, which for this sketch is the entire runtime.

SDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
FATFileSystem sdFs("sd");
WavReader wav;

float rateFactor = 1.0f;  // wav's native sample rate / 48000 - computed once in setup()

// -- Ring buffer and resampler ---------------------------------------------

const uint32_t RING_CAPACITY = 16384;  // ~370ms of headroom at a 44.1kHz source
AudioRingBuffer<RING_CAPACITY> ring;
Resampler resampler;

// Written by loop() at control rate, read by the audio interrupt every
// tick - a plain float read/write is atomic on this 32-bit core, the
// same "no lock needed for a single scalar" case AudioKnobsDemo's
// freq/gain pointers rely on (see RingBuffer.h's comment for why the
// ring buffer itself is a different, larger problem that does need
// real atomics).
volatile float currentStep = 1.0f;
volatile int16_t lastGoodSample = 0;
volatile uint32_t underrunTicks = 0;

// Diagnostic only: the min/max ratio actually computed at control rate
// since the last status print, to check whether currentStep is
// spiking between prints (which only show a single 500ms snapshot) or
// whether the buffer wobble seen at "steady" knob positions has some
// other cause entirely.
float minRatioSeen = 1e9f;
float maxRatioSeen = -1e9f;

// -- Audio-rate interrupt ---------------------------------------------

FspTimer audioTimer;

void audioCallback(timer_callback_args_t *args) {
  int16_t y[4];
  int16_t outSample;

  if (ring.peek4(resampler.index, y)) {
    outSample = (int16_t)catmullRom(y[0], y[1], y[2], y[3], resampler.frac);
    lastGoodSample = outSample;
    ring.release(resampler.advance(currentStep));
  } else {
    outSample = lastGoodSample;  // hold the last good sample - see header comment
    underrunTicks++;
  }

  // 16-bit signed -> 12-bit unsigned, same shift-based rescale used
  // throughout this pipeline for cheapness; a few LSBs of resolution
  // given up is inaudible against this DAC's own ~24-LSB absolute
  // accuracy (RA6M5 datasheet, Table 53.48).
  uint16_t dacValue = (uint16_t)(((int32_t)outSample + 32768) >> 4);
  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- Pot and touch pad -------------------------------------------------

RV112FF pot1(A2, A3);
MultiTurnKnob knob1(pot1);
CapTouch touch1(20);

const unsigned long CONTROL_INTERVAL = 10;  // ms (~100Hz)
unsigned long previousControlMillis = 0;

const unsigned long STATUS_INTERVAL = 500;  // ms
unsigned long previousStatusMillis = 0;

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("SD resample demo: turn the pot for a continuously-variable playback speed.");
  Serial.println("Touch the pad while turning for fast/coarse sweeps; let go for slow/fine control.");

  touch1.begin();

  if (sdFs.mount(&sdBlockDevice) != 0) {
    Serial.println("SD card mount failed - check it's inserted and FAT32-formatted.");
    while (1) {}
  }

  if (!wav.open("/sd/AUDIO.WAV")) {
    while (1) {}  // WavReader has already printed why
  }
  rateFactor = wav.nativeSampleRate() / 48000.0f;
  currentStep = rateFactor;  // native-speed passthrough until the knob is calibrated

  Serial.print("AUDIO.WAV native rate: ");
  Serial.print(wav.nativeSampleRate());
  Serial.println(" Hz");

  // Pre-fill the ring buffer before the audio interrupt starts, so
  // playback never begins from a cold, empty buffer - which would just
  // be an immediate, uninteresting underrun.
  int16_t primer[1024];
  while (ring.freeSpace() >= 1024) {
    size_t got = wav.readSamples(primer, 1024);
    if (got == 0) break;
    ring.push(primer, got);
  }

  // Open and start both DAC channels through the normal Arduino path
  // once, then let the audio interrupt write the data register
  // directly each sample - same split as AudioKnobsDemo/DACAudioTest.
  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);

  // A hardware timer fires at a fixed 48000Hz; the speed knob changes
  // how far the resampler steps through the source each tick, not this
  // timer's own rate - see the header comment on why.
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  audioTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                    48000.0f, 50.0, audioCallback, nullptr);
  audioTimer.setup_overflow_irq(1);  // high priority: audio timing is jitter-sensitive
  audioTimer.open();
  audioTimer.start();
}

void loop() {
  // Greedy, unthrottled SD refill: the whole point of this demo is to
  // let the SD card's real sustained read throughput be the limiting
  // factor once the speed ratio is cranked high enough, so this isn't
  // paced against anything - it just tops the buffer up as fast as
  // loop() can cycle.
  const size_t CHUNK_SAMPLES = 1024;
  if (ring.freeSpace() >= CHUNK_SAMPLES) {
    int16_t chunk[CHUNK_SAMPLES];
    size_t got = wav.readSamples(chunk, CHUNK_SAMPLES);
    if (got > 0) ring.push(chunk, got);
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    previousControlMillis = currentMillis;
    CapTouch::scan();

    float turnsPerSweep = touch1.isTouched() ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float angle = knobAngle(knob1.read(), turnsPerSweep);
    if (angle >= 0) {
      float ratio = angleToSpeedRatio(angle);
      currentStep = ratio * rateFactor;
      if (ratio < minRatioSeen) minRatioSeen = ratio;
      if (ratio > maxRatioSeen) maxRatioSeen = ratio;
    }
    // else: pot not yet calibrated - leave currentStep at the
    // native-speed passthrough default set in setup().
  }

  // Deliberately short and infrequent: Serial here is native USB CDC
  // (see CLAUDE.md), and a blocking print sitting in a hot loop would
  // itself steal cycles from the SD refill above, manufacturing the
  // very underrun this demo is trying to measure honestly.
  if (currentMillis - previousStatusMillis >= STATUS_INTERVAL) {
    previousStatusMillis = currentMillis;
    float ratio = currentStep / rateFactor;
    float fillPercent = 100.0f * ring.usedSpace() / RING_CAPACITY;
    Serial.print("speed ");
    Serial.print(ratio, 2);
    Serial.print("x (range ");
    if (minRatioSeen <= maxRatioSeen) {
      Serial.print(minRatioSeen, 3);
      Serial.print("-");
      Serial.print(maxRatioSeen, 3);
    } else {
      Serial.print("n/a");  // pot wasn't calibrated during this window
    }
    Serial.print(")  buffer ");
    Serial.print(fillPercent, 0);
    Serial.print("%  underruns ");
    Serial.println(underrunTicks);
    minRatioSeen = 1e9f;
    maxRatioSeen = -1e9f;
  }
}
