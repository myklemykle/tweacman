/*
  RingBuffer - a fixed-capacity, lock-free single-producer/single-consumer
  circular buffer of 16-bit PCM samples that can slide in EITHER direction.

  Used to hand raw audio data from the SD-reading producer in loop() to
  the DAC-writing consumer in the audio-rate interrupt (see
  SDResampleDemo.ino), for both forward and backward playback, without
  either side ever blocking on the other. CAPACITY must be a power of two
  so index wraparound is a cheap bitmask instead of a modulo.

  validLow_/validHigh_ mark the half-open range [validLow_, validHigh_) of
  samples currently buffered, in signed VIRTUAL SAMPLE POSITION terms -
  not array indices, and not real file offsets. "Virtual" because this
  position can go negative and can wrap around the source file's length
  many times over a long session; SDResampleDemo.ino's loop() is the only
  place that ever maps a virtual position onto a real, in-range file
  offset (see fileOffsetFor() there) - this buffer and WavReader both stay
  ignorant of that mapping entirely. The physical backing array is still
  indexed via the same `position & (CAPACITY-1)` bitmask trick regardless
  of sign or direction: (uint32_t)someNegativeInt32 is well-defined
  (int-to-unsigned conversion is modulo 2^32), unlike a negative float
  converted to unsigned, which is undefined behavior - see Resampler.h's
  comment on the bug that distinction fixes there.

  Both producer-facing edges (pushHigh/pushLow) are owned and moved only
  by the producer (loop()) - unlike the original forward-only version of
  this class, the consumer (the audio ISR) never writes anything back
  through this buffer at all. Direction-aware reclaim policy (which edge
  to trim to make room on the other) lives in loop() itself, based on the
  current sign of currentStep - see that function's comment.

  Why atomics, not just volatile: this isn't a single scalar control
  value shared between loop() and an interrupt (Resampler.h's `index`
  covers that simpler case, now that the consumer no longer needs to
  publish anything through this buffer) - validLow_/validHigh_ gate
  access to a whole array. The producer must finish writing sample data
  into the array before the consumer is allowed to see the updated bound
  that says "this data is ready," or the consumer could read a half-
  written slot. std::atomic's release/acquire ordering is what actually
  guarantees that ordering; a plain volatile read/write does not promise
  it. Both counters are int32_t (not int64_t): a 64-bit atomic needs
  LDREXD/STREXD on this Cortex-M33 core, not a single instruction, and
  int32_t gives 12+ hours of continuous one-direction travel at 48kHz
  before wraparound - a non-issue since virtual position re-anchors to 0
  on every file load (see SDResampleDemo.ino's loadFile()).

  This assumes the producer never lets usedSpace() exceed CAPACITY -
  guaranteed as long as it only pushes when freeSpace() says there's
  room, which is how SDResampleDemo.ino uses this.

  Named AudioRingBuffer, not RingBuffer: the core itself already defines
  `arduino::RingBuffer` (a fixed 64-byte serial buffer type, pulled in by
  Arduino.h) and brings it into scope - a same-named template here
  collides with it at the point of use.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

template <uint32_t CAPACITY>
class AudioRingBuffer {
 public:
  static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

  // -- Producer side (call from loop()) --------------------------------

  uint32_t usedSpace() const {
    return (uint32_t)(validHigh_.load(std::memory_order_relaxed) -
                       validLow_.load(std::memory_order_acquire));
  }

  uint32_t freeSpace() const { return CAPACITY - usedSpace(); }

  int32_t validLow() const { return validLow_.load(std::memory_order_acquire); }
  int32_t validHigh() const { return validHigh_.load(std::memory_order_acquire); }

  // Appends n samples (ascending virtual-position order) at the current
  // high edge, extending it by n. Caller must have already checked
  // freeSpace() >= n.
  void pushHigh(const int16_t *src, uint32_t n) {
    int32_t start = validHigh_.load(std::memory_order_relaxed);
    uint32_t writeIndex = (uint32_t)start & (CAPACITY - 1);
    uint32_t firstRun = CAPACITY - writeIndex;
    if (firstRun >= n) {
      memcpy(&data_[writeIndex], src, n * sizeof(int16_t));
    } else {
      memcpy(&data_[writeIndex], src, firstRun * sizeof(int16_t));
      memcpy(&data_[0], src + firstRun, (n - firstRun) * sizeof(int16_t));
    }
    // Publish only after the data above is actually in memory - this
    // release pairs with the acquire load in contains().
    validHigh_.store(start + (int32_t)n, std::memory_order_release);
  }

  // Prepends n samples at the current low edge, extending it downward by
  // n. src[] is still in ascending order: src[0] lands at the new lowest
  // position, src[n-1] immediately below the old low edge - i.e. exactly
  // what a forward read starting at (validLow() - n) produces, so the
  // caller never needs to reverse anything in memory to play backward.
  void pushLow(const int16_t *src, uint32_t n) {
    int32_t oldLow = validLow_.load(std::memory_order_relaxed);
    int32_t newLow = oldLow - (int32_t)n;
    uint32_t writeIndex = (uint32_t)newLow & (CAPACITY - 1);
    uint32_t firstRun = CAPACITY - writeIndex;
    if (firstRun >= n) {
      memcpy(&data_[writeIndex], src, n * sizeof(int16_t));
    } else {
      memcpy(&data_[writeIndex], src, firstRun * sizeof(int16_t));
      memcpy(&data_[0], src + firstRun, (n - firstRun) * sizeof(int16_t));
    }
    validLow_.store(newLow, std::memory_order_release);
  }

  // Trims the trailing edge to make room on the leading edge, without
  // ever exceeding CAPACITY. newLow/newHigh must stay within the
  // current [validLow(), validHigh()] window - the caller (loop()) is
  // responsible for that, same as it's responsible for checking
  // freeSpace() before push*().
  void releaseLow(int32_t newLow) { validLow_.store(newLow, std::memory_order_release); }
  void releaseHigh(int32_t newHigh) { validHigh_.store(newHigh, std::memory_order_release); }

  // -- Consumer side (call from the audio interrupt) --------------------

  // Whether every position from first to last (inclusive) is currently
  // buffered - checked once before an interpolation reads that whole span
  // with at(). False means an underrun, which can happen on the low side
  // (playing backward faster than the producer can seek-and-fill) just as
  // easily as the high side.
  bool contains(int32_t first, int32_t last) const {
    int32_t low = validLow_.load(std::memory_order_acquire);
    int32_t high = validHigh_.load(std::memory_order_acquire);
    return first >= low && last < high;
  }

  // The sample at one virtual position. Only meaningful for a position
  // contains() has just vouched for.
  int16_t at(int32_t pos) const { return data_[(uint32_t)pos & (CAPACITY - 1)]; }

  // Discards any buffered samples, leaving an empty window at virtual
  // position `at` - 0 for a freshly loaded file, or wherever a voice
  // switching to a different cached copy needs to start (see
  // SDResampleDemo.ino's startLevelChange()). Relaxed ordering is enough
  // because the caller guarantees the audio interrupt isn't reading this
  // buffer meanwhile: either it's masked (loadFile(), rewindToStart()) or
  // this buffer belongs to an idle voice the interrupt doesn't touch.
  void reset(int32_t at = 0) {
    validLow_.store(at, std::memory_order_relaxed);
    validHigh_.store(at, std::memory_order_relaxed);
  }

 private:
  int16_t data_[CAPACITY];
  std::atomic<int32_t> validLow_{0};   // written by the producer (loop()) only
  std::atomic<int32_t> validHigh_{0};  // written by the producer (loop()) only
};
