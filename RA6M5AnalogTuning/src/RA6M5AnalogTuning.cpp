#include "RA6M5AnalogTuning.h"

// Everything chip-specific sits behind this one test. ARDUINO_ARCH_RENESAS
// is defined by the Arduino core for every Renesas RA board; checking it
// first means other boards never try to include the Renesas-only header
// below. BSP_MCU_GROUP_RA6M5 comes from that header, and narrows it to the
// RA6M5 itself (the Portenta C33 as well as the Thing Plus) rather than,
// say, the UNO R4's RA4M1, whose ADC is different.
#if defined(ARDUINO_ARCH_RENESAS)
#include "bsp_api.h"  // for R_ADC0, R_ADC1, R_MSTP
#endif

#if defined(ARDUINO_ARCH_RENESAS) && defined(BSP_MCU_GROUP_RA6M5)

// ADDISCR bits 0-3: how many ADC clock cycles to spend discharging (15,
// the most allowed). Bit 4 left at 0 selects discharging to ground rather
// than precharging to the reference voltage.
static const uint8_t DISCHARGE_SETTING = 15;

// Applies the settings to one ADC unit. The manual only allows these
// registers to change while the unit is idle, which it always is between
// analogRead() calls: analogRead() waits for its conversion to finish.
static void tuneUnit(R_ADC0_Type *adc, bool isUnit1, uint8_t samplingStates,
                     bool dischargeFirst) {
  // Each unit has one sampling-time register per group of channels
  // (manual Table 43.21). Unit 0: one per channel, AN000-AN010, AN012 and
  // AN013. Unit 1: one each for AN100-AN102, plus ADSSTRL shared by all of
  // AN116-AN128, which is where most of the Thing Plus's analog pins are
  // (A2-A5 are AN121-AN118). The temperature sensor and internal
  // reference have their own registers, left alone.
  if (isUnit1) {
    for (int n = 0; n <= 2; n++) {
      adc->ADSSTR[n] = samplingStates;
    }
    adc->ADSSTRL = samplingStates;
  } else {
    for (int n = 0; n <= 13; n++) {
      if (n != 11) {  // there's no AN011
        adc->ADSSTR[n] = samplingStates;
      }
    }
  }

  adc->ADDISCR = dischargeFirst ? DISCHARGE_SETTING : 0;
}

bool RA6M5AnalogTuning::begin(uint8_t pin, uint8_t samplingStates, bool dischargeFirst) {
  if (samplingStates < 5) {
    samplingStates = 5;  // the manual's minimum
  }

  // Switches on this pin's ADC unit - the unit's registers ignore writes
  // until then.
  analogRead(pin);

  // A unit is on when its "module stop" bit is 0: MSTPD16 for unit 0,
  // MSTPD15 for unit 1.
  if (R_MSTP->MSTPCRD_b.MSTPD16 == 0) {
    tuneUnit(R_ADC0, false, samplingStates, dischargeFirst);
  }
  if (R_MSTP->MSTPCRD_b.MSTPD15 == 0) {
    tuneUnit(R_ADC1, true, samplingStates, dischargeFirst);
  }
  return true;
}

#else

bool RA6M5AnalogTuning::begin(uint8_t pin, uint8_t samplingStates, bool dischargeFirst) {
  (void)pin;
  (void)samplingStates;
  (void)dischargeFirst;
  return false;  // not an RA6M5 - nothing to tune
}

#endif
