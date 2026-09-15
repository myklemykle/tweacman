/*
  AudioKnobsWebBLEDemo - SparkFun Thing Plus RA6M5

  AudioKnobsDemo (two RV112FF-40 pots each bending a Faust oscillator's
  pitch, touch-gated fast/slow speed via CapTouch and MultiTurnKnob -
  see that sketch's own header comment for all of that, unchanged here)
  plus a wireless remote control surface: a companion web page, talking
  to this board over the onboard DA14531MOD's "CodeLess" BLE link (see
  HelloWorldBLE.ino), can hold either touch pad "virtually" from across
  the room and see live freq1/freq2/touched1/touched2 readout.

  Why a companion app instead of Safari directly: iOS has no Web
  Bluetooth support in any browser (Apple requires every iOS browser to
  use WebKit, which doesn't implement it) - a third-party app that
  ships its own Bluetooth stack (Bluefy, WebBLE) is required to run a
  Web Bluetooth page on an iPhone at all.

  Wiring: identical to AudioKnobsDemo - see its header comment.

  Protocol, built on exactly the same CodeLess GATT service and pipe
  convention HelloWorldBLE already uses (service
  866d3b04-e674-40dc-9c05-b7f91bec6e83; Inbound Command
  914f8fb9-...-5425 to write to; Outbound Command 3bb535aa-...-a404 to
  read from Flow Control's notify "doorbell", e2048b39-...-5639):

    Outbound (this board -> phone), every BLE_TELEMETRY_INTERVAL:
      a "|"-prefixed line: "F1:<hz>,F2:<hz>,T1:<0|1>,T2:<0|1>\n" - T1/T2
      reflect the *effective* touched state (physical pad OR held BLE
      button), so the phone's UI can show which is actually driving the
      sound regardless of which one caused it.

    Inbound (phone -> this board), read off ble.available()/
    readOutput() same as HelloWorldBLE already reads the module's own
    +CONNECTED/+DISCONNECTED status lines: plain text commands "T1:1",
    "T1:0", "T2:1", "T2:0" - press-and-hold semantics matching the
    physical pads, not a toggle.

  Genuinely untested going in: HelloWorldBLE only ever pushed data out
  over this link - nothing in this project has had the phone write
  application data back yet. Renesas' own CodeLess documentation
  confirms the "|" pipe convention for outbound data but doesn't
  clearly document what happens to arbitrary inbound writes (parsed as
  an AT command and rejected, or passed through) - worth knowing before
  reading further into this sketch's inbound handling below, in case
  it needs a "AT+BINREQ" binary-mode fallback that isn't here yet.

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

const unsigned long BLE_TELEMETRY_INTERVAL = 200;  // milliseconds (5 Hz) -
                                                     // plenty for a readout,
                                                     // and keeps sendCommand()'s
                                                     // brief local-UART block
                                                     // off the 100Hz knob loop
unsigned long previousTelemetryMillis = 0;

// Handles one line already read off the BLE link that wasn't a CodeLess
// module status message - see loop() below. Tolerates a leading "|" in
// case the module passes that through on the inbound side the same way
// it's used outbound (untested - see header comment); logs anything it
// doesn't recognize to Serial (USB), which is exactly what to watch
// during the first live test from the phone.
void handleBLECommand(String line) {
  line.trim();
  if (line.startsWith("|")) line = line.substring(1);

  if (line == "T1:1") bleTouch1Held = true;
  else if (line == "T1:0") bleTouch1Held = false;
  else if (line == "T2:1") bleTouch2Held = true;
  else if (line == "T2:0") bleTouch2Held = false;
  else {
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
  // check when there's nothing waiting.
  if (ble.available()) {
    String line = ble.readOutput();
    if (line.indexOf("+CONNECTED") != -1) {
      bleConnected = true;
      Serial.println("BLE central connected.");
    } else if (line.indexOf("+DISCONNECTED") != -1) {
      bleConnected = false;
      bleTouch1Held = false;  // don't leave a virtual pad "stuck" held
      bleTouch2Held = false;  // across a dropped connection
      Serial.println("BLE central disconnected. Advertising again.");
      ble.sendCommand("AT+ADVSTART");
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

    if (bleConnected && currentMillis - previousTelemetryMillis >= BLE_TELEMETRY_INTERVAL) {
      previousTelemetryMillis = currentMillis;
      String telemetry = "|F1:" + String(*freq1Ptr, 1) + ",F2:" + String(*freq2Ptr, 1) +
                          ",T1:" + String(effectiveTouched1 ? 1 : 0) +
                          ",T2:" + String(effectiveTouched2 ? 1 : 0);
      ble.sendCommand(telemetry);
    }
  }
}
