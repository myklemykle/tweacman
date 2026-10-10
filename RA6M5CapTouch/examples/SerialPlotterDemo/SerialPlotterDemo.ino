// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  SerialPlotterDemo - RA6M5CapTouch library

  Reads two touch-capable pins and streams their raw graduated reading,
  tracked baseline, and boolean touched state to the Arduino Serial
  Plotter (Tools > Serial Plotter in the Arduino IDE - arduino-cli's
  `monitor` command shows the same raw text but won't draw the graph).

  Wiring (SparkFun Thing Plus RA6M5):
    D6  -> a ~10nF capacitor to GND (TSCAP - required for the whole
           CTSU peripheral to work at all; not specific to either pin
           below)
    D20 -> a wire or touch pad (TS05)
    D21 -> a wire or touch pad (TS04)

  The two touched1/touched2 traces are plotted at the minimum and
  maximum values a raw reading can take (0 and 65535) rather than a
  plain 0/1, so they show up as clear top/bottom rails on the plot
  instead of a barely-visible blip near the raw traces.

  Watch baseline1/baseline2 alongside raw1/raw2 to see CapTouch's
  automatic baseline tracking in action - baseline slowly follows raw
  when nothing's touching (temperature, humidity, or swapping in a
  different object entirely - a piece of produce reads very
  differently than a finger), then holds still during an actual touch
  so the touch itself doesn't get absorbed into it. See the class
  comment in RA6M5CapTouch.h for the full explanation, including what
  happens if something ends up resting against the sensor indefinitely
  instead of a real touch-and-hold.

  CapTouch's default touch threshold is just a starting point - watch
  raw1/raw2 against baseline1/baseline2 here and adjust with
  setThreshold() if isTouched() doesn't track what you'd expect.
*/

#include <RA6M5CapTouch.h>

CapTouch wire1(20);
CapTouch wire2(21);

const unsigned long PLOT_INTERVAL = 20;  // milliseconds (~50 Hz)
unsigned long previousMillis = 0;

void setup() {
  Serial.begin(115200);
  wire1.begin();
  wire2.begin();
}

void loop() {
  // Same non-blocking pattern used throughout this project: compare the
  // clock instead of delay().
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= PLOT_INTERVAL) {
    previousMillis = currentMillis;

    CapTouch::scan();

    // Serial Plotter format: comma-separated "label:value" pairs, one
    // line per sample.
    Serial.print("raw1:");
    Serial.print(wire1.readRaw());
    Serial.print(",baseline1:");
    Serial.print(wire1.readBaseline());
    Serial.print(",touched1:");
    Serial.print(wire1.isTouched() ? 65535 : 0);
    Serial.print(",raw2:");
    Serial.print(wire2.readRaw());
    Serial.print(",baseline2:");
    Serial.print(wire2.readBaseline());
    Serial.print(",touched2:");
    Serial.println(wire2.isTouched() ? 65535 : 0);
  }
}
