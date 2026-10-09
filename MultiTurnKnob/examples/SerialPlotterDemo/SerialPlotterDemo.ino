// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  SerialPlotterDemo - MultiTurnKnob library

  Wraps one RV112FF-40 pot in a MultiTurnKnob and streams both its raw
  single-turn angle and the unbounded accumulated position to the Arduino
  Serial Plotter (Tools > Serial Plotter in the Arduino IDE - arduino-
  cli's `monitor` command shows the same raw text but won't draw the
  graph).

  Wiring (SparkFun Thing Plus RA6M5): pot wipers -> A2, A3; both track
  ends -> 3.3V and GND (same as RV112FF's own SerialPlotterDemo).

  Spin the knob past a full turn in either direction and watch
  accumulated keep counting past +-360, while angle keeps resetting to
  [0, 360) every revolution. Press USR_BTN to call write(0) and watch
  accumulated jump to wherever the shaft physically is right now, minus
  nothing - i.e. "here" becomes the new zero.

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>

RV112FF pot(A2, A3);
MultiTurnKnob knob(pot);

const unsigned long PLOT_INTERVAL = 20;  // milliseconds (~50 Hz)
unsigned long previousMillis = 0;

bool lastButtonState = HIGH;  // USR_BTN is active-low (INPUT_PULLUP)

void setup() {
  Serial.begin(115200);
  pinMode(USR_BTN, INPUT_PULLUP);
}

void loop() {
  // Same non-blocking pattern used throughout this project: compare the
  // clock instead of delay().
  unsigned long currentMillis = millis();

  bool buttonState = digitalRead(USR_BTN);
  if (buttonState == LOW && lastButtonState == HIGH) {
    knob.write(0);
  }
  lastButtonState = buttonState;

  if (currentMillis - previousMillis >= PLOT_INTERVAL) {
    previousMillis = currentMillis;

    // knob.read() is what actually samples the wrapped pot - see
    // MultiTurnKnob.h - so pot.read() is never called directly here;
    // lastAngle() reports what that same read() just saw the pot's own
    // single-turn angle as, without sampling it again.
    float accumulated = knob.read();
    float angle = knob.lastAngle();

    // Serial Plotter format: comma-separated "label:value" pairs, one
    // line per sample.
    Serial.print("angle:");
    Serial.print(angle);
    Serial.print(",accumulated:");
    Serial.println(accumulated);
  }
}
