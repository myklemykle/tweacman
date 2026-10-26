// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

#include "RV112FF.h"
#include <math.h>

RV112FF::RV112FF(uint8_t wiperPinA, uint8_t wiperPinB,
                  float smoothTau, float rpmSmoothTau)
  : _pinA(wiperPinA), _pinB(wiperPinB),
    _smoothTau(smoothTau), _rpmSmoothTau(rpmSmoothTau) {
  _smooth[0] = _smooth[1] = 0.5;
  _minVal[0] = _minVal[1] = 1.0;
  _maxVal[0] = _maxVal[1] = 0.0;
  restartTurnCount();
  // Deliberately no hardware access here - a global "RV112FF pot(A2, A3);"
  // runs its constructor before setup(), and this board's ADC isn't
  // guaranteed ready yet at that point. analogReadResolution() below is
  // called lazily instead, from the first real read().
}

void RV112FF::resetCalibration() {
  _minVal[0] = _minVal[1] = 1.0;
  _maxVal[0] = _maxVal[1] = 0.0;
  _calibrated = false;
  _angle = -1;
  _prevAngle = -1;
  _rpm = 0;
  restartTurnCount();
}

// Starts counting a fresh turn from the observed range as it stands now.
void RV112FF::restartTurnCount() {
  for (int i = 0; i < 2; i++) {
    _markMin[i] = _minVal[i];
    _markMax[i] = _maxVal[i];
  }
  _turnPos = _turnLow = _turnHigh = 0;
}

// Whether either wiper's observed range has grown, by more than the
// tolerance, since the current turn started counting.
bool RV112FF::rangeGrewSinceMark() const {
  for (int i = 0; i < 2; i++) {
    if (_minVal[i] < _markMin[i] - RANGE_GROWTH_TOLERANCE) return true;
    if (_maxVal[i] > _markMax[i] + RANGE_GROWTH_TOLERANCE) return true;
  }
  return false;
}

float RV112FF::readWiper(uint8_t pin, uint8_t channel, float dtSeconds) {
  // Throwaway conversions first, to let the ADC settle on this pin - see
  // setSettlingReads() in RV112FF.h for why, and when to change how many.
  //
  // Measured on the bench with two RV112FFs on one board: without them,
  // turning one pot biased the *other* pot's reported speed in the opposite
  // direction - mean -0.304 RPM while turning one way and +0.407 the other,
  // with excursions to +/-2.5 RPM - which is audible when the second pot's
  // rpm() drives anything in real time. The pots' wiring was confirmed
  // physically separate, so this is inside the converter, not crosstalk
  // between wires.
  for (uint8_t i = 0; i < _settlingReads; i++) {
    analogRead(pin);
  }
  // Average setOversampling()'s worth of readings into one.
  long sum = 0;
  for (uint8_t i = 0; i < _oversampling; i++) {
    sum += analogRead(pin);
  }
  float normalized = (float)sum / _oversampling / ADC_MAX_VALUE;

  // One-pole lowpass with a real time constant, not a fixed per-call
  // coefficient - this is polled from loop() at whatever rate the
  // sketch happens to run at, so the coefficient is recomputed from the
  // actual elapsed time each call, to mean the same thing regardless of
  // how often read() gets called.
  if (dtSeconds <= 0) {
    // First call, or clock hasn't advanced: nothing to smooth toward yet.
    _smooth[channel] = normalized;
  } else {
    float coeff = 1.0 - exp(-dtSeconds / _smoothTau);
    _smooth[channel] += coeff * (normalized - _smooth[channel]);
  }

  // Track the observed extremes of the *smoothed* signal, not the raw
  // one - the smoothing already rejects spikes, so anything that moves
  // these envelopes is a real, if slow, part of the pot's actual range.
  // Deliberately no decay back toward the live signal: see the class
  // comment in RV112FF.h for why that seemingly-reasonable idea causes
  // an idle knob's reported angle to drift. Once a bound is set, it stays.
  if (_smooth[channel] < _minVal[channel]) _minVal[channel] = _smooth[channel];
  if (_smooth[channel] > _maxVal[channel]) _maxVal[channel] = _smooth[channel];

  return _smooth[channel];
}

float RV112FF::read() {
  if (!_begun) {
    // Every bit the ADC has, regardless of what any other code in the
    // sketch may have set analogReadResolution() to - see the class
    // comment. Safe to call more than once (e.g. with two RV112FF
    // instances) - it's a board-wide setting, and re-asserting the same
    // value is a no-op.
#ifndef RV112FF_FIXED_ADC_RESOLUTION
    analogReadResolution(RV112FF_ADC_BITS);
#endif
    _begun = true;
  }

  unsigned long nowMicros = micros();
  float dtSeconds = (_lastMicros == 0) ? 0 : (nowMicros - _lastMicros) / 1000000.0;

  float a = readWiper(_pinA, 0, dtSeconds);
  float b = readWiper(_pinB, 1, dtSeconds);

  // An angle can be worked out once each wiper has swept some of its
  // range, but it's only accurate once the whole range has been seen -
  // until then it's tracked here, to count the turn that completes
  // calibration, but not reported.
  bool rangeOkA = (_maxVal[0] - _minVal[0]) >= _minCalibrationRange;
  bool rangeOkB = (_maxVal[1] - _minVal[1]) >= _minCalibrationRange;

  if (rangeOkA && rangeOkB) {
    // Center and normalize each wiper into roughly [-1, 1] using its
    // own observed range, turning the pair of triangle waves into
    // something atan2() can treat like a quadrature pair.
    float x = 2.0 * (a - _minVal[0]) / (_maxVal[0] - _minVal[0]) - 1.0;
    float y = 2.0 * (b - _minVal[1]) / (_maxVal[1] - _minVal[1]) - 1.0;
    float angle = fmod(360.0 - atan2(y, x) * 180.0 / PI, 360.0);

    if (_prevAngle >= 0) {
      // Shortest signed distance around the circle, so crossing the
      // 359-to-0 seam reads as a small step rather than a 359-degree
      // spike in the wrong direction.
      float delta = angle - _prevAngle;
      while (delta > 180.0) delta -= 360.0;
      while (delta < -180.0) delta += 360.0;

      if (dtSeconds > 0.0001) {
        float instantRpm = (delta / dtSeconds) / 360.0 * 60.0;
        float rpmCoeff = 1.0 - exp(-dtSeconds / _rpmSmoothTau);
        _rpm += rpmCoeff * (instantRpm - _rpm);
      }

      // The span of angles visited since the turn count started, rather
      // than the distance travelled: turning back and forth over the same
      // half turn shouldn't count as having seen the whole range.
      if (!_calibrated) {
        _turnPos += delta;
        if (_turnPos < _turnLow) _turnLow = _turnPos;
        if (_turnPos > _turnHigh) _turnHigh = _turnPos;
      }
    }
    _prevAngle = angle;

    if (!_calibrated) {
      if (rangeGrewSinceMark()) {
        restartTurnCount();
      } else if (_turnHigh - _turnLow >= 360.0) {
        _calibrated = true;
      }
    }
    _angle = _calibrated ? angle : -1;
  } else {
    _angle = -1;
    _prevAngle = -1;
  }

  _lastMicros = nowMicros;
  return _angle;
}

float RV112FF::rpm() {
  return _calibrated ? _rpm : 0.0;
}

uint8_t RV112FF::sumBytes(const void *data, size_t len, uint8_t sum) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; i++) sum += bytes[i];
  return sum;
}

uint8_t RV112FF::calibrationChecksum(const RV112FFCalibration &cal) const {
  // Deliberately not over the whole struct - magic/version are constants
  // and checksumming them adds nothing, and reading raw bytes across the
  // struct would depend on compiler padding between fields, which the
  // min/max arrays' own sizeof() sidesteps entirely.
  uint8_t sum = 0;
  sum = sumBytes(cal.minVal, sizeof(cal.minVal), sum);
  sum = sumBytes(cal.maxVal, sizeof(cal.maxVal), sum);
  sum += cal.complete;
  return sum;
}

RV112FFCalibration RV112FF::getCalibration() const {
  RV112FFCalibration cal;
  cal.magic = CALIBRATION_MAGIC;
  cal.version = CALIBRATION_VERSION;
  cal.minVal[0] = _minVal[0];
  cal.minVal[1] = _minVal[1];
  cal.maxVal[0] = _maxVal[0];
  cal.maxVal[1] = _maxVal[1];
  cal.complete = _calibrated ? 1 : 0;
  cal.checksum = calibrationChecksum(cal);
  return cal;
}

bool RV112FF::setCalibration(const RV112FFCalibration &cal) {
  if (cal.magic != CALIBRATION_MAGIC) return false;
  if (cal.version != CALIBRATION_VERSION) return false;
  if (cal.checksum != calibrationChecksum(cal)) return false;

  // Belt and suspenders on top of the checksum: a technically-intact but
  // physically nonsensical range (inverted, or outside what a normalized
  // ADC reading can ever produce) shouldn't be trusted either.
  for (int i = 0; i < 2; i++) {
    if (cal.minVal[i] < 0.0 || cal.minVal[i] > 1.0) return false;
    if (cal.maxVal[i] < 0.0 || cal.maxVal[i] > 1.0) return false;
    if (cal.maxVal[i] <= cal.minVal[i]) return false;
    // A complete calibration has seen far more than the minimum range.
    if (cal.complete && cal.maxVal[i] - cal.minVal[i] < _minCalibrationRange) return false;
  }
  if (cal.complete > 1) return false;

  _minVal[0] = cal.minVal[0];
  _minVal[1] = cal.minVal[1];
  _maxVal[0] = cal.maxVal[0];
  _maxVal[1] = cal.maxVal[1];

  // A complete snapshot is trusted straight away; an incomplete one just
  // gives calibration a head start, still needing a full turn to finish.
  _calibrated = (cal.complete == 1);
  restartTurnCount();

  return true;
}
