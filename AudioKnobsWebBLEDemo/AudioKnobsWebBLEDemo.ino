// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

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

  Entering Binary mode is a three-party handshake - web page, module,
  and this sketch - and every step below was established by tracing raw
  bytes on Serial2 with BLE_TRACE while driving the link from a laptop
  (see tools/ble_binary_test.py, which replays exactly what the web page
  does but with a readable log). None of it is documented upstream in
  the form you need, and two of the steps are actively counter-intuitive.

    1. Web page writes "AT+BINREQ" to the Inbound Command characteristic
       (866d3b04-.../914f8fb9-...-5425) - the normal AT-command link,
       still used only for this one negotiation step. It must do this
       AFTER subscribing to the DSPS notifications and writing 0x01 to
       DSPS flow control, not before.
    2. The module announces this to the local host as an unsolicited
       "+BINREQ" line on Serial2 - see handleBLELine() below, which
       approves it with ble.sendCommand("AT+BINREQACK").
    3. That approval is answered with "OK" - but not when the module
       receives it. The "OK" is deferred until the mode transition
       actually completes, which is at disconnect (step 4), so it can
       arrive anywhere from a few hundred milliseconds to many seconds
       later depending on how long the central stays connected. Measured:
       481ms when the page hung up immediately, 14.3s when a test client
       sat connected first. sendCommand() gives up after two seconds, so
       usually all it returns is the echo. That is not a failure, and
       lengthening the timeout is the wrong fix - sendCommand() blocks,
       and waiting seconds here would stall the knob and touch loop.
    4. The module then relays "AT+BINREQACK" to the web page on the AT
       Outbound characteristic - and carries on in command mode for the
       rest of this connection. The switch to Binary mode only takes
       effect when the connection ENDS. So the first connection after a
       request never carries data, and the web page deliberately hangs
       up and reconnects at this point to pick the pipe up.

  Two consequences worth knowing before changing any of this:

    - Once the module IS in Binary mode it stops emitting status lines
      altogether: no +CONNECTED, no +BINREQ, no +DISCONNECTED, because
      the CodeLess parser is bypassed. Waiting for an announcement to
      confirm the mode is waiting for something that cannot arrive.
      What the module does offer is an echo - in command mode it echoes
      every byte received on the UART ("ATR\r" comes back as
      "ATR\r\r\n\r\n+READY\r\n"), and in Binary mode it does not. That
      echo is the only honest local test of which mode is in force, and
      it is what BleLinkMode's LINK_PENDING probe uses.

    - Binary mode survives a reset of THIS chip. The DA14531 is a
      separate coprocessor; pressing RST or re-flashing the RA6M5 does
      not touch it. A sketch that boots into a module left in Binary
      mode will have its "ATR"/"AT+ADVSTOP"/"AT+ADVSTART" swallowed as
      raw pipe data - startup appears to succeed, nothing works, and no
      amount of restarting either side helps. bleExitBinaryMode() runs
      first for exactly this reason.

  From there:
    Outbound (this board -> phone), every BLE_TELEMETRY_INTERVAL, once
      the link is confirmed binary: a plain line, no prefix -
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
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <RA6M5AnalogTuning.h>
#include <RA6M5CapTouch.h>
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

// What the Serial2 link is actually doing right now.
//
// Approving the web page's Binary mode request does NOT switch the module
// over there and then - measured on the wire, the module stays in command
// mode for the rest of that connection and only switches when the connection
// ends. So approval means "pending", not "active". (The module does answer
// AT+BINREQACK with "OK", but it holds that reply back until the transition
// completes at disconnect, so sendCommand() has normally timed out long
// before it arrives - see the header comment.)
//
// Worse, there is no status line to wait for: once the module IS in Binary
// mode it stops emitting +CONNECTED, +BINREQ and +DISCONNECTED entirely,
// because the CodeLess parser is bypassed. Waiting for an announcement that
// can never arrive is exactly how this sketch used to deadlock.
//
// What the module does give us is an echo. In command mode it echoes every
// byte it receives on the UART (visible in any AT exchange: "ATR\r" comes
// back as "ATR\r\r\n\r\n+READY\r\n"). In Binary mode it does not, because the
// bytes leave over the DSPS pipe instead. That makes a single telemetry line
// a perfectly good probe: echoed means command mode, silence means the pipe
// is open. LINK_PENDING probes on a slow timer until one comes back unechoed.
enum BleLinkMode {
  LINK_COMMAND,  // normal AT-command link; telemetry would just bounce
  LINK_PENDING,  // we approved Binary mode; probing to find out if it took
  LINK_BINARY    // confirmed transparent: telemetry flows, no echo
};
BleLinkMode linkMode = LINK_COMMAND;

// Probe pacing while LINK_PENDING. The probe is a real telemetry line, so
// nothing is wasted if the link turns out to be live; 2s is slow enough to
// stay quiet in the common case where the page never asks for Binary mode.
const unsigned long BINARY_PROBE_INTERVAL = 2000;
// An echo comes back in ~26ms at 57600 baud, so 300ms is a wide margin.
const unsigned long BINARY_PROBE_TIMEOUT = 300;
unsigned long probeSentMillis = 0;
bool awaitingProbeEcho = false;

const unsigned long BLE_TELEMETRY_INTERVAL = 200;  // milliseconds (5 Hz) -
                                                     // plenty for a readout
unsigned long previousTelemetryMillis = 0;

// -- Serial2 wire trace ---------------------------------------------------

// Set to 0 to compile all of this out. When 1, every byte crossing Serial2
// in either direction is mirrored to the USB serial monitor, timestamped,
// with control characters escaped - so the handshake with the DA14531 can be
// read as a conversation rather than guessed at. This is the same technique
// that turned up the local-host-must-approve behaviour of Binary mode in the
// first place (see the header comment); it lives in the sketch permanently
// because that handshake is undocumented enough to need re-checking whenever
// anything about the link changes.
#define BLE_TRACE 0

#if BLE_TRACE
// Prints one direction of traffic as printable text: "\r", "\n" and "\xNN"
// for anything non-printable, so an invisible stray byte (a NUL terminator, a
// high-bit character from a half-configured UART) shows up instead of
// silently vanishing into the monitor.
void bleTrace(const char *arrow, const String &bytes) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("ms] ");
  Serial.print(arrow);
  Serial.print(" \"");
  for (unsigned int i = 0; i < bytes.length(); i++) {
    char c = bytes[i];
    if (c == '\r') Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c >= 32 && c <= 126) Serial.print(c);
    else {
      Serial.print("\\x");
      if ((uint8_t)c < 0x10) Serial.print('0');
      Serial.print((uint8_t)c, HEX);
    }
  }
  Serial.print("\" (");
  Serial.print(bytes.length());
  Serial.println(" bytes)");
}
#else
#define bleTrace(arrow, bytes) ((void)0)
#endif

// Traced wrappers, so nothing reaches Serial2 without being logged first.
// ble.sendCommand() blocks until the module answers "OK"/"ERROR"/"+READY" or
// its 2-second timeout expires, and until now that reply was thrown away at
// every call site - including the one that matters most, the Binary mode
// approval below.
String bleCommand(const String &cmd) {
  bleTrace("host ->", cmd + "\r");
  String reply = ble.sendCommand(cmd);
  bleTrace("mod  ->", reply);
  return reply;
}

void bleWriteRaw(const String &data) {
  bleTrace("host ->", data);
  Serial2.print(data);
}

// Last telemetry line handed to Serial2, kept only so the loop can recognise
// its own words coming back at it (see loop()).
String lastTelemetrySent = "";

// Gets the module out of a leftover Binary mode session.
//
// The DA14531 is a separate coprocessor with its own state: pressing RST on
// the RA6M5, or re-flashing it, does not reset the module. If a previous run
// left the module in Binary mode - which it will, since nothing ever exits it
// cleanly - then its CodeLess AT parser is bypassed and every command this
// sketch sends at startup ("ATR", "AT+ADVSTOP", "AT+ADVSTART") is not parsed
// at all: it is pushed into the DSPS buffers as raw data. Startup appears to
// succeed, no reply is checked, and the sketch then spends the whole session
// talking to a pipe instead of a command interpreter. Observed directly: a
// central connected and the module emitted no "+CONNECTED" on Serial2 at all.
//
// The way out is the escape sequence from UM-140: a guard time of silence,
// the three characters "+++" with NO carriage return appended, another guard
// time, then "AT+BINREQEXIT" as a normal AT command. The guard times are what
// distinguish an escape sequence from three plus signs that happen to appear
// in the data, so they are not optional and not shortenable.
//
// Harmless when the module is already in Command mode: the escape does
// nothing and AT+BINREQEXIT simply answers ERROR, which is why this runs
// unconditionally at startup rather than trying to detect the mode first -
// there is no way to ask the module which mode it is in.
const unsigned long BINARY_ESCAPE_GUARD_MS = 1100;

void bleExitBinaryMode() {
  Serial.println("Clearing any leftover Binary mode on the module...");

  delay(BINARY_ESCAPE_GUARD_MS);   // guard time before
  bleTrace("host ->", "+++");
  Serial2.print("+++");            // no "\r" - that would make it data, not an escape
  delay(BINARY_ESCAPE_GUARD_MS);   // guard time after

  String reply = bleCommand("AT+BINREQEXIT");
  if (reply.indexOf("ERROR") != -1) {
    Serial.println("  ...module was already in Command mode (AT+BINREQEXIT -> ERROR). Fine.");
  } else if (reply.length() == 0) {
    Serial.println("  !! No reply to AT+BINREQEXIT - the module may still be wedged.");
  } else {
    Serial.println("  ...module returned to Command mode.");
  }
}

// Handles one line of DSPS pass-through data (see the header comment) -
// only ever called once linkMode is LINK_BINARY, so this is genuinely raw text
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

// Handles one complete line pulled out of a Serial2 burst by loop(). Split
// out of loop() so each line of a multi-line burst gets the same treatment -
// see the comment at the call site.
void handleBLELine(const String &line) {
  // The echo test comes first, because it is the one measurement that is
  // valid in every state - see BleLinkMode. Our own telemetry coming back
  // verbatim means the module is in command mode, whatever this sketch
  // currently believes.
  if (line == lastTelemetrySent && lastTelemetrySent.length() > 0) {
    awaitingProbeEcho = false;
    if (linkMode != LINK_PENDING) {
      Serial.println("Telemetry echoed back - module is in command mode. Probing.");
      linkMode = LINK_PENDING;
    }
    return;
  }

  // Checked in every mode, not just the command-mode branch: the module
  // asks to leave Binary mode by sending "+++" on the UART and retrying
  // every two seconds until the host answers, and an unanswered request
  // just repeats forever. Confirmed live - it arrived while the link was
  // believed binary, where it was being handed to the application as an
  // unrecognised command.
  //
  // Per UM-140 the acknowledging side replies with any AT command to
  // confirm it is back in Command mode, optionally then AT+BINREQEXITACK.
  // Dropping to LINK_PENDING rather than LINK_COMMAND is deliberate: if
  // this really was the module leaving Binary mode, the probe will find the
  // echo and stay put, and if it was not, the probe promotes and the link
  // recovers on its own. LINK_COMMAND would instead sit waiting for a
  // +BINREQ that Binary mode can never send.
  if (line == "+++") {
    Serial.println("Module requested Binary mode exit - acknowledging.");
    bleWriteRaw("AT\r");
    bleWriteRaw("AT+BINREQEXITACK\r");
    linkMode = LINK_PENDING;
    probeSentMillis = 0;
    awaitingProbeEcho = false;
    return;
  }

  // Checked regardless of mode: a disconnect needs catching either way, to
  // start advertising again and to reset state for the next handshake. (Only
  // ever seen while the module is still in command mode - once it is in
  // Binary mode it stops announcing anything at all.)
  if (line.indexOf("+DISCONNECTED") != -1) {
    bleConnected = false;
    linkMode = LINK_COMMAND;
    awaitingProbeEcho = false;
    lastTelemetrySent = "";
    bleTouch1Held = false;  // don't leave a virtual pad "stuck" held
    bleTouch2Held = false;  // across a dropped connection
    Serial.println("BLE central disconnected. Advertising again.");
    bleCommand("AT+ADVSTART");
  } else if (linkMode != LINK_BINARY) {
    if (line.indexOf("+CONNECTED") != -1) {
      bleConnected = true;
      Serial.println("BLE central connected.");
    } else if (line.indexOf("+BINREQ") != -1) {
      // The web page has asked to enter Binary mode - see the header comment
      // for the full sequence.
      //
      // Deliberately left as a permissive indexOf() for now, because that is
      // exactly the open question: it matches the central's own command text
      // "AT+BINREQ" (if the module relays inbound AT commands to the local
      // UART) just as readily as a genuine unsolicited "+BINREQ" status line.
      // Those two mean opposite things about whose turn it is to speak, and
      // approving the wrong one earns a bare ERROR. The trace below prints
      // the exact line that matched, and the module's exact reply, so the
      // next run settles it instead of guessing.
      Serial.print("Binary mode request matched on line: \"");
      Serial.print(line);
      Serial.println("\"");
      String reply = bleCommand("AT+BINREQACK");
      if (reply.indexOf("ERROR") != -1) {
        Serial.println("!! Module answered ERROR to AT+BINREQACK - Binary mode was NOT granted.");
      }
      // Getting only an echo back is the normal, expected outcome here: the
      // module holds its "OK" until the mode transition completes at
      // disconnect, which is usually long after sendCommand()'s two-second
      // timeout. So it is not treated as a failure - it just means
      // "pending", which is what the probe below is for. The late "OK"
      // turns up later as an ordinary unsolicited status line.
      linkMode = LINK_PENDING;
      probeSentMillis = 0;
      awaitingProbeEcho = false;
    } else {
      // +AWAKE, +COMMAND MODE SUPPORTED, +BINARY MODE SUPPORTED and friends.
      // Printed rather than dropped: which of them appear, and in what order,
      // is part of what the trace is here to establish.
      Serial.print("BLE status line (ignored): \"");
      Serial.print(line);
      Serial.println("\"");
    }
  } else {
    handleBLECommand(line);
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

  // Bring up the BLE coprocessor - same as HelloWorldBLE.
  ble.begin();

  // CodelessBase never sets this, so it stays at Arduino's default Stream
  // timeout (1000ms) - meaning every internal readString() call in
  // sendCommand()/readOutput() can block for up to a full second waiting for
  // more bytes that were never coming. At 57600 baud a whole reply arrives in
  // well under a millisecond, so 20ms is a huge margin while cutting the
  // worst case ~50x. Set before setupDefault() rather than after it, because
  // every AT round-trip below now depends on it - including the Binary mode
  // recovery.
  Serial2.setTimeout(20);

  // Before any AT command can mean anything, make sure the module is actually
  // listening for AT commands - see bleExitBinaryMode().
  bleExitBinaryMode();

  // Anything the module echoed or complained about above is still sitting in
  // the receive buffer; drop it so the first real command below starts from a
  // clean slate rather than parsing the tail of the previous exchange.
  while (Serial2.available()) Serial2.read();

  // This is what CodelessPeripheralDevice::setupDefault() does, written out
  // so each step goes through bleCommand() and lands in the trace. The
  // library version issues exactly these three and discards every reply, so
  // when one of them fails there is nothing to see - which is precisely the
  // situation this sketch kept ending up in.
  bleCommand("ATR");
  bleCommand("AT+ADVSTOP");
  bleCommand("AT+ADVSTART");

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
  // once the link is binary, DSPS pass-through data.
  if (ble.available()) {
    String burst = ble.readOutput();
    bleTrace("mod  ->", burst);

    // readOutput() is Serial2.readString(): it hands back everything that
    // arrived in one burst, which at 57600 baud is routinely several
    // \r\n-terminated status lines at once. Running indexOf() over the whole
    // burst and acting on the first match - as this did - silently discards
    // the rest, so a "+CONNECTED\r\n+BINREQ\r\n" pair loses the +BINREQ
    // entirely and the handshake stalls with nothing logged. Split the burst
    // into lines first, then dispatch each one.
    int start = 0;
    while (start < (int)burst.length()) {
      int end = start;
      while (end < (int)burst.length() && burst[end] != '\r' && burst[end] != '\n') end++;
      String line = burst.substring(start, end);
      start = end + 1;
      line.trim();
      if (line.length() > 0) handleBLELine(line);
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

    // Gated on linkMode, not on bleConnected: while the module is still on
    // the AT-command link, a telemetry line is not data, it is nonsense fed
    // to a command parser, and it comes straight back as an echo.
    //
    // LINK_BINARY sends at the full rate. LINK_PENDING sends the same line,
    // just rarely, as a probe: if it is echoed the link is still in command
    // mode (handled in handleBLELine), and if nothing comes back within
    // BINARY_PROBE_TIMEOUT the pipe is open and the link is promoted.
    unsigned long telemetryInterval =
        (linkMode == LINK_BINARY) ? BLE_TELEMETRY_INTERVAL : BINARY_PROBE_INTERVAL;

    if (linkMode == LINK_PENDING && awaitingProbeEcho &&
        currentMillis - probeSentMillis >= BINARY_PROBE_TIMEOUT) {
      awaitingProbeEcho = false;
      linkMode = LINK_BINARY;
      Serial.println("Probe went unechoed - Binary mode is live. Streaming telemetry.");
    }

    if (linkMode != LINK_COMMAND &&
        currentMillis - previousTelemetryMillis >= telemetryInterval) {
      previousTelemetryMillis = currentMillis;
      String telemetry = "F1:" + String(*freq1Ptr, 1) + ",F2:" + String(*freq2Ptr, 1) +
                          ",T1:" + String(effectiveTouched1 ? 1 : 0) +
                          ",T2:" + String(effectiveTouched2 ? 1 : 0);
      lastTelemetrySent = telemetry;
      if (linkMode == LINK_PENDING) {
        probeSentMillis = currentMillis;
        awaitingProbeEcho = true;
      }
      bleWriteRaw(telemetry + "\n");
    }
  }
}
