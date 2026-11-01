// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  RA6M5CapTouch.h - capacitive touch sensing on the RA6M5's built-in CTSU

  The RA6M5 has a real hardware peripheral for this (the Capacitive Touch
  Sensing Unit, or CTSU) - not a software RC-timing trick like classic
  Arduino capacitive-sense libraries use. It measures a graduated value
  roughly proportional to sensed capacitance, not just touched/not-touched;
  isTouched() below is a threshold comparison built on top of that, not the
  only thing the hardware can tell you.

  Which pins can sense touch is fixed by the chip, not the board: each of
  the RA6M5's 20 touch channels (TS00-TS19) belongs to one specific port
  pin (RA6M5 Hardware User's Manual, Table 1.17 - P204 is TS00, P415 is
  TS11, and so on). So the library takes an ordinary Arduino pin number,
  asks the board's core which port pin that is, and looks the port pin up
  in that table. Any RA6M5 board works, whatever numbers it gives its
  pins; a pin whose port has no touch channel is simply refused by
  begin(). On the SparkFun Thing Plus RA6M5 that leaves three usable pins:
  D20 (P409, TS05), D21 (P408, TS04), and D31 (P415, TS11, which is also
  the board's onboard USR button - touch-sensing that pin picks up
  whatever's mechanically wired there too, so it works but isn't a great
  choice for a breadboard wire). Other RA6M5 boards, such as the Portenta
  C33, haven't been tested.

  The peripheral also has one shared analog reference pin, TSCAP, which
  every measurement depends on. It's fixed to port P207 on every RA6M5
  (D6 on the Thing Plus), so the library sets it up by port and a sketch
  never names it. It needs an external capacitor to ground (10nF is a
  reasonable starting point; up to 100nF is fine, the exact value isn't
  critical) wired directly to that pin, or nothing here will work.

  This is a clean-room implementation written directly against Renesas'
  RA6M5 Hardware User's Manual (chapter 46, "Capacitive Touch Sensing
  Unit"), not adapted from any third-party library. A well-regarded
  community library exists for the Arduino UNO R4's version of this same
  peripheral (R4_Touch by delta-G) and was useful to know about while
  planning this - its README's capacitor recommendation, and confirming a
  from-scratch approach was even feasible, both saved real time - but it's
  GPLv3-licensed, and this library is LGPL so that sketches using it
  needn't be GPL themselves - so nothing here is derived from its code.

  API style matches this project's other libraries (see RV112FF): the
  constructor only stores the pin (safe to call from a global's static
  initializer, before setup() runs); begin() does the actual hardware
  setup and must be called from setup(). Where this diverges from a
  library like Encoder is the same place it diverges from R4_Touch, and
  for the same hardware reason in both cases: the chip has exactly one
  CTSU scanning unit, not one per pin, so triggering a measurement
  (scan()) is a class-level operation shared by every registered
  CapTouch instance, not something any one instance can do alone -
  reading back a specific pin's result from that shared measurement is
  the only part that's per-instance.

  Polled from loop() - matching this project's established preference
  for polling over interrupts (see RV112FF) - rather than the
  free-running, interrupt-driven mode the hardware also supports. Two
  ways to poll: scan() measures every pin and waits until it's done,
  which is simplest; or startScan() begins one and update(), called
  every pass through loop(), moves it along and reports when it's
  done, so the sketch never waits. The CPU has to step in between
  pins (to give the hardware each pin's settings, then take its
  result), but the hardware waits for it at each step, so a loop()
  that calls update() less often only makes a scan take longer.
  Measured on the Thing Plus with two pins: scan() takes about 400us,
  most of it waiting - update() calls add up to about 40us of real work
  per scan.

  Automatic baseline tracking: the CTSU hardware only reports a raw
  graduated count (readRaw()) - it has no concept of "touched" and no
  ability to recalibrate itself. An untouched baseline drifts with
  temperature, humidity, and whatever's near the wire (a finger versus,
  say, a houseplant or a piece of produce touching it instead read very
  differently), so isTouched() can't just compare readRaw() against one
  fixed number forever. Instead, each scan() nudges a slowly-adapting
  per-pin baseline (readBaseline()) toward the current raw reading -
  but only while the pin isn't currently considered touched, so a long,
  genuine touch-and-hold doesn't slowly get absorbed as "the new
  normal" while it's happening. isTouched() is then readRaw() compared
  against readBaseline() plus setThreshold()'s delta, not an absolute
  value. If isTouched() somehow stays true continuously for longer than
  getStuckTimeout() (default 60 seconds) - something resting on the
  sensor rather than a real touch, which is exactly what a long-lived
  conductive object left wired up looks like - scan() assumes the
  baseline itself has genuinely changed and force-recalibrates around
  the current reading instead of reporting touched forever.
*/

#ifndef RA6M5CapTouch_h
#define RA6M5CapTouch_h

#include <Arduino.h>

class CapTouch {
  public:
    // pin: an Arduino pin whose port has a touch channel - see the class
    // comment above for how that's decided (and which pins that means on
    // the Thing Plus), and for TSCAP's capacitor requirement, which the
    // whole peripheral depends on regardless of which sensing pin(s) you
    // use.
    CapTouch(uint8_t pin);

    // One-time setup for this pin: brings up the shared CTSU hardware on
    // the first call from any CapTouch instance, then registers this
    // pin as one of the channels future scan() calls will measure.
    // Returns false if the pin's port has no touch channel, or another
    // CapTouch instance already claimed it.
    bool begin();

    // Raw graduated reading from the most recently completed scan()  -
    // roughly proportional to sensed capacitance. Doesn't trigger a new
    // measurement itself; reflects whichever scan() last ran.
    uint16_t readRaw() const { return _raw; }

    // The CTSU's reference counter from that same scan - a compensation
    // baseline from a matched internal oscillator, meant to help cancel
    // out drift common to both (temperature, supply voltage) rather
    // than indicate anything about touch on its own. Exposed in case
    // readRaw() - readReference() turns out to be a cleaner signal than
    // readRaw() alone for your specific wiring; not used by isTouched()
    // below, which works directly off readRaw().
    uint16_t readReference() const { return _reference; }

    // This pin's slowly-adapting "untouched" baseline - see the class
    // comment on automatic baseline tracking. Reflects whichever
    // scan() last ran, like readRaw(); useful mainly for plotting
    // alongside readRaw() to see the tracking happen (see
    // the CapTouchPlotter example).
    uint16_t readBaseline() const { return (uint16_t)_baseline; }

    // A cached decision from the most recently completed scan() -
    // readRaw() compared against readBaseline() plus setThreshold()'s
    // delta - not something recomputed live at call time.
    bool isTouched() const { return _touched; }

    // How far above the baseline (see readBaseline()) counts as
    // touched. Not an absolute reading - the default is a starting
    // point, not a calibrated value for your setup. Confirmed on this
    // project's own bench wiring (short jumper wires soldered to
    // D20/D21, ~10nF TSCAP capacitor): baseline settled around
    // 9100-9200, and touches pushed readRaw() 5000-12000 counts above
    // that. Watch readRaw() and readBaseline() together with and
    // without a touch (the Serial Plotter example plots both) and
    // adjust if isTouched() doesn't track what you'd expect.
    void setThreshold(uint16_t delta) { _threshold = delta; }
    uint16_t getThreshold() const { return _threshold; }

    // If isTouched() stays continuously true for longer than this,
    // scan() assumes the baseline itself has genuinely changed - not a
    // real touch-and-hold - and force-recalibrates (see recalibrate())
    // instead of reporting touched forever. Default 60 seconds.
    void setStuckTimeout(unsigned long ms) { _stuckTimeoutMs = ms; }
    unsigned long getStuckTimeout() const { return _stuckTimeoutMs; }

    // Immediately sets this pin's baseline to its most recent raw
    // reading and clears isTouched(). scan() calls this on its own
    // once getStuckTimeout() elapses; call it directly for an explicit
    // "recalibrate now" gesture in your own sketch (e.g. a boot-time
    // calibration step, or a button the user can press while not
    // touching the sensor).
    void recalibrate();

    // Triggers one measurement across every registered CapTouch pin at
    // once - the hardware measures them together in a single pass, it
    // can't scan just one in isolation. Blocks until done (about 200us
    // per registered pin, and that again for each extra repeat - see
    // setMeasurementCount()). Call this once per loop() pass, same
    // polling pattern as RV112FF.
    static void scan();

    // The same scan without waiting for it. startScan() begins one (and
    // does nothing if one is already under way); update() then does
    // whatever the hardware is waiting for and returns at once - true
    // just once, when the scan has finished and every pin's readings
    // are new. Call update() on every pass through loop():
    //
    //   if (!CapTouch::scanning()) CapTouch::startScan();
    //   if (CapTouch::update()) { ...readRaw(), isTouched()... }
    static void startScan();
    static bool update();
    static bool scanning() { return s_scanning; }

    // How many times the hardware repeats its measurement of each pin
    // and adds up the results, 1-64 (default 1). More is quieter but
    // takes proportionally longer - and readRaw() grows in proportion
    // too, so scale setThreshold() with it, and watch overflowed(): with
    // nothing subtracting the pad's background reading, a few repeats
    // are enough to fill the 16-bit counter. Takes effect from the next
    // scan, which re-centres every pin's baseline on its new reading.
    static void setMeasurementCount(uint8_t count);
    static uint8_t getMeasurementCount() { return s_measurementCount; }

    // True if a pin's reading in the most recent scan hit the counter's
    // ceiling (65535), so it reads lower than it really is.
    static bool overflowed() { return s_overflowed; }

    // How many scans have been abandoned because the hardware stopped
    // making progress. A stuck scan is stopped and the hardware's
    // control block reset, so the next scan starts clean; the pins
    // keep their previous readings.
    static uint32_t scanFailures() { return s_scanFailures; }

  private:
    uint8_t _pin;
    int8_t _tsChannel = -1;  // set by begin(); -1 means not registered
    uint16_t _threshold = DEFAULT_THRESHOLD;
    uint16_t _raw = 0;
    uint16_t _reference = 0;
    bool _touched = false;

    // Baseline tracking state - see the class comment. _baseline is a
    // float rather than matching readRaw()'s uint16_t: the exponential
    // moving average below nudges it by a fraction of a count on most
    // calls, and integer truncation would silently round that away and
    // let the baseline get stuck (this bit us in early testing of this
    // approach), where a float just accumulates the fractional part
    // correctly for free - this chip has a hardware FPU, so it costs
    // nothing here.
    float _baseline = 0;
    bool _baselineInitialized = false;
    unsigned long _lastBaselineUpdateMillis = 0;
    unsigned long _touchStartMillis = 0;  // 0 = not currently mid-touch
    unsigned long _stuckTimeoutMs = DEFAULT_STUCK_TIMEOUT_MS;

    // How long, in milliseconds, the baseline takes to close roughly
    // 63% of the gap to a new steady reading (a standard first-order
    // low-pass time constant) - long enough to ignore normal sensor
    // noise between scans, short enough to track real environmental
    // drift (temperature, humidity, a different object resting on the
    // wire) within a few seconds. Not exposed for tuning - unlike
    // getStuckTimeout(), which is about how long a genuine touch could
    // plausibly last, this is closer to an implementation detail.
    static const unsigned long BASELINE_SMOOTHING_MS = 2000;

    static const uint16_t DEFAULT_THRESHOLD = 3000;
    static const unsigned long DEFAULT_STUCK_TIMEOUT_MS = 60000;

    // One instance per touch channel at most (begin() refuses a second
    // claim on the same channel), and the RA6M5 has 20.
    static const uint8_t MAX_INSTANCES = 20;
    static CapTouch *s_instances[MAX_INSTANCES];
    static uint8_t s_instanceCount;
    static bool s_hardwareReady;

    // The scan under way, if any - see update().
    static bool s_scanning;
    static uint8_t s_resultsRead;      // pins whose result has been read
    static uint8_t s_settingsWritten;  // pins the hardware has been given settings for
    static unsigned long s_lastProgressMicros;
    static uint8_t s_measurementCount;
    static uint8_t s_scanMeasurementCount;  // the count the scan under way started with
    static bool s_recentrePending;     // set by setMeasurementCount()
    static bool s_overflowed;
    static uint32_t s_scanFailures;

    static void initHardwareOnce();
    static bool lookupTsChannel(uint8_t pin, uint8_t &tsChannel);

    // Called from update() with a freshly-read measurement for this
    // instance: updates readRaw()/readReference(), decides
    // isTouched(), and advances (or freezes, or resets) the baseline -
    // see the class comment on automatic baseline tracking.
    void finishReading(uint16_t raw, uint16_t reference);
};

#endif
