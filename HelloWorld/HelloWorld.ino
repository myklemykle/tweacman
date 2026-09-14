/*
  Hello World - SparkFun Thing Plus RA6M5

  The traditional first project: blink the on-board LED and say hello over
  the USB serial port. If you can see the blink and read the messages, your
  whole toolchain works and you're ready to build something interesting.

  Hardware:
  - SparkFun Thing Plus RA6M5 (Renesas RA6M5, Cortex-M33 @ 200 MHz)
  - The blue STAT LED is on pin 14, which the core also calls LED_BUILTIN.

  Notes:
  - On this board `Serial` is the native USB port, not a UART. The port only
    exists while the sketch is running, so the board disappears from /dev
    when you reset it and comes back a moment later.
  - If an upload fails, double-tap the RST button to force the bootloader
    (DFU mode). The STAT LED fades slowly in and out to show it's waiting.

  This example code is in the public domain.
*/

// How long the LED stays in each state, in milliseconds.
const unsigned long BLINK_INTERVAL = 500;

// Counts how many times we've blinked, so the serial output keeps changing.
unsigned long blinkCount = 0;

// Remembers when we last toggled the LED. `unsigned long` because millis()
// overflows back to zero after about 49 days, and unsigned subtraction keeps
// working correctly when it does.
unsigned long previousMillis = 0;

// Tracks the LED state ourselves rather than reading the pin back.
bool ledOn = false;

// Whether a serial monitor was attached last time we checked. Native USB
// only carries data while a program on the computer has the port open, so we
// watch for that to change.
bool wasConnected = false;

// Printed whenever a serial monitor attaches.
void greet() {
  Serial.println();
  Serial.println("Hello, world!");
  Serial.println("SparkFun Thing Plus RA6M5 is alive.");
  Serial.print("Blinking pin ");
  Serial.print(LED_BUILTIN);
  Serial.print(" every ");
  Serial.print(BLINK_INTERVAL);
  Serial.println(" ms.");
  Serial.println();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  // Deliberately no waiting here. The sketch should run the same whether
  // it's plugged into a computer or a battery, and loop() below greets the
  // monitor whenever it shows up.
}

void loop() {
  // Anything we print before the monitor opens the port is discarded, so
  // say hello on the transition from disconnected to connected. That way
  // you see the greeting no matter how long after boot you attach.
  bool isConnected = Serial;
  if (isConnected && !wasConnected) {
    greet();
  }
  wasConnected = isConnected;

  // Blink without delay(): compare the clock instead of blocking. This keeps
  // loop() free to do other work, which matters once we start generating
  // audio and can't afford to stall.
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= BLINK_INTERVAL) {
    previousMillis = currentMillis;

    ledOn = !ledOn;
    digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);

    // Only count and report on the rising edge, so we print once per full
    // blink rather than twice.
    if (ledOn) {
      blinkCount++;
      Serial.print("blink ");
      Serial.println(blinkCount);
    }
  }
}
