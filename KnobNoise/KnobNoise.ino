// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  KnobNoise - how much a resting knob's reading jitters, in ADC counts and
  in degrees

  A diagnostic sketch, a companion to NoiseMeter. NoiseMeter plots noise
  live, for chasing it down; this one prints a table, for choosing the
  knobs' noise settings - smoothing, oversampling, backlash and thresholds -
  from numbers. It reads the pots exactly as SDResampleDemo does: through
  RV112FF, with RA6M5AnalogTuning on and no settling reads, once every 10ms
  (SDResampleDemo's control tick).

  Wiring (SparkFun Thing Plus RA6M5), the same as SDResampleDemo:
    A2/A3 -> pot 1's two wipers    (Taiwan Alpha RV112FF-40, tracks on VREF/GND)
    A4/A5 -> pot 2's two wipers
    D20/D21 -> the touch pads, D6 -> ~10nF to GND (only used under load)
    A0/A1 -> the audio jack (a quiet 440Hz tone plays under load - turn
      the volume down)
    A micro SD card with a file at its top level (read under load)

  It cycles through four 5-second phases, printing one line at the end of
  each:
    quiet os=1 - nothing else running, one ADC reading per wiper
    quiet os=4 - the same, averaging four readings (RV112FF::setOversampling)
    load  os=1 - with the board as busy as SDResampleDemo keeps it: a tone
                 out of both DACs from a 40kHz interrupt, the card's first
                 file read in 2KB chunks as fast as loop() runs, and a
                 touch scan every tick
    load  os=4

  Each line gives, for each wiper pin, the RMS and peak-to-peak of its
  reading in 12-bit counts (RV112FF reads at 16 bits; divided by 16 here
  so the numbers match NoiseMeter's) and the mean reading; and for each
  knob, the RMS and peak-to-peak of its angle in degrees - through
  RV112FF's 20ms smoothing, as SDResampleDemo has it ("smooth"), and with
  the smoothing all but off ("raw"). Degrees are per knob, since the angle
  is decoded from both wipers together. The peak-to-peak is what a
  backlash or threshold has to clear; set one at a few times the worst
  peak-to-peak seen.

  Noise depends on where the knob sits: a wiper is a stiffer source near
  the ends of its track than in the middle, and one parked at an end reads
  exactly 0 or full scale, with no noise at all. So take readings at
  several angles. Keep your hands off the knobs while a phase runs.

  The knobs' calibration is restored from SDResampleDemo's slots in the
  Data Flash, read only - nothing is written to the flash or the card. If
  SDResampleDemo hasn't saved one yet, the knob's degrees print "uncal".

  Measured 2026-11-08 on the PCB, at a few angles per knob: every wiper
  under 0.5 counts RMS and 2 counts peak-to-peak; angle 0.003-0.007
  degrees RMS smoothed (up to 0.08 peak-to-peak), 0.003-0.015 raw at os=1
  and 0.003-0.007 at os=4; the load made no difference.

  Open the Serial Monitor at 115200 baud.
*/

#include <EEPROM.h>
#include <RV112FF.h>
#include <RA6M5AnalogTuning.h>
#include <RA6M5CapTouch.h>
#include "FspTimer.h"
#include "SDCardBlockDevice.h"
#include "FATFileSystem.h"

const uint8_t POT_PINS[2][2] = { { A2, A3 }, { A4, A5 } };
const char *const POT_PIN_NAMES[2][2] = { { "A2", "A3" }, { "A4", "A5" } };
const unsigned long TICK_MS = 10;     // SDResampleDemo's control tick
const unsigned long PHASE_MS = 5000;

// Where SDResampleDemo saves each knob's calibration.
const int CAL_EEPROM_ADDR[2] = { 0, sizeof(RV112FFCalibration) };

// Each knob twice: as SDResampleDemo has it (RV112FF's default 20ms
// smoothing), and with the smoothing all but off, to show what it's worth.
RV112FF smoothPots[2] = { RV112FF(A2, A3), RV112FF(A4, A5) };
RV112FF rawPots[2] = { RV112FF(A2, A3, 0.0001f), RV112FF(A4, A5, 0.0001f) };
bool calibrated[2];

// -- The load ---------------------------------------------------------------

CapTouch touchPads[2] = { CapTouch(20), CapTouch(21) };

const uint32_t DAC_SAMPLE_RATE = 40000;
const uint32_t DAC_PHASE_STEP = (uint32_t)(440.0 / DAC_SAMPLE_RATE * 4294967296.0);
FspTimer dacTimer;

SDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
FATFileSystem sdFs("sd");
FILE *sdFile = nullptr;
uint8_t sdChunk[2048];  // SDResampleDemo's 1024 16-bit samples

// A triangle wave at half scale, written straight to the DAC registers as
// the audio sketches write their samples.
void dacCallback(timer_callback_args_t *) {
  static uint32_t phase = 0;
  phase += DAC_PHASE_STEP;
  uint32_t ramp = phase >> 20;
  uint16_t dacValue = 1024 + ((ramp < 2048) ? ramp : 4095 - ramp);
  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// Mounts the card and opens the first ordinary file at its top level.
bool openSdFile() {
  if (sdFs.mount(&sdBlockDevice) != 0) return false;
  DIR *dir = opendir("/sd");
  if (dir == nullptr) return false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type == DT_REG) {
      sdFile = fopen((String("/sd/") + entry->d_name).c_str(), "rb");
      break;
    }
  }
  closedir(dir);
  return sdFile != nullptr;
}

// -- Statistics -------------------------------------------------------------

// Running mean, variance (Welford's method) and range.
struct Stat {
  uint32_t n;
  double mean, m2, lo, hi;
  void reset() { n = 0; mean = m2 = 0; }
  void add(double x) {
    if (n == 0 || x < lo) lo = x;
    if (n == 0 || x > hi) hi = x;
    n++;
    double d = x - mean;
    mean += d / n;
    m2 += d * (x - mean);
  }
  double rms() const { return n > 1 ? sqrt(m2 / (n - 1)) : 0; }
  double peakToPeak() const { return n > 0 ? hi - lo : 0; }
};

Stat wiperStats[2][2];   // [pot][wiper]
Stat angleStats[2][2];   // [pot][0 smooth, 1 raw], in degrees from angleRef
float angleRef[2][2];    // each phase's first angle, to unwrap across 0/360

void resetStats() {
  for (int pot = 0; pot < 2; pot++) {
    for (int i = 0; i < 2; i++) {
      wiperStats[pot][i].reset();
      angleStats[pot][i].reset();
      angleRef[pot][i] = -1;
    }
  }
}

void addAngle(int pot, int which, float angle) {
  if (angle < 0) return;  // not calibrated
  if (angleRef[pot][which] < 0) angleRef[pot][which] = angle;
  float d = angle - angleRef[pot][which];
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  angleStats[pot][which].add(d);
}

// -- Phases -----------------------------------------------------------------

int phase = 0;  // 0 quiet os=1, 1 quiet os=4, 2 load os=1, 3 load os=4
uint8_t oversampling = 1;
bool loaded = false;
unsigned long phaseStartMillis, lastTickMillis;

void setPhase(int newPhase) {
  phase = newPhase;
  oversampling = (phase & 1) ? 4 : 1;
  bool wantLoad = phase >= 2;
  if (wantLoad != loaded) {
    loaded = wantLoad;
    if (loaded) {
      dacTimer.start();
    } else {
      dacTimer.stop();
      R_DAC->DADR[0] = 2048;  // park both outputs at mid-scale
      R_DAC->DADR[1] = 2048;
    }
  }
  for (int pot = 0; pot < 2; pot++) {
    smoothPots[pot].setOversampling(oversampling);
    rawPots[pot].setOversampling(oversampling);
  }
  resetStats();
  phaseStartMillis = millis();
}

void printPhase() {
  Serial.print(loaded ? "load  os=" : "quiet os=");
  Serial.print(oversampling);
  Serial.print(" (n=");
  Serial.print(wiperStats[0][0].n);
  Serial.println(")");
  for (int pot = 0; pot < 2; pot++) {
    Serial.print("  ");
    for (int i = 0; i < 2; i++) {
      const Stat &s = wiperStats[pot][i];
      Serial.print(POT_PIN_NAMES[pot][i]);
      Serial.print(": rms ");
      Serial.print(s.rms(), 3);
      Serial.print(" p-p ");
      Serial.print(s.peakToPeak(), 2);
      Serial.print(" @");
      Serial.print(s.mean, 0);
      Serial.print("   ");
    }
    Serial.print("knob");
    Serial.print(pot + 1);
    if (!calibrated[pot]) {
      Serial.println(": uncal");
      continue;
    }
    Serial.print(" @");
    Serial.print(smoothPots[pot].read(), 1);
    Serial.print(": smooth rms ");
    Serial.print(angleStats[pot][0].rms(), 4);
    Serial.print(" p-p ");
    Serial.print(angleStats[pot][0].peakToPeak(), 3);
    Serial.print(", raw rms ");
    Serial.print(angleStats[pot][1].rms(), 4);
    Serial.print(" p-p ");
    Serial.print(angleStats[pot][1].peakToPeak(), 3);
    Serial.println(" deg");
  }
}

// -- Setup and loop ---------------------------------------------------------

void setup() {
  Serial.begin(115200);
  analogReadResolution(16);  // as RV112FF reads, so the first tick's raw reads match

  // ---- RA6M5-specific ----
  // The ADC settings that remove crosstalk between the pots - see
  // RA6M5AnalogTuning - with RV112FF's settling read off, as SDResampleDemo
  // runs them.
  bool tuned = RA6M5AnalogTuning::begin(A2);
  // ---- end RA6M5-specific ----

  for (int pot = 0; pot < 2; pot++) {
    RV112FFCalibration cal;
    EEPROM.get(CAL_EEPROM_ADDR[pot], cal);
    calibrated[pot] = smoothPots[pot].setCalibration(cal) && rawPots[pot].setCalibration(cal)
                      && smoothPots[pot].isCalibrated();
    smoothPots[pot].setSettlingReads(tuned ? 0 : 1);
    rawPots[pot].setSettlingReads(tuned ? 0 : 1);
  }

  touchPads[0].begin();
  touchPads[1].begin();

  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  dacTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                 (float)DAC_SAMPLE_RATE, 50.0, dacCallback, nullptr);
  dacTimer.setup_overflow_irq(1);
  dacTimer.open();

  openSdFile();  // without a file the load phases just skip the SD reads

  setPhase(0);
  lastTickMillis = millis();
}

void loop() {
  // The SD reads run flat out between ticks, as SDResampleDemo's refills do.
  if (loaded && sdFile && fread(sdChunk, 1, sizeof sdChunk, sdFile) < sizeof sdChunk) {
    rewind(sdFile);
  }

  unsigned long now = millis();
  if (now - lastTickMillis < TICK_MS) return;
  lastTickMillis = now;

  if (loaded) CapTouch::scan();

  for (int pot = 0; pot < 2; pot++) {
    // Each wiper read the way RV112FF reads it, to see the pins themselves.
    for (int i = 0; i < 2; i++) {
      long sum = 0;
      for (uint8_t r = 0; r < oversampling; r++) sum += analogRead(POT_PINS[pot][i]);
      wiperStats[pot][i].add((double)sum / oversampling / 16.0);
    }
    addAngle(pot, 0, smoothPots[pot].read());
    addAngle(pot, 1, rawPots[pot].read());
  }

  if (now - phaseStartMillis >= PHASE_MS) {
    if (Serial) printPhase();
    setPhase((phase + 1) % 4);
  }
}
