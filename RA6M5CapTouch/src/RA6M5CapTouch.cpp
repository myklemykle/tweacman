// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

#include "RA6M5CapTouch.h"

// This library drives the RA6M5's touch peripheral register by register,
// so refuse to build anywhere else rather than misbehave. The first test
// keeps non-Renesas boards from even reaching the Renesas-only header;
// BSP_MCU_GROUP_RA6M5 comes from that header and rules out other RA
// chips (the UNO R4's RA4M1, say), whose touch hardware differs.
#if !defined(ARDUINO_ARCH_RENESAS)
#error "RA6M5CapTouch only works on Renesas RA6M5 boards."
#endif
#include "bsp_api.h"  // for R_CTSU, R_MSTP, R_IOPORT_PinCfg()
#if !defined(BSP_MCU_GROUP_RA6M5)
#error "RA6M5CapTouch only works on Renesas RA6M5 boards."
#endif

// Every RA6M5 port pin that has a touch channel, and which channel it is -
// the TSnn column of the RA6M5 Hardware User's Manual, Table 1.17 (Pin
// list). These are fixed in the silicon, so they hold on any RA6M5 board;
// which of them a given board actually brings out to a header is up to
// that board. Listed in channel order.
struct TouchPort {
  bsp_io_port_pin_t port;
  uint8_t tsChannel;
};
static const TouchPort TOUCH_PORTS[] = {
  { BSP_IO_PORT_02_PIN_04, 0 },  { BSP_IO_PORT_02_PIN_05, 1 },
  { BSP_IO_PORT_02_PIN_06, 2 },  { BSP_IO_PORT_04_PIN_07, 3 },
  { BSP_IO_PORT_04_PIN_08, 4 },  { BSP_IO_PORT_04_PIN_09, 5 },
  { BSP_IO_PORT_04_PIN_10, 6 },  { BSP_IO_PORT_04_PIN_11, 7 },
  { BSP_IO_PORT_04_PIN_12, 8 },  { BSP_IO_PORT_04_PIN_13, 9 },
  { BSP_IO_PORT_04_PIN_14, 10 }, { BSP_IO_PORT_04_PIN_15, 11 },
  { BSP_IO_PORT_07_PIN_08, 12 }, { BSP_IO_PORT_07_PIN_09, 13 },
  { BSP_IO_PORT_07_PIN_10, 14 }, { BSP_IO_PORT_07_PIN_11, 15 },
  { BSP_IO_PORT_07_PIN_12, 16 }, { BSP_IO_PORT_07_PIN_13, 17 },
  { BSP_IO_PORT_02_PIN_03, 18 }, { BSP_IO_PORT_02_PIN_02, 19 },
};
static const uint8_t TOUCH_PORT_COUNT = sizeof(TOUCH_PORTS) / sizeof(TOUCH_PORTS[0]);

// TSCAP - the shared analog reference pin every measurement depends on,
// not a sensing channel. The RA6M5 has exactly one, on port P207 (Table
// 1.17 again), so it's set up by port rather than by any board's Arduino
// pin number, and isn't part of the public API.
static const bsp_io_port_pin_t TSCAP_PORT = BSP_IO_PORT_02_PIN_07;

CapTouch *CapTouch::s_instances[CapTouch::MAX_INSTANCES] = {};
uint8_t CapTouch::s_instanceCount = 0;
bool CapTouch::s_hardwareReady = false;
bool CapTouch::s_scanning = false;
uint8_t CapTouch::s_resultsRead = 0;
uint8_t CapTouch::s_settingsWritten = 0;
unsigned long CapTouch::s_lastProgressMicros = 0;
uint8_t CapTouch::s_measurementCount = 1;
uint8_t CapTouch::s_scanMeasurementCount = 1;
bool CapTouch::s_recentrePending = false;
bool CapTouch::s_overflowed = false;
uint32_t CapTouch::s_scanFailures = 0;

CapTouch::CapTouch(uint8_t pin) : _pin(pin) {
  // Deliberately no hardware access here - see RV112FF.h for why: a
  // global "CapTouch touch(20);" runs its constructor before setup(),
  // before this board's peripherals are guaranteed ready. Everything
  // that touches hardware happens in begin() instead.
}

void CapTouch::recalibrate() {
  _baseline = _raw;
  _baselineInitialized = true;
  _touched = false;
  _touchStartMillis = 0;
  _lastBaselineUpdateMillis = millis();
}

void CapTouch::finishReading(uint16_t raw, uint16_t reference) {
  _raw = raw;
  _reference = reference;

  unsigned long now = millis();

  if (!_baselineInitialized) {
    // First reading ever for this pin: nothing to compare against yet,
    // so start the baseline here (assumes nothing's touching at boot,
    // the same assumption virtually every touch sensor design makes)
    // and report untouched this cycle.
    _baseline = raw;
    _baselineInitialized = true;
    _lastBaselineUpdateMillis = now;
    _touched = false;
    return;
  }

  bool touchedNow = ((float)raw - _baseline) >= (float)_threshold;

  if (touchedNow) {
    if (_touchStartMillis == 0) {
      _touchStartMillis = now;  // this is the start of a new touch
    } else if (now - _touchStartMillis >= _stuckTimeoutMs) {
      // Held "touched" for longer than getStuckTimeout() allows for a
      // real touch-and-hold - assume the baseline itself has shifted
      // (something resting on the sensor, not a finger) and recover by
      // recalibrating around the current reading instead of reporting
      // touched forever.
      recalibrate();
      return;
    }
  } else {
    _touchStartMillis = 0;  // ready to time a fresh touch next time
  }

  _touched = touchedNow;

  if (!_touched) {
    // Nudge the baseline toward the current raw reading - but only
    // while untouched, so a genuine touch-and-hold doesn't slowly get
    // absorbed as "the new normal" while it's still happening. Linear
    // approximation of a first-order low-pass filter (alpha = dt/tau,
    // clamped to 1) - accurate enough since scan() is normally called
    // many times within one BASELINE_SMOOTHING_MS window, and avoids
    // needing an actual exp() for it.
    unsigned long dt = now - _lastBaselineUpdateMillis;
    float alpha = (float)dt / (float)BASELINE_SMOOTHING_MS;
    if (alpha > 1.0f) alpha = 1.0f;
    _baseline += alpha * ((float)raw - _baseline);
  }
  _lastBaselineUpdateMillis = now;
}

bool CapTouch::lookupTsChannel(uint8_t pin, uint8_t &tsChannel) {
  if (pin >= PINS_COUNT) return false;
  bsp_io_port_pin_t port = digitalPinToBspPin(pin);  // the board's core knows its own wiring
  for (uint8_t i = 0; i < TOUCH_PORT_COUNT; i++) {
    if (TOUCH_PORTS[i].port == port) {
      tsChannel = TOUCH_PORTS[i].tsChannel;
      return true;
    }
  }
  return false;
}

void CapTouch::initHardwareOnce() {
  if (s_hardwareReady) return;

  // Step 1 of the manual's initial settings flow (RA6M5 Hardware User's
  // Manual, Figure 46.9): discharge whatever charge is already on the
  // external LPF capacitor wired to TSCAP, using the pin as a plain
  // GPIO output driven low, before the CTSU peripheral touches it.
  R_IOPORT_PinCfg(NULL, TSCAP_PORT,
                  (uint32_t)(IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW));
  delay(1);
  // TSCAP isn't one of the 20 general-purpose TS channels, but it still
  // needs to be explicitly routed to the CTSU peripheral function
  // (PmnPFS.PSEL[4:0] = 01100b, same peripheral-select value as the
  // touch channel pins below) - earlier testing left it as a plain
  // digital input here instead, which is wrong: without this, the
  // CTSU's internal switch (CTSUCR1.CTSUCSW below) has nowhere to
  // actually connect.
  R_IOPORT_PinCfg(NULL, TSCAP_PORT,
                  (uint32_t)(IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_CTSU));

  // Step 3: enable the CTSU module's clock. RA-family peripherals are
  // clock-gated off by default; MSTPC3 is CTSU's specific bit in Module
  // Stop Control Register C (0 = clock running, 1 = stopped).
  R_MSTP->MSTPCRC_b.MSTPC3 = 0;

  // Reset the CTSU's internal control block before configuring it.
  // CTSUCR0.CTSUINIT ("write 1 to initialize the internal control
  // registers", per the manual) isn't mentioned as its own numbered
  // step in the initial settings flow (Figure 46.9), but skipping it
  // left CTSUERRS.CTSUICOMP (TSCAP Voltage Error Monitor) permanently
  // latched abnormal on this board, blocking every measurement -
  // found by comparing against a known-working reference sequence on
  // real hardware.
  R_CTSU->CTSUCR0 = 0;
  R_CTSU->CTSUCR0_b.CTSUINIT = 1;

  // Steps 4-6: power supply, base clock, and transmission power select.
  // CTSUATUNE1=0 (normal output, not high-current) and CTSUCLK=00b
  // (PCLKB, undivided - side-steps a documented restart-timing
  // constraint that only applies when PCLKB/4 is selected) are both
  // the manual's defaults. CTSUMD=01b selects self-capacitance
  // multiscan mode - measuring every enabled channel in one pass,
  // which is what scan() below expects.
  R_CTSU->CTSUCR1_b.CTSUATUNE1 = 0;
  R_CTSU->CTSUCR1_b.CTSUCLK = 0;
  R_CTSU->CTSUCR1_b.CTSUMD = 0b01;

  // CTSUTXVSEL only matters in mutual capacitance full scan mode, which
  // this library doesn't use.
  R_CTSU->CTSUCR0_b.CTSUTXVSEL = 0;

  // Synchronous/high-pass noise reduction settings. CTSUPRRATIO=3 and
  // CTSUPRMODE=10b (62 base pulses) are the manual's own recommended
  // values; CTSUSOFF=0 leaves high-pass noise reduction on.
  R_CTSU->CTSUSDPRS_b.CTSUPRRATIO = 3;
  R_CTSU->CTSUSDPRS_b.CTSUPRMODE = 0b10;
  R_CTSU->CTSUSDPRS_b.CTSUSOFF = 0;

  // CTSUSST and the CTSUDCLKC noise-reduction bits are fixed, specific
  // values per the manual - not tunable parameters, just required
  // constants for correct operation.
  R_CTSU->CTSUSST = 0b00010000;
  R_CTSU->CTSUDCLKC_b.CTSUSSMOD = 0b00;
  R_CTSU->CTSUDCLKC_b.CTSUSSCNT = 0b11;

  // Step 7: power on the CTSU and start charging the TSCAP capacitor at
  // the same time, as the manual specifies.
  R_CTSU->CTSUCR1_b.CTSUPON = 1;
  R_CTSU->CTSUCR1_b.CTSUCSW = 1;

  // Step 8: wait for the TSCAP capacitor to charge and stabilize before
  // the first measurement. The manual doesn't give an exact time for
  // this; a few milliseconds is a generous margin for the small
  // capacitance (tens of nF) this circuit expects - lengthen this if
  // early readings look unstable.
  delay(10);

  s_hardwareReady = true;
}

bool CapTouch::begin() {
  uint8_t tsChannel;
  if (!lookupTsChannel(_pin, tsChannel)) {
    return false;  // this pin's port has no touch channel
  }

  for (uint8_t i = 0; i < s_instanceCount; i++) {
    if (s_instances[i]->_tsChannel == tsChannel) {
      return false;  // already claimed by another CapTouch instance
    }
  }

  if (s_instanceCount >= MAX_INSTANCES) {
    return false;  // can't happen: one instance per channel, and MAX_INSTANCES covers them all
  }

  initHardwareOnce();

  // Configure this pin for its CTSU touch function: peripheral-select
  // CTSU (matches PmnPFS.PSEL[4:0] = 01100b in the manual) with the pin
  // mode register set to route it to that peripheral rather than plain
  // GPIO.
  R_IOPORT_PinCfg(NULL, digitalPinToBspPin(_pin),
                  (uint32_t)(IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_CTSU));

  _tsChannel = tsChannel;

  // Keep s_instances sorted by channel number, lowest first: the hardware
  // measures channels in that order, and scan() matches each result to
  // its pin purely by position in this list - see scan() for why.
  uint8_t slot = s_instanceCount;
  while (slot > 0 && s_instances[slot - 1]->_tsChannel > tsChannel) {
    s_instances[slot] = s_instances[slot - 1];
    slot--;
  }
  s_instances[slot] = this;
  s_instanceCount++;
  return true;
}

void CapTouch::setMeasurementCount(uint8_t count) {
  if (count < 1) count = 1;
  if (count > 64) count = 64;  // CTSUSNUM is 6 bits: 0-63 means 1-64 repeats
  if (count == s_measurementCount) return;
  s_measurementCount = count;
  // Every reading is about to jump in proportion, which the baseline
  // would otherwise take for a touch - so re-centre each pin on the
  // first reading made with the new count. (Not the scan under way now,
  // if there is one: it started with the old count, and keeps it.)
  s_recentrePending = true;
}

void CapTouch::startScan() {
  if (s_instanceCount == 0 || s_scanning) return;

  // A result left unread by an earlier scan would otherwise be taken
  // for this scan's first result. Reading CTSURC is what clears it.
  if (R_CTSU->CTSUST_b.CTSUDTSR) {
    (void)R_CTSU->CTSUSC;
    (void)R_CTSU->CTSURC;
  }
  // The overflow flag stays set until software writes 0 to it.
  if (R_CTSU->CTSUST_b.CTSUSOVF) R_CTSU->CTSUST_b.CTSUSOVF = 0;

  // CTSUCHAC0 covers TS00-TS07, CTSUCHAC1 TS08-TS15 and CTSUCHAC2
  // TS16-TS19 - clear all three and set exactly the bits for
  // currently-registered instances.
  R_CTSU->CTSUCHAC[0] = 0;
  R_CTSU->CTSUCHAC[1] = 0;
  R_CTSU->CTSUCHAC[2] = 0;
  for (uint8_t i = 0; i < s_instanceCount; i++) {
    uint8_t ch = s_instances[i]->_tsChannel;
    R_CTSU->CTSUCHAC[ch / 8] |= (1 << (ch % 8));
  }

  s_resultsRead = 0;
  s_settingsWritten = 0;
  s_scanMeasurementCount = s_measurementCount;
  s_lastProgressMicros = micros();
  s_scanning = true;
  R_CTSU->CTSUCR0_b.CTSUSTRT = 1;  // software trigger: start the scan
}

bool CapTouch::update() {
  if (!s_scanning) return false;

  // The hardware measures the enabled channels in ascending order, one
  // at a time, and for each one it stops twice to wait for the CPU:
  //
  //   Status 2: waiting for this channel's settings to be written.
  //   Status 3: waiting (after its settling time) for the *previous*
  //             channel's result to be read, before measuring this one.
  //
  // So it never runs ahead of us, however rarely update() is called -
  // it just sits and waits. All update() has to do is answer whichever
  // question is pending, using two counts to keep its place: pins whose
  // result has been read, and pins whose settings have been written.
  // begin() keeps s_instances in channel order, so the Nth result and
  // the Nth settings both belong to s_instances[N].
  //
  // Counting rather than watching which channel the hardware says it's
  // on (CTSUMCH0) matters: an earlier version noted the channel number
  // at Status 2 and credited the next result to it, which raced - by the
  // time an interrupt-delayed loop() looked, the hardware could already
  // be on the next channel's Status 2, and a result landed on the wrong
  // pin.
  bool progress = false;

  // A result is ready (CTSUST.CTSUDTSR). Polling for Status 5 instead
  // missed real ones in testing - it can be too brief to catch - which
  // stalled the next channel in Status 3 forever. Reading CTSURC is what
  // clears the flag and lets the hardware go on.
  if (R_CTSU->CTSUST_b.CTSUDTSR && s_resultsRead < s_instanceCount) {
    uint16_t raw = R_CTSU->CTSUSC;         // read order matters: SC then RC
    uint16_t reference = R_CTSU->CTSURC;
    s_instances[s_resultsRead]->finishReading(raw, reference);
    s_resultsRead++;
    progress = true;
  }

  // Status 2: write the next channel's settings. A channel's Status 2
  // comes after the previous channel's result is ready, which the check
  // above has just read - so "settings written == results read" means
  // this Status 2 hasn't been answered yet. (Status 2 can still read back
  // for a moment after the answer; the counts keep that from being
  // answered twice.)
  //
  // The write to CTSUSO1 is what moves the hardware on, so it goes last.
  // CTSUSSC=0x0200 sets CTSUSSDIV=2 (spectrum diffusion divisor for a
  // ~1.33-2.00MHz base clock, Table 46.6 - matches this board's
  // PCLKB-derived base clock with CTSUCLK left undivided). CTSUSO0 holds
  // the measurement count (CTSUSNUM, repeats minus one) and leaves the
  // sensor offset current (CTSUSO) at 0. CTSUSO1=0x0F00 sets CTSUSDPA=15
  // (base clock divided by (15+1)*2 = 32 - Step 5 of the manual's initial
  // settings flow, "set CTSU base clock", left at its reset value of /2
  // by earlier code) with CTSURICOA=0 and CTSUICOG=00b (ICO gain 100%,
  // both defaults).
  if (R_CTSU->CTSUST_b.CTSUSTC == 2 && s_settingsWritten == s_resultsRead &&
      s_settingsWritten < s_instanceCount) {
    R_CTSU->CTSUSSC = 0x0200;
    R_CTSU->CTSUSO0 = (uint16_t)((s_scanMeasurementCount - 1) << R_CTSU_CTSUSO0_CTSUSNUM_Pos);
    R_CTSU->CTSUSO1 = 0x0F00;  // must be written last - see above
    s_settingsWritten++;
    progress = true;
  }

  if (s_resultsRead == s_instanceCount) {
    s_scanning = false;
    s_overflowed = R_CTSU->CTSUST_b.CTSUSOVF;
    if (s_recentrePending && s_scanMeasurementCount == s_measurementCount) {
      for (uint8_t i = 0; i < s_instanceCount; i++) s_instances[i]->recalibrate();
      s_recentrePending = false;
    }
    return true;
  }

  // Give up on a scan that has stopped moving - not merely one update()
  // hasn't been called for, since the hardware waits for us, but one
  // where nothing new has happened in far longer than a channel's
  // measurement takes (roughly 0.2ms per repeat). Stopping a scan by
  // hand needs CTSUSTRT=0 and CTSUINIT=1 in a single write, per the
  // manual.
  unsigned long now = micros();
  if (progress) {
    s_lastProgressMicros = now;
  } else if (now - s_lastProgressMicros > 5000UL + 1000UL * s_scanMeasurementCount) {
    R_CTSU->CTSUCR0 = (uint8_t)((R_CTSU->CTSUCR0 & ~R_CTSU_CTSUCR0_CTSUSTRT_Msk) |
                                R_CTSU_CTSUCR0_CTSUINIT_Msk);
    s_scanning = false;
    s_scanFailures++;
    return true;
  }
  return false;
}

void CapTouch::scan() {
  startScan();
  while (s_scanning) update();
}
