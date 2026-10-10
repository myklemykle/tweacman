// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  DAC Audio Test - SparkFun Thing Plus RA6M5

  Alternates a 220 Hz test tone between the RA6M5's two on-chip DAC
  channels, one second at a time: A0 (left) for a second, then A1 (right),
  and so on. Good for confirming both channels actually work, and for A/B
  comparing audio circuits between them - a coupling capacitor wired to one
  channel but not the other, for instance.

  Hardware:
  - A0 (pin 7) and A1 (pin 8) are true 12-bit hardware DAC outputs on the
    RA6M5 - a real analog voltage, not a PWM approximation.
  - Wire a 3.5mm TRS jack's tip and ring to A0 and A1, and its sleeve to
    ground.
  - This DAC's raw output rides on a DC bias rather than swinging
    symmetrically around 0V the way a proper line-level signal does.
    Driven straight into a jack with nothing in between, that shows up as
    a click at the start/end of each beep. A series coupling capacitor
    (several uF) between a DAC pin and its jack conductor blocks the DC
    and passes only the audio swing - compare a channel with one wired in
    against a channel without to hear the difference.
  - The RA6M5's DAC also has an optional internal output amplifier that
    lowers its output impedance for driving an external load like this
    one. The Arduino core leaves it off by default with no exposed way to
    turn it on - see the register-level hack below for a quick A/B test.
    This sketch toggles it on and off every 4 seconds on both channels at
    once, while the L/R tone alternation keeps running - so each channel
    gets heard with the amplifier both on and off over time, on top of
    whatever else differs between them (A1 has a coupling cap, A0 doesn't).

  This example code is in the public domain.
*/

#include "analogWave.h"
#include "bsp_api.h"  // for R_DAC - see setDacOutputAmplifier() below

const float TONE_HZ = 220.0;
const unsigned long BEEP_INTERVAL = 1000;  // milliseconds per channel

analogWave left(DAC);    // A0
analogWave right(DAC1);  // A1

// --- Experimental: DAC output amplifier hack --------------------------
//
// The RA6M5's DAC really does have an internal output amplifier - Renesas'
// own driver (r_dac.c) supports enabling it - but the Arduino core
// hardcodes it off (dac.cpp: output_amplifier_enabled = 0) with no
// parameter anywhere to turn it on. setDacOutputAmplifier() reaches
// directly into the DAC peripheral's registers and flips it by hand,
// after analogWave has already opened and started the channel: the same
// enable/disable sequence Renesas' driver runs internally when you *do*
// have the option (see "Initialization Procedure with the Output
// Amplifier" in the RA6M5 hardware manual) - briefly disable the
// channel's output, arm the amplifier, give it ~4us to stabilize, then
// re-enable output. analogWave's background DTC transfer keeps writing
// new samples into the data register throughout; that's harmless here,
// since the output-enable bit gates whether any of it actually reaches
// the pin.
//
// This is a deliberate, temporary hack for a quick listening test, not
// something to build on: it pokes a register the Arduino core doesn't
// know about, and a core update could change this layout without notice.

void setDacOutputAmplifier(int channel, bool enabled) {
  if (channel == 0) {
    R_DAC->DACR_b.DAOE0 = 0;                  // briefly disable channel 0's output
    R_DAC->DAASWCR_b.DAASW0 = 1;               // arm amplifier stabilization wait
    R_DAC->DAAMPCR_b.DAAMP0 = enabled ? 1 : 0; // set the amplifier state
    R_DAC->DACR_b.DAOE0 = 1;                  // re-enable output
  } else {
    R_DAC->DACR_b.DAOE1 = 0;
    R_DAC->DAASWCR_b.DAASW1 = 1;
    R_DAC->DAAMPCR_b.DAAMP1 = enabled ? 1 : 0;
    R_DAC->DACR_b.DAOE1 = 1;
  }

  delayMicroseconds(4);  // matches DAC_CONVERSION_TIME_WITH_OUTPUT_AMPLIFIER

  if (channel == 0) {
    R_DAC->DAASWCR_b.DAASW0 = 0;  // stabilization done; new state is live
  } else {
    R_DAC->DAASWCR_b.DAASW1 = 0;
  }
}

void enableDacOutputAmplifier(int channel) { setDacOutputAmplifier(channel, true); }
void disableDacOutputAmplifier(int channel) { setDacOutputAmplifier(channel, false); }

// Both-channels-at-once versions, since this sketch drives A0 and A1
// together rather than experimenting on just one.
void setDacOutputAmplifiers(bool enabled) {
  setDacOutputAmplifier(0, enabled);
  setDacOutputAmplifier(1, enabled);
}

const unsigned long AMP_TOGGLE_INTERVAL = 4000;  // milliseconds

unsigned long previousBeepMillis = 0;
unsigned long previousAmpMillis = 0;
bool leftIsPlaying = true;
bool ampIsEnabled = true;  // matches the setDacOutputAmplifiers(true) call in setup()

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("DAC audio test: alternating 220 Hz beeps, A0 then A1.");

  // .sine() both fully sets up a channel (grabs a hardware timer and a DTC
  // transfer to stream samples to the DAC in the background) and starts it
  // playing immediately - do that once per channel, here, then control
  // on/off from loop() with start()/stop(). Calling .sine() again later
  // would grab a fresh timer each time without releasing the last one, and
  // the RA6M5 only has a handful to go around.
  left.sine(TONE_HZ);
  right.sine(TONE_HZ);
  right.stop();  // begin with only the left channel sounding

  setDacOutputAmplifiers(true);
  Serial.println("Output amplifier hack-enabled on both channels.");

  previousBeepMillis = millis();
  previousAmpMillis = millis();
}

void loop() {
  // Same non-blocking pattern as HelloWorld's blink: compare the clock
  // instead of using delay(), so nothing else in loop() would ever stall.
  // Two independent timers run side by side here - which channel is
  // sounding, and whether the amplifier hack is currently on - so a
  // channel's tone gets heard with the amplifier in both states over time
  // without needing to reflash.
  unsigned long currentMillis = millis();

  if (currentMillis - previousBeepMillis >= BEEP_INTERVAL) {
    previousBeepMillis = currentMillis;

    leftIsPlaying = !leftIsPlaying;

    if (leftIsPlaying) {
      right.stop();
      left.start();
      Serial.println("left (A0)");
    } else {
      left.stop();
      right.start();
      Serial.println("right (A1)");
    }
  }

  if (currentMillis - previousAmpMillis >= AMP_TOGGLE_INTERVAL) {
    previousAmpMillis = currentMillis;

    ampIsEnabled = !ampIsEnabled;

    setDacOutputAmplifiers(ampIsEnabled);
    Serial.println(ampIsEnabled ? "amplifiers ON" : "amplifiers OFF");
  }
}
