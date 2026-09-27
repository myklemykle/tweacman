/*
  WS2812TouchDemo - SparkFun Thing Plus RA6M5

  Drives the board's onboard WS2812 RGB LED (D13, aka P105 - see this
  project's CLAUDE.md) with FastLED, and layers its color with two
  CapTouch pads: green breathes on its own the whole time (so you can
  tell the sketch is alive without touching anything), and touching
  each pad blends in another color on top of it, in time with the same
  pulse - touch1 adds red, touch2 adds blue. Touch both at once and you
  get white-ish. All three channels share one running 2-second pulse
  clock rather than each having their own, so a color blending in or
  out mid-breath doesn't look like two unrelated rhythms colliding.

  Wiring: D6 to a ~10nF capacitor to GND (TSCAP, shared by both pads -
  see RA6M5CapTouch.h), D20 (touch1) and D21 (touch2) each to their own wire
  or touch pad - same as CapTouch's own SerialPlotterDemo. The RGB LED
  itself needs no wiring; it's onboard.

  This example code is in the public domain.
*/

#include <FastLED.h>
#include <RA6M5CapTouch.h>

// -- LED ----------------------------------------------------------------

const uint8_t LED_PIN = 13;  // D13 / P105, the board's onboard WS2812
const uint8_t NUM_LEDS = 1;
CRGB leds[NUM_LEDS];

const unsigned long PULSE_PERIOD_MS = 2000;  // one full dim-bright-dim breath

// One shared brightness envelope every active color channel rides on -
// see the header comment on why this isn't computed per-channel.
// (1 - cos(2*pi*t))/2 is 0 at t=0, rises smoothly to 1 at t=0.5, and
// smoothly back to 0 at t=1 - a standard "breathing" shape, gentler on
// the eyes than a linear ramp because it eases in and out at both ends
// instead of snapping direction at the peak.
uint8_t pulseBrightness() {
  float t = (millis() % PULSE_PERIOD_MS) / (float)PULSE_PERIOD_MS;  // 0..1
  float envelope = (1.0f - cosf(2.0f * PI * t)) / 2.0f;             // 0..1
  return (uint8_t)(envelope * 255.0f);
}

// -- Touch pads -----------------------------------------------------------

CapTouch touch1(20);  // adds red to the pulse
CapTouch touch2(21);  // adds blue to the pulse

const unsigned long CONTROL_INTERVAL = 20;  // milliseconds (~50 Hz) - smooth
                                             // enough for the eye, same
                                             // cadence as CapTouch's own demo
unsigned long previousMillis = 0;

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);

  // This LED is bright enough at full output to be uncomfortable to look
  // at directly - scale everything down globally rather than changing
  // the 0-255 math above, so pulseBrightness() and the color logic below
  // stay in terms of "full scale" regardless of this setting.
  FastLED.setBrightness(64);  // 25%

  touch1.begin();
  touch2.begin();
}

void loop() {
  // Same non-blocking pattern used throughout this project: compare the
  // clock instead of delay().
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= CONTROL_INTERVAL) {
    previousMillis = currentMillis;

    CapTouch::scan();  // one scan() covers both pads - see RA6M5CapTouch.h

    uint8_t brightness = pulseBrightness();
    leds[0] = CRGB(touch1.isTouched() ? brightness : 0,  // red
                    brightness,                          // green - always on
                    touch2.isTouched() ? brightness : 0); // blue
    FastLED.show();
  }
}
