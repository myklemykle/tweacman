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
    one. This sketch leaves it off - the Arduino core's default - as a
    baseline to compare against later.

  This example code is in the public domain.
*/

#include "analogWave.h"

const float TONE_HZ = 220.0;
const unsigned long BEEP_INTERVAL = 1000;  // milliseconds per channel

analogWave left(DAC);    // A0
analogWave right(DAC1);  // A1

unsigned long previousMillis = 0;
bool leftIsPlaying = true;

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

  previousMillis = millis();
}

void loop() {
  // Same non-blocking pattern as HelloWorld's blink: compare the clock
  // instead of using delay(), so nothing else in loop() would ever stall.
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= BEEP_INTERVAL) {
    previousMillis = currentMillis;

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
}
