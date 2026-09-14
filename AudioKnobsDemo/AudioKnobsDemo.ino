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
  still apply here; nothing about those changes with Faust in the mix.

  Pitch mapping: each pot's raw angle (0-360 degrees, wrapping) is
  folded around its 180-degree midpoint before use - foldedAngle is 0
  at 0 and 360 (the wrap point) and rises to 180 exactly at 180 - so
  the frequency sweep reverses direction there instead of jumping
  discontinuously at the 360-to-0 seam. That folded angle is then
  mapped exponentially, not linearly, onto the 50-10000 Hz range: equal
  angular steps become equal pitch *ratios* rather than equal Hz steps,
  which is what a linear-feeling pitch sweep actually requires (we hear
  pitch logarithmically - the interval from 100 to 200 Hz sounds like
  the same size jump as 5000 to 10000 Hz, not 1/50th the size).

  Each oscillator stays silent (gain 0) until its pot finishes its own
  self-calibration (see RV112FF.h) - otherwise it would play some
  arbitrary tone from the moment the sketch boots, before the knob has
  ever been touched.

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

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include "faust_support.h"
#include "synth.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR

// -- Pitch range and mapping -------------------------------------------

const float MIN_FREQ = 50.0;
const float MAX_FREQ = 10000.0;

// See the header comment above: fold the angle around its 180-degree
// midpoint, then map that exponentially onto the frequency range.
float angleToFrequency(float angleDegrees) {
  float foldedAngle = (angleDegrees <= 180.0) ? angleDegrees : (360.0 - angleDegrees);
  float t = foldedAngle / 180.0;  // 0 at the reversal points, 1 at the peak
  return MIN_FREQ * pow(MAX_FREQ / MIN_FREQ, t);
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

// -- Pots -------------------------------------------

RV112FF pot1(A2, A3);
RV112FF pot2(A4, A5);

const unsigned long CONTROL_INTERVAL = 10;  // milliseconds (~100 Hz)
unsigned long previousControlMillis = 0;

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("Audio + knobs demo: turn each pot to bend its oscillator's pitch.");

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

    float angle1 = pot1.read();
    *freq1Ptr = angleToFrequency(angle1);
    *gain1Ptr = pot1.isCalibrated() ? 1.0 : 0.0;

    float angle2 = pot2.read();
    *freq2Ptr = angleToFrequency(angle2);
    *gain2Ptr = pot2.isCalibrated() ? 1.0 : 0.0;
  }
}
