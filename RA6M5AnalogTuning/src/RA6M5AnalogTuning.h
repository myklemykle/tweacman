// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  RA6M5AnalogTuning.h - tunes the RA6M5's ADC for potentiometers

  The Renesas RA6M5's analog-to-digital converter (ADC) measures a pin by
  first charging a tiny internal capacitor - the "sample-and-hold" - to the
  pin's voltage, then measuring the capacitor. That capacitor is shared by
  every analog pin, and it only gets a short window to charge: the Arduino
  core leaves the sampling time at the chip's reset value of 11 ADC clock
  cycles, 220ns on these boards. A potentiometer's wiper is a slow,
  high-impedance source (up to a quarter of the pot's resistance - 2.5k for
  a 10k pot) and can't charge it fully in that time. So each reading is
  still partly the voltage of whichever pin was measured before it -
  crosstalk: turn one pot, and the pot on the neighbouring pin appears to
  move too.

  (On this core it's always the same neighbour, too. analogRead() converts
  every analog pin it has ever read on the same ADC unit, lowest channel
  number first, and returns just the one you asked for - so reading pins
  in a different order in your sketch doesn't change which pin follows
  which inside the converter.)

  begin() sets two ADC options that fix this between them:

    - A longer sampling time (64 cycles, 1.28us, by default), giving the
      wiper time to charge the capacitor fully. The RA6M5 User's Manual
      gives a formula for the time needed (section 43.6.14); for a 10k pot
      it's well over the default 220ns.

    - Discharging the capacitor before every conversion (the ADC's
      "disconnection detection assist", section 43.3.10), so whatever
      charging shortfall remains no longer depends on the previous pin.
      This makes readings very slightly low, by an amount proportional to
      the pin's own voltage - a small, steady scaling error, which
      self-calibrating code like the RV112FF library absorbs. If you need
      exact absolute voltages, pass false for dischargeFirst.

  Measured on this project's bench (SparkFun Thing Plus RA6M5, two 10k
  RV112FF-40 pots on A2-A5, with the DAC, SD card and RGB LED all busy -
  see the NoiseMeter sketch): crosstalk from turning one pot was 230-280
  counts on the other with the core's defaults, 2.5-7.8 with the longer
  sampling time alone, 1-3.6 with discharging alone, and about 1 - the
  ADC's own noise floor - with both. The random noise on a pot sitting
  still, about 1 count, was the same either way.

  The settings apply to a whole ADC unit - every analog pin on it, not
  just the one you pass. The RA6M5 has two units; begin() tunes each one
  that's switched on, which it ensures for the pin you pass by reading it
  once (the ADC's settings can't be changed until analogRead() has
  switched its unit on). If your sketch uses analog pins on both units,
  call begin() once with a pin from each.

  One caution from the manual: discharging must be turned off if you use
  the ADC to read the chip's internal temperature sensor or internal
  reference voltage. Nothing in this project does.

  On any chip other than an RA6M5, begin() does nothing and returns false
  - so a sketch can call it unconditionally and still build for other
  boards, and a library like RV112FF stays free of chip-specific code.

  This example code is in the public domain.
*/

#ifndef RA6M5AnalogTuning_h
#define RA6M5AnalogTuning_h

#include <Arduino.h>

class RA6M5AnalogTuning {
  public:
    // Reads pin once (to make sure its ADC unit is switched on), then
    // applies the settings above to every ADC unit that's on. Call from
    // setup(), before your first real analogRead(). Returns false, having
    // done nothing, on any chip other than an RA6M5.
    //
    // samplingStates: sampling time in ADC clock cycles (20ns each on these
    //   boards). The manual allows 5-255.
    // dischargeFirst: discharge the sample-and-hold before each conversion.
    static bool begin(uint8_t pin, uint8_t samplingStates = DEFAULT_SAMPLING_STATES,
                      bool dischargeFirst = true);

    static const uint8_t DEFAULT_SAMPLING_STATES = 64;
};

#endif
