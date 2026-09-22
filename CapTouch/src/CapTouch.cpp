#include "CapTouch.h"
#include "bsp_api.h"  // for R_CTSU, R_MSTP, R_IOPORT_PinCfg()

// See the class comment in CapTouch.h for how these three were confirmed
// against the RA6M5 Hardware User's Manual (Table 1.17).
const CapTouch::KnownChannel CapTouch::KNOWN_CHANNELS[3] = {
  { 21, 4 },   // D21 -> TS04
  { 20, 5 },   // D20 -> TS05
  { 31, 11 },  // D31 -> TS11 (also the onboard USR button)
};

// TSCAP - the shared analog reference pin every measurement depends on,
// not a sensing channel. Fixed to D6/P207 on this board (see the class
// comment); unlike the touch pins themselves, it isn't something a
// sketch chooses, so it isn't part of the public API.
static const uint8_t TSCAP_PIN = 6;

CapTouch *CapTouch::s_instances[CapTouch::MAX_INSTANCES] = { nullptr, nullptr, nullptr };
uint8_t CapTouch::s_instanceCount = 0;
bool CapTouch::s_hardwareReady = false;

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

  bool candidate = ((float)raw - _baseline) >= (float)_threshold;

  // Debounce: only accept a change of state once the same answer has come
  // back _debounceSamples scans running.
  //
  // The threshold comparison alone is a bare test of a single sample, so one
  // noisy reading flips isTouched() for exactly one scan. That is invisible
  // in a demo that merely displays or colours something from it - a
  // one-scan blip among fifty a second - but it is not harmless to anything
  // edge-triggered. Confirmed live: a sketch that latched its transport on
  // a rising edge picked up spurious latches with nobody touching the pad,
  // while the plotter demo on the same hardware looked perfectly clean. The
  // glitches were always there; only the consumer changed.
  //
  // Requiring agreement across consecutive scans costs one scan interval of
  // latency per state change (20-30ms at the rates these sketches run) and
  // rejects any disturbance shorter than that outright.
  if (candidate == _touched) {
    _pendingScans = 0;
  } else if (++_pendingScans < _debounceSamples) {
    // Not yet convinced - hold the current state and let the rest of this
    // function carry on with it, so baseline adaptation and the stuck
    // timeout keep behaving as though nothing happened.
    candidate = _touched;
  } else {
    _pendingScans = 0;
  }

  bool touchedNow = candidate;

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
  for (uint8_t i = 0; i < 3; i++) {
    if (KNOWN_CHANNELS[i].pin == pin) {
      tsChannel = KNOWN_CHANNELS[i].tsChannel;
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
  pinMode(TSCAP_PIN, OUTPUT);
  digitalWrite(TSCAP_PIN, LOW);
  delay(1);
  // TSCAP isn't one of the 20 general-purpose TS channels, but it still
  // needs to be explicitly routed to the CTSU peripheral function
  // (PmnPFS.PSEL[4:0] = 01100b, same peripheral-select value as the
  // touch channel pins below) - earlier testing left it as a plain
  // digital input here instead, which is wrong: without this, the
  // CTSU's internal switch (CTSUCR1.CTSUCSW below) has nowhere to
  // actually connect.
  R_IOPORT_PinCfg(NULL, g_pin_cfg[TSCAP_PIN].pin,
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
    return false;  // not one of this board's three touch-capable pins
  }

  for (uint8_t i = 0; i < s_instanceCount; i++) {
    if (s_instances[i]->_tsChannel == tsChannel) {
      return false;  // already claimed by another CapTouch instance
    }
  }

  if (s_instanceCount >= MAX_INSTANCES) {
    return false;  // shouldn't happen - there are only 3 known channels
  }

  initHardwareOnce();

  // Configure this pin for its CTSU touch function: peripheral-select
  // CTSU (matches PmnPFS.PSEL[4:0] = 01100b in the manual) with the pin
  // mode register set to route it to that peripheral rather than plain
  // GPIO.
  R_IOPORT_PinCfg(NULL, g_pin_cfg[_pin].pin,
                  (uint32_t)(IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_CTSU));

  _tsChannel = tsChannel;
  s_instances[s_instanceCount] = this;
  s_instanceCount++;
  return true;
}

void CapTouch::scan() {
  if (s_instanceCount == 0) return;

  // CTSUCHAC0 covers TS00-TS07, CTSUCHAC1 covers TS08-TS15 - clear both
  // (this board's three known channels never reach TS16+, so CTSUCHAC2
  // is never used) and set exactly the bits for currently-registered
  // instances.
  R_CTSU->CTSUCHAC[0] = 0;
  R_CTSU->CTSUCHAC[1] = 0;
  for (uint8_t i = 0; i < s_instanceCount; i++) {
    uint8_t ch = s_instances[i]->_tsChannel;
    R_CTSU->CTSUCHAC[ch / 8] |= (1 << (ch % 8));
  }

  R_CTSU->CTSUCR0_b.CTSUSTRT = 1;  // software trigger: start the scan

  unsigned long start = millis();
  const unsigned long TIMEOUT_MS = 50;  // generous - a full scan should
                                          // take well under this even
                                          // with all channels registered
  uint8_t lastStatus = 0xFF;
  uint8_t currentChannel = 0;  // updated at Status 2 - see below

  while (millis() - start < TIMEOUT_MS) {
    uint8_t status = R_CTSU->CTSUST_b.CTSUSTC;

    // Status 2 ("wait for sensor drive pulse setting"): write this
    // channel's measurement settings. The manual says a write to
    // CTSUSO1 specifically is what triggers the transition onward to
    // Status 3, so it must be written last - and it needs writing on
    // every visit to Status 2, not just once, so gate on the status
    // *changing* rather than a one-shot flag.
    //
    // CTSUSSC=0x0200 sets CTSUSSDIV=2 (spectrum diffusion divisor for
    // a ~1.33-2.00MHz base clock, Table 46.6 - matches this board's
    // PCLKB-derived base clock with CTSUCLK left undivided above).
    // CTSUSO0=0 leaves the sensor offset/measurement-count at their
    // defaults. CTSUSO1=0x0F00 sets CTSUSDPA=15 (base clock divided by
    // (15+1)*2 = 32 - this is Step 5 of the manual's initial settings
    // flow, "set CTSU base clock", which earlier code never actually
    // did, leaving the divisor at its reset value of /2 instead) with
    // CTSURICOA=0 and CTSUICOG=00b (ICO gain 100%, both defaults).
    if (status == 2 && status != lastStatus) {
      // CTSUMCH0 identifies which channel is about to be measured, per
      // the manual's own description of CTSUSO0 ("Set the TS pin that
      // is to be measured next after a CTSU_CTSUWR interrupt occurs").
      // Capture it *here* rather than re-reading it later once
      // CTSUDTSR fires: for the last channel in a scan, CTSUMCH0
      // apparently gets reset back to an idle/invalid value (0x1F) as
      // part of the hardware wrapping the scan up, racing against a
      // read of it after the fact - found by tracing every channel/raw
      // pair live and seeing the last channel's reads misattributed to
      // channel 31, which isn't a real channel at all.
      currentChannel = R_CTSU->CTSUMCH0_b.CTSUMCH0;

      R_CTSU->CTSUSSC = 0x0200;
      R_CTSU->CTSUSO0 = 0x0000;
      R_CTSU->CTSUSO1 = 0x0F00;  // must be written last - see CapTouch.h
    }
    lastStatus = status;

    // CTSUST.CTSUDTSR ("result ready, not yet read") is the reliable
    // signal that a channel's measurement is done - polling for the
    // status counter to equal 5 directly missed real transitions in
    // testing (Status 5 can be too brief to catch by polling, the same
    // problem Status 1 turned out to have), which then permanently
    // stalled the *next* channel in Status 3 forever, since per the
    // manual "Status 3 continues until the CTSURC counter is read,
    // even if the stabilization time...elapses". Reading CTSURC here
    // is what clears CTSUDTSR and lets the next channel proceed.
    if (R_CTSU->CTSUST_b.CTSUDTSR) {
      uint16_t raw = R_CTSU->CTSUSC;         // read order matters: SC then RC
      uint16_t reference = R_CTSU->CTSURC;

      for (uint8_t i = 0; i < s_instanceCount; i++) {
        if (s_instances[i]->_tsChannel == currentChannel) {
          s_instances[i]->finishReading(raw, reference);
          break;
        }
      }
    }

    if (R_CTSU->CTSUCR0_b.CTSUSTRT == 0) {
      break;  // hardware auto-clears this when every registered channel is done
    }
  }
}
