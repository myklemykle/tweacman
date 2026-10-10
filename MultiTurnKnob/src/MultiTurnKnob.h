// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  MultiTurnKnob.h - turns an RV112FF into an Encoder-style multi-turn counter

  The RV112FF-40 (see RV112FF.h) is an "endless rotary" potentiometer -
  confirmed against its manufacturer datasheet, no mechanical stop, rated
  for 15,000 rotation cycles - but the sensor itself has no memory of how
  many times it's been spun: RV112FF::read() only ever reports where the
  shaft is *right now*, wrapped to [0, 360).

  MultiTurnKnob adds that memory on top, without changing RV112FF itself:
  each read() compares the pot's new angle against its previous one, and
  if the shaft has crossed the 0/360 seam - detected as a jump of more
  than 180 degrees, which a real single-sample motion can't produce at any
  sane polling rate - it credits or debits a full revolution instead of
  the raw (and wrong) short way around. The result is an accumulated
  position that keeps counting past +-360 for as many turns as you like,
  the same shape as PJRC's Encoder library, which is where read()/write()
  and the overall API style are borrowed from.

  This only works if read() is called often enough that the shaft can't
  plausibly turn more than 180 degrees between two calls - true at any
  sane polling rate for a hand-turned knob (this project's own demos poll
  at ~100Hz; even a fast flick of a small panel pot doesn't complete a
  full revolution in under ~150ms).

  Wraps an RV112FF by reference rather than owning one - construct your
  own RV112FF first (same as any other sketch would) and hand it to
  MultiTurnKnob. Calls the wrapped pot's read() internally, so don't also
  call it yourself elsewhere - see RV112FF.h's note that its read() (and
  the rpm() it feeds) expects to be called once per loop() pass, and
  MultiTurnKnob::read() is what does that here.

  write(), unlike RV112FF's own deliberate lack of one, makes sense here:
  once there's an accumulated position rather than just a physical
  reading, "what does the current position mean" becomes an arbitrary
  choice again - same as resetting an Encoder's count.
*/

#ifndef MultiTurnKnob_h
#define MultiTurnKnob_h

#include <Arduino.h>
#include <RV112FF.h>

class MultiTurnKnob {
  public:
    // pot: an already-constructed RV112FF to wrap. Kept by reference, not
    // copied or owned - the sketch is still responsible for constructing
    // it (with begin()/pins as usual) and keeping it alive.
    MultiTurnKnob(RV112FF &pot);

    // Samples the wrapped pot (calling its read() - see the class
    // comment) and returns the accumulated position in degrees, free to
    // go past +-360 as the shaft turns multiple times. Returns -1, like
    // RV112FF::read() itself, until the wrapped pot has finished its own
    // self-calibration.
    float read();

    // Redefines the current physical position as this logical value -
    // matching Encoder::write(). Doesn't touch the wrapped RV112FF or
    // require a fresh read() first; takes effect on the next read().
    void write(float value);

    // The wrapped pot's own single-turn angle [0, 360) as of the most
    // recent read() - the same value RV112FF::read() would return, just
    // without calling it again (see the class comment on why not to).
    // Mainly useful for showing both alongside each other, e.g. on a
    // plotter - see SerialPlotterDemo.
    float lastAngle() const { return _prevAngle; }

  private:
    RV112FF &_pot;
    float _accumulated = 0;
    float _prevAngle = -1;
    bool _initialized = false;  // true once a first real angle is known
};

#endif
