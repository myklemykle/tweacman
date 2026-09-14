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
  assuming a fixed range. read() returns -1 until the shaft has swept
  enough of its range to trust the calibration - in practice, about one
  full rotation from power-on.

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

  This example code is in the public domain.
*/

#ifndef RV112FF_h
#define RV112FF_h

#include <Arduino.h>

class RV112FF {
  public:
    // wiperPinA, wiperPinB: the two analog pins wired to the pot's wipers.
    // smoothTau: time constant (seconds) of the lowpass filter smoothing
    //   each wiper's raw reading - the main defense against ADC noise.
    // minCalibrationRange: fraction (0-1) of full-scale a wiper's smoothed
    //   signal must have swept before its calibration is trusted.
    RV112FF(uint8_t wiperPinA, uint8_t wiperPinB,
            float smoothTau = 0.02, float minCalibrationRange = 0.15);

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

    // True once both wipers have swept enough range for read() to
    // return a real angle instead of -1.
    bool isCalibrated() const { return _calibrated; }

    // Forgets the observed min/max range and starts calibrating again
    // from the next read(). Not needed in normal use - the shaft only
    // has one range to learn - but useful if you suspect a bad reading
    // latched into the envelope before the pot had settled (e.g. right
    // after a noisy power-on).
    void resetCalibration();

  private:
    uint8_t _pinA, _pinB;
    float _smoothTau;
    float _minCalibrationRange;

    float _smooth[2];
    float _minVal[2];
    float _maxVal[2];

    float _angle = -1;
    float _prevAngle = -1;
    float _rpm = 0;
    bool _calibrated = false;
    bool _begun = false;
    unsigned long _lastMicros = 0;

    static constexpr float RPM_SMOOTH_TAU = 0.15;  // seconds
    static constexpr float ADC_MAX_VALUE = 4095.0;  // 2^12 - 1

    float readWiper(uint8_t pin, uint8_t channel, float dtSeconds);
};

#endif
