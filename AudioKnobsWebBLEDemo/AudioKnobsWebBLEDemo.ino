/*
  AudioKnobsWebBLEDemo - SparkFun Thing Plus RA6M5

  AudioKnobsDemo (two RV112FF-40 pots each bending a Faust oscillator's
  pitch, touch-gated fast/slow speed via CapTouch and MultiTurnKnob -
  see that sketch's own header comment for all of that, unchanged here)
  plus a wireless remote control surface: a companion web page, talking
  to this board over the onboard DA14531MOD's "CodeLess" BLE link (see
  HelloWorldBLE.ino), can hold either touch pad "virtually" from across
  the room and see live freq1/freq2/touched1/touched2 readout.

  The companion web page (web/oscillator-remote.html in this sketch's
  own folder) has to be self-hosted somewhere with a real HTTPS
  certificate you control - it can't be a Claude Artifact. Confirmed
  live: Claude's artifact hosting applies its own Permissions-Policy
  that blocks navigator.bluetooth outright ("Access to the feature
  'bluetooth' is disallowed by permissions policy"), regardless of
  whether the page is viewed embedded or opened directly.

  Why a companion app instead of Safari directly: iOS has no Web
  Bluetooth support in any browser (Apple requires every iOS browser to
  use WebKit, which doesn't implement it) - a third-party app that
  ships its own Bluetooth stack is required to run a Web Bluetooth page
  on an iPhone at all. Of the ones tried, Bluefy worked once the page
  was self-hosted; WebBLE's own UI didn't render usably; a second app,
  "WebBLE Browser," could see the board but never actually connected -
  a native BLE scanner app (LightBlue) confirmed the board and the
  phone's Bluetooth were both fine throughout, isolating each failure
  to that specific app's own Web Bluetooth implementation rather than
  anything on this end. Also confirmed live: Bluefy's implementation of
  writeValueWithoutResponse() doesn't reliably deliver the write at all
  (resolves with no error, nothing arrives at the peripheral) - the web
  page uses plain writeValue() for that reason, even on
  write-without-response-only characteristics, where it still works
  correctly per spec.

  Wiring: identical to AudioKnobsDemo - see its header comment.

  Protocol: CodeLess's normal AT-command link (the same one HelloWorldBLE
  uses) turns out not to support arbitrary inbound data at all - writing
  plain text to its Inbound Command characteristic gets parsed as an AT
  command and rejected ("EC06: Non AT command") rather than forwarded
  anywhere. Confirmed live, the hard way, before this sketch existed in
  its current form.

  What actually works is CodeLess's separate "Binary mode" / DSPS
  (Direct Serial Port Service) path - documented for a "Generic GATT
  Client" (i.e. exactly a web page, not another CodeLess device) in
  Renesas' UM-140 manual, section 8.6. Once negotiated, it turns Serial2
  into a genuinely transparent pipe: whatever the web page writes to the
  DSPS RX characteristic arrives here as plain raw bytes, and whatever
  this sketch writes to Serial2 arrives at the page as a DSPS TX
  notification - no AT parsing, no pipe-prefix convention, in either
  direction.

  Entering Binary mode is a short local/remote handshake, and - found
  only by tracing every raw byte on Serial2 during a live attempt - the
  local host (this sketch) is an active participant, not a bystander:
  the module does NOT grant Binary mode unilaterally just because the
  remote central asked for it.

    1. Web page writes "AT+BINREQ" to the Inbound Command characteristic
       (866d3b04-.../914f8fb9-...-5425) - the normal AT-command link,
       still used only for this one negotiation step.
    2. The module announces this to the local host as an unsolicited
       "+BINREQ" line on Serial2 - see loop() below, which is what
       actually approves it by replying with ble.sendCommand("AT+BINREQACK").
    3. Once approved, Serial2 becomes the transparent DSPS pipe described
       above; binaryModeActive latches true and stays true until the
       next disconnect.

  From there:
    Outbound (this board -> phone), every BLE_TELEMETRY_INTERVAL, once
      binaryModeActive: a plain line, no prefix -
      "F1:<hz>,F2:<hz>,T1:<0|1>,T2:<0|1>\n" - written straight to
      Serial2 with Serial2.print(), not through ble.sendCommand() (which
      expects an AT-style OK/ERROR reply that Binary mode no longer
      sends). T1/T2 reflect the *effective* touched state (physical pad
      OR held BLE button).
    Inbound (phone -> this board): plain text commands "T1:1", "T1:0",
      "T2:1", "T2:0" - press-and-hold semantics matching the physical
      pads, not a toggle - read the same way as any other Serial2 data,
      via ble.available()/readOutput() (which are just thin Serial2
      wrappers - they were never AT-aware themselves; only the module's
      own firmware parses AT syntax, and Binary mode is precisely what
      turns that parsing off).

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <CapTouch.h>
#include <codelessBLEPeripheral.h>
#include "faust_support.h"
#include "synth.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR

// -- Pitch range and mapping (identical to AudioKnobsDemo) --------------

const float MIN_FREQ = 50.0;
const float MAX_FREQ = 10000.0;

float angleToFrequency(float angleDegrees) {
  float foldedAngle = (angleDegrees <= 180.0) ? angleDegrees : (360.0 - angleDegrees);
  float t = foldedAngle / 180.0;
  return MIN_FREQ * pow(MAX_FREQ / MIN_FREQ, t);
}

const float TURNS_PER_SWEEP_FAST = 0.5;
const float TURNS_PER_SWEEP_SLOW = 2.0;

float knobAngle(float accumulatedDegrees, float turnsPerSweep) {
  if (accumulatedDegrees == -1.0f) return -1;
  float wrapped = fmodf(accumulatedDegrees / (turnsPerSweep * 2.0f), 360.0f);
  if (wrapped < 0) wrapped += 360.0f;
  return wrapped;
}

// -- Faust engine and its controls (identical to AudioKnobsDemo) --------

const uint32_t SAMPLE_RATE = 40000;

KnobSynth synth;

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

float *freq1Ptr;
float *freq2Ptr;
float *gain1Ptr;
float *gain2Ptr;

// -- Audio-rate interrupt (identical to AudioKnobsDemo) -----------------

FspTimer audioTimer;

void audioCallback(timer_callback_args_t *args) {
  FAUSTFLOAT sampleBuf[1];
  FAUSTFLOAT *outputs[1] = { sampleBuf };
  synth.compute(1, nullptr, outputs);

  float sample = sampleBuf[0];
  if (sample > 1.0f) sample = 1.0f;
  if (sample < -1.0f) sample = -1.0f;
  uint16_t dacValue = (uint16_t)((sample * 0.5f + 0.5f) * 4095.0f);

  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- Pots and touch pads --------------------------------------------------

RV112FF pot1(A2, A3);
RV112FF pot2(A4, A5);
MultiTurnKnob knob1(pot1);
MultiTurnKnob knob2(pot2);

CapTouch touch1(20);
CapTouch touch2(21);

// Set by BLE commands "T1:1"/"T1:0"/"T2:1"/"T2:0" (see header comment) -
// a phone holding a virtual pad works exactly like a finger on the real
// one: OR'd together below, not a separate mode.
bool bleTouch1Held = false;
bool bleTouch2Held = false;

const unsigned long CONTROL_INTERVAL = 10;  // milliseconds (~100 Hz), same as AudioKnobsDemo
unsigned long previousControlMillis = 0;

// -- BLE ------------------------------------------------------------------

CodelessPeripheralDevice ble;
bool bleConnected = false;

// Latches true once the local host (this sketch) has approved the web
// page's Binary mode request - see the header comment for the full
// handshake. Serial2 only becomes a transparent DSPS pipe after this;
// before it, everything on Serial2 is still normal CodeLess AT-command
// traffic. Reset on every disconnect so a fresh connection renegotiates
// from scratch rather than assuming stale state.
bool binaryModeActive = false;

const unsigned long BLE_TELEMETRY_INTERVAL = 200;  // milliseconds (5 Hz) -
                                                     // plenty for a readout
unsigned long previousTelemetryMillis = 0;

// Handles one line of DSPS pass-through data (see the header comment) -
// only ever called once binaryModeActive, so this is genuinely raw text
// from the web page, not AT-command traffic. Logs anything it doesn't
// recognize to Serial (USB) for live debugging.
void handleBLECommand(String line) {
  line.trim();
  if (line == "T1:1") bleTouch1Held = true;
  else if (line == "T1:0") bleTouch1Held = false;
  else if (line == "T2:1") bleTouch2Held = true;
  else if (line == "T2:0") bleTouch2Held = false;
  else if (line.length() > 0) {
    Serial.print("Unrecognized BLE command: \"");
    Serial.print(line);
    Serial.println("\"");
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("Audio + knobs + WebBLE demo.");
  Serial.println("Turn each pot to bend its oscillator's pitch; touch a pad (physically or via the web UI) for fast sweeps.");

  touch1.begin();
  touch2.begin();

  // Bring up the BLE coprocessor - same as HelloWorldBLE.
  ble.begin();
  ble.setupDefault();

  // CodelessBase never sets this, so it stays at Arduino's default
  // Stream timeout (1000ms) - meaning every internal readString() call
  // in sendCommand()/readOutput() can block for up to a full second
  // waiting for more bytes that were never coming. At 57600 baud a
  // whole reply arrives in well under a millisecond, so 20ms is a huge
  // margin while cutting the worst case ~50x. This was silently
  // stalling the knob-control loop on every BLE round-trip once real
  // traffic started flowing.
  Serial2.setTimeout(20);

  synth.init(SAMPLE_RATE);

  static CaptureUI ui;
  synth.buildUserInterface(&ui);
  freq1Ptr = ui.freq1;
  freq2Ptr = ui.freq2;
  gain1Ptr = ui.gain1;
  gain2Ptr = ui.gain2;

  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);

  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  audioTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                    (float)SAMPLE_RATE, 50.0, audioCallback, nullptr);
  audioTimer.setup_overflow_irq(1);
  audioTimer.open();
  audioTimer.start();
}

void loop() {
  unsigned long currentMillis = millis();

  // BLE housekeeping and inbound commands - checked every loop() pass
  // (not gated to CONTROL_INTERVAL) so a held virtual-pad release isn't
  // delayed by up to 10ms for no reason; ble.available() is a cheap
  // check when there's nothing waiting. ble.readOutput() is just a thin
  // Serial2 wrapper (see the header comment) - it reads the same raw
  // bytes whether they're AT-command status lines or, once
  // binaryModeActive, DSPS pass-through data.
  if (ble.available()) {
    String line = ble.readOutput();

    // Checked regardless of binaryModeActive: a disconnect needs
    // catching either way, to stop advertising being needed again and
    // to reset binaryModeActive for the next connection's handshake.
    if (line.indexOf("+DISCONNECTED") != -1) {
      bleConnected = false;
      binaryModeActive = false;
      bleTouch1Held = false;  // don't leave a virtual pad "stuck" held
      bleTouch2Held = false;  // across a dropped connection
      Serial.println("BLE central disconnected. Advertising again.");
      ble.sendCommand("AT+ADVSTART");
    } else if (!binaryModeActive) {
      if (line.indexOf("+CONNECTED") != -1) {
        bleConnected = true;
        Serial.println("BLE central connected.");
      } else if (line.indexOf("+BINREQ") != -1) {
        // The web page has asked to enter Binary mode. Confirmed live:
        // the module doesn't grant this unilaterally just because the
        // remote central requested it - it waits for exactly this local
        // approval first. See the header comment for the full sequence.
        Serial.println("BLE central requested Binary mode - approving.");
        ble.sendCommand("AT+BINREQACK");
        binaryModeActive = true;
      }
      // Other unsolicited status lines (+AWAKE, +COMMAND MODE
      // SUPPORTED, +BINARY MODE SUPPORTED, ...) are expected here and
      // otherwise ignored.
    } else {
      handleBLECommand(line);
    }
  }

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    previousControlMillis = currentMillis;

    CapTouch::scan();

    bool effectiveTouched1 = touch1.isTouched() || bleTouch1Held;
    bool effectiveTouched2 = touch2.isTouched() || bleTouch2Held;

    float turnsPerSweep1 = effectiveTouched1 ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float angle1 = knobAngle(knob1.read(), turnsPerSweep1);
    *freq1Ptr = angleToFrequency(angle1);
    *gain1Ptr = pot1.isCalibrated() ? 1.0 : 0.0;

    float turnsPerSweep2 = effectiveTouched2 ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float angle2 = knobAngle(knob2.read(), turnsPerSweep2);
    *freq2Ptr = angleToFrequency(angle2);
    *gain2Ptr = pot2.isCalibrated() ? 1.0 : 0.0;

    // Gated on binaryModeActive, not just bleConnected: before the
    // handshake completes, Serial2 is still the AT-command link, and
    // ble.sendCommand() would block waiting for an OK/ERROR reply that
    // Binary mode no longer sends at all - see the header comment.
    if (binaryModeActive && currentMillis - previousTelemetryMillis >= BLE_TELEMETRY_INTERVAL) {
      previousTelemetryMillis = currentMillis;
      String telemetry = "F1:" + String(*freq1Ptr, 1) + ",F2:" + String(*freq2Ptr, 1) +
                          ",T1:" + String(effectiveTouched1 ? 1 : 0) +
                          ",T2:" + String(effectiveTouched2 ? 1 : 0);
      Serial2.print(telemetry);
      Serial2.print("\n");
    }
  }
}
