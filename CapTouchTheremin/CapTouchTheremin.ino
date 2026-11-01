// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  CapTouchTheremin - SparkFun Thing Plus RA6M5

  A theremin-ish instrument from the RA6M5's capacitive touch pads: hold
  one hand near the pitch pad and the other near the volume pad, and
  play without touching anything. The touch hardware measures a pad's
  capacitance, not just touched or not, and a hand nearby adds a little
  capacitance - enough to sense from an inch or so away with a plain
  wire. A real theremin works the same way, with antennas sensing much
  further.

  Wiring (the same as the other demos in this project):
    D6      -> a ~10nF capacitor to GND (TSCAP, which the touch hardware
               needs - see RA6M5CapTouch.h)
    D21     -> the pitch pad: a wire or small metal plate (TS04) - on
               the right, as on a real theremin, on this project's board
    D20     -> the volume pad, on the left (TS05)
    A0, A1  -> a 3.5mm jack's tip and ring, sleeve to GND (both get the
               same mono signal - see DACAudioTest for the DC offset,
               and why a coupling capacitor helps)
    Knob 1  -> an RV112FF-40 pot, wipers on A2/A3, track ends 3.3V/GND
    Knob 2  -> the same, wipers on A4/A5
  The board's USR button (D31) is used too; it needs no wiring.

  Playing:
  - Pitch pad: the closer your hand, the higher the note, over three
    octaves. Away from the pad it rests on the bottom note.
  - Volume pad: the closer your hand, the louder. Unlike a real theremin,
    whose volume antenna mutes as you approach, it's silent with your
    hand away - so it doesn't drone whenever nobody's playing.
  - Knob 1: smoothing - how many of the pads' most recent readings are
    averaged, from 1 to 64. Turn it down and the pitch gets jittery;
    turn it up and it steadies but lags behind your hand. See "Noise
    and smoothing" below.
  - Knob 2: the bottom note, anywhere from C2 to C5.
  - USR button: snaps the pitch to notes - glide (no snapping, like a
    real theremin), then semitones, then a major pentatonic scale on
    the bottom note, then back to glide.
  - Keep your hands away from the pads for the first second after power
    on (the blue STAT LED is lit meanwhile): that's when each pad's
    hands-away reading is measured. Send "c" over Serial to measure it
    again, and "s" to step the snapping, as the USR button does.

  The pots report nothing until they're calibrated: give each a couple
  of full turns. Until then knob 1 acts as if set to 16 readings and
  knob 2 to C3. If SDResampleDemo has saved the pots' calibration to the
  Data Flash, it's read back at boot and the knobs work straight away
  (this sketch only reads it, never writes it).

  How a hand becomes a note: each pad reads its hands-away level plus
  whatever your hand adds. The added part grows faster and faster as
  your hand closes in - roughly in proportion to 1/distance - so a
  linear mapping would put the whole pitch range in the last few
  millimetres. Taking a logarithm of it instead (see closeness())
  spreads the range out evenly over the distance you can actually play
  in. PITCH_FULL and VOLUME_FULL say how much your hand adds at the
  top of each range; watch the Serial Plotter (below) to tune them for
  your own pads - a bigger pad senses further, and reads higher.

  Noise and smoothing: the touch hardware's readings wobble by a dozen
  or so counts from one scan to the next. Your ear notices that in the
  pitch long before it notices it in the volume. Averaging the most
  recent readings quietens it, but an average of N readings trails
  N/2 readings behind your hand. This sketch scans as fast as the
  hardware allows - about 2500 times a second, using the touch library's
  non-blocking startScan()/update() - so even 64 readings is only about
  13ms of lag. That's knob 1's range: listen for the point where the
  wobble goes away without the pitch feeling sluggish. Some of the
  noise is too slow to average away in that time (see the RA6M5CapTouch
  notes in OPEN_ITEMS.md), which is part of why a real theremin's pitch
  is never perfectly steady either.

  Serial Plotter (115200 baud), 50 lines a second: pitchHand and
  volumeHand are how much each hand adds to its pad's reading; pitch
  and volume are where that puts you in each range, 0 at the bottom to
  1000 at the top; smoothing is knob 1's setting.

  The sound is a sine wave with a little second and third harmonic
  added, played from a table by a 48kHz timer interrupt - the same
  "interrupt writes the DAC directly" pattern as AudioKnobsDemo, but in
  plain C++ rather than Faust. The interrupt glides each sample a little
  of the way towards the latest pitch and volume, so the steps between
  scans don't click.
*/

#include <RA6M5CapTouch.h>
#include <RV112FF.h>
#include <RA6M5AnalogTuning.h>
#include <EEPROM.h>    // the Data Flash, to read back SDResampleDemo's pot calibration
#include "bsp_api.h"   // for R_DAC - the audio interrupt writes it directly
#include "FspTimer.h"  // for the audio-rate interrupt

// -- Tuning: how your hands map onto the instrument -----------------------

// How much a hand adds to a pad's reading at the top of each range, in
// the touch hardware's counts. A hand right against the bench pads adds
// about 14500; the plotter's pitchHand and volumeHand show what yours do.
const float PITCH_FULL = 14000;
const float VOLUME_FULL = 14000;

// Where the logarithmic curve (see closeness()) bends: below about this
// many counts it's nearly linear. Smaller spreads the far end of the
// range out more, but gives the noise more say there.
const float KNEE = 150;

// Each pad ignores a hand that adds less than this, so the readings'
// noise can't make the volume whisper, or the resting note wobble, by
// itself.
const float PITCH_DEAD_ZONE = 30;
const float VOLUME_DEAD_ZONE = 40;

const float PITCH_RANGE_OCTAVES = 3;

// Knob 2's range for the bottom note: C2 up to three octaves above.
const float LOWEST_BASE_HZ = 65.41;  // C2
const float BASE_RANGE_OCTAVES = 3;

// Before the pots are calibrated (see the header comment).
const int DEFAULT_SMOOTHING = 16;
const float DEFAULT_BASE_HZ = 130.81;  // C3

// -- The sound: a wavetable oscillator in the audio interrupt -----------

const uint32_t SAMPLE_RATE = 48000;
const int TABLE_SIZE = 1024;
float waveTable[TABLE_SIZE + 1];  // one extra entry, a copy of the first, for interpolating

// Set by loop(), read by the interrupt. Single floats, so each read or
// write is one instruction and they need no lock.
volatile float targetHz = DEFAULT_BASE_HZ;
volatile float targetLevel = 0;  // 0 silent - 1 full scale

// How far each sample glides towards those targets: a time constant of
// about 2ms, set in setup().
float glide;

FspTimer audioTimer;

// optimize("O2"): the core builds everything for size (-Os), but this
// runs 48000 times a second.
__attribute__((optimize("O2"))) void audioCallback(timer_callback_args_t *args) {
  static float hz = DEFAULT_BASE_HZ, level = 0, phase = 0;

  hz += (targetHz - hz) * glide;
  level += (targetLevel - level) * glide;

  // Phase runs from 0 to 1 once per cycle of the wave.
  phase += hz * (1.0f / SAMPLE_RATE);
  if (phase >= 1.0f) phase -= 1.0f;

  // Look the wave up in the table, interpolating between its entries.
  float position = phase * TABLE_SIZE;
  int index = (int)position;
  float frac = position - index;
  float sample = waveTable[index] + (waveTable[index + 1] - waveTable[index]) * frac;

  // The DAC takes 0-4095, with silence in the middle.
  uint16_t dacValue = (uint16_t)(2048.0f + sample * level * 2047.0f);
  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- Pads ---------------------------------------------------------------

CapTouch pitchPad(21);
CapTouch volumePad(20);

// The most recent readings from each pad, oldest overwritten first, for
// the moving average knob 1 sets.
const int MAX_SMOOTHING = 64;
uint16_t history[2][MAX_SMOOTHING];
int historyPos = 0;
int smoothing = DEFAULT_SMOOTHING;

// Each pad's hands-away reading, measured at boot (and on "c").
float handsAway[2] = {0, 0};
const unsigned long CALIBRATE_MS = 1000;
unsigned long calibrateStart = 0;
bool calibrating = true;
uint32_t calibrateSum[2] = {0, 0};
uint32_t calibrateScans = 0;

// After calibrating, a pad's hands-away reading still drifts slowly with
// temperature. Follow it while no hand is near - while the reading is
// within DRIFT_BAND of it, either way - with a time constant of about 20
// seconds, slow enough to ignore the noise. A reading well below it
// can't be a hand, so follow that faster, in about half a second.
const float DRIFT_BAND = 30;
const float DRIFT_PER_SCAN = 1.0f / (20.0f * 2500.0f);  // ~2500 scans a second
const float DROP_PER_SCAN = 1.0f / (0.5f * 2500.0f);

// How much each hand adds now (averaged), and where that puts it.
float handAmount[2] = {0, 0};
float pitchCloseness = 0, volumeCloseness = 0;  // 0 - 1

// 0 at a hand that adds nothing, 1 at one that adds `full` counts or
// more, on a logarithmic curve - see the header comment.
float closeness(float amount, float full) {
  if (amount <= 0) return 0;
  float c = logf(1.0f + amount / KNEE) / logf(1.0f + full / KNEE);
  return c < 1.0f ? c : 1.0f;
}

// -- Snapping to notes ----------------------------------------------------

enum Snap { GLIDE, SEMITONES, PENTATONIC };
Snap snap = GLIDE;
const char *SNAP_NAMES[] = {"glide", "semitones", "pentatonic"};

const int PENTATONIC_STEPS[] = {0, 2, 4, 7, 9};  // major pentatonic, in semitones

// The allowed note nearest to `semitones` above the bottom note.
float nearestNote(float semitones) {
  if (snap == SEMITONES) return roundf(semitones);
  // PENTATONIC: try each scale step in this octave and the next.
  int octave = (int)floorf(semitones / 12.0f);
  float best = 0, bestDistance = 1e9;
  for (int o = octave; o <= octave + 1; o++) {
    for (int step : PENTATONIC_STEPS) {
      float note = o * 12 + step;
      float distance = fabsf(note - semitones);
      if (distance < bestDistance) { best = note; bestDistance = distance; }
    }
  }
  return best;
}

// Snapped, a hand hovering right between two notes would flicker between
// them with every wobble of the readings. So hold the current note until
// another is nearer by at least this many semitones.
const float SNAP_HYSTERESIS = 0.15;
float heldNote = 0;

float snapped(float semitones) {
  if (snap == GLIDE) return semitones;
  float candidate = nearestNote(semitones);
  if (fabsf(semitones - candidate) + SNAP_HYSTERESIS < fabsf(semitones - heldNote)) {
    heldNote = candidate;
  }
  return heldNote;
}

void stepSnap() {
  snap = (Snap)((snap + 1) % 3);
  heldNote = nearestNote(pitchCloseness * PITCH_RANGE_OCTAVES * 12);
  Serial.print("Snapping set to ");
  Serial.println(SNAP_NAMES[snap]);
}

// -- Knobs ----------------------------------------------------------------

RV112FF pot1(A2, A3);
RV112FF pot2(A4, A5);
float baseHz = DEFAULT_BASE_HZ;

const unsigned long KNOB_INTERVAL = 10;  // ms - plenty for a knob
unsigned long previousKnobMillis = 0;

// SDResampleDemo keeps each pot's calibration at these Data Flash
// addresses; restore it if it's there. setCalibration() checks the data
// and refuses anything that isn't a real calibration.
void restorePotCalibration() {
  RV112FF *pots[2] = {&pot1, &pot2};
  for (int i = 0; i < 2; i++) {
    RV112FFCalibration cal;
    EEPROM.get(i * (int)sizeof(RV112FFCalibration), cal);
    pots[i]->setCalibration(cal);
  }
}

void readKnobs() {
  float angle1 = pot1.read();  // -1 until calibrated
  float angle2 = pot2.read();
  // Knob 1: 1 to 64 readings, doubling every sixth of a turn.
  smoothing = (angle1 < 0) ? DEFAULT_SMOOTHING
                           : (int)roundf(powf(2.0f, angle1 / 360.0f * 6.0f));
  if (smoothing > MAX_SMOOTHING) smoothing = MAX_SMOOTHING;
  // Knob 2: the bottom note, on an octave scale so equal turns are equal
  // musical intervals - and higher clockwise, which is the opposite way
  // to the angle RV112FF reports.
  baseHz = (angle2 < 0) ? DEFAULT_BASE_HZ
                        : LOWEST_BASE_HZ * powf(2.0f, (360.0f - angle2) / 360.0f * BASE_RANGE_OCTAVES);
}

// -- Scans --------------------------------------------------------------

void startCalibrating() {
  calibrating = true;
  calibrateStart = millis();
  calibrateSum[0] = calibrateSum[1] = 0;
  calibrateScans = 0;
  targetLevel = 0;
  digitalWrite(LED_BUILTIN, HIGH);
}

// Called with each finished scan's readings.
void handleScan(uint16_t pitchRaw, uint16_t volumeRaw) {
  if (calibrating) {
    calibrateSum[0] += pitchRaw;
    calibrateSum[1] += volumeRaw;
    calibrateScans++;
    if (millis() - calibrateStart >= CALIBRATE_MS) {
      handsAway[0] = (float)calibrateSum[0] / calibrateScans;
      handsAway[1] = (float)calibrateSum[1] / calibrateScans;
      // Start the history off at the hands-away level, not zero.
      for (int i = 0; i < MAX_SMOOTHING; i++) {
        history[0][i] = (uint16_t)handsAway[0];
        history[1][i] = (uint16_t)handsAway[1];
      }
      calibrating = false;
      digitalWrite(LED_BUILTIN, LOW);
    }
    return;
  }

  history[0][historyPos] = pitchRaw;
  history[1][historyPos] = volumeRaw;
  historyPos = (historyPos + 1) % MAX_SMOOTHING;

  for (int pad = 0; pad < 2; pad++) {
    // Average the most recent `smoothing` readings.
    uint32_t sum = 0;
    for (int i = 1; i <= smoothing; i++) {
      sum += history[pad][(historyPos - i + MAX_SMOOTHING) % MAX_SMOOTHING];
    }
    float amount = (float)sum / smoothing - handsAway[pad];

    // Follow the hands-away reading's slow drift - see DRIFT_BAND.
    if (amount < -DRIFT_BAND) handsAway[pad] += amount * DROP_PER_SCAN;
    else if (amount < DRIFT_BAND) handsAway[pad] += amount * DRIFT_PER_SCAN;

    handAmount[pad] = amount;
  }

  pitchCloseness = closeness(handAmount[0] - PITCH_DEAD_ZONE, PITCH_FULL);
  volumeCloseness = closeness(handAmount[1] - VOLUME_DEAD_ZONE, VOLUME_FULL);

  float semitones = snapped(pitchCloseness * PITCH_RANGE_OCTAVES * 12);
  targetHz = baseHz * powf(2.0f, semitones / 12.0f);
  // Squared, because loudness follows level on a curve - this makes the
  // volume hand feel more even.
  targetLevel = volumeCloseness * volumeCloseness;
}

// -- Serial ---------------------------------------------------------------

bool wasConnected = false;

const unsigned long PLOT_INTERVAL = 20;  // ms (~50 Hz)
unsigned long previousPlotMillis = 0;

void greet() {
  Serial.println("CapTouchTheremin: pitch hand near D21 (right), volume hand near D20 (left).");
  Serial.println("Knob 1 smoothing, knob 2 bottom note, USR button snaps to notes.");
  Serial.println("Send c to remeasure the pads with hands away, s to step the snapping.");
}

void plot() {
  Serial.print("pitchHand:");
  Serial.print(handAmount[0], 0);
  Serial.print(",volumeHand:");
  Serial.print(handAmount[1], 0);
  Serial.print(",pitch:");
  Serial.print(pitchCloseness * 1000, 0);
  Serial.print(",volume:");
  Serial.print(volumeCloseness * 1000, 0);
  Serial.print(",smoothing:");
  Serial.println(smoothing);
}

// -- USR button -----------------------------------------------------------

bool lastButton = HIGH;  // active low, with the internal pull-up
unsigned long lastButtonChange = 0;

void checkButton() {
  bool button = digitalRead(USR_BTN);
  if (button != lastButton && millis() - lastButtonChange > 30) {  // 30ms debounce
    lastButtonChange = millis();
    lastButton = button;
    if (button == LOW) stepSnap();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(USR_BTN, INPUT_PULLUP);

  pitchPad.begin();
  volumePad.begin();

  // ---- RA6M5-specific ----
  // Tune the ADC for the pots, removing crosstalk between them - see
  // RA6M5AnalogTuning.h - which makes RV112FF's own workaround for it
  // unnecessary.
  if (RA6M5AnalogTuning::begin(A2)) {
    pot1.setSettlingReads(0);
    pot2.setSettlingReads(0);
  }
  // ---- end RA6M5-specific ----
  restorePotCalibration();

  // One cycle of the wave: a sine, with a touch of second and third
  // harmonic for a less glassy tone. The weights add up to 1, so it can't
  // exceed full scale.
  for (int i = 0; i <= TABLE_SIZE; i++) {
    float angle = 2.0f * PI * i / TABLE_SIZE;
    waveTable[i] = 0.8f * sinf(angle) + 0.13f * sinf(2 * angle) + 0.07f * sinf(3 * angle);
  }
  glide = 1.0f - expf(-1.0f / (0.002f * SAMPLE_RATE));

  // Open both DAC channels the normal way, at mid-scale, then let the
  // interrupt write their data registers directly. 12 bits, as the DAC
  // really has - analogWrite() otherwise assumes 8.
  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);

  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  audioTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                   (float)SAMPLE_RATE, 50.0, audioCallback, nullptr);
  audioTimer.setup_overflow_irq(1);  // high priority: audio timing is jitter-sensitive
  audioTimer.open();
  audioTimer.start();

  startCalibrating();
  CapTouch::startScan();
}

void loop() {
  // The pads: scan continuously, without waiting - see the header
  // comment on smoothing.
  if (CapTouch::update()) {
    handleScan(pitchPad.readRaw(), volumePad.readRaw());
    CapTouch::startScan();
  }

  unsigned long now = millis();

  if (now - previousKnobMillis >= KNOB_INTERVAL) {
    previousKnobMillis = now;
    readKnobs();
    checkButton();
  }

  // Serial is native USB: anything sent before a computer opens the port
  // is lost, so greet whenever it does.
  bool connected = Serial;
  if (connected && !wasConnected) greet();
  wasConnected = connected;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'c') startCalibrating();
    if (c == 's') stepSnap();
  }

  if (connected && !calibrating && now - previousPlotMillis >= PLOT_INTERVAL) {
    previousPlotMillis = now;
    plot();
  }
}
