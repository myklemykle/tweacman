// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  RV112FF.h - Taiwan Alpha RV112FF-40 dual 360-degree potentiometer

  The RV112FF-40 is a single-turn potentiometer with two independent
  wipers, mechanically offset 90 degrees from each other on the same
  shaft. Wire its two track ends to a supply and ground, and its two
  wipers to two analog input pins: as the shaft turns, each wiper's
  voltage traces a triangle wave, and the two waves are 90 degrees out
  of phase with each other - like a pair of quadrature signals, just
  triangular rather than sinusoidal. That's enough to reconstruct an
  absolute shaft angle with plain trigonometry, no moving reference
  point and no interrupts required.

  API style follows PJRC's Encoder library - read() returns the current
  position, polled from loop() rather than interrupt-driven - with two
  differences that follow from the hardware itself rather than taste:

    - read() returns a float in degrees [0, 360), not an accumulating
      integer count. This pot has no multi-turn memory: it's a single-
      turn absolute sensor, so "position" means "where the shaft is
      right now," not "how far it's been turned since power-on."

    - There's no write(). An encoder's count is arbitrary and needs an
      external reference to mean anything, so write() lets you set that
      reference. This pot's angle is read directly off where the shaft
      physically is; there's nothing meaningful to set it to.

  Self-calibrating: each wiper's real-world voltage range varies with
  the individual pot and its wiring, so this tracks the observed
  min/max of each wiper as the shaft turns and uses that instead of
  assuming a fixed range. read() returns -1 until calibration is
  complete: the shaft has turned one full revolution since either
  wiper's observed range last grew. A full turn carries each wiper
  through both of its extremes, so a turn that finds nothing new means
  the whole range has been seen - in practice, about two turns from
  power-on. To skip that at every boot, save the calibration once with
  getCalibration() and restore it in setup() with setCalibration().

  Written for boards with a 12-bit ADC (calls analogReadResolution(12)
  on first use, regardless of what the rest of the sketch has set it
  to, for the best angular precision this hardware can give).

  Ported from a working implementation on a different platform (Bela) -
  see that project's notes for the original derivation and a specific
  pitfall worth knowing about if you modify the calibration logic:
  don't let the calibration envelope decay back toward the live signal.
  It seems like a reasonable way to let the calibration "self-heal,"
  but it makes an idle knob's reported angle drift, because the
  envelope's center slides toward wherever the knob happens to be
  sitting rather than staying anchored to the shaft's real range.

  A note on NVM wear, if you're persisting getCalibration()'s result
  (e.g. with the built-in EEPROM library, which is backed by this
  board's dedicated 8KB data flash rather than the much larger code
  flash): that storage doesn't wear-level, and EEPROM.put() only skips
  a write when the new bytes are byte-for-byte identical to what's
  already there. That's no help here, since the calibration envelope
  legitimately changes a little on almost every read() while a shaft is
  actively being calibrated. Call getCalibration() and save its result
  deliberately - once calibration completes, on a button press, rate-
  limited - not on every read().
*/

#ifndef RV112FF_h
#define RV112FF_h

#include <Arduino.h>

// Snapshot of a pot's calibration state, for storing in NVM (e.g. via
// EEPROM.put()/EEPROM.get() - see getCalibration()/setCalibration() below)
// and restoring after a reboot, so the sketch doesn't need the user to
// sweep the shaft through a full rotation again every power-on.
//
// magic and version aren't meant to be inspected by sketches - they let
// setCalibration() tell "this really is calibration data, written by a
// version of this library that laid it out the way I expect" apart from
// "this is erased flash, garbage, or an incompatible version" and refuse
// it rather than trust something it can't verify. checksum catches a
// write that got interrupted partway (e.g. power loss mid-save) -
// deliberately a plain byte sum rather than a real CRC, since the
// failure modes worth guarding against here are "garbage" and "partial
// write," not subtle single-bit corruption.
struct RV112FFCalibration {
  uint32_t magic;
  uint8_t version;
  uint8_t checksum;
  uint8_t complete;  // 1 if this snapshot was taken after calibration completed
  float minVal[2];
  float maxVal[2];
};

class RV112FF {
  public:
    // wiperPinA, wiperPinB: the two analog pins wired to the pot's wipers.
    // smoothTau: time constant (seconds) of the lowpass filter smoothing
    //   each wiper's raw reading - the main defense against ADC noise.
    // minCalibrationRange: fraction (0-1) of full-scale each wiper's
    //   smoothed signal must have swept before an angle can be worked out
    //   at all - the point where counting the full turn that completes
    //   calibration (see isCalibrated()) can begin.
    // rpmSmoothTau: time constant (seconds) of rpm()'s own separate
    //   smoothing filter (see that method's comment). The default
    //   (0.15s) was tuned for using this pot as a position control,
    //   where rpm() is a secondary signal - a use that's read directly
    //   as an audio-rate control input (e.g. a tape-scrub effect) will
    //   likely want this much smaller, since a 150ms decay reads as
    //   noticeable "momentum" once rpm() itself is driving something
    //   audibly in real time.
    RV112FF(uint8_t wiperPinA, uint8_t wiperPinB,
            float smoothTau = 0.02, float minCalibrationRange = 0.15,
            float rpmSmoothTau = 0.15);

    // Samples both wipers, updates calibration and the speed estimate,
    // and returns the current shaft angle in degrees [0, 360). Returns
    // -1 if not yet calibrated (see the class comment above). Call this
    // once per loop() pass; rpm() reports the speed computed as of the
    // most recent call here rather than sampling on its own.
    float read();

    // Rotational speed in RPM, signed: positive and negative indicate
    // opposite directions of rotation. Reflects the state as of the
    // last read() call. Lightly smoothed on its own, separately from
    // the position filter - speed is a derivative and amplifies noise
    // more than position does. Returns 0 before calibration.
    float rpm();

    // One wiper's reading as of the last read(), 0.0 to 1.0 of the ADC's
    // range: channel 0 is wiperPinA, 1 is wiperPinB. These are the
    // smoothed values the angle is decoded from, available even before
    // calibration completes - handy for plotting, to see what the pot
    // itself is doing.
    float wiper(uint8_t channel) const { return _smooth[channel ? 1 : 0]; }

    // True once calibration is complete - the shaft has turned a full
    // revolution since either wiper's observed range last grew (see the
    // class comment) - or a complete calibration has been restored with
    // setCalibration(). Until then read() returns -1 and rpm() returns 0.
    // Once true it stays true until resetCalibration(). The observed
    // range can still widen a little after that, and read() uses it.
    bool isCalibrated() const { return _calibrated; }

    // Forgets the observed min/max range and starts calibrating again
    // from the next read(). Not needed in normal use - the shaft only
    // has one range to learn - but useful if you suspect a bad reading
    // latched into the envelope before the pot had settled (e.g. right
    // after a noisy power-on).
    void resetCalibration();

    // Returns a snapshot of the current calibration, ready to hand to
    // something like EEPROM.put() for storage across a reboot. Save it
    // once isCalibrated() is true. A snapshot taken earlier is still
    // valid, but restoring it just resumes calibrating from that point,
    // with read() returning -1 until another full turn completes it.
    RV112FFCalibration getCalibration() const;

    // Restores a previously-saved calibration (see getCalibration()).
    // Returns true and adopts it if it passes validation (magic, version,
    // checksum, and a basic sanity check on the ranges); otherwise leaves
    // the current calibration state untouched and returns false. Safe to
    // call with anything - freshly-erased flash, a different pot's data,
    // data from an incompatible future version of this library - it will
    // simply refuse rather than trust something it can't verify. Deciding
    // *when* to call this, and when to call getCalibration() to persist
    // one, is left to the sketch - see the class comment's note on NVM
    // wear if you're saving what this returns.
    bool setCalibration(const RV112FFCalibration &cal);

    // How many extra conversions to make, and throw away, before each
    // wiper reading that counts. Default 1.
    //
    // Why: most microcontrollers' ADCs measure a pin by charging one small
    // internal capacitor shared by every analog pin, and a pot's wiper is a
    // high-impedance source that charges it slowly - so a reading can come
    // out pulled toward whichever pin was measured just before it. With
    // two pots on neighbouring pins, turning one then makes the other seem
    // to move. A throwaway conversion first is the standard portable
    // remedy, and costs a few microseconds per wiper.
    //
    // Set it to 0 if your board fixes this properly in hardware - e.g.
    // this project's RA6M5AnalogTuning library, which lengthens the ADC's
    // sampling time instead and measured as removing it outright, making
    // the extra conversion pure overhead. Raise it if you measure crosstalk
    // left over on a board without such an option.
    void setSettlingReads(uint8_t reads) { _settlingReads = reads; }
    uint8_t getSettlingReads() const { return _settlingReads; }

    // How many readings of each wiper to average into one, taken back to
    // back. Default 1 (no averaging). Averaging N readings cuts fast,
    // random ADC noise by about the square root of N - 4 halves it - at
    // the cost of N conversions instead of one, a few microseconds each on
    // most boards. It adds no lag, since all N are taken at once: unlike
    // raising smoothTau, it doesn't make the knob respond more slowly.
    // It can't reduce slow noise such as mains hum, which looks the same
    // in all N readings. Worth raising when rpm() drives something that
    // needs to sit perfectly still at rest.
    void setOversampling(uint8_t readings) { _oversampling = (readings < 1) ? 1 : readings; }
    uint8_t getOversampling() const { return _oversampling; }

  private:
    uint8_t _pinA, _pinB;
    float _smoothTau;
    float _minCalibrationRange;
    float _rpmSmoothTau;
    uint8_t _settlingReads = 1;
    uint8_t _oversampling = 1;

    float _smooth[2];
    float _minVal[2];
    float _maxVal[2];

    float _angle = -1;
    float _prevAngle = -1;
    float _rpm = 0;
    bool _calibrated = false;

    // Progress toward a complete calibration: the observed range as it
    // stood when the current turn started counting, and the lowest and
    // highest unwrapped angle reached since then. See read().
    float _markMin[2];
    float _markMax[2];
    float _turnPos = 0;
    float _turnLow = 0;
    float _turnHigh = 0;

    // How far, as a fraction of full scale, a wiper's observed range may
    // grow without restarting the turn count. The smoothed signal creeps
    // up on a peak in tiny steps, and noise can nudge it, so any growth
    // at all would make a turn that finds nothing new hard to finish.
    static constexpr float RANGE_GROWTH_TOLERANCE = 0.005;
    bool _begun = false;
    unsigned long _lastMicros = 0;

    static constexpr float ADC_MAX_VALUE = 4095.0;  // 2^12 - 1

    static constexpr uint32_t CALIBRATION_MAGIC = 0x52563132;  // ASCII "RV12"
    static constexpr uint8_t CALIBRATION_VERSION = 2;

    float readWiper(uint8_t pin, uint8_t channel, float dtSeconds);
    bool rangeGrewSinceMark() const;
    void restartTurnCount();

    static uint8_t sumBytes(const void *data, size_t len, uint8_t sum);
    uint8_t calibrationChecksum(const RV112FFCalibration &cal) const;
};

#endif
