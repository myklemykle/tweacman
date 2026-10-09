// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  PowerOffLatchTest - SparkFun Thing Plus RA6M5

  Grounding this board's EN pin is supposed to power it off, but if you
  do it while the board is running, the board only half turns off: the
  red PWR LED, the RGB LED and the Bluetooth module all stay powered.
  This sketch was an experiment to see whether software could fix that.

  Why it happens (see the board schematic, sheet 2): EN only switches
  off U4, the regulator that makes the 3.3V rail. A second regulator,
  U6, makes the analog reference voltage (VREF) for the ADC. U6 is
  powered straight from VIN, so EN doesn't touch it, and its enable pin
  is pulled up to the 3.3V rail through 10K. When U4 shuts off, U6's
  output leaks back onto the 3.3V rail through the MCU's analog pins.
  That holds the rail at about 2.3V, which keeps U6's enable high, so U6
  stays on and the whole thing latches. 2.3V is below the MCU's
  power-on-reset level (2.5-2.7V), so the MCU itself sits in reset
  while everything else stays powered. If you ground EN *before*
  plugging in, the board stays fully dark, because U6 never starts.

  What this sketch tried: U6's enable is also wired to an MCU pin
  (VREF_EN, D38 / P500, open-drain). On its way down from 3.3V to 2.3V,
  the rail passes through a window where the MCU is still running. The
  RA6M5's voltage monitor 1 (LVD1) flags VCC dropping below 2.99V, and
  the sketch pulls VREF_EN low when it does, turning U6 off.

  Result: it DOESN'T work, and no software can. U6 does switch off, but
  the rail then keeps falling, and at ~2.6V the MCU enters power-on reset,
  which releases all its pins, VREF_EN included. The rail is still ~2.5V
  at that moment, so U6's 10K pull-up turns U6 straight back on (it only
  needs ~0.9V), and the latch re-forms. Turning VREF off earlier doesn't
  help either; what matters is that the MCU lets go of the pin long before
  the rail drops below U6's 0.4V "off" threshold. The fix has to be
  hardware: for example, a Schottky diode from VREF_EN (the TP2 test pad,
  anode) to EN (cathode), or switch the battery line instead of using EN.

  It's still a useful probe of how the chip behaves as its power fails,
  so it's kept here as an experiment rather than a demo.

  Two modes, toggled with the USR button; the RGB LED shows which one
  is active (dimmed to 10%):

    GREEN = cut mode. Pulls VREF_EN low when LVD1 trips. (Measured: the
            board stays latched, with 3V3 at 2.35V and VREF back at 3.3V.)

    BLUE  = measure mode. Leaves VREF alone and instead counts CPU
            cycles from the LVD1 trip until the MCU drops into reset.
            The count is kept in the chip's 1KB Standby SRAM: main RAM
            doesn't survive a reset on this board (the Arduino bootloader,
            which runs first, reuses it), but Standby SRAM does, and the
            latch keeps it powered at ~2.3V. Release EN and the board
            reboots and prints the result on the serial monitor, along
            with a boot counter that proves the RAM survived. The count
            starts when LVD1 *reports* the drop, so the monitor's own
            response delay (up to 200us per the datasheet) comes on top.
            (Measured: ~29,300 cycles, about 147us.)

  While the board is running normally you can also send 'v' on the serial
  monitor to toggle VREF_EN by hand and check the VREF header pin with a
  meter: ~3.3V when on, ~0V when off.

  Note the RGB LED can't show blue or green at the latched 2.3V; the red
  PWR LED is the reliable sign that the rail is still live.

  Testing (board on USB, serial monitor open: make run SKETCH=PowerOffLatchTest):
    1. Blue (press USR if it's green). Ground EN for a couple of seconds,
       then release it. The monitor should reconnect and print the count.
    2. Green. Ground EN and measure the 3V3 and VREF pins, then release.

  This example code is in the public domain.
*/

#include <FastLED.h>

// -- LED ----------------------------------------------------------------

const uint8_t LED_PIN = 13;  // D13 / P105, the board's onboard WS2812
CRGB led;

// -- Modes ----------------------------------------------------------------

bool measureMode = false;  // false = cut mode (green), true = measure mode (blue)

void showMode() {
  led = measureMode ? CRGB::Blue : CRGB::Green;
  FastLED.show();
}

// -- Measurement record ---------------------------------------------------

// Kept in the RA6M5's 1KB Standby SRAM (0x28000000), a small RAM block
// separate from main RAM. Main RAM doesn't survive a reset on this
// board (the Arduino bootloader, which runs first, reuses it), but
// nothing touches Standby SRAM unless we do. The magic numbers tell real
// data apart from whatever random contents it had at first power-up.
const uint32_t RECORD_MAGIC = 0x4C564431;  // "LVD1"
const uint32_t BOOT_MAGIC = 0x424F4F54;    // "BOOT"

struct LatchRecord {
  uint32_t magic;
  uint32_t cycles;     // CPU cycles from the LVD1 trip until reset
  uint32_t recovered;  // 1 if the rail climbed back instead of resetting
};

struct SavedState {
  // Counts boots since power-up - a check that this RAM really does
  // survive a reset. Press RST: it should go up by one each time.
  uint32_t bootMagic;
  uint32_t bootCount;
  LatchRecord record;
};

volatile SavedState &saved = *reinterpret_cast<volatile SavedState *>(0x28000000);

// ---- RA6M5-specific ----

// Set up voltage monitor 1 to watch VCC against 2.99V, following the
// RA6M5 hardware manual, table 7.2 ("Monitoring Vdet1"). Nothing here
// generates an interrupt or a reset; we just poll the result flag.
// The digital filter stays off (its reset default), since it would
// only add delay.
void beginVoltageMonitor() {
  R_SYSTEM->PRCR = 0xA508;       // unlock the LVD registers (key 0xA5, PRC3)

  R_SYSTEM->LVD1CMPCR = 0x11;    // monitor off, level 0x11 = 2.99V (the level
                                 // can only change while the monitor is off)
  R_SYSTEM->LVD1CMPCR = 0x91;    // monitor on (LVD1E), same level
  delayMicroseconds(20);         // settle time: td(E-A) is at most 10us
  R_SYSTEM->LVD1CR0 = 0x8E;      // enable result output (CMPE), filter off
                                 // (DFDIS), no interrupt/reset; bit 3 must be 1

  R_SYSTEM->PRCR = 0xA500;       // lock again
}

// True while VCC is at or above 2.99V.
inline bool supplyOK() {
  return R_SYSTEM->LVD1SR_b.MON;
}

// Free-running CPU cycle counter, the Cortex-M33's DWT unit.
void beginCycleCounter() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// ---- end RA6M5-specific ----

// Called the moment the supply drops below 2.99V. Both paths spin
// here until power actually runs out, since there's nothing left for
// loop() to usefully do.
void onSupplyDrop() {
  if (!measureMode) {
    // Cut mode: turn off U6, the VREF regulator. The core set VREF_EN up
    // as open-drain (variant.cpp), so LOW pulls U6's enable pin to ground.
    digitalWrite(VREF_EN, LOW);
    while (true) {}
  }

  // Measure mode: keep storing the elapsed cycle count until the MCU
  // drops into reset. The last value written survives in saved.record.
  uint32_t start = DWT->CYCCNT;
  saved.record.recovered = 0;
  saved.record.cycles = 0;
  saved.record.magic = RECORD_MAGIC;
  while (!supplyOK()) {
    saved.record.cycles = DWT->CYCCNT - start;
  }
  // We only get here if VCC climbed back above 2.99V instead: it was a
  // brief dip, not a power-off.
  saved.record.recovered = 1;
}

// -- Serial ---------------------------------------------------------------

// Native USB serial drops anything printed while no monitor is attached,
// so the greeting (including the last measurement) is printed each time
// one connects.
bool wasConnected = false;
bool haveRecord = false;
LatchRecord lastRecord;

void greet() {
  Serial.println();
  Serial.println("PowerOffLatchTest - press USR to toggle mode");
  Serial.println(measureMode ? "Mode: MEASURE (blue)" : "Mode: CUT (green)");
  Serial.print("Boot count since power-up: ");
  Serial.println(saved.bootCount);

  if (!haveRecord) {
    Serial.println("No measurement from the previous power-down.");
    return;
  }
  Serial.print("Last power-down: ");
  Serial.print(lastRecord.cycles);
  Serial.print(" cycles (");
  Serial.print(lastRecord.cycles / 200.0f, 1);  // 200 MHz CPU clock
  Serial.print(" us) between the LVD1 trip and ");
  Serial.println(lastRecord.recovered ? "the rail recovering." : "reset.");
}

// -- Main -----------------------------------------------------------------

const unsigned long POLL_INTERVAL = 20;  // ms, for the button and serial checks
unsigned long previousMillis = 0;
bool buttonWasDown = false;
bool vrefOn = true;

void setup() {
  Serial.begin(115200);

  // Take a copy of any measurement left over from the last power-down,
  // then clear it so it isn't reported twice.
  if (saved.record.magic == RECORD_MAGIC) {
    lastRecord.cycles = saved.record.cycles;
    lastRecord.recovered = saved.record.recovered;
    haveRecord = true;
  }
  saved.record.magic = 0;

  if (saved.bootMagic != BOOT_MAGIC) {
    saved.bootMagic = BOOT_MAGIC;
    saved.bootCount = 0;
  }
  saved.bootCount = saved.bootCount + 1;

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(&led, 1);
  FastLED.setBrightness(26);  // ~10%
  showMode();

  beginCycleCounter();
  beginVoltageMonitor();
}

void loop() {
  // Checked on every pass, so the reaction time is a few instructions.
  // Everything else in loop() is kept short for the same reason.
  if (!supplyOK()) {
    onSupplyDrop();
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis < POLL_INTERVAL) {
    return;
  }
  previousMillis = currentMillis;

  // USR button toggles the mode (it reads LOW when pressed). Polling it
  // every 20ms is enough to skip over contact bounce.
  bool buttonDown = (digitalRead(USR_BTN) == LOW);
  if (buttonDown && !buttonWasDown) {
    measureMode = !measureMode;
    showMode();
    if (Serial) {
      Serial.println(measureMode ? "Mode: MEASURE (blue)" : "Mode: CUT (green)");
    }
  }
  buttonWasDown = buttonDown;

  // Debug: send 'v' on the serial monitor to toggle VREF_EN by hand,
  // with the board running normally. Measure the AREF pin: it should
  // read ~3.3V with VREF on and ~0V with it off.
  if (Serial.available() && Serial.read() == 'v') {
    vrefOn = !vrefOn;
    digitalWrite(VREF_EN, vrefOn ? HIGH : LOW);
    Serial.println(vrefOn ? "VREF_EN released (VREF on)" : "VREF_EN pulled low (VREF off)");
  }

  bool isConnected = Serial;
  if (isConnected && !wasConnected) {
    greet();
  }
  wasConnected = isConnected;
}
