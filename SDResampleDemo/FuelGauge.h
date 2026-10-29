// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  FuelGauge - reads the MAX17048 battery fuel gauge built into the SparkFun
  Thing Plus RA6M5, so the page can show how the LiPo is doing and how fast
  playback is draining it.

  The MAX17048 sits on the board's one I2C bus (Wire, D15/D16, the same bus
  as the Qwiic connector) at address 0x36. It watches the cell's voltage and
  models its chemistry to estimate the charge left, with no current-sense
  resistor. Three of its 16-bit registers are read here, each MSB first:

    VCELL  0x02  cell voltage, 78.125uV per bit
    SOC    0x04  state of charge, 1/256% per bit
    CRATE  0x16  charge or discharge rate, signed, 0.208% per hour per bit -
                 negative while discharging

  CRATE is the one that answers "how long will it run": the charge left
  divided by the rate it's falling is the hours left. It comes from the
  gauge's own model, so it settles over the first few minutes after a change
  in load rather than jumping straight to the new rate.

  With no battery plugged in, the gauge measures the charger's output
  instead, and its numbers mean nothing - read them only on battery.

  Each read is a few short I2C transactions (about 1.5ms at Wire's default
  100kHz), done from loop(), never from the audio interrupt. The numbers
  move slowly, so read() is meant to be called every few seconds, not every
  pass. A board without the gauge just answers present() == false.
*/

#pragma once

#include <Arduino.h>
#include <Wire.h>

class FuelGauge {
public:
  // Starts Wire and checks the gauge answers. Safe to call once in setup().
  bool begin() {
    Wire.begin();
    uint16_t version;
    found = readRegister(REG_VERSION, version);
    return found;
  }

  bool present() const { return found; }

  // Refreshes the three readings. Returns false (keeping the old ones) if
  // the gauge didn't answer.
  bool read() {
    if (!found) return false;
    uint16_t vcell, soc, crate;
    if (!readRegister(REG_VCELL, vcell) || !readRegister(REG_SOC, soc) ||
        !readRegister(REG_CRATE, crate)) {
      return false;
    }
    volts = vcell * 78.125e-6f;
    percent = soc / 256.0f;
    percentPerHour = (int16_t)crate * 0.208f;
    return true;
  }

  float volts = 0;           // cell voltage
  float percent = 0;         // charge left, 0-100 (it can read a little over 100 when full)
  float percentPerHour = 0;  // negative while discharging

private:
  static const uint8_t ADDRESS = 0x36;
  static const uint8_t REG_VCELL = 0x02;
  static const uint8_t REG_SOC = 0x04;
  static const uint8_t REG_VERSION = 0x08;
  static const uint8_t REG_CRATE = 0x16;

  bool found = false;

  // Writes the register's address, then reads its two bytes back after a
  // repeated start, as the MAX17048's datasheet describes.
  bool readRegister(uint8_t reg, uint16_t& value) {
    Wire.beginTransmission(ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(ADDRESS, (size_t)2) != 2) return false;
    uint8_t high = Wire.read();
    uint8_t low = Wire.read();
    value = ((uint16_t)high << 8) | low;
    return true;
  }
};
