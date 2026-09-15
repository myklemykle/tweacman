/*
  SerialPlotterDemo - CapTouch library

  Reads two touch-capable pins and streams both their boolean touched
  state and raw graduated reading to the Arduino Serial Plotter (Tools >
  Serial Plotter in the Arduino IDE - arduino-cli's `monitor` command
  shows the same raw text but won't draw the graph).

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

  CapTouch's default touch threshold is just a starting point - watch
  raw1/raw2 here with nothing touching each wire versus actually
  touching it, and adjust with setThreshold() if isTouched() doesn't
  track what you'd expect.

  This example code is in the public domain.
*/

#include <CapTouch.h>

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
    Serial.print(",touched1:");
    Serial.print(wire1.isTouched() ? 65535 : 0);
    Serial.print(",raw2:");
    Serial.print(wire2.readRaw());
    Serial.print(",touched2:");
    Serial.println(wire2.isTouched() ? 65535 : 0);
  }
}
