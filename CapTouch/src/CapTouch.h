/*
  CapTouch.h - capacitive touch sensing on the RA6M5's built-in CTSU

  The RA6M5 has a real hardware peripheral for this (the Capacitive Touch
  Sensing Unit, or CTSU) - not a software RC-timing trick like classic
  Arduino capacitive-sense libraries use. It measures a graduated value
  roughly proportional to sensed capacitance, not just touched/not-touched;
  isTouched() below is a threshold comparison built on top of that, not the
  only thing the hardware can tell you.

  Only three physical pins on this board are wired to real CTSU channels
  (confirmed against the RA6M5 Hardware User's Manual, Table 1.17 - not
  guessed): D20 (TS05), D21 (TS04), and D31 (TS11, which is also the
  board's onboard USR button - touch-sensing that pin picks up whatever's
  mechanically wired there too, so it's included for completeness but not
  a great choice for a breadboard wire). A fourth pin, D6, is TSCAP - a
  shared analog reference the whole peripheral depends on, not a sensing
  channel of its own. It needs an external capacitor to ground (10nF is a
  reasonable starting point; up to 100nF is fine, the exact value isn't
  critical) wired directly to D6, or nothing here will work.

  This is a clean-room implementation written directly against Renesas'
  RA6M5 Hardware User's Manual (chapter 46, "Capacitive Touch Sensing
  Unit"), not adapted from any third-party library. A well-regarded
  community library exists for the Arduino UNO R4's version of this same
  peripheral (R4_Touch by delta-G) and was useful to know about while
  planning this - its README's capacitor recommendation, and confirming a
  from-scratch approach was even feasible, both saved real time - but it's
  GPLv3-licensed, and this project intends to stay public domain, so
  nothing here is derived from its code.

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

  Implemented as a blocking, polled scan() (typically well under a
  millisecond per registered pin) - matching this project's established
  preference for polling over interrupts (see RV112FF) - rather than the
  free-running, interrupt-driven mode the hardware also supports.

  This example code is in the public domain.
*/

#ifndef CapTouch_h
#define CapTouch_h

#include <Arduino.h>

class CapTouch {
  public:
    // pin: a touch-capable pin - see the class comment above for which
    // three this board actually has, and D6/TSCAP's capacitor
    // requirement, which the whole peripheral depends on regardless of
    // which sensing pin(s) you use.
    CapTouch(uint8_t pin);

    // One-time setup for this pin: brings up the shared CTSU hardware on
    // the first call from any CapTouch instance, then registers this
    // pin as one of the channels future scan() calls will measure.
    // Returns false if the pin isn't one of the three touch-capable
    // pins, or another CapTouch instance already claimed it.
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

    // Convenience boolean: readRaw() compared against this instance's
    // threshold.
    bool isTouched() const { return _raw >= _threshold; }

    // The default threshold is a starting point, not a calibrated
    // value - it will need adjusting for your specific wire/pad and
    // environment. Watch readRaw() with nothing touching it versus
    // touching it (the Serial Plotter example does exactly this) and
    // set the threshold somewhere between the two.
    void setThreshold(uint16_t threshold) { _threshold = threshold; }
    uint16_t getThreshold() const { return _threshold; }

    // Triggers one measurement across every registered CapTouch pin at
    // once - the hardware measures them together in a single pass, it
    // can't scan just one in isolation. Blocks until done (typically
    // well under a millisecond per registered pin). Call this once per
    // loop() pass, same polling pattern as RV112FF.
    static void scan();

  private:
    uint8_t _pin;
    int8_t _tsChannel = -1;  // set by begin(); -1 means not registered
    uint16_t _threshold = DEFAULT_THRESHOLD;
    uint16_t _raw = 0;
    uint16_t _reference = 0;

    // A starting point only, not a calibrated value for your setup -
    // raw() readings depend on wire/electrode size and the TSCAP
    // capacitor, so an untouched baseline can land anywhere. Confirmed
    // on this project's own bench wiring (short jumper wires soldered
    // to D20/D21, ~10nF TSCAP capacitor): baseline ~9100-9200, touched
    // ~15000-21000+. Watch raw() with and without a touch and adjust
    // with setThreshold() if isTouched() doesn't track what you'd
    // expect - see SerialPlotterDemo.
    static const uint16_t DEFAULT_THRESHOLD = 12000;

    // The three known touch-capable pins on this board and their CTSU
    // channel numbers (TS00-TS19) - see the class comment for how these
    // were confirmed. Not exhaustive of the chip's 20 possible channels,
    // just of what this specific board actually wires up.
    struct KnownChannel {
      uint8_t pin;
      uint8_t tsChannel;
    };
    static const KnownChannel KNOWN_CHANNELS[3];

    static const uint8_t MAX_INSTANCES = 3;
    static CapTouch *s_instances[MAX_INSTANCES];
    static uint8_t s_instanceCount;
    static bool s_hardwareReady;

    static void initHardwareOnce();
    static bool lookupTsChannel(uint8_t pin, uint8_t &tsChannel);
};

#endif
