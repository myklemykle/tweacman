// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  Hello World - SparkFun Thing Plus RA6M5

  The traditional first project: blink the on-board LED and say hello, both
  over USB serial and over Bluetooth LE. If you can see the blink and read
  the messages on either channel, your whole toolchain works and you're
  ready to build something interesting.

  Hardware:
  - SparkFun Thing Plus RA6M5 (Renesas RA6M5, Cortex-M33 @ 200 MHz)
  - The blue STAT LED is on pin 14, which the core also calls LED_BUILTIN.
  - A Renesas DA14531MOD Bluetooth LE module, wired to the RA6M5 over a
    second UART (`Serial2`). It is a separate little computer, not part of
    the RA6M5, running Renesas' "CodeLess" firmware: you talk to it with
    AT-style text commands instead of a BLE library running on the RA6M5
    itself. This sketch uses SparkFun's small CodelessBLE library to send
    those commands.

  Library setup:
  - Install "SparkFun Renesas Codeless BLE" by hand: download
    https://github.com/sparkfun/SparkFun_Thing_Plus_RA6M5/tree/main/Firmware/CodelessBLE
    and copy that folder into your sketchbook's libraries directory (it
    isn't in the Arduino Library Manager).

  Testing the Bluetooth side:
  - The board advertises as "CLv2-CodeLess". Connect to it with any BLE
    explorer app (nRF Connect and LightBlue both work) and open its
    "866d3b04-e674-40dc-9c05-b7f91bec6e83" service. That service carries
    the CodeLess AT-command link itself, which this sketch rides piggyback
    on to deliver plain text:
      - Enable notifications on characteristic e2048b39-...-56 39
        ("Flow Control"). It pulses a single byte whenever new text has
        arrived - a doorbell, not the message itself.
      - Each time it pulses, read (not notify - a plain GATT read)
        characteristic 3bb535aa-...-a4 04 ("Outbound Command"). That's
        where the actual text - "Hello, world!" or "blink 12" - shows up.
    This two-step "notify, then go read" dance is CodeLess's own protocol
    for pushing unsolicited text over BLE; it isn't something this sketch
    invented. It was confirmed by scanning a live board with Python's
    `bleak` library while this sketch ran.
  - You can also just talk AT commands to the module remotely: write
    "AT+BDADDR\r" as text to the Inbound Command characteristic
    (914f8fb9-...-54 25) and read the same Outbound characteristic (behind
    the same Flow Control doorbell) to get the board's Bluetooth address
    back, from across the room.

  Notes:
  - On this board `Serial` is the native USB port, not a UART. The port
    only exists while the sketch is running, so the board disappears from
    /dev when you reset it and comes back a moment later.
  - If an upload fails, double-tap the RST button to force the bootloader
    (DFU mode). The STAT LED fades slowly in and out to show it's waiting.

  This example code is in the public domain.
*/

#include <codelessBLEPeripheral.h>

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

// The BLE coprocessor. `ble` talks AT commands to it over Serial2, which is
// the default this library assumes - see the header comment above.
CodelessPeripheralDevice ble;

// Whether a BLE central (phone, laptop, ...) is currently connected. Set by
// watching the CodeLess module's own status messages in loop() below.
bool bleConnected = false;

// Sends one line of text out over Bluetooth. A message prefixed with "|" is
// treated by the CodeLess firmware as data to forward to the connected
// central, rather than as an AT command - see the header comment for how a
// central retrieves it. This only works while something is connected; the
// caller is expected to check bleConnected first.
void sayOverBLE(const String &message) {
  ble.sendCommand("|" + message);
}

// Printed - and sent over BLE, if connected - whenever a listener attaches.
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

  if (bleConnected) {
    sayOverBLE("Hello, world!");
    sayOverBLE("SparkFun Thing Plus RA6M5 is alive.");
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  // Deliberately no waiting here. The sketch should run the same whether
  // it's plugged into a computer or a battery, and loop() below greets the
  // monitor whenever it shows up.

  // Bring up the BLE coprocessor: this opens Serial2 at the module's fixed
  // 57600 baud, resets it, and starts advertising so phones can find it.
  // Unlike the native-USB `Serial`, Serial2 is a real UART pin pair with
  // nothing to wait for - it's ready as soon as begin() returns.
  ble.begin();
  ble.setupDefault();
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

  // The CodeLess module reports its own connection state as plain text
  // lines ("+CONNECTED", "+DISCONNECTED") whenever we're not in the middle
  // of waiting for a command reply. Watch for those to know when a phone
  // has joined or left.
  if (ble.available()) {
    String status = ble.readOutput();
    if (status.indexOf("+CONNECTED") != -1) {
      bleConnected = true;
      Serial.println("BLE central connected.");
      greet();
    } else if (status.indexOf("+DISCONNECTED") != -1) {
      bleConnected = false;
      Serial.println("BLE central disconnected. Advertising again.");
      // Advertising stops once a central connects, so restart it or the
      // module becomes unreachable after the first visitor leaves. This
      // is the one command here that isn't "|"-prefixed, so it blocks
      // briefly waiting for OK - fine for a one-off event like this.
      ble.sendCommand("AT+ADVSTART");
    }
  }

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
      String line = "blink " + String(blinkCount);
      Serial.println(line);
      if (bleConnected) {
        sayOverBLE(line);
      }
    }
  }
}
