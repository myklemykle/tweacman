/*
  NoiseMeter - measures this board's noise and interference problems

  A diagnostic sketch rather than a demo: it streams numbers to the Arduino
  Serial Plotter (Tools > Serial Plotter in the Arduino IDE) that each
  measure one kind of unwanted signal, arranged so that a perfect, noise-free
  board would plot every trace as a flat line at zero. The plotter zooms to
  fit whatever it's showing, so as you fix things the traces shrink and the
  plotter zooms in with them - you can keep chasing noise until the traces
  are flat. A set of software experiments (see below) can each be switched
  on and off from the keyboard while it runs, to measure what each one is
  worth.

  Wiring (SparkFun Thing Plus RA6M5), the same bench setup as AudioKnobsDemo:
    A2/A3 -> pot 1's two wipers    (Taiwan Alpha RV112FF-40, tracks on VREF/GND)
    A4/A5 -> pot 2's two wipers
    D20   -> touch pad 1 (a wire or pad)
    D21   -> touch pad 2
    D6    -> ~10nF capacitor to GND (TSCAP - required for touch sensing)
    A0/A1 -> the audio jack, if you have one (only used by experiment d)

  What each trace means (all in raw counts):

    touchD20, touchD21 - spurious touch triggers. The touch reading minus
      CapTouch's tracked untouched baseline, i.e. exactly the number
      isTouched() compares against its threshold (3000 by default). Each
      point is the largest excursion seen since the previous point, so a
      single-scan glitch can't slip through between plot updates. Keep
      your hands off the pads: anything here is interference.

    noiseA2..noiseA5 - ADC noise. The RMS (standard deviation) of the
      readings on that pin, i.e. how much the reading jitters while the knob
      sits still. Measured over 50ms windows - long enough to catch 50/60Hz
      mains hum as well as fast random noise.

    xtalkA2..xtalkA5 - ADC crosstalk: how far one pot's readings move
      when you turn the *other* pot. Turn one knob and the other knob's two
      traces show how far its readings have drifted from where they were
      just before you started. They read zero whenever the other knob is
      still, since there's nothing to measure then.

  Two display modes:

    Summary (the default) - one point per second. Touch and crosstalk show
      the worst value of that second, noise the RMS over the whole second.
      At one point per second the plotter's window holds about the last
      minute, and its zoom stays steady - this is the mode for comparing
      one experiment against another.

    Live - one point every 50ms, for watching what happens as it happens
      (e.g. the crosstalk following a knob as you turn it).

  How to test: open the Serial Plotter at 115200 baud. Leave everything
  alone for a while to see the touch and noise baseline, then slowly turn
  one knob through a full rotation or two and watch the other knob's
  xtalk traces. Stop, wait a couple of seconds, then try the other knob.
  Only turn one knob at a time - the sketch works out which knob is moving
  by assuming only one of them is.

  While a knob is being turned (and for MOTION_HOLD_MS afterward) its own
  noise and xtalk traces are held at zero: the knob moving is the signal,
  not noise, and would otherwise swamp everything else on the plot.

  Keyboard commands: type a letter into the message box at the bottom of the
  Serial Plotter and press Enter. Several letters at once work too ("bc"
  switches both b and c). The blue STAT LED blinks each time a command is
  received, so you can tell a keypress arrived even when its effect on the
  plot is too small to see.

  Experiments - each letter switches one on or off:

    a - double read (on at startup). Read each pin twice and keep the
        second result, as the RV112FF library does. See readPot().

    b - long sampling time. Gives the ADC 64 clock cycles (1.28us) to
        charge its sample-and-hold capacitor from each pin, instead of the
        11 cycles (220ns) the Arduino core leaves it at. A pot wiper is a
        slow, high-impedance source, and the RA6M5 manual's own formula
        (section 43.6.14) says a 10k pot needs well over 220ns; too short,
        and each reading is still partly the previous pin's voltage -
        crosstalk. Expected to reduce crosstalk.

    c - discharge before sampling. Empties the sample-and-hold capacitor
        before every conversion (the ADC's "disconnection detection assist"
        feature, section 43.3.10), so no reading can inherit anything from
        the pin before it. Any charging shortfall that remains depends only
        on the pin's own voltage - a small, steady scaling error that the
        RV112FF library's self-calibration absorbs - rather than on the
        other knob. Expected to reduce crosstalk.

    d - DAC running. Plays a 440Hz triangle wave out of A0/A1, written from
        a 40kHz timer interrupt exactly as AudioKnobsDemo and SDResampleDemo
        write their audio. Turn the volume down first! This isn't a fix -
        it recreates the conditions experiment e is meant to fix, so you
        can measure how much a running DAC disturbs the pots.

    e - DAC/ADC synchronization. The RA6M5 manual (section 44.3.1) warns
        that each DAC update draws a burst of current from the analog
        supply the ADC shares, disturbing any conversion in progress, and
        offers a hardware fix: hold each DAC update until the ADC finishes
        converting. The Arduino core leaves this off. Only makes a
        difference while d is on.

  Display:
    s - switch between summary and live mode
    t - show/hide the touch traces
    n - show/hide the noise traces
    x - show/hide the crosstalk traces
  Hiding the biggest traces lets the plotter zoom in on the smaller ones.

  This example code is in the public domain.
*/

#include <CapTouch.h>
#include "FspTimer.h"  // for experiment d's audio-rate interrupt

// Each pot's two wiper pins, and a label for each pin in the plot legend.
const uint8_t POT_PINS[2][2] = { { A2, A3 }, { A4, A5 } };
const char *const POT_PIN_NAMES[2][2] = { { "A2", "A3" }, { "A4", "A5" } };

CapTouch touchPads[2] = { CapTouch(20), CapTouch(21) };
const char *const TOUCH_PIN_NAMES[2] = { "D20", "D21" };

// Measurements are taken as fast as loop() runs, and summarized into one
// "frame" every FRAME_INTERVAL_MS - one plot point in live mode. Summary
// mode then combines SUMMARY_FRAMES frames into each point.
const unsigned long FRAME_INTERVAL_MS = 50;
const int SUMMARY_FRAMES = 20;  // 20 x 50ms = one point per second

// A knob counts as "being turned" when either wiper's average reading
// moves by more than this many counts from one frame to the next - about
// half a degree of shaft rotation. Comfortably above the noise, which is
// averaged over a whole frame here, but low enough to notice a slow,
// careful turn.
const float MOTION_THRESHOLD = 10;

// How long a knob still counts as "being turned" after it stops moving.
// Crosstalk depends on where the turned knob *is*, not just on it moving,
// so pausing mid-turn shouldn't reset the measurement.
const unsigned long MOTION_HOLD_MS = 1500;

// -- Experiments ----------------------------------------------------------

bool doubleRead = true;      // a
bool longSampling = false;   // b
bool dischargeFirst = false; // c
bool dacRunning = false;     // d
bool dacSync = false;        // e

// Experiment b: the ADC's sampling time, in ADC clock cycles (50MHz on
// this board, so 20ns each). One register, ADSSTRL, sets it for every
// analog pin numbered AN116 and up - which covers A2-A5 (AN121-AN118).
// The value the core leaves there is read at startup, so switching the
// experiment off restores exactly that.
const uint8_t LONG_SAMPLING_STATES = 64;
uint8_t defaultSamplingStates;

// Experiment c: ADDISCR's bits 0-3 set how many ADC clock cycles to spend
// discharging (15, the most allowed); bit 4 left at 0 means discharge to
// ground rather than precharge to the reference voltage.
const uint8_t DISCHARGE_SETTING = 15;

// Experiment d: a 440Hz triangle wave, half of full scale, at the same
// 40kHz sample rate AudioKnobsDemo uses. A 32-bit phase accumulator
// steps through one cycle of the wave every 2^32 counts.
const uint32_t DAC_SAMPLE_RATE = 40000;
const uint32_t DAC_PHASE_STEP = (uint32_t)(440.0 / DAC_SAMPLE_RATE * 4294967296.0);
FspTimer dacTimer;
bool dacOpened = false;

// -- Measurement state ----------------------------------------------------

bool summaryMode = true;
bool showTouch = true;
bool showNoise = true;
bool showCrosstalk = true;

// Running mean and standard deviation of one ADC pin's readings, updated
// one reading at a time with Welford's method. The textbook shortcut
// (average of the squares minus the square of the average) subtracts two
// huge, nearly-equal numbers here - readings around 2000 against noise of
// a count or two - and float rounding would swamp the answer.
struct RunningStats {
  unsigned long count = 0;
  float mean = 0;
  float sumSquares = 0;  // sum of squared differences from the mean

  void add(float x) {
    count++;
    float delta = x - mean;
    mean += delta / count;
    sumSquares += delta * (x - mean);
  }

  float rms() const {
    return count > 0 ? sqrtf(sumSquares / count) : 0;
  }
};

// One frame's worth of results - also used to accumulate a summary.
struct Measurements {
  float touch[2];
  float noise[2][2];
  float crosstalk[2][2];
};

RunningStats potStats[2][2];   // readings since the last frame
float previousMean[2][2];      // each pin's average at the last frame
float crosstalkStart[2][2];    // where each pin sat before the other knob moved
bool havePreviousFrame = false;

int turningPot = -1;  // which knob is being turned (0 or 1), or -1 for neither
unsigned long lastMotionMillis = 0;

int32_t touchPeak[2] = { 0, 0 };  // largest touch excursion since the last frame

// Summary mode's running totals: worst touch and crosstalk seen, and the
// sum of each pin's noise variance (RMS squared) over the frames where
// that knob sat still, so the summary's noise is a true RMS over the second.
Measurements summaryWorst;
float noiseVarianceSum[2][2];
int noiseFrameCount[2][2];
int summaryFrameCount = 0;

unsigned long lastFrameMillis = 0;

const unsigned long LED_BLINK_MS = 150;
unsigned long ledOnMillis = 0;
bool ledOn = false;

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  analogReadResolution(12);
  touchPads[0].begin();
  touchPads[1].begin();

  // The ADC's registers can only be read or written once analogRead() has
  // switched it on, so read once before touching them.
  analogRead(A2);
  defaultSamplingStates = R_ADC1->ADSSTRL;

  // Experiment d's timer is set up now but only started when d is
  // switched on.
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  dacTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                 (float)DAC_SAMPLE_RATE, 50.0, dacCallback, nullptr);
  dacTimer.setup_overflow_irq(1);
  dacTimer.open();

  clearSummary();
}

void loop() {
  handleCommands();

  // Read all four wipers the same way AudioKnobsDemo does, so the noise and
  // crosstalk measured here are what the real sketches get.
  for (int pot = 0; pot < 2; pot++) {
    for (int wiper = 0; wiper < 2; wiper++) {
      potStats[pot][wiper].add(readPot(POT_PINS[pot][wiper]));
    }
  }

  CapTouch::scan();
  for (int pad = 0; pad < 2; pad++) {
    int32_t excursion = (int32_t)touchPads[pad].readRaw() - touchPads[pad].readBaseline();
    if (abs(excursion) > abs(touchPeak[pad])) {
      touchPeak[pad] = excursion;
    }
  }

  unsigned long currentMillis = millis();
  if (currentMillis - lastFrameMillis >= FRAME_INTERVAL_MS) {
    lastFrameMillis = currentMillis;

    Measurements frame;
    measureFrame(currentMillis, frame);

    if (!summaryMode) {
      plot(frame);
    } else {
      addToSummary(frame);
      if (summaryFrameCount >= SUMMARY_FRAMES) {
        plot(finishSummary());
        clearSummary();
      }
    }
  }

  if (ledOn && currentMillis - ledOnMillis >= LED_BLINK_MS) {
    digitalWrite(LED_BUILTIN, LOW);
    ledOn = false;
  }
}

int readPot(uint8_t pin) {
  // Experiment a: read twice and keep the second result, as the RV112FF
  // library does. It was meant to give the ADC extra time to settle on
  // this pin, but that isn't quite what happens on this core: analogRead()
  // converts *every* pin you've ever read on the same ADC unit, lowest
  // channel number first, and returns just the one you asked for - so all
  // four wipers are converted, in the order A5, A4, A3, A2, on every call,
  // and the first read just repeats the whole scan.
  if (doubleRead) {
    analogRead(pin);
  }
  return analogRead(pin);
}

// -- Experiments ----------------------------------------------------------

// Writes experiments b, c and e into the hardware. The manual only allows
// these registers to change while the ADC is idle, which it always is
// here: analogRead() waits for its conversion to finish before returning.
void applyExperiments() {
  R_ADC1->ADSSTRL = longSampling ? LONG_SAMPLING_STATES : defaultSamplingStates;
  R_ADC1->ADDISCR = dischargeFirst ? DISCHARGE_SETTING : 0;

  if (dacSync) {
    // The manual's order: pick the ADC unit to synchronize with (only
    // unit 1 is allowed on this chip, which is the one the pots are on),
    // then switch synchronization on. The unit can only be picked while
    // synchronization is off, so skip this if it's already on.
    openDac();
    if (!R_DAC->DAADSCR_b.DAADST) {
      R_DAC->DAADUSR_b.AMADSEL1 = 1;
      R_DAC->DAADSCR_b.DAADST = 1;
    }
  } else if (dacOpened) {
    R_DAC->DAADSCR_b.DAADST = 0;
  }

  if (dacRunning) {
    openDac();
    dacTimer.start();
  } else {
    dacTimer.stop();
    if (dacOpened) {
      R_DAC->DADR[0] = 2048;  // park both outputs at mid-scale
      R_DAC->DADR[1] = 2048;
    }
  }
}

// Switches on both DAC channels through the normal Arduino path, once;
// dacCallback() then writes the DAC's data registers directly, the same
// way the audio sketches do.
void openDac() {
  if (!dacOpened) {
    analogWriteResolution(12);
    analogWrite(A0, 2048);
    analogWrite(A1, 2048);
    dacOpened = true;
  }
}

void dacCallback(timer_callback_args_t *args) {
  static uint32_t phase = 0;
  phase += DAC_PHASE_STEP;

  // The top 12 bits of the phase ramp from 0 to 4095 once per cycle; fold
  // that into a triangle, then scale it to half of full scale around the
  // DAC's mid-point (1024-3071).
  uint32_t ramp = phase >> 20;
  uint32_t triangle = (ramp < 2048) ? ramp : 4095 - ramp;  // 0-2047
  uint16_t dacValue = 1024 + triangle;

  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- Measuring ------------------------------------------------------------

void measureFrame(unsigned long currentMillis, Measurements &frame) {
  // Work out which knob, if either, is being turned: whichever one moved
  // further since the last frame, provided it moved far enough to count.
  if (havePreviousFrame) {
    float motion[2];
    for (int pot = 0; pot < 2; pot++) {
      motion[pot] = max(fabsf(potStats[pot][0].mean - previousMean[pot][0]),
                        fabsf(potStats[pot][1].mean - previousMean[pot][1]));
    }
    int fastest = (motion[1] > motion[0]) ? 1 : 0;
    if (motion[fastest] > MOTION_THRESHOLD) {
      turningPot = fastest;
      lastMotionMillis = currentMillis;
    } else if (currentMillis - lastMotionMillis > MOTION_HOLD_MS) {
      turningPot = -1;
    }
  }

  for (int pot = 0; pot < 2; pot++) {
    int otherPot = 1 - pot;
    for (int wiper = 0; wiper < 2; wiper++) {
      float mean = potStats[pot][wiper].mean;

      // A knob that's being turned is changing on purpose - that isn't
      // noise, so hold its traces at zero rather than plot the turn.
      frame.noise[pot][wiper] = (turningPot == pot) ? 0 : potStats[pot][wiper].rms();

      // While the other knob is turning, this one's crosstalk is how far
      // it has drifted from where it sat just before that started.
      // Otherwise keep updating that starting point, ready for next time.
      if (havePreviousFrame && turningPot == otherPot) {
        frame.crosstalk[pot][wiper] = mean - crosstalkStart[pot][wiper];
      } else {
        crosstalkStart[pot][wiper] = mean;
        frame.crosstalk[pot][wiper] = 0;
      }

      previousMean[pot][wiper] = mean;
      potStats[pot][wiper] = RunningStats();  // start fresh for the next frame
    }
  }
  havePreviousFrame = true;

  for (int pad = 0; pad < 2; pad++) {
    frame.touch[pad] = touchPeak[pad];
    touchPeak[pad] = 0;
  }
}

// Keeps whichever of two values is further from zero, sign and all.
float worstOf(float a, float b) {
  return (fabsf(b) > fabsf(a)) ? b : a;
}

void addToSummary(const Measurements &frame) {
  for (int pad = 0; pad < 2; pad++) {
    summaryWorst.touch[pad] = worstOf(summaryWorst.touch[pad], frame.touch[pad]);
  }
  for (int pot = 0; pot < 2; pot++) {
    for (int wiper = 0; wiper < 2; wiper++) {
      summaryWorst.crosstalk[pot][wiper] =
        worstOf(summaryWorst.crosstalk[pot][wiper], frame.crosstalk[pot][wiper]);

      // Frames where this knob was being turned report zero noise - leave
      // them out rather than let them dilute the average.
      if (turningPot != pot) {
        float rms = frame.noise[pot][wiper];
        noiseVarianceSum[pot][wiper] += rms * rms;
        noiseFrameCount[pot][wiper]++;
      }
    }
  }
  summaryFrameCount++;
}

Measurements finishSummary() {
  Measurements summary = summaryWorst;
  for (int pot = 0; pot < 2; pot++) {
    for (int wiper = 0; wiper < 2; wiper++) {
      int frames = noiseFrameCount[pot][wiper];
      summary.noise[pot][wiper] = (frames > 0) ? sqrtf(noiseVarianceSum[pot][wiper] / frames) : 0;
    }
  }
  return summary;
}

void clearSummary() {
  summaryWorst = Measurements();
  for (int pot = 0; pot < 2; pot++) {
    for (int wiper = 0; wiper < 2; wiper++) {
      noiseVarianceSum[pot][wiper] = 0;
      noiseFrameCount[pot][wiper] = 0;
    }
  }
  summaryFrameCount = 0;
}

// -- Output ---------------------------------------------------------------

void plot(const Measurements &m) {
  // Serial Plotter format: comma-separated "label:value" pairs, one line
  // per point. Skip printing while nothing's listening - on this board's
  // native USB serial port the text would only be thrown away anyway.
  if (!Serial) {
    return;
  }

  bool anyShown = false;
  if (showTouch) {
    for (int pad = 0; pad < 2; pad++) {
      plotValue("touch", TOUCH_PIN_NAMES[pad], m.touch[pad], anyShown);
    }
  }
  if (showNoise) {
    for (int pot = 0; pot < 2; pot++) {
      for (int wiper = 0; wiper < 2; wiper++) {
        plotValue("noise", POT_PIN_NAMES[pot][wiper], m.noise[pot][wiper], anyShown);
      }
    }
  }
  if (showCrosstalk) {
    for (int pot = 0; pot < 2; pot++) {
      for (int wiper = 0; wiper < 2; wiper++) {
        plotValue("xtalk", POT_PIN_NAMES[pot][wiper], m.crosstalk[pot][wiper], anyShown);
      }
    }
  }
  if (anyShown) {
    Serial.println();
  }
}

// Prints one "labelPin:value" pair, with a comma first unless it's the
// first on the line.
void plotValue(const char *label, const char *pin, float value, bool &anyShown) {
  if (anyShown) {
    Serial.print(',');
  }
  Serial.print(label);
  Serial.print(pin);
  Serial.print(':');
  Serial.print(value, 2);
  anyShown = true;
}

void handleCommands() {
  while (Serial.available() > 0) {
    switch (Serial.read()) {
      case 'a': doubleRead = !doubleRead; break;
      case 'b': longSampling = !longSampling; break;
      case 'c': dischargeFirst = !dischargeFirst; break;
      case 'd': dacRunning = !dacRunning; break;
      case 'e': dacSync = !dacSync; break;
      case 's': summaryMode = !summaryMode; break;
      case 't': showTouch = !showTouch; break;
      case 'n': showNoise = !showNoise; break;
      case 'x': showCrosstalk = !showCrosstalk; break;
      default: continue;  // Enter's line ending, or an unknown key: ignore
    }

    // A recognized command: blink to acknowledge it, start a fresh summary
    // so the next point reflects only the new settings, and push any
    // experiment changes into the hardware.
    digitalWrite(LED_BUILTIN, HIGH);
    ledOn = true;
    ledOnMillis = millis();
    clearSummary();
    applyExperiments();
  }
}
