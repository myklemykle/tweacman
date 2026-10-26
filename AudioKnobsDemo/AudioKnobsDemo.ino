// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  AudioKnobsDemo - SparkFun Thing Plus RA6M5

  Two RV112FF-40 pots (see the RV112FF library), each playing its own
  sine oscillator through a shared audio engine written in Faust (see
  synth.dsp) - turning a knob bends that oscillator's pitch in real
  time. Mono output: the two oscillators are mixed together, and the
  identical mixed signal is sent to both DAC channels (A0 and A1), so
  it doesn't matter which one your speaker or headphone is wired to.

  Wiring: same as RV112FF/examples/SerialPlotterDemo (pot 1 wipers on
  A2/A3, pot 2 wipers on A4/A5, both pots' track ends on 3.3V/GND) plus
  a 3.5mm jack on A0/A1/GND as in DACAudioTest - see that sketch's
  header comment for the DC-offset and output-impedance notes that
  still apply here; nothing about those changes with Faust in the mix -
  plus, for the two touch pads: D6 to a ~10nF capacitor to GND (TSCAP,
  shared by both pads - see RA6M5CapTouch.h), and D20 (touch1, gates knob1)
  and D21 (touch2, gates knob2) each to their own wire or touch pad.

  Pitch mapping: each pot is wrapped in a MultiTurnKnob (see that
  library), whose unbounded accumulated-degrees position is folded
  around its own 180-degree midpoint before use - foldedAngle is 0 at
  every multiple of 360 and rises back to 180 exactly halfway between
  them - so the frequency sweep reverses direction there instead of
  jumping discontinuously at each wrap. That folded angle is then
  mapped exponentially, not linearly, onto the 50-10000 Hz range: equal
  angular steps become equal pitch *ratios* rather than equal Hz steps,
  which is what a linear-feeling pitch sweep actually requires (we hear
  pitch logarithmically - the interval from 100 to 200 Hz sounds like
  the same size jump as 5000 to 10000 Hz, not 1/50th the size).

  Touch-gated speed: each knob has a CapTouch pad (see that library)
  next to it. Touching it while turning gets you "fast" - half a
  physical turn sweeps the whole range, the same rate this demo always
  used. Let go and it's "slow" - a 4x reduction in sensitivity, taking
  two full turns per sweep - for finer pitch control than a single-turn
  mapping could ever allow. This is exactly why MultiTurnKnob exists:
  RV112FF's own read() only ever reports where the shaft is *right
  now*, wrapped to one turn, which can't represent "I've turned two
  full revolutions since I let go."

  One known wrinkle: switching speed mid-turn (touching or releasing)
  changes how much physical rotation maps to how much pitch, so the
  played pitch can visibly/audibly jump at the exact moment you touch
  or release - see knobAngle() below. Left as-is for now since it
  wasn't part of what was asked for; worth revisiting with a
  continuity-preserving offset if it sounds bad in practice.

  Each oscillator stays silent (gain 0) until its pot finishes its own
  self-calibration (see RV112FF.h) - otherwise it would play some
  arbitrary tone from the moment the sketch boots, before the knob has
  ever been touched.

  Serial Plotter: open Tools > Serial Plotter (115200 baud) to watch,
  20 times a second, each knob's two wiper readings (knob1_ad1 and
  knob1_ad2, the A/D readings its angle is decoded from), that angle
  (knob1_angle) and the pitch it's playing (knob1_pitch) - and the
  same for knob2. Everything shares one 0-360 vertical axis: the wiper
  readings are scaled so 0 is 0V and 360 is the top of the ADC's range,
  and each pitch is drawn so 0 is 50 Hz, 360 is 10 kHz, and equal steps
  between are equal musical intervals (about 47 per octave) - see
  plotPitch(). The wiper lines start at boot, so you can watch a pot
  calibrate; its angle and pitch lines appear once it has.

  Why Faust: two independently-tuned oscillators that need mixing on
  every sample can't use analogWave (see DACAudioTest.ino) - it streams
  a fixed, precomputed waveform table via DMA with no CPU involvement,
  which is exactly why it's efficient, but also why it can't compute
  something new and different every sample. This sketch instead runs
  its own audio-rate interrupt (see audioCallback() below) that asks
  the Faust-generated engine for one fresh sample at a time.

  Faust toolchain notes:
  - synth.dsp is the actual source; synth.h is generated from it and
    checked in so this sketch builds without Faust installed. After
    editing synth.dsp, regenerate with `make faust` from the project
    root (needs the `faust` compiler on PATH).
  - faust_support.h is a trimmed copy of Faust's own C++ support
    headers - see its own comment for what it is and why it's here
    instead of a normal Faust install path.
  - CaptureUI below is how the sketch gets a direct pointer to each of
    synth.dsp's four named controls (freq1, freq2, gain1, gain2) without
    building an actual user interface - see its comment.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <RA6M5AnalogTuning.h>
#include <RA6M5CapTouch.h>
#include "faust_support.h"
#include "synth.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR

// -- Pitch range and mapping -------------------------------------------

const float MIN_FREQ = 50.0;
const float MAX_FREQ = 10000.0;

// Fold a (possibly multi-turn, possibly negative) angle around its own
// 180-degree midpoint, then map that exponentially onto the frequency
// range - see the header comment above.
float angleToFrequency(float angleDegrees) {
  float foldedAngle = (angleDegrees <= 180.0) ? angleDegrees : (360.0 - angleDegrees);
  float t = foldedAngle / 180.0;  // 0 at the reversal points, 1 at the peak
  return MIN_FREQ * pow(MAX_FREQ / MIN_FREQ, t);
}

// How many physical turns of the knob cover one sweep from MIN_FREQ to
// MAX_FREQ (or back) - see the header comment on touch-gated speed.
// TURNS_PER_SWEEP_FAST=0.5 reproduces exactly what this demo always
// did: angleToFrequency() above treats a full 360-degree cycle as two
// sweeps (up, then back down), so half a turn is one sweep already,
// with no scaling needed.
const float TURNS_PER_SWEEP_FAST = 0.5;
const float TURNS_PER_SWEEP_SLOW = 2.0;

// Turns a knob's unbounded accumulated position (MultiTurnKnob::read())
// into the [0, 360) angle angleToFrequency() expects, scaled by how
// many physical turns should cover one sweep. Dividing by
// turnsPerSweep*2 rather than turnsPerSweep directly accounts for
// angleToFrequency()'s own 360-degrees-is-two-sweeps convention (see
// TURNS_PER_SWEEP_FAST above) - at turnsPerSweep=0.5 that's a divisor
// of 1 (no scaling, matching the original single-turn behavior
// exactly); at turnsPerSweep=2.0 it's a divisor of 4, i.e. a full
// sweep now takes 4x the physical rotation the fast rate needs.
float knobAngle(float accumulatedDegrees, float turnsPerSweep) {
  // MultiTurnKnob::read()'s not-yet-calibrated sentinel is the exact
  // value -1 (passed straight through from RV112FF before any
  // accumulation begins) - not "any negative number," which is a
  // perfectly normal, valid accumulated position once the knob's been
  // turned backward past wherever calibration happened to complete.
  if (accumulatedDegrees == -1.0f) return -1;
  float wrapped = fmodf(accumulatedDegrees / (turnsPerSweep * 2.0f), 360.0f);
  if (wrapped < 0) wrapped += 360.0f;
  return wrapped;
}

// Where a frequency sits on the Serial Plotter: the same 0-360 scale as
// the knob angles beside it, logarithmic like the pitch mapping above, so
// 0 is MIN_FREQ, 360 is MAX_FREQ and each octave is the same height.
float plotPitch(float freq) {
  return 360.0 * log(freq / MIN_FREQ) / log(MAX_FREQ / MIN_FREQ);
}

// -- Faust engine and its controls --------------------------------------

const uint32_t SAMPLE_RATE = 40000;  // comfortably above 2x the 10kHz ceiling

KnobSynth synth;

// A minimal UI that builds nothing - it just records the memory address
// Faust assigned to each named slider (see synth.dsp), so the sketch can
// write straight through it later. There's no actual user interface;
// every method below except addHorizontalSlider is a required, unused
// override of Faust's UI interface (see faust_support.h).
class CaptureUI : public UI {
 public:
  float *freq1 = nullptr;
  float *freq2 = nullptr;
  float *gain1 = nullptr;
  float *gain2 = nullptr;

  void addHorizontalSlider(const char *label, FAUSTFLOAT *zone, FAUSTFLOAT init,
                            FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override {
    if (strcmp(label, "freq1") == 0) freq1 = zone;
    else if (strcmp(label, "freq2") == 0) freq2 = zone;
    else if (strcmp(label, "gain1") == 0) gain1 = zone;
    else if (strcmp(label, "gain2") == 0) gain2 = zone;
  }

  void openTabBox(const char *) override {}
  void openHorizontalBox(const char *) override {}
  void openVerticalBox(const char *) override {}
  void closeBox() override {}
  void addButton(const char *, FAUSTFLOAT *) override {}
  void addCheckButton(const char *, FAUSTFLOAT *) override {}
  void addVerticalSlider(const char *, FAUSTFLOAT *, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override {}
  void addNumEntry(const char *, FAUSTFLOAT *, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override {}
  void addHorizontalBargraph(const char *, FAUSTFLOAT *, FAUSTFLOAT, FAUSTFLOAT) override {}
  void addVerticalBargraph(const char *, FAUSTFLOAT *, FAUSTFLOAT, FAUSTFLOAT) override {}
  void addSoundfile(const char *, const char *, Soundfile **) override {}
};

// Written once in setup(), read continuously by loop() (to update) and
// audioCallback() (to synthesize). Plain float reads/writes are atomic
// on a 32-bit ARM core, so loop() and the audio interrupt can share
// these without a lock - standard practice for control-rate parameters
// feeding an audio-rate engine.
float *freq1Ptr;
float *freq2Ptr;
float *gain1Ptr;
float *gain2Ptr;

// -- Audio-rate interrupt -------------------------------------------

FspTimer audioTimer;

void audioCallback(timer_callback_args_t *args) {
  FAUSTFLOAT sampleBuf[1];
  FAUSTFLOAT *outputs[1] = { sampleBuf };
  synth.compute(1, nullptr, outputs);

  // Faust's output is centered on 0 (nominally [-1, 1]); the DAC wants
  // an unsigned 12-bit code centered on its own mid-scale.
  float sample = sampleBuf[0];
  if (sample > 1.0f) sample = 1.0f;
  if (sample < -1.0f) sample = -1.0f;
  uint16_t dacValue = (uint16_t)((sample * 0.5f + 0.5f) * 4095.0f);

  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- Pots and touch pads -------------------------------------------

RV112FF pot1(A2, A3);
RV112FF pot2(A4, A5);
MultiTurnKnob knob1(pot1);
MultiTurnKnob knob2(pot2);

CapTouch touch1(20);  // gates knob1's speed
CapTouch touch2(21);  // gates knob2's speed

const unsigned long CONTROL_INTERVAL = 10;  // milliseconds (~100 Hz)
unsigned long previousControlMillis = 0;

// Plot every 5th control tick (20 lines a second): plenty to follow a
// turning knob, without scrolling the plotter's window by too fast.
const int PLOT_EVERY_TICKS = 5;
int ticksSincePlot = 0;

// Prints one knob's part of a Serial Plotter line, as label:value pairs:
// its two wiper readings scaled to 0-360, then - once it's calibrated -
// its shaft angle and its pitch on the matching scale (see plotPitch()).
void plotKnob(const char *name, RV112FF &pot, MultiTurnKnob &knob, float freq) {
  Serial.print(name); Serial.print("_ad1:");
  Serial.print(pot.wiper(0) * 360.0);
  Serial.print(","); Serial.print(name); Serial.print("_ad2:");
  Serial.print(pot.wiper(1) * 360.0);
  if (pot.isCalibrated()) {
    Serial.print(","); Serial.print(name); Serial.print("_angle:");
    Serial.print(knob.lastAngle());
    Serial.print(","); Serial.print(name); Serial.print("_pitch:");
    Serial.print(plotPitch(freq));
  }
}

void plotKnobs(float freq1, float freq2) {
  plotKnob("knob1", pot1, knob1, freq1);
  Serial.print(",");
  plotKnob("knob2", pot2, knob2, freq2);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("Audio + knobs demo: turn each pot to bend its oscillator's pitch.");
  Serial.println("Touch a knob's pad while turning it for fast (single-turn) sweeps; let go for slow (multi-turn).");

  touch1.begin();
  touch2.begin();

  // ---- RA6M5-specific ----
  // Tune the ADC for the pots: a longer sampling time and a discharge
  // before each conversion, which between them remove the crosstalk
  // between the two pots (see RA6M5AnalogTuning.h). With that fixed in
  // hardware, RV112FF's portable workaround - a throwaway conversion
  // before each reading - is pure overhead, so switch it off. On any
  // other chip begin() does nothing, and this sketch keeps working as
  // it did before.
  if (RA6M5AnalogTuning::begin(A2)) {
    pot1.setSettlingReads(0);
    pot2.setSettlingReads(0);
  }
  // ---- end RA6M5-specific ----

  synth.init(SAMPLE_RATE);

  static CaptureUI ui;
  synth.buildUserInterface(&ui);
  freq1Ptr = ui.freq1;
  freq2Ptr = ui.freq2;
  gain1Ptr = ui.gain1;
  gain2Ptr = ui.gain2;

  // Open and start both DAC channels through the normal Arduino path
  // once, establishing a known baseline - the audio interrupt below
  // then writes the data register directly each sample, bypassing
  // analogWrite()'s per-call overhead, the same "open normally, then
  // poke the register directly for speed" split used in DACAudioTest.
  // analogWrite()'s value scaling depends on analogWriteResolution(),
  // which otherwise defaults to 8 bits (0-255) rather than this DAC's
  // real 12-bit range - set explicitly so 2048 really means mid-scale.
  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);

  // A hardware timer fires SAMPLE_RATE times per second; each time it
  // calls audioCallback() to compute and output exactly one new sample.
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  audioTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                    (float)SAMPLE_RATE, 50.0, audioCallback, nullptr);
  audioTimer.setup_overflow_irq(1);  // high priority: audio timing is jitter-sensitive
  audioTimer.open();
  audioTimer.start();
}

void loop() {
  // Same non-blocking pattern used throughout this project: compare the
  // clock instead of delay(). Polling the pots is control-rate work -
  // ~100 Hz is plenty responsive for a knob - the audio itself runs
  // entirely on the interrupt above and never waits on loop() at all.
  unsigned long currentMillis = millis();

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    previousControlMillis = currentMillis;

    // One CapTouch::scan() covers both touch pads at once - see
    // RA6M5CapTouch.h - so it only needs calling once here, not per-knob.
    CapTouch::scan();

    float turnsPerSweep1 = touch1.isTouched() ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float angle1 = knobAngle(knob1.read(), turnsPerSweep1);
    *freq1Ptr = angleToFrequency(angle1);
    *gain1Ptr = pot1.isCalibrated() ? 1.0 : 0.0;

    float turnsPerSweep2 = touch2.isTouched() ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float angle2 = knobAngle(knob2.read(), turnsPerSweep2);
    *freq2Ptr = angleToFrequency(angle2);
    *gain2Ptr = pot2.isCalibrated() ? 1.0 : 0.0;

    if (++ticksSincePlot >= PLOT_EVERY_TICKS) {
      ticksSincePlot = 0;
      plotKnobs(*freq1Ptr, *freq2Ptr);
    }
  }
}
