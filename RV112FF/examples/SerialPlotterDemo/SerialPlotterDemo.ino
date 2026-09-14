/*
  SerialPlotterDemo - RV112FF library

  Reads two RV112FF-40 dual potentiometers and streams their shaft
  angles to the Arduino Serial Plotter (Tools > Serial Plotter in the
  Arduino IDE - arduino-cli's `monitor` command shows the same raw text
  but won't draw the graph).

  Wiring (SparkFun Thing Plus RA6M5):
    Pot 1 wipers -> A2, A3
    Pot 2 wipers -> A4, A5
    Both pots' track ends -> 3.3V and GND

  Each angle briefly reads -1 right after power-on - that's the pot's
  self-calibration warm-up (see RV112FF.h); it clears after roughly one
  full turn of that shaft.

  This example code is in the public domain.
*/

#include <RV112FF.h>

RV112FF pot1(A2, A3);
RV112FF pot2(A4, A5);

const unsigned long PLOT_INTERVAL = 20;  // milliseconds (~50 Hz)
unsigned long previousMillis = 0;

void setup() {
  Serial.begin(115200);
}

void loop() {
  // Same non-blocking pattern used throughout this project: compare the
  // clock instead of delay(), so nothing here would ever stall - the
  // pots themselves are polled by read() at whatever rate loop() gets
  // to it, not on a hardware timer.
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= PLOT_INTERVAL) {
    previousMillis = currentMillis;

    float angle1 = pot1.read();
    float angle2 = pot2.read();

    // Serial Plotter format: comma-separated "label:value" pairs, one
    // line per sample.
    Serial.print("pot1:");
    Serial.print(angle1);
    Serial.print(",pot2:");
    Serial.println(angle2);
  }
}
