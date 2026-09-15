#include "MultiTurnKnob.h"

MultiTurnKnob::MultiTurnKnob(RV112FF &pot) : _pot(pot) {
}

float MultiTurnKnob::read() {
  float angle = _pot.read();

  if (angle < 0) {
    // Wrapped pot hasn't finished calibrating yet - nothing real to
    // accumulate from. Match RV112FF::read()'s own not-ready signal
    // rather than returning 0, which would be indistinguishable from a
    // genuine logical position 0.
    return -1;
  }

  if (!_initialized) {
    // First real angle: this becomes the reference point for future
    // deltas. Deliberately doesn't touch _accumulated - if write() was
    // already called (e.g. once in setup(), before calibration finished),
    // that starting value is preserved rather than reset to 0 here.
    _prevAngle = angle;
    _initialized = true;
    return _accumulated;
  }

  // How far the shaft moved since the last read(), the short way around.
  // A single-sample jump of more than half a revolution can't be a real
  // motion at any sane polling rate - see the class comment - so treat
  // it as having crossed the 0/360 seam instead, and credit/debit a full
  // turn to land on the same short-way delta a continuous position
  // would have.
  float delta = angle - _prevAngle;
  if (delta > 180.0f) {
    delta -= 360.0f;
  } else if (delta < -180.0f) {
    delta += 360.0f;
  }

  _accumulated += delta;
  _prevAngle = angle;
  return _accumulated;
}

void MultiTurnKnob::write(float value) {
  _accumulated = value;
  // _prevAngle is deliberately left alone: it's only ever used to measure
  // future motion relative to wherever the shaft physically is, which
  // this call doesn't change - only what the *current* position is
  // called changes.
}
