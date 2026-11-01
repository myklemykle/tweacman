// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  CapTouchPlotter - RA6M5CapTouch library

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

  Scanning without waiting: rather than calling CapTouch::scan(), which
  waits for the hardware, this sketch starts a scan and then calls
  CapTouch::update() on every pass through loop() until it's done - the
  pattern to use in a sketch with other things to do. It scans as fast
  as the hardware allows (about 2500 times a second for two pins) while
  still printing only 50 lines a second, which leaves room to average.

  Two settings to try, by typing into the Serial Plotter's message box
  (or the Serial Monitor) and pressing Enter:
    a   averaging on/off: plot the average of every scan since the last
        line (around 45 of them), rather than just the latest one.
        Watch the raw traces' fuzz shrink - though by less than you
        might expect, as some of the noise is too slow to average out
        in 20ms.
    +   one more hardware repeat per measurement (setMeasurementCount()),
    -   one fewer (1-64, starting at 1). Each repeat adds the pin's
        reading again, so raw and baseline climb in steps, the threshold
        is scaled to match, and noise relative to the reading falls - at
        the cost of a slower scan. With nothing subtracting each pin's
        background reading, the 16-bit counter tops out after only a
        few repeats: a flat line at 65535 means it overflowed.
*/

#include <RA6M5CapTouch.h>

CapTouch wire1(20);
CapTouch wire2(21);

const uint16_t THRESHOLD_PER_REPEAT = 3000;  // the library's default, per repeat

const unsigned long PLOT_INTERVAL = 20;  // milliseconds (~50 Hz)
unsigned long previousMillis = 0;

bool averaging = false;

// Every scan's readings since the last printed line, for averaging.
uint32_t sum1 = 0, sum2 = 0;
uint16_t scansSincePrint = 0;

void setup() {
  Serial.begin(115200);
  wire1.begin();
  wire2.begin();
  CapTouch::startScan();
}

void setMeasurementCount(int count) {
  if (count < 1 || count > 64) return;
  CapTouch::setMeasurementCount(count);
  wire1.setThreshold(THRESHOLD_PER_REPEAT * count);
  wire2.setThreshold(THRESHOLD_PER_REPEAT * count);
}

void loop() {
  // Keep the scan moving; whenever one finishes, add it up and start
  // the next.
  if (CapTouch::update()) {
    sum1 += wire1.readRaw();
    sum2 += wire2.readRaw();
    scansSincePrint++;
    CapTouch::startScan();
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'a') averaging = !averaging;
    if (c == '+') setMeasurementCount(CapTouch::getMeasurementCount() + 1);
    if (c == '-') setMeasurementCount(CapTouch::getMeasurementCount() - 1);
  }

  // Same non-blocking pattern used throughout this project: compare the
  // clock instead of delay().
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= PLOT_INTERVAL && scansSincePrint > 0) {
    previousMillis = currentMillis;

    uint16_t raw1 = averaging ? sum1 / scansSincePrint : wire1.readRaw();
    uint16_t raw2 = averaging ? sum2 / scansSincePrint : wire2.readRaw();
    sum1 = sum2 = 0;
    scansSincePrint = 0;

    // Serial Plotter format: comma-separated "label:value" pairs, one
    // line per sample.
    Serial.print("raw1:");
    Serial.print(raw1);
    Serial.print(",baseline1:");
    Serial.print(wire1.readBaseline());
    Serial.print(",touched1:");
    Serial.print(wire1.isTouched() ? 65535 : 0);
    Serial.print(",raw2:");
    Serial.print(raw2);
    Serial.print(",baseline2:");
    Serial.print(wire2.readBaseline());
    Serial.print(",touched2:");
    Serial.println(wire2.isTouched() ? 65535 : 0);
  }
}
