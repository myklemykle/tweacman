/*
  RingBuffer - a fixed-capacity, lock-free single-producer/single-consumer
  circular buffer of 16-bit PCM samples.

  Used to hand raw audio data from the SD-reading producer in loop() to
  the DAC-writing consumer in the audio-rate interrupt (see
  SDResampleDemo.ino) without either side ever blocking on the other.
  CAPACITY must be a power of two so index wraparound is a cheap bitmask
  instead of a modulo.

  Why atomics, not just volatile: this isn't a single scalar control
  value shared between loop() and an interrupt (AudioKnobsDemo.ino's
  comment covers that simpler case) - it's an index that *gates* access
  to a whole array. The producer must finish writing sample data into
  the array before the consumer is allowed to see the updated index
  that says "this data is ready," or the consumer could read a
  half-written slot. std::atomic's release/acquire ordering is what
  actually guarantees that ordering; a plain volatile read/write does
  not promise it.

  tail/head are monotonically increasing counts of samples ever
  produced/consumed - not array indices; wrapping into the backing
  array happens only where it's actually touched, via the CAPACITY-1
  bitmask. Both counters are uint32_t, a single atomic instruction on
  this Cortex-M33 core (a uint64_t counter would risk a torn read
  across two registers). They're allowed to wrap past UINT32_MAX -
  since CAPACITY is tiny compared to 2^32, unsigned subtraction
  (tail - head) stays correct across that wrap, the same trick used by
  e.g. Linux's kfifo.

  This assumes the producer never gets far enough ahead of the consumer
  to overwrite data the consumer hasn't read yet - guaranteed as long as
  the producer only pushes when freeSpace() says there's room, which is
  how SDResampleDemo.ino uses this.

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

  uint32_t freeSpace() const {
    return CAPACITY - (tail_.load(std::memory_order_relaxed) -
                        head_.load(std::memory_order_acquire));
  }

  // Copies n samples in, wrapping around the backing array as needed.
  // Caller must have already checked freeSpace() >= n.
  void push(const int16_t *src, uint32_t n) {
    uint32_t writeIndex = tail_.load(std::memory_order_relaxed) & (CAPACITY - 1);
    uint32_t firstRun = CAPACITY - writeIndex;
    if (firstRun >= n) {
      memcpy(&data_[writeIndex], src, n * sizeof(int16_t));
    } else {
      memcpy(&data_[writeIndex], src, firstRun * sizeof(int16_t));
      memcpy(&data_[0], src + firstRun, (n - firstRun) * sizeof(int16_t));
    }
    // Publish only after the data above is actually in memory - this
    // release pairs with the acquire load in peek4()/freeSpace().
    tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release);
  }

  // -- Consumer side (call from the audio interrupt) --------------------

  // Reads the 4 samples a Catmull-Rom interpolation needs to evaluate
  // at sourceIndex (sourceIndex-1 .. sourceIndex+2). Returns false (an
  // underrun) unless the producer has actually produced through
  // sourceIndex+2 yet.
  bool peek4(uint32_t sourceIndex, int16_t out[4]) const {
    uint32_t produced = tail_.load(std::memory_order_acquire);
    if (produced < sourceIndex + 3) return false;

    uint32_t before = (sourceIndex == 0) ? 0 : sourceIndex - 1;
    out[0] = data_[before & (CAPACITY - 1)];
    out[1] = data_[sourceIndex & (CAPACITY - 1)];
    out[2] = data_[(sourceIndex + 1) & (CAPACITY - 1)];
    out[3] = data_[(sourceIndex + 2) & (CAPACITY - 1)];
    return true;
  }

  // Tells the producer everything before newIndex is consumed and its
  // slots can be reused.
  void release(uint32_t newIndex) {
    head_.store(newIndex, std::memory_order_release);
  }

  // For status reporting only (see SDResampleDemo.ino's periodic print).
  uint32_t usedSpace() const {
    return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_acquire);
  }

  // Discards any buffered samples and returns both counters to zero, for
  // switching to a different source file. Relaxed ordering is enough
  // here specifically because the caller is required to have the audio
  // interrupt masked first (see SDResampleDemo.ino's loadFile()) - unlike
  // every other method here, there's no concurrent ISR access to order
  // against during this call.
  void reset() {
    tail_.store(0, std::memory_order_relaxed);
    head_.store(0, std::memory_order_relaxed);
  }

 private:
  int16_t data_[CAPACITY];
  std::atomic<uint32_t> tail_{0};  // written by producer only
  std::atomic<uint32_t> head_{0};  // written by consumer only
};
