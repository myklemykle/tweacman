/*
  Basic - RA6M5AnalogTuning library

  Tunes the ADC in setup(), then prints four analog readings twice a
  second. Wire a potentiometer's wiper to each of A2-A5 (its ends to 3.3V
  and GND), open the Serial Monitor at 115200 baud, and turn one pot: with
  the tuning applied, the others' readings should hold still. Comment out
  the begin() call to see the crosstalk it removes - the pin read right
  after the one you're turning follows it by up to a few percent. (To
  measure that properly rather than by eye, see this project's NoiseMeter
  sketch.)

  On any board other than an RA6M5, begin() does nothing, and this sketch
  still builds and runs - it just says so.

  This example code is in the public domain.
*/

#include <RA6M5AnalogTuning.h>

const uint8_t PINS[] = { A2, A3, A4, A5 };
const char *const PIN_NAMES[] = { "A2", "A3", "A4", "A5" };

const unsigned long PRINT_INTERVAL = 500;  // milliseconds
unsigned long previousMillis = 0;

bool tuned = false;

// Native USB serial only carries data while a program on the computer has
// the port open, so greet on the transition to connected, not in setup().
bool wasConnected = false;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);

  // Any one of the analog pins in use will do - all four are on the same
  // ADC unit (see RA6M5AnalogTuning.h).
  tuned = RA6M5AnalogTuning::begin(A2);
}

void loop() {
  bool isConnected = Serial;
  if (isConnected && !wasConnected) {
    Serial.println();
    Serial.println(tuned ? "ADC tuned for potentiometers."
                         : "Not an RA6M5 - ADC left at its defaults.");
  }
  wasConnected = isConnected;

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= PRINT_INTERVAL) {
    previousMillis = currentMillis;

    for (int i = 0; i < 4; i++) {
      Serial.print(PIN_NAMES[i]);
      Serial.print(": ");
      Serial.print(analogRead(PINS[i]));
      Serial.print(i < 3 ? "   " : "\n");
    }
  }
}
