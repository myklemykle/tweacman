/*
  SDResampleDemo - SparkFun Thing Plus RA6M5

  Streams a WAV file off the onboard SD card and plays it out the DAC,
  with one pot controlling a completely smooth, arbitrary, BIDIRECTIONAL
  playback-speed ratio - not discrete multiples, and slowing down (in
  either direction) genuinely interpolates new sample values rather than
  just repeating or stepping through existing ones. The pot sweeps from
  -MAX_RATIO through a real pause at the middle up to +MAX_RATIO;
  negative plays the file backward. A second pot adds a tape-scrub
  effect on top - see "Tape scrub" below. A phone/browser can also pick
  which file plays and watch the same live stats over BLE - see "BLE
  control" below.

  Wiring: an SD card (FAT32, any SDHC card - see the SD note in
  CLAUDE.md, this board's slot is wired to the RA6M5's native SDHI
  peripheral, not SPI) with one or more WAV files at its root - use
  tools/make_test_wav.py to generate a simple one, a slow sine sweep
  that makes speed changes and any resampling artifacts obvious by ear.
  Otherwise same as DACAudioTest (3.5mm jack on A0/A1/GND) plus two pots
  (RV112FF wipers on A2/A3 for speed, A4/A5 for tape scrub, both pots'
  track ends on 3.3V/GND) and one CapTouch pad (D20, with the shared
  TSCAP capacitor on D6 - see CapTouch.h). The DA14531MOD Bluetooth
  module used for BLE control is already onboard -
  no extra wiring for that part.

  Why the DAC runs at a fixed 48kHz: an earlier version of this idea
  just changed the DAC's own output clock to change speed, tape-style -
  simple, but it directly couples "how fast can we go" to the DAC
  hardware's own conversion-time ceiling (~333kHz per the RA6M5
  datasheet, Table 53.48), which isn't the interesting limit here. This
  version fixes the DAC at a constant, comfortable 48kHz always, and
  does real-time resampling from whatever rate the SD card is actually
  being asked to supply. That decouples the speed knob from the DAC
  entirely and points it at the thing this demo is actually about: SD
  read throughput (the RA6M5's SDHI peripheral has a theoretical ~25MB/s
  ceiling per the datasheet's Table 53.34). Turn the speed ratio up far
  enough and the SD reader in loop() should eventually fail to keep the
  ring buffer fed before the DAC or CPU ever struggle - see "underrun"
  below.

  Measured on the first bench card (a plain microSD, brand/speed class
  unrecorded), forward playback only: underruns start right around speed
  ratio ~23-25x with this sketch's default settings, regardless of
  whether SD reads happen in 2KB or 32KB chunks and regardless of the
  ring buffer's size - both were tried and neither moved the wall, which
  rules out per-fread()-call overhead as the cause. That points at
  ~2-2.2MB/s as this card's real sustained sequential read speed - a
  small fraction of the SDHI bus's theoretical ceiling above, and
  apparently the card's own limit, not this sketch's. This number needs
  re-measuring now that bidirectional playback reserves REVERSAL_MARGIN
  samples of the ring buffer as standing headroom on the trailing edge
  at all times (see loop()'s refill comment) - forward runway is smaller
  than it used to be, so the ceiling has likely moved down somewhat.
  Backward playback's ceiling turned out to be dramatically lower, not
  just somewhat lower: underruns on the same card start around ratio
  magnitude ~0.28x going backward - *below* native speed, not some large
  multiple of it. Consistent with the suspicion that FAT's singly-linked
  cluster chain (no native "previous cluster" pointer) makes backward
  seeks meaningfully more expensive than forward reads on this SD/FatFs
  stack - confirmed as a real, large effect rather than a minor one.

  How the resampling works: RingBuffer.h holds raw samples read from SD,
  as a window that can slide in EITHER direction; Resampler.h tracks a
  signed fractional read position into that window and advances it by a
  continuously-variable, signed amount (`step`) once per 48kHz tick,
  interpolating a fresh output value with a 4-point Catmull-Rom curve
  each time. The same mechanism handles speeding up, slowing down,
  reversing direction, and pausing (step == 0, which freezes the
  position exactly with no special-casing needed) with no separate code
  path for any of them - see both of those files' own comments for more
  detail, and loop()'s refill comment for how the SD-reading side stays
  in sync with whichever direction is currently active.

  Aliasing at high |speed ratio| is expected, not a bug: no filter tracks
  the ratio to prevent high source frequencies folding down as `step`'s
  magnitude grows past ~2 in either direction. A proper fix needs a
  cutoff that tracks the live ratio - real DSP scope this hand-rolled
  kernel deliberately skips. It's also a useful signpost, since it shows
  up audibly before the harder SD-bandwidth wall.

  Tape scrub: the second pot's ROTATIONAL VELOCITY (not its position)
  adds directly onto whatever the first pot already requests, rather
  than replacing it - RV112FF already computes this itself as rpm(),
  driven by each read() (see that library's header), so this needed no
  new velocity-tracking code of its own. With the first pot parked at
  its pause deadband, this alone drives playback, sounding like
  manually scrubbing a reel of tape back and forth; layered on top of
  ordinary forward/backward playback it adds a wow/flutter-style warble
  instead; sitting still, rpm() reads 0 and it contributes nothing at
  all - all three cases fall out of one plain sum (see the control-rate
  block in loop()), no special-casing needed. SCRUB_SCALE (how much
  speed-ratio contribution one RPM is worth) is tuned by ear, same as
  DEADBAND_T - see its own comment for the seconds-of-audio-per-turn
  arithmetic, and for why scrubbing against ordinary forward playback
  partly cancels itself.

  Relative speed control: the first pot is RELATIVE (like a DJ jog wheel
  or a DAW's jog control), not ABSOLUTE (like a normal volume knob, where
  a given shaft position always means the same value). Playback speed
  always starts at native (1.0x) on boot and on every file switch,
  regardless of wherever the physical pot happens to be sitting - only
  turning the pot moves speed away from 1.0x, and by however much you
  turn it, not to wherever the pot's absolute position would otherwise
  map. This matters because the pot has no mechanical center detent or
  home position (see RV112FF.h - it's an endless-rotation part with no
  absolute "zero" a hobbyist could return it to), so an absolute mapping
  would make "start every file at native speed" impossible to hit
  reliably by hand. See angleForRatio()/resetVirtualKnobToNative() and
  the control-rate block in loop() for the implementation - a small
  virtual position that starts anchored at native speed and accumulates
  the pot's own physical deltas from then on, the same idea as a
  relative/incremental encoder even though the underlying sensor is an
  absolute one.

  Underrun policy: if the ring buffer can't supply what the resampler
  needs, the DAC ISR holds the last good sample rather than dropping to
  silence - that's what a real buffer-starved device actually sounds
  like (a stuck stutter), and it avoids an extra click on both entering
  and leaving the underrun. The resampler's position is frozen too, so
  playback resumes exactly where it left off with no skipped content.
  underrunTicks (printed periodically over Serial, along with live ring
  buffer occupancy) is this demo's actual headline measurement - watch
  it, and the occupancy trending toward zero, as you turn the ratio up.
  It counts underruns for the *current file only*, resetting on every
  load, and does not tick at all while no file is loaded: with nothing
  playing there is no producer to fall behind, so idling is not an
  underrun. Before that distinction existed the counter accumulated
  48000 phantom underruns a second while the board sat idle at boot,
  which made the real figure meaningless.

  BLE control: connect with web/wav-remote.html (self-hosted with real
  HTTPS - can't be a Claude Artifact, see CLAUDE.md's Bluetooth section)
  to list the WAV files on the card, pick one, and watch the same speed/
  buffer/underrun stats live instead of over USB Serial. Reuses the
  CodeLess Binary mode/DSPS bidirectional protocol already validated in
  AudioKnobsWebBLEDemo - see that sketch and the codeless-binary-mode-
  dsps-protocol memory for the handshake itself. On top of that
  transport, this sketch speaks a small plain-text line protocol:
  `LIST` requests a `FILES:BEGIN`/one-name-per-line/`FILES:END` block;
  `PLAY:<name>` requests a switch, answered with `PLAYING:<name>` on
  success or `ERR:<name>:<reason>` on failure - a bad or unsupported
  file is a normal, reported failure, not a hang. This demo only plays
  mono 16-bit PCM; picking anything else from the pile of real-world
  samples (stereo, 8/24-bit, float, WAVE_FORMAT_EXTENSIBLE - all present
  on a real sample library) will cleanly error via BLE until WavReader
  grows support for them - a deliberately separate, not-yet-done task.

  Nothing is loaded or played until a BLE PLAY: command asks for it -
  the board boots silent, with no SD reads happening in loop() at all.
  This started as a fix for a bug it turned out not to cause. Inbound BLE
  commands were never reaching the board, and loop()'s greedy, unthrottled
  SD refill was the prime suspect - the one thing structurally different
  from AudioKnobsWebBLEDemo, which never touches SD. The theory was that a
  `wav.readSamples()` call could take long enough to let Serial2's RX
  buffer overflow before the next ble.available() check, silently dropping
  exactly this kind of small, rare inbound write.

  The handshake described as "confirmed clean" at the time was not clean.
  The sketch set its Binary mode flag on faith, while the module was still
  in AT-command mode for the whole session - so inbound data was being fed
  to a command parser rather than a pipe, and outbound data was echoed
  straight back. See the BLE section below and AudioKnobsWebBLEDemo.ino's
  header comment for what was actually happening.

  Staying off the SD card until a file is wanted is kept anyway: it is
  still the right shape for a demo whose whole point is to measure the
  card's sustained throughput honestly, and it removes one variable from
  the always-important connect/LIST moment. It just isn't a bug fix.

  Switching files: audioCallback() runs on a hardware interrupt totally
  independent of loop(), so swapping the WavReader/ring buffer/resampler
  state under it isn't safe to do casually. loadFile() below validates
  a candidate file completely (into a throwaway WavReader) before
  touching anything shared - a bad PLAY: request never disturbs whatever
  is already playing - then masks just the audio timer's own interrupt
  vector (FspTimer::disable_overflow_irq(), confirmed to touch nothing
  but that one NVIC vector) for the brief swap, rather than a checked
  flag the ISR has to branch around. The DAC output freezes for that
  handful of milliseconds - a deliberate, acceptable click on switch,
  not a bug.

  How to test: bring this up in stages rather than expecting it to work
  end-to-end on the first try.
    1. Flash the stock Storage/examples/TestSDCARD sketch from the core
       first, unmodified, to confirm the card mounts and lists files at
       all before any of this sketch's code is involved.
    2. With this sketch, confirm the board boots silent and idle - it no
       longer auto-loads anything at boot (see "BLE control" below for
       why); it just sits there doing no SD reads at all until a BLE
       PLAY: command picks a file. A WAV file not being exactly 48000Hz
       already means the resampler is doing real, non-trivial
       interpolation from the very first sample, even before the knob
       is touched.
    3. Turn the pot slowly across its full range once a file is playing;
       confirm smooth, continuous pitch/tempo tracking with no
       zippering or clicks, symmetric behavior above/below 1x forward,
       and that touching the pad switches between fast/coarse and
       slow/fine sweep rates.
    4. Turn the pot slowly through the middle of its range - confirm a
       real, silent-but-click-free pause, and that continuing past it
       resumes playback smoothly in reverse, from the same place in the
       file, with no gap or glitch. Then spin the pot past either
       physical extreme - confirm an instant speed/direction hop there
       (not a slow fold-back through the range) - that seam is the only
       place a discontinuity is expected.
    5. Push the ratio hard (either direction) to see the ring buffer's
       occupancy fall and underrunTicks start climbing - that onset
       point is a real, card-dependent number (~23-25x measured
       forward-only, before bidirectional support reserved some of the
       buffer as standing reversal headroom - see above) worth
       re-checking against whatever card is on the bench, in both
       directions.
    6. Open web/wav-remote.html, connect, confirm LIST populates real
       filenames from the card, PLAY: on a mono-16-bit file switches
       cleanly, and PLAY: on anything else returns a clean ERR: instead
       of hanging.
    7. With the first pot parked at its pause deadband, twist the
       second pot back and forth - confirm it sounds like manually
       scrubbing tape (pitch/direction tracking the twist directly,
       silent when you stop). Then set the first pot to some steady
       forward or backward speed and twist the second pot again -
       confirm it adds a warble on top rather than taking over
       entirely, and that leaving it alone (not touching it) adds
       nothing audible.
    8. Confirm relative control: with the first pot sitting anywhere at
       all, load a file (power-on, or a fresh PLAY: over BLE) and
       confirm playback always starts at native (1.0x) speed regardless
       of the pot's physical position. Then turn the pot a small amount
       and confirm speed changes by a correspondingly small amount from
       wherever it just was - not by jumping to whatever an absolute
       mapping of the pot's new position would imply.

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <CapTouch.h>
#include <codelessBLEPeripheral.h>
#include "SDCardBlockDevice.h"
#include "FATFileSystem.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR
#include "RingBuffer.h"
#include "Resampler.h"
#include "WavReader.h"
#include <utility>  // for std::move in loadFile()

// -- Speed range and knob mapping ----------------------------------------
// knobAngle() (below) is reused unchanged from AudioKnobsDemo's pitch-
// mapping pattern - it already produces a continuous, unbounded-turn-
// aware [0,360) angle with a discontinuity only at its own 360->0 seam
// (an fmodf wraparound artifact). AudioKnobsDemo's angleToFrequency()
// deliberately FOLDED that angle around its 180-degree midpoint, to
// smooth the seam over into a symmetric back-and-forth sweep. This
// sketch wants the opposite: a bipolar speed that sweeps smoothly
// through zero in the middle of the range, with the only allowed
// discontinuity being an instant "hop" exactly at that 360->0 seam (the
// knob's own physical wrap point) - so angleToSpeedRatio() below maps
// the angle directly, with no fold.

const float MIN_RATIO = 0.1f;
const float MAX_RATIO = 128.0f;  // magnitude at both extremes - deliberately probing well past the SD ceiling
const float DEADBAND_T = 0.03f;  // fraction of the half-sweep that reads as a real pause - tune on the bench

// Maps knobAngle()'s continuous [0,360) angle directly onto a SIGNED
// ratio: wrapped=0 -> +MAX_RATIO, wrapped=180 -> paused, wrapped
// approaching (but never reaching) 360 -> -MAX_RATIO. Continuous
// everywhere a physical sweep passes through smoothly, including
// straight through zero at the middle - the only discontinuity is
// exactly at wrapped's own 360/0 seam, which is the knob's own
// physical wrap point, not anything this function branches on.
// Exponential in magnitude, same curve AudioKnobsDemo's pitch mapping
// uses - a linear version was tried and confirmed live to feel too
// twitchy below 100%, since a purely linear mapping spends the same
// physical rotation on every equal-sized ratio step regardless of
// magnitude, spreading only a small fraction of the knob's travel
// across the whole sub-1x range. Exponential spends equal rotation on
// equal *ratios* instead, giving much finer control at low speed.
float angleToSpeedRatio(float wrappedAngle) {
  float t = 1.0f - (wrappedAngle / 180.0f);  // +1 .. -1 across the full sweep - the
                                              // 1.0f-(...) order (not (...)-1.0f) is
                                              // what reverses the direction sense:
                                              // confirmed live the un-reversed mapping
                                              // spun opposite the intuitive way.
  float magT = fabsf(t);
  if (magT < DEADBAND_T) return 0.0f;  // a real pause, not a vanishingly small nonzero speed
  float scaledT = (magT - DEADBAND_T) / (1.0f - DEADBAND_T);
  float magnitude = MIN_RATIO * powf(MAX_RATIO / MIN_RATIO, scaledT);
  return copysignf(magnitude, t);
}

// Inverse of angleToSpeedRatio(), for a nonzero ratio - used only to
// find where "native speed" (ratio 1.0) lives on the dial, so the
// virtual-knob tracking below can (re-)anchor itself there. Not
// meaningful for ratio 0, which isn't a single point on the dial - it's
// the whole deadband.
float angleForRatio(float ratio) {
  float magnitude = fabsf(ratio);
  float scaledT = logf(magnitude / MIN_RATIO) / logf(MAX_RATIO / MIN_RATIO);
  float magT = DEADBAND_T + scaledT * (1.0f - DEADBAND_T);
  float t = copysignf(magT, ratio);
  return (1.0f - t) * 180.0f;
}

const float TURNS_PER_SWEEP_FAST = 0.5f;
const float TURNS_PER_SWEEP_SLOW = 2.0f;

float knobAngle(float accumulatedDegrees, float turnsPerSweep) {
  if (accumulatedDegrees == -1.0f) return -1;
  float wrapped = fmodf(accumulatedDegrees / (turnsPerSweep * 2.0f), 360.0f);
  if (wrapped < 0) wrapped += 360.0f;
  return wrapped;
}

// -- Relative ("virtual knob") speed control -------------------------------
// The physical knob is absolute (its position directly maps to a
// point on the dial) but the speed control is relative: pot1's
// MOVEMENT, not its position, is what moves a separate virtual dial
// position - virtualKnobDegrees below - through the exact same mapping
// above. This means the real knob's physical position at any moment is
// meaningless on its own; only how far it's turned since the last tick
// matters. virtualKnobAnchored tracks "no baseline yet" explicitly,
// rather than testing lastPhysicalKnobDegrees's sign the way
// knobAngle() tests for MultiTurnKnob's own -1 not-ready sentinel -
// unlike that single-turn [0,360) reading, MultiTurnKnob::read()'s
// accumulated multi-turn position legitimately goes negative during
// perfectly ordinary counterclockwise turns, so a negative value here
// can't be trusted to mean "not calibrated yet" (a real bug caught on
// the bench: turning one direction silently froze the ratio the moment
// the accumulator crossed zero). "No baseline yet" is true at boot
// before pot1 first calibrates, and again briefly after every
// resetVirtualKnobToNative() call (see loadFile()), which is what makes
// native speed on every fresh sample and at boot come for free: the
// control-rate block below establishes the virtual knob fresh, right at
// the ratio-1.0 point, the moment it next finds no baseline to diff
// against.
float virtualKnobDegrees = 0.0f;
float lastPhysicalKnobDegrees = 0.0f;
bool virtualKnobAnchored = false;

// Doesn't touch virtualKnobDegrees directly - just clears the baseline
// so the control-rate block (which alone knows the current
// touch-gated turnsPerSweep) re-anchors it at native speed on its very
// next tick. Safe to call from anywhere, at any time.
void resetVirtualKnobToNative() {
  virtualKnobAnchored = false;
}

// -- SD card and WAV source -----------------------------------------------
// Declared at file scope, not inside setup(): FATFileSystem keeps a
// reference to the block device for as long as any file on it stays
// open, which for this sketch is the entire runtime.

SDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
FATFileSystem sdFs("sd");
WavReader wav;

// volatile because audioCallback() reads it - see the idle branch there.
// Only ever written by loadFile(), which disables the audio interrupt around
// the whole state swap, so no further guarding is needed.
volatile bool fileLoaded = false;
String currentFilename = "NONE";
float rateFactor = 1.0f;  // wav's native sample rate / 48000 - recomputed on every file load

// -- Ring buffer and resampler ---------------------------------------------

const uint32_t RING_CAPACITY = 16384;  // ~370ms total at a 44.1kHz source - loop()'s
                                        // REVERSAL_MARGIN reserves part of this as
                                        // standing headroom on the trailing edge always,
                                        // so usable one-directional runway is smaller
AudioRingBuffer<RING_CAPACITY> ring;
Resampler resampler;

// Written by loop() at control rate, read by the audio interrupt every
// tick - a plain float read/write is atomic on this 32-bit core, the
// same "no lock needed for a single scalar" case AudioKnobsDemo's
// freq/gain pointers rely on (see RingBuffer.h's comment for why the
// ring buffer itself is a different, larger problem that does need
// real atomics).
volatile float currentStep = 1.0f;
volatile int16_t lastGoodSample = 0;
volatile uint32_t underrunTicks = 0;

// The knob-requested ratio as of the last control-rate tick, kept
// outside that block too: loadFile() runs on its own schedule (whenever
// a BLE PLAY: command arrives, not at CONTROL_INTERVAL), and needs this
// to recompute currentStep for the newly-loaded file's rateFactor
// without waiting for the next knob tick.
float lastKnownRatio = 1.0f;

// Diagnostic only: the min/max ratio actually computed at control rate
// since the last status print, to check whether currentStep is
// spiking between prints (which only show a single 500ms snapshot) or
// whether the buffer wobble seen at "steady" knob positions has some
// other cause entirely.
float minRatioSeen = 1e9f;
float maxRatioSeen = -1e9f;

// -- Audio-rate interrupt ---------------------------------------------

FspTimer audioTimer;

void audioCallback(timer_callback_args_t *args) {
  int16_t y[4];
  int16_t outSample;

  if (!fileLoaded) {
    // Nothing loaded: idle, not starved. An underrun means the producer
    // could not keep up with the consumer, and with no file there is no
    // producer and nothing is expected - counting these would (and did)
    // bury the real measurement under 48000 phantom underruns per second
    // for as long as the board sat at the menu. Hold the output steady and
    // leave the resampler alone.
    outSample = lastGoodSample;
  } else if (ring.peek4(resampler.index, y)) {
    outSample = (int16_t)catmullRom(y[0], y[1], y[2], y[3], resampler.frac);
    lastGoodSample = outSample;
    // No ring.release() anymore: the consumer no longer publishes
    // anything back through the ring buffer at all - the producer
    // (loop()) decides reclaim policy itself, based on direction. See
    // RingBuffer.h's header comment and loop()'s refill logic.
    resampler.advance(currentStep);
  } else {
    outSample = lastGoodSample;  // hold the last good sample - see header comment
    underrunTicks++;
  }

  // 16-bit signed -> 12-bit unsigned, same shift-based rescale used
  // throughout this pipeline for cheapness; a few LSBs of resolution
  // given up is inaudible against this DAC's own ~24-LSB absolute
  // accuracy (RA6M5 datasheet, Table 53.48).
  uint16_t dacValue = (uint16_t)(((int32_t)outSample + 32768) >> 4);
  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- File switching (BLE PLAY: command, and the boot-time default) -------

// The only place a virtual (signed, possibly-wrapped-many-times-over)
// sample position gets mapped onto a real, in-range file sample offset -
// WavReader and AudioRingBuffer both stay ignorant of wraparound in
// either direction. Used both here (priming) and in loop() (ongoing
// refill).
uint32_t fileOffsetFor(int32_t virtualPos, uint32_t total) {
  int32_t m = virtualPos % (int32_t)total;
  if (m < 0) m += (int32_t)total;
  return (uint32_t)m;
}

// Validates and switches to a new file. On failure, errorOut is set and
// whatever was already playing (if anything) is left completely
// untouched - a bad request never disturbs current playback. See the
// header comment for why the swap itself masks the audio timer's IRQ
// rather than using a checked pause flag.
bool loadFile(const String &filename, String &errorOut) {
  if (filename.length() == 0 || filename.indexOf('/') != -1) {
    errorOut = "invalid filename";
    return false;
  }
  String fullPath = "/sd/" + filename;

  WavReader candidate;
  if (!candidate.open(fullPath.c_str())) {
    errorOut = candidate.lastError();
    return false;
  }

  audioTimer.disable_overflow_irq();

  wav = std::move(candidate);  // closes the old file (if any), adopts the new one
  ring.reset();
  resampler.index = 0;
  resampler.frac = 0.0f;
  lastGoodSample = 0;  // don't carry a stale sample into the new file
  // Per-file, not cumulative: the interesting number is how badly *this*
  // file underruns at the ratio being asked for, which is not comparable
  // across files with different native sample rates anyway. Safe to write
  // bare here - the audio interrupt is disabled for this whole block.
  underrunTicks = 0;
  rateFactor = wav.nativeSampleRate() / 48000.0f;
  // Every fresh sample starts at native speed, regardless of wherever
  // the (relative, not absolute - see resetVirtualKnobToNative()'s
  // comment) speed knob currently sits: set lastKnownRatio directly
  // rather than waiting for the next control-rate tick to catch up, so
  // there's no gap where a stale ratio from the previous file applies.
  lastKnownRatio = 1.0f;
  currentStep = lastKnownRatio * rateFactor;
  resetVirtualKnobToNative();

  // Prime the buffer before resuming, same as the original boot-time
  // pre-fill, so playback doesn't wake into an instant underrun. Always
  // fills forward from virtual position 0 regardless of which direction
  // the knob currently requests - if it's already parked at a negative
  // ratio the instant a file loads, a brief low-side underrun is
  // possible for a fraction of a second until loop()'s normal backward
  // refill catches up. Same accepted trade-off as the file-switch click
  // this function already causes - see the header comment.
  int16_t primer[1024];
  while (ring.freeSpace() >= 1024) {
    uint32_t offset = fileOffsetFor(ring.validHigh(), wav.totalSamples());
    size_t got = wav.readSamplesAt(offset, primer, 1024);
    if (got == 0) break;
    ring.pushHigh(primer, got);
  }

  audioTimer.enable_overflow_irq();

  currentFilename = filename;
  fileLoaded = true;
  return true;
}

// -- BLE control -------------------------------------------------------
// CodeLess Binary mode / DSPS bidirectional protocol - see the header
// comment and AudioKnobsWebBLEDemo.ino, whose handshake this mirrors and
// whose header comment documents the whole thing. Once the link is binary,
// Serial2 is a raw pipe: ble.readOutput() for inbound lines, and a direct
// Serial2 write for outbound (not ble.sendCommand(), which blocks waiting
// for an OK/ERROR reply that Binary mode no longer sends).

CodelessPeripheralDevice ble;
bool bleConnected = false;

// Set to 1 to mirror every byte crossing Serial2 to the USB monitor,
// timestamped and escaped. This is the diagnostic that made the handshake
// legible in the first place; off by default because it is noisy.
#define BLE_TRACE 0

#if BLE_TRACE
void bleTrace(const char *arrow, const String &bytes) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("ms] ");
  Serial.print(arrow);
  Serial.print(" \"");
  for (unsigned int i = 0; i < bytes.length(); i++) {
    char c = bytes[i];
    if (c == '\r') Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c >= 32 && c <= 126) Serial.print(c);
    else {
      Serial.print("\\x");
      if ((uint8_t)c < 0x10) Serial.print('0');
      Serial.print((uint8_t)c, HEX);
    }
  }
  Serial.print("\" (");
  Serial.print(bytes.length());
  Serial.println(" bytes)");
}
#else
#define bleTrace(arrow, bytes) ((void)0)
#endif

// Blocking AT command. Fine at boot, never in the runtime path - it spins
// until the module answers OK/ERROR/+READY or its two-second timeout
// expires, which would stall loop() and therefore the SD refill.
String bleCommand(const String &cmd) {
  bleTrace("host ->", cmd + "\r");
  String reply = ble.sendCommand(cmd);
  bleTrace("mod  ->", reply);
  return reply;
}

// Non-blocking raw write, for everything after boot.
void bleWriteRaw(const String &data) {
  bleTrace("host ->", data);
  Serial2.print(data);
}

// Last full line of application data handed to the module, kept so the loop
// can recognise its own words coming back at it - see handleBLELine().
String lastLineSent = "";

void blePrintLine(const String &line) {
  lastLineSent = line;
  bleWriteRaw(line + "\n");
}

// What the Serial2 link is actually doing. Approving a Binary mode request
// does not switch the module over there and then: measured on the wire, it
// stays in command mode for the rest of that connection and only switches
// when the connection ends, so approval means "pending", not "active".
//
// There is also no status line to wait for - once the module IS in Binary
// mode it stops emitting +CONNECTED, +BINREQ and +DISCONNECTED entirely,
// because the CodeLess parser is bypassed. What it does give us is an echo:
// in command mode it echoes every byte received on the UART, and in Binary
// mode it does not. So one line of telemetry doubles as a probe - echoed
// means command mode, silence means the pipe is open.
enum BleLinkMode {
  LINK_COMMAND,  // AT-command link; application data would just bounce
  LINK_PENDING,  // we approved Binary mode; probing to find out if it took
  LINK_BINARY    // confirmed transparent: data flows, no echo
};
BleLinkMode linkMode = LINK_COMMAND;

const unsigned long BINARY_PROBE_INTERVAL = 2000;
const unsigned long BINARY_PROBE_TIMEOUT = 300;
unsigned long probeSentMillis = 0;
bool awaitingProbeEcho = false;

// Set after sending anything large (currently just the file list) to
// hold off the periodic telemetry line below for a bit longer, as a
// safety margin on top of the pacing in sendFileList() itself - see
// that function's comment for why a fixed cooldown alone isn't enough.
unsigned long telemetrySuppressedUntilMillis = 0;

// Sending the file list is spread across loop() passes rather than done in
// one go, because it has to be paced and pacing it with delay() blocks
// everything else.
//
// The pacing itself is necessary: sending all lines back-to-back reliably
// outruns how fast the module can relay them over the air, and whatever the
// sketch writes next - the periodic telemetry tick - just gets serialised on
// top, splicing a status line into the middle of the client's file list. But
// the original delay(40) per name meant a LIST stalled loop() for 40ms times
// however many .WAV files are on the card. With 34 files that is 1.4 seconds
// with no SD refill happening at all, and since the audio interrupt keeps
// draining the ring buffer the whole time, playback audibly drops out - right
// at connect, because clients request LIST as soon as they attach.
//
// So: one name per pumpFileList() call, at the same 40ms spacing, with the
// directory handle held open in between. Same rate on the wire, no blocking.
// Telemetry is suppressed for the duration rather than relying on a fixed
// cooldown afterwards, which is also strictly more correct - a long list can
// legitimately take longer to drain than any fixed window.
const unsigned long FILE_LIST_LINE_INTERVAL = 40;  // ms between names

void finishFileList();

DIR *fileListDir = nullptr;
unsigned long nextFileLineMillis = 0;

bool fileListInProgress() { return fileListDir != nullptr; }

void abortFileList() {
  if (fileListDir) {
    closedir(fileListDir);
    fileListDir = nullptr;
  }
}

void startFileList() {
  abortFileList();  // a second LIST mid-send restarts cleanly
  blePrintLine("FILES:BEGIN");
  fileListDir = opendir("/sd");
  nextFileLineMillis = millis() + FILE_LIST_LINE_INTERVAL;
  if (!fileListDir) finishFileList();
}

void finishFileList() {
  abortFileList();
  blePrintLine("FILES:END");
  // Piggybacked here rather than sent separately right after the
  // handshake: a client already reliably requests LIST once on
  // connect, so answering "what's currently playing" as part of that
  // same response needs no new early-handshake message - and an
  // earlier attempt at a connect-time-only PLAYING: sync collided with
  // the AT+BINREQACK echo (see git history) precisely because that
  // moment is far more collision-prone than this one.
  blePrintLine("PLAYING:" + String(fileLoaded ? currentFilename : "NONE"));
  telemetrySuppressedUntilMillis = millis() + 1000;
}

// Emits at most one name per call. Cheap and returns immediately when no
// list is in flight, so loop() can call it unconditionally.
void pumpFileList() {
  if (!fileListDir) return;
  if ((long)(millis() - nextFileLineMillis) < 0) return;

  struct dirent *ent;
  while ((ent = readdir(fileListDir)) != nullptr) {
    if (ent->d_type != DT_REG) continue;
    String name(ent->d_name);
    String upper = name;
    upper.toUpperCase();
    if (upper.endsWith(".WAV")) {
      blePrintLine(name);
      nextFileLineMillis = millis() + FILE_LIST_LINE_INTERVAL;
      return;
    }
  }
  finishFileList();  // directory exhausted
}

void handleBLECommand(String line) {
  line.trim();
  if (line == "LIST") {
    startFileList();
  } else if (line.startsWith("PLAY:")) {
    String filename = line.substring(5);
    filename.trim();
    String err;
    if (loadFile(filename, err)) {
      blePrintLine("PLAYING:" + filename);
    } else {
      blePrintLine("ERR:" + filename + ":" + err);
    }
  } else if (line.length() > 0) {
    Serial.print("Unrecognized BLE command: \"");
    Serial.print(line);
    Serial.println("\"");
  }
}

// Recovers the module from a leftover Binary mode session.
//
// The DA14531 is a separate coprocessor: pressing RST on the RA6M5, or
// re-flashing it, does not reset it. A sketch that boots into a module left
// in Binary mode has its "ATR"/"AT+ADVSTOP"/"AT+ADVSTART" swallowed as raw
// pipe data - startup appears to succeed, nothing works, and restarting
// either side does not help. The escape sequence from UM-140 is a guard time
// of silence, "+++" with NO carriage return, another guard time, then an AT
// command. "+++" alone only *suspends* binary operation locally; it is the
// ATR below that actually resets the module, and it only gets through after
// the escape. Harmless when the module is already in Command mode, which is
// why it runs unconditionally - there is no way to ask which mode it is in.
const unsigned long BINARY_ESCAPE_GUARD_MS = 1100;

void bleExitBinaryMode() {
  Serial.println("Clearing any leftover Binary mode on the module...");
  delay(BINARY_ESCAPE_GUARD_MS);
  bleTrace("host ->", "+++");
  Serial2.print("+++");            // no "\r" - that would make it data
  delay(BINARY_ESCAPE_GUARD_MS);

  String reply = bleCommand("AT+BINREQEXIT");
  if (reply.indexOf("ERROR") != -1) {
    Serial.println("  ...module was already in Command mode. Fine.");
  } else if (reply.length() == 0) {
    Serial.println("  !! No reply to AT+BINREQEXIT - the module may still be wedged.");
  } else {
    Serial.println("  ...module returned to Command mode.");
  }
}

// Handles one complete line pulled out of a Serial2 burst by loop().
void handleBLELine(const String &line) {
  // The echo test comes first: it is the one measurement valid in every
  // state. Our own output coming back verbatim means the module is in
  // command mode, whatever this sketch currently believes.
  if (line == lastLineSent && lastLineSent.length() > 0) {
    awaitingProbeEcho = false;
    if (linkMode != LINK_PENDING) {
      Serial.println("Output echoed back - module is in command mode. Probing.");
      linkMode = LINK_PENDING;
    }
    return;
  }

  // Checked in every mode, not just the command-mode branch: the module
  // asks to leave Binary mode by sending "+++" on the UART and retrying
  // every two seconds until the host answers, and an unanswered request
  // just repeats forever. Confirmed live - it arrived while the link was
  // believed binary, where it was being handed to the application as an
  // unrecognised command.
  //
  // Per UM-140 the acknowledging side replies with any AT command to
  // confirm it is back in Command mode, optionally then AT+BINREQEXITACK.
  // Dropping to LINK_PENDING rather than LINK_COMMAND is deliberate: if
  // this really was the module leaving Binary mode, the probe will find the
  // echo and stay put, and if it was not, the probe promotes and the link
  // recovers on its own. LINK_COMMAND would instead sit waiting for a
  // +BINREQ that Binary mode can never send.
  if (line == "+++") {
    Serial.println("Module requested Binary mode exit - acknowledging.");
    bleWriteRaw("AT\r");
    bleWriteRaw("AT+BINREQEXITACK\r");
    linkMode = LINK_PENDING;
    probeSentMillis = 0;
    awaitingProbeEcho = false;
    return;
  }

  if (line.indexOf("+DISCONNECTED") != -1) {
    bleConnected = false;
    linkMode = LINK_COMMAND;
    awaitingProbeEcho = false;
    lastLineSent = "";
    abortFileList();  // nobody left to send it to
    Serial.println("BLE central disconnected. Advertising again.");
    // Direct write, not bleCommand(): that blocks for its full timeout
    // waiting for a reply - fine at boot, a real problem here, where it
    // would stall loop() and therefore ble.available() itself for a second
    // or more right as a new connection might already be starting up.
    bleWriteRaw("AT+ADVSTART\r");
  } else if (line.indexOf("+CONNECTED") != -1) {
    // A fresh +CONNECTED always means a fresh handshake is starting, even if
    // the link was still believed binary from a previous session -
    // +DISCONNECTED isn't guaranteed to arrive for every disconnect
    // (confirmed live: a browser-side reconnect can start a brand new
    // session with no +DISCONNECTED ever seen for the old one), and trusting
    // stale state here would wedge the link until the board is reset.
    bleConnected = true;
    linkMode = LINK_COMMAND;
    awaitingProbeEcho = false;
    lastLineSent = "";
  } else if (linkMode != LINK_BINARY) {
    if (line.indexOf("+BINREQ") != -1) {
      // Direct write again, and this one matters even more: it sits in the
      // path of a handshake the client is actively waiting on, and blocking
      // here risks swallowing whatever the client sends immediately after
      // (confirmed live - this was intermittently eating a LIST sent right
      // after connecting).
      //
      // The module answers this with "OK", but holds that reply until the
      // mode transition completes at disconnect, so there is nothing useful
      // to wait for here anyway. Approval means pending, not active - the
      // probe below is what establishes when the pipe is really open.
      bleWriteRaw("AT+BINREQACK\r");
      linkMode = LINK_PENDING;
      probeSentMillis = 0;
      awaitingProbeEcho = false;
    }
    // Other status lines (+AWAKE, +COMMAND MODE SUPPORTED, ...) are expected
    // here and ignored.
  } else {
    handleBLECommand(line);
  }
}

// -- Pots and touch pad -------------------------------------------------

RV112FF pot1(A2, A3);
MultiTurnKnob knob1(pot1);
CapTouch touch1(20);

// The second pot: a tape-scrub control, not a position control - no
// MultiTurnKnob wrapper needed, since scrubbing only cares about how
// fast the shaft is currently turning, not its accumulated position.
// RV112FF already computes this itself (rpm(), driven by each read()
// call) - see the control-rate block below for how it's used. The
// default rpmSmoothTau (0.15s, tuned for rpm() as a secondary signal
// behind a position control) read as sluggish "momentum" here, where
// rpm() drives audio directly and in real time - confirmed live and
// cut to a much snappier 0.02s, matching the position filter's own
// smoothTau below it - going much lower risks audible at-rest jitter,
// since a derivative can't be more trustworthy than the signal (here,
// smoothed at that same 0.02s) it's derived from.
RV112FF pot2(A4, A5, 0.02f, 0.15f, 0.02f);

// Maps pot2's RPM onto an equivalent contribution to currentStep.
//
// The useful way to think about this is in seconds of audio per turn.
// Position advances by `step` every output tick at 48000 ticks/s, and the
// scrub contribution to step is rpm * SCRUB_SCALE, so one full revolution
// displaces the cursor by 48000 * 60 * SCRUB_SCALE source samples - i.e.
// 60 * SCRUB_SCALE seconds of a 48kHz file.
//
// The original 1/60 therefore bought exactly one second of audio per turn,
// which is why scrubbing felt like it went nowhere: knob1 keeps playing
// forward underneath (this is a sum, not an override), so at 1x on a
// 44.1kHz file the forward motion eats 44100 samples/s while a turn only
// yields 48000. The two cancel at about 0.92 turns per second, and any
// gesture slower than that still drifts *forward* while sounding like it is
// going backward.
//
// 1/15 gives four seconds per turn, so the cancellation point drops to
// roughly a quarter turn per second and an ordinary half-turn flick moves a
// couple of seconds of speech. Still a tuned-by-ear number, same as
// DEADBAND_T - raise it for coarser scrubbing, lower it for finer. Note it
// scales the wow/flutter warble mode by the same factor, since that is the
// same sum with knob1 left running.
const float SCRUB_SCALE = 1.0f / 15.0f;

const unsigned long CONTROL_INTERVAL = 10;  // ms (~100Hz)
unsigned long previousControlMillis = 0;

const unsigned long STATUS_INTERVAL = 500;  // ms
unsigned long previousStatusMillis = 0;

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("SD resample demo: turn the pot for a continuously-variable playback speed.");
  Serial.println("Touch the pad while turning for fast/coarse sweeps; let go for slow/fine control.");
  Serial.println("Or connect over BLE with web/wav-remote.html to pick a file and watch live stats.");

  touch1.begin();

  if (sdFs.mount(&sdBlockDevice) != 0) {
    Serial.println("SD card mount failed - check it's inserted and FAT32-formatted.");
    while (1) {}
  }

  // Deliberately does not auto-load anything at boot: the board starts
  // silent and idle, with no SD reads happening in loop() at all, until
  // a BLE PLAY: command picks a file (see fileLoaded's use in loop()).
  // This isn't just a UX choice - loop()'s greedy SD refill (however
  // brief per call) turned out to be a real suspect for corrupting
  // inbound BLE UART traffic during testing, and never touching the SD
  // card until a file is actually wanted removes that variable entirely
  // during the always-important connect/LIST moment.
  Serial.println("No file loaded yet - waiting for a BLE PLAY command.");

  // Open and start both DAC channels through the normal Arduino path
  // once, then let the audio interrupt write the data register
  // directly each sample - same split as AudioKnobsDemo/DACAudioTest.
  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);

  ble.begin();
  // Set before anything else talks AT: Arduino's default 1000ms Stream
  // timeout stalls every BLE round trip, including the recovery below.
  Serial2.setTimeout(20);

  // Before any AT command can mean anything, make sure the module is
  // actually listening for AT commands - see bleExitBinaryMode().
  bleExitBinaryMode();

  // Anything the module echoed or complained about above is still in the
  // receive buffer; drop it so the first real command starts clean.
  while (Serial2.available()) Serial2.read();

  // What setupDefault() does, written out so each step is traced. The
  // library version issues exactly these three and discards every reply, so
  // when one fails there is nothing to see.
  bleCommand("ATR");
  bleCommand("AT+ADVSTOP");
  bleCommand("AT+ADVSTART");

  // A hardware timer fires at a fixed 48000Hz; the speed knob changes
  // how far the resampler steps through the source each tick, not this
  // timer's own rate - see the header comment on why. It starts even
  // with nothing loaded yet - audioCallback() just holds lastGoodSample
  // (0, i.e. silence) until a file is loaded.
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  audioTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                    48000.0f, 50.0, audioCallback, nullptr);
  audioTimer.setup_overflow_irq(1);  // high priority: audio timing is jitter-sensitive
  audioTimer.open();
  audioTimer.start();
}

void loop() {
  // Checked unconditionally every pass, not gated to CONTROL_INTERVAL -
  // a PLAY: request shouldn't wait up to 10ms for no reason, and this
  // is a cheap check when nothing's waiting (see AudioKnobsWebBLEDemo
  // for the same reasoning).
  if (ble.available()) {
    String burst = ble.readOutput();
    bleTrace("mod  ->", burst);

    // readOutput() is Serial2.readString(): it returns everything that
    // arrived in one burst, which at 57600 baud is routinely several
    // \r\n-terminated status lines at once. Running indexOf() over the whole
    // burst and acting on the first match silently discards the rest, so a
    // "+CONNECTED\r\n+BINREQ\r\n" pair loses the +BINREQ entirely and the
    // handshake stalls with nothing logged. Split first, dispatch each line.
    int start = 0;
    while (start < (int)burst.length()) {
      int end = start;
      while (end < (int)burst.length() && burst[end] != '\r' && burst[end] != '\n') end++;
      String line = burst.substring(start, end);
      start = end + 1;
      line.trim();
      if (line.length() > 0) handleBLELine(line);
    }
  }

  // Emits the next file-list line if one is due - see pumpFileList().
  pumpFileList();

  // A probe that was never echoed means the module stopped echoing, i.e. the
  // pipe is finally open. Checked every pass so the promotion is prompt.
  if (linkMode == LINK_PENDING && awaitingProbeEcho &&
      millis() - probeSentMillis >= BINARY_PROBE_TIMEOUT) {
    awaitingProbeEcho = false;
    linkMode = LINK_BINARY;
    Serial.println("Probe went unechoed - Binary mode is live.");
  }

  // Greedy, unthrottled SD refill: the whole point of this demo is to
  // let the SD card's real sustained read throughput be the limiting
  // factor once the speed ratio is cranked high enough, so this isn't
  // paced against anything - it just tops the buffer up as fast as
  // loop() can cycle. Direction-aware: maintains a standing margin
  // (REVERSAL_MARGIN) on whichever edge is currently *trailing* the
  // consumer, continuously - not just reactively after a reversal -
  // since an ordinary zero-crossing during real-time knob turning must
  // never produce an audible gap, and there's no way to detect a
  // reversal in advance. Only a margin that already exists before the
  // reversal happens can guarantee that. This costs REVERSAL_MARGIN
  // samples of one-directional lookahead capacity (RING_CAPACITY minus
  // that margin is the real forward runway now), which likely lowers
  // the previously-measured ~23-25x forward underrun ceiling somewhat -
  // worth re-benchmarking, not a regression to avoid; click-free
  // reversal matters more here.
  const size_t CHUNK_SAMPLES = 1024;
  const int32_t REVERSAL_MARGIN = 4096;  // ~93ms at 44.1kHz; must stay < RING_CAPACITY
  if (fileLoaded) {
    float step = currentStep;      // snapshot once per pass
    int32_t pos = resampler.index;

    if (step > 0.0f) {
      int32_t wantLow = pos - REVERSAL_MARGIN;
      if (wantLow > ring.validLow()) ring.releaseLow(wantLow);
      if (ring.freeSpace() >= CHUNK_SAMPLES) {
        int16_t chunk[CHUNK_SAMPLES];
        uint32_t offset = fileOffsetFor(ring.validHigh(), wav.totalSamples());
        size_t got = wav.readSamplesAt(offset, chunk, CHUNK_SAMPLES);
        if (got > 0) ring.pushHigh(chunk, got);
      }
    } else if (step < 0.0f) {
      int32_t wantHigh = pos + REVERSAL_MARGIN;
      if (wantHigh < ring.validHigh()) ring.releaseHigh(wantHigh);
      if (ring.freeSpace() >= CHUNK_SAMPLES) {
        int16_t chunk[CHUNK_SAMPLES];
        // A "backward" chunk needs no in-memory reversal: it's an
        // ordinary forward read starting earlier in the file, which
        // lands in exactly the ascending order pushLow() expects.
        int32_t startVirtual = ring.validLow() - (int32_t)CHUNK_SAMPLES;
        uint32_t offset = fileOffsetFor(startVirtual, wav.totalSamples());
        size_t got = wav.readSamplesAt(offset, chunk, CHUNK_SAMPLES);
        if (got > 0) ring.pushLow(chunk, got);
      }
    }
    // step == 0: paused - deliberately untouched. See Resampler.h's
    // advance() and audioCallback() above: this needs no special case
    // here either, since the consumer's position simply stops moving.
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    previousControlMillis = currentMillis;
    CapTouch::scan();

    float turnsPerSweep = touch1.isTouched() ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float physicalNow = knob1.read();
    if (physicalNow != -1.0f) {
      if (!virtualKnobAnchored) {
        // First calibration, or a reset just requested one (new file
        // loaded): anchor the virtual knob at native (1.0x) speed right
        // here, ignoring physicalNow's absolute position entirely - the
        // whole point of relative control is that wherever the real
        // knob happens to be sitting doesn't matter.
        virtualKnobDegrees = angleForRatio(1.0f) * turnsPerSweep * 2.0f;
        virtualKnobAnchored = true;
      } else {
        virtualKnobDegrees += physicalNow - lastPhysicalKnobDegrees;
      }
      lastPhysicalKnobDegrees = physicalNow;

      float angle = knobAngle(virtualKnobDegrees, turnsPerSweep);
      if (angle >= 0) {
        float ratio = angleToSpeedRatio(angle);
        lastKnownRatio = ratio;
        if (ratio < minRatioSeen) minRatioSeen = ratio;
        if (ratio > maxRatioSeen) maxRatioSeen = ratio;
      }
    }
    // else: pot not yet calibrated - lastKnownRatio keeps whatever it
    // already had (the native-speed passthrough default from setup()/
    // loadFile(), until pot1 calibrates for the first time).

    // Tape-scrub: pot2's rotational velocity adds directly onto
    // whatever knob1 already requests, rather than replacing it -
    // with knob1 parked at its pause deadband (ratio 0) this alone
    // drives playback, sounding like manually scrubbing a reel of
    // tape; layered on top of ordinary forward/backward playback it
    // adds a wow/flutter-style warble instead; sitting still, rpm() is
    // 0 and it contributes nothing at all - no special-casing needed
    // for any of those three cases, they all fall out of one sum.
    pot2.read();  // drives pot2's own rpm() computation - see RV112FF.h
    // Negated: confirmed live the un-reversed mapping scrubbed opposite
    // the intuitive direction, same as knob1 originally did.
    float scrub = pot2.isCalibrated() ? -pot2.rpm() * SCRUB_SCALE : 0.0f;

    currentStep = lastKnownRatio * rateFactor + scrub;
  }

  // Deliberately short and infrequent: Serial here is native USB CDC
  // (see CLAUDE.md), and a blocking print sitting in a hot loop would
  // itself steal cycles from the SD refill above, manufacturing the
  // very underrun this demo is trying to measure honestly.
  if (currentMillis - previousStatusMillis >= STATUS_INTERVAL) {
    previousStatusMillis = currentMillis;
    float ratio = currentStep / rateFactor;
    float fillPercent = 100.0f * ring.usedSpace() / RING_CAPACITY;
    Serial.print("speed ");
    Serial.print(ratio, 2);
    Serial.print("x (range ");
    if (minRatioSeen <= maxRatioSeen) {
      Serial.print(minRatioSeen, 3);
      Serial.print("-");
      Serial.print(maxRatioSeen, 3);
    } else {
      Serial.print("n/a");  // pot wasn't calibrated during this window
    }
    Serial.print(")  buffer ");
    Serial.print(fillPercent, 0);
    Serial.print("%  underruns ");
    Serial.println(underrunTicks);

    // Deliberately short and fixed-shape, unlike the Serial line above
    // (FILE is covered separately by the PLAYING: line sent on connect
    // and on every switch, so dropping it here loses no information a
    // client needs) - though the real fix for telemetry splicing into
    // a file list mid-send turned out to be telemetrySuppressedUntilMillis
    // above, not message size (an isolated short-form test still showed
    // the same symptom before that cooldown existed).
    // LINK_BINARY sends this every tick. LINK_PENDING sends the same line
    // far more rarely, as the mode probe: if it comes back echoed the module
    // is still in command mode (handled in handleBLELine), and if nothing
    // comes back the pipe is open and loop() promotes the link. LINK_COMMAND
    // sends nothing at all - there is nobody to send to, and the bytes would
    // only be fed to a command parser.
    bool probing = (linkMode == LINK_PENDING) &&
                    (millis() - probeSentMillis >= BINARY_PROBE_INTERVAL);
    if ((linkMode == LINK_BINARY || probing) && !fileListInProgress() &&
        millis() >= telemetrySuppressedUntilMillis) {
      String stat = "SPD:" + String(ratio, 2) +
                     ",BUF:" + String((int)fillPercent) + ",UND:" + String(underrunTicks);
      if (probing) {
        probeSentMillis = millis();
        awaitingProbeEcho = true;
      }
      blePrintLine(stat);
    }

    minRatioSeen = 1e9f;
    maxRatioSeen = -1e9f;
  }
}
