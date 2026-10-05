/*
  SDResampleDemo - SparkFun Thing Plus RA6M5

  Streams a WAV file off the onboard SD card and plays it out the DAC,
  with one pot controlling a completely smooth, arbitrary, BIDIRECTIONAL
  playback-speed ratio - not discrete multiples, and slowing down (in
  either direction) genuinely interpolates new sample values rather than
  just repeating or stepping through existing ones. The pot sweeps from
  -MAX_RATIO smoothly through zero at the middle up to +MAX_RATIO;
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
  track ends on 3.3V/GND) and two CapTouch pads (D20 toggles knob1 between
  speed and pitch, and chorded with D21 latches the transport;
  D21 is the transport itself - hold it to run, release to stop; both
  share the TSCAP capacitor on D6 - see RA6M5CapTouch.h). The onboard WS2812
  on D13 is used as the transport indicator - no extra wiring. The DA14531MOD Bluetooth
  module used for BLE control is already onboard -
  no extra wiring for that part.

  Why the DAC runs at a fixed 48kHz: an earlier version of this idea
  just changed the DAC's own output clock to change speed, tape-style -
  simple, but it directly couples "how fast can we go" to the DAC
  hardware's own conversion-time ceiling (~333kHz per the RA6M5
  datasheet, Table 53.48). This version fixes the DAC at a constant,
  comfortable 48kHz always, and does real-time resampling from whatever
  rate the source is actually being read at. That decouples the speed
  knob from the DAC entirely.

  How the resampling works: RingBuffer.h holds raw samples read from SD,
  as a window that can slide in EITHER direction; Resampler.h tracks a
  signed fractional read position into that window and advances it by a
  continuously-variable, signed amount (`step`) once per 48kHz tick,
  interpolating a fresh output value with a windowed-sinc kernel each
  time (SincInterpolator.h) - one that low-passes as it goes when reading
  faster than real time, so speeding up never aliases. The same mechanism handles speeding up, slowing down,
  reversing direction, and pausing (step == 0, which freezes the
  position exactly with no special-casing needed) with no separate code
  path for any of them - see both of those files' own comments for more
  detail, and loop()'s refill comment for how the SD-reading side stays
  in sync with whichever direction is currently active.

  Very high speeds - cached copies: the speed range runs to MAX_RATIO,
  1024x, which is enough to sweep a 20Hz tone right up past 20kHz. Read
  straight from the WAV, that would be hopeless on two counts. The card
  can't deliver it - reading a 44.1kHz file at 20x already means reading
  20 seconds of audio every second, and that's where this demo used to
  hit its wall (~20x forward, ~13x backward). And it would sound wrong
  long before that: content pushed above what the DAC can reproduce
  doesn't vanish, it folds back down as aliasing - tones that move the
  wrong way as the speed rises, with a ring-modulator sound to them.

  So MipCache.h builds a ladder of filtered copies of each file on the
  card, in a cache/ folder - at 1/2, 1/4 ... 1/1024 of its sample rate,
  each holding only what will still be audible at the speeds it serves.
  At any speed, playback reads whichever copy keeps it stepping through
  about one sample per output sample (see chooseLevel()), so the card is
  never asked for much more than real time, whatever the speed, and
  aliasing stays out of the way. Changing copies is a 20ms crossfade
  between two playback voices (see "Playback voices" below, and
  startLevelChange()). Copies are built in the background the first time
  a file is cued - a few seconds for a short sample - and until they're
  ready the fastest speeds are capped; the Serial status and the BLE
  CACHE: field show progress. The copies live as long as their WAV and
  are rebuilt if it changes - see "BLE control" below for what happens to
  them when the web page uploads, renames or deletes a file.

  The copies alone don't make the crossfades smooth, though. Each copy is
  free of aliasing only up to one sample per tick, so reading one any
  faster - which happens just below every switch - used to alias, and the
  switch made it suddenly vanish. What fixes that is the interpolator: it
  low-passes as it reads (SincInterpolator.h), so every copy is band-
  limited the same way on both sides of a switch. That costs CPU in the
  audio interrupt - the Serial status line reports how much ("isr avg/max",
  roughly 20-45% normally, up to ~80% mid-crossfade with the pitch shifter
  on), along with "dropped", a count of audio ticks that never ran because
  something held interrupts off for longer than a tick (it restarts at
  each file load and transport start, which pause the audio on purpose).
  It should stay at zero; if it climbs, something is blocking too long.

  Tape scrub: the second pot's ROTATIONAL VELOCITY (not its position)
  adds directly onto whatever the first pot already requests, rather
  than replacing it - RV112FF already computes this itself as rpm(),
  driven by each read() (see that library's header), so this needed no
  new velocity-tracking code of its own. With the transport stopped (or
  the first pot turned to zero), this alone drives playback, sounding like
  manually scrubbing a reel of tape back and forth; layered on top of
  ordinary forward/backward playback it adds a wow/flutter-style warble
  instead; sitting still, rpm() reads 0 and it contributes nothing at
  all - all three cases fall out of one plain sum (see the control-rate
  block in loop()), no special-casing needed. SCRUB_SCALE (how much
  speed-ratio contribution one RPM is worth) is tuned by ear - see its
  own comment for the seconds-of-audio-per-turn
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
  buffer occupancy) should stay at zero at any speed once a file's
  copies are built - if it climbs, something is starving playback. It counts underruns for the *current file only*, resetting on every
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
  file is a normal, reported failure, not a hang. The periodic stats line
  carries `RUN:0|1` alongside `SPD:`/`BUF:`/`UND:`, so a client can show
  whether the transport is running. `SPD:` is the effective ratio, so it
  reads 0.00 while stopped and follows scrub while running; `REQ:` is the
  speed knob's requested ratio, which survives being stopped, so a client
  can show what the transport *would* run at; `PIT:` is the pitch ratio,
  1.000 meaning unshifted; `LAT:0|1` is whether the transport is latched;
  `LVL:` is which copy is playing (0 the original, k the cached copy at
  1/2^k its rate); `CACHE:` is how far building those copies has got, in
  percent.

  The page can also drive the transport and manage the card:
    `LATCH:<name>`       load a file and latch the transport on - plays from
                         the top and keeps playing, answered like PLAY:
    `STOP`               clear the latch, as a touch2 press would
    `RESET:SPEED`        speed back to 1.0x
    `RESET:PITCH`        pitch back to unshifted
    `SPEED:<ratio>`      jump to an exact speed (negative is backward, 0
                         pauses); the knob carries on from there
    `PITCH:<ratio>`      set an exact pitch ratio (1 is unshifted)
    `PROFILE`            print, over USB Serial, how often each stage of
                         loop() did work since the last PROFILE and how long
                         it took - see "loop() profiling" further down
    `PUT:<bytes>:<name>` upload a file, creating or replacing <name> -
                         answered `READY:<name>`, then raw data flows with
                         `ACK:<bytes so far>` coming back, then
                         `SAVED:<name>`; see startUpload() for the flow
                         control, which is the tricky part
    `REN:<old>/<new>`    rename, answered `RENAMED:<old>/<new>`
    `DEL:<name>`         delete, answered `DELETED:<name>`
  Any of them can fail with the same `ERR:<name>:<reason>` as PLAY:. Names
  the page creates must end in .WAV, since those are the only files LIST
  shows. Touching the file that is playing closes it first (FatFs can't
  change an open file) and then reloads it, or cues the next playable file
  if it's gone, restarting playback from the top. Each also keeps the
  file's cached copies in step: an upload that replaces a file deletes its
  old copies (they're rebuilt when it's next cued), a rename moves them to
  the new name rather than rebuilding, and a delete removes them. Copies
  whose WAV has vanished some other way - deleted from a computer, say -
  are cleared out at boot, and if the cache/ folder is deleted outright it
  is simply rebuilt.

  Every one of these commands also works typed into the USB serial
  monitor, one per line, which makes for easy testing without a phone:
  LATCH:<name> then SPEED:500, for instance.

  Transport: touch pad D21 held runs the tape, released stops it. Pressing
  D20 while D21 is held latches it on, so playback continues after the finger
  lifts; the next press of D21 clears the latch, without interrupting
  playback, since that press is holding the transport up by itself. Starting
  rewinds to the top of the file, so each start plays from the beginning
  rather than resuming where scrubbing left off - engaging or clearing the
  latch mid-playback does not rewind, because playback never stops. Scrubbing
  still works while stopped - that is the point of expressing "stopped" as a
  speed of zero.

  Pitch: pressing D20 on its own (not chorded with D21, which latches)
  toggles knob1 between setting speed and setting pitch. Each mode keeps its
  own virtual knob position, so switching back and forth leaves both settings
  where they were, and pitch survives a file change since it is a setting the
  player dialled in rather than a property of the file. Range is
  PITCH_OCTAVE_RANGE octaves either way, and the pitch dial parks at its
  extremes rather than wrapping round to the opposite one - unlike the speed
  dial, which wraps deliberately. Speed and pitch are genuinely independent: the nominal playback
  position still advances at the speed ratio alone - which is what the SD
  refill is built around - while a two-tap granular shifter reads around it
  at speed * pitch. See the pitch shifting comment further down for how that
  works and what it costs.

  The onboard WS2812 on D13 shows both: dark when stopped, blinking at half
  brightness while running from a held finger, steady at full brightness when
  latched - green while knob1 sets speed, blue while it sets pitch. "Full" is
  RGBLED_MAX_BRIGHTNESS, which is 20% of what the LED can actually do - it is
  startlingly bright otherwise. Note the mode is not visible while stopped,
  since the LED is dark then. This demo only plays
  mono 16-bit PCM; picking anything else from the pile of real-world
  samples (stereo, 8/24-bit, float, WAVE_FORMAT_EXTENSIBLE - all present
  on a real sample library) will cleanly error via BLE until WavReader
  grows support for them - a deliberately separate, not-yet-done task.

  Nothing is loaded or played until a BLE PLAY: command asks for it -
  the board boots silent, with no SD reads happening in loop() at all.
  At boot the first playable file on the card is cued automatically, so the
  board works standing on its own: hold the transport pad and it makes a
  sound, with no phone or BLE connection involved. Files this demo cannot
  open are skipped rather than stopping the search, which matters on a real
  sample card. Nothing is read from the card after that until the transport
  actually runs.

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
    4. Turn the pot slowly through the middle of its range - confirm the
       speed slows smoothly right down to a standstill, with no jump to
       silence, and that continuing past it carries on smoothly in
       reverse, from the same place in the file, with no gap or glitch. Then spin the pot past either
       physical extreme - confirm an instant speed/direction hop there
       (not a slow fold-back through the range) - that seam is the only
       place a discontinuity is expected.
    5. Cue a file and watch Serial: "Cache: building ... copies" then
       "ready ... built in Nms". While it builds, speed is capped; once
       it's ready, push the ratio hard in both directions, all the way
       to 1024x - underrunTicks should stay at zero, and "Crossfading to
       copy N" lines should appear as the speed passes each power of two.
       Listen at those moments for anything untoward in the crossfade.
       Then reboot and confirm the copies are reused ("building" doesn't
       appear again), and delete the card's cache/ folder from a computer
       to confirm it's rebuilt.
    6. Open web/wav-remote.html, connect, confirm LIST populates real
       filenames from the card, PLAY: on a mono-16-bit file switches
       cleanly, and PLAY: on anything else returns a clean ERR: instead
       of hanging.
    7. With the transport stopped (pad released), twist the
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
    9. From the page: play a file with its play button and confirm it
       latches (steady LED, keeps playing with no pad touched), that Stop
       releases it, and that the two Reset buttons snap speed to 1.00x and
       pitch to 1.000x in either knob mode.
    10. Upload a short WAV, then upload it again to confirm the overwrite
       prompt, rename it, and delete it - each time checking the list
       updates. Repeat the overwrite, rename and delete on the file that is
       playing, and cancel an upload halfway to confirm the old file
       survives. Watch the underrun count while uploading during playback.
       Check the card's cache/ folder follows along: the renamed file's
       folder renamed, the deleted file's gone, and the overwritten
       file's copies rebuilt (Serial says so) the next time it's cued.

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <RA6M5AnalogTuning.h>
#include <RA6M5CapTouch.h>
#include <FastLED.h>
#include <codelessBLEPeripheral.h>
#include <RA6M5FastSD.h>  // the core's SD driver, but reading and writing many sectors per command
#include "FATFileSystem.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR
#include "RingBuffer.h"
#include "Resampler.h"
#include "WavReader.h"
#include "MipCache.h"
#include "Voice.h"
#include "SincInterpolator.h"
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

const float MAX_RATIO = 1024.0f;  // magnitude at both extremes - enough to sweep a 20Hz tone up
                                  // past 20kHz, which the cached copies make possible (MipCache.h)
const float NATIVE_SPEED_T = 0.27f;  // how far out from the middle 1x sits, as a fraction of
                                     // the half-sweep - where it sat on the older curve below

// The speed curve: ratio = SPEED_CURVE_R0 * (e^(SPEED_CURVE_K * t) - 1), for
// t from 0 (the middle of the dial) to 1 (either end), mirrored for reverse.
//
// Exponential for most of its range - a linear version was tried and
// confirmed live to feel too twitchy below 1x, since a linear mapping spends
// the same rotation on every equal-sized ratio step, leaving only a sliver of
// the knob's travel for everything under 1x. An exponential spends equal
// rotation on equal *ratios* instead, which is what fine control at low
// speed needs.
//
// But a pure exponential never reaches zero, which is why this used to need
// a dead band in the middle: a flat "paused" stretch filling the gap between
// -0.1x and +0.1x, with a jump from 0 straight to 0.1x at its edges. The
// "- 1" fixes that: near t = 0 the curve flattens into a straight line
// through zero, so the speed slows smoothly through 0.05x, 0.01x, 0.001x...
// to exactly 0 in the middle and on into reverse, with no jump anywhere.
// (Stopping outright is the transport's job now - release the touch pad.)
//
// The two constants are whatever puts MAX_RATIO at the end of the sweep and
// 1x at NATIVE_SPEED_T - solved once at startup by initSpeedCurve(), since
// there's no closed form. Above about 0.2x this matches the old curve
// closely, so the knob's feel there is unchanged.
float SPEED_CURVE_K = 1.0f;
float SPEED_CURVE_R0 = 1.0f;

void initSpeedCurve() {
  // Find k where (e^k - 1) / (e^(k * NATIVE_SPEED_T) - 1) == MAX_RATIO, by
  // bisection: the ratio grows steadily with k, so halving the interval
  // forty times pins it down far past float precision.
  float lo = 0.01f, hi = 50.0f;
  for (int i = 0; i < 40; i++) {
    float k = 0.5f * (lo + hi);
    float spread = (expf(k) - 1.0f) / (expf(k * NATIVE_SPEED_T) - 1.0f);
    if (spread < MAX_RATIO) lo = k; else hi = k;
  }
  SPEED_CURVE_K = 0.5f * (lo + hi);
  SPEED_CURVE_R0 = 1.0f / (expf(SPEED_CURVE_K * NATIVE_SPEED_T) - 1.0f);
}

// Maps knobAngle()'s continuous [0,360) angle directly onto a SIGNED
// ratio: wrapped=0 -> +MAX_RATIO, wrapped=180 -> 0, wrapped approaching
// (but never reaching) 360 -> -MAX_RATIO. Continuous everywhere, straight
// through zero in the middle included - the only discontinuity is exactly
// at wrapped's own 360/0 seam, which is the knob's own physical wrap point,
// not anything this function branches on.
float angleToSpeedRatio(float wrappedAngle) {
  float t = 1.0f - (wrappedAngle / 180.0f);  // +1 .. -1 across the full sweep - the
                                              // 1.0f-(...) order (not (...)-1.0f) is
                                              // what reverses the direction sense:
                                              // confirmed live the un-reversed mapping
                                              // spun opposite the intuitive way.
  float magnitude = SPEED_CURVE_R0 * (expf(SPEED_CURVE_K * fabsf(t)) - 1.0f);
  return copysignf(magnitude, t);
}

// Inverse of angleToSpeedRatio() - used to find where a given speed lives
// on the dial, so the virtual-knob tracking below can (re-)anchor itself
// there: native speed on every file load, or whatever SPEED: asked for.
float angleForRatio(float ratio) {
  float t = logf(1.0f + fabsf(ratio) / SPEED_CURVE_R0) / SPEED_CURVE_K;
  return (1.0f - copysignf(t, ratio)) * 180.0f;
}

// Maps the same [0,360) knob angle onto a pitch ratio, one octave either
// way, with the same direction sense and the same exponential feel as the
// speed mapping - exponential in ratio means equal rotation buys equal
// musical intervals, which is the only mapping that feels right for pitch.
// The deadband exists so unity is findable by hand rather than being an
// infinitely thin point on the dial.
const float PITCH_DEADBAND_T = 0.02f;

// How far the shift goes, in octaves either side of unity, and how much
// physical knob a single octave costs. Expressed this way round because
// these are the two numbers worth tuning by ear - everything else is derived.
//
// On range: nothing in the signal path limits this. Both taps read a fixed
// GRAIN_LENGTH_SAMPLES behind the nominal position no matter what the ratio
// is, so the buffer margin does not care; the nominal position still
// advances at the speed ratio alone, so the SD bandwidth does not care; and
// it is two taps either way, so the CPU does not care. What changes with
// range is only the *character*: shifting up reads faster and aliases (past
// a read rate of about 2x, high frequencies fold down and go crunchy), and
// the grain rate rises as |1 - pitchRatio| grows, so at wide settings the
// crossfade seam climbs into the audible band and the shifter starts to
// behave like a ring modulator. At +2 octaves the grains tick past at around
// 70Hz. That is a straightforwardly musical effect rather than a malfunction,
// so the limit is taste, not headroom - push these numbers and see.
// NOTE these are octaves EITHER SIDE of unity, so the full sweep is twice
// this many octaves and takes PITCH_TURNS_PER_OCTAVE * 2 * range turns end to
// end. At the values below that is ten octaves across ten turns of the knob -
// a long way, but this pot spins freely and the far ends are meant to be a
// destination you go looking for.
const float PITCH_OCTAVE_RANGE = 5.0f;      // +/- octaves
const float PITCH_TURNS_PER_OCTAVE = 1.0f;  // physical turns to travel one octave

// Accumulated physical degrees spanning the whole dial, top end to bottom.
const float PITCH_KNOB_SPAN_DEGREES =
    360.0f * PITCH_TURNS_PER_OCTAVE * PITCH_OCTAVE_RANGE * 2.0f;

// Takes a position along the dial in [0,360] - NOT wrapped, see the control
// block - and returns the pitch ratio. Exponential in ratio, so equal
// rotation buys equal musical intervals. The deadband exists so unity is
// findable by hand rather than being an infinitely thin point on the dial.
float angleToPitchRatio(float dialAngle) {
  float t = 1.0f - (dialAngle / 180.0f);  // +1 .. -1, same sense as speed
  float magT = fabsf(t);
  if (magT < PITCH_DEADBAND_T) return 1.0f;
  float scaledT = (magT - PITCH_DEADBAND_T) / (1.0f - PITCH_DEADBAND_T);
  return powf(2.0f, copysignf(scaledT * PITCH_OCTAVE_RANGE, t));
}

// Retained from when touch1 gated a fast/slow sweep; touch1 is the mode
// button now, so one rate serves both modes.
const float TURNS_PER_SWEEP = 2.0f;

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
// One virtual knob per mode, so switching between speed and pitch leaves
// each setting exactly where it was. lastPhysicalKnobDegrees is shared and
// updated every tick regardless of mode, which is what makes a mode switch
// seamless: the first delta after switching is measured from the knob's
// position now, not from wherever it was when this mode was last active, so
// nothing jumps.
float speedKnobDegrees = 0.0f;
bool speedKnobAnchored = false;
float pitchKnobDegrees = 0.0f;
bool pitchKnobAnchored = false;
float lastPhysicalKnobDegrees = 0.0f;

// The ratio the speed knob re-anchors at the next time it finds no
// baseline - native speed, except after a SPEED: command picks another.
float speedAnchorRatio = 1.0f;

// Doesn't touch virtualKnobDegrees directly - just clears the baseline
// so the control-rate block (which alone knows the current
// touch-gated turnsPerSweep) re-anchors it at native speed on its very
// next tick. Safe to call from anywhere, at any time.
void resetVirtualKnobToNative() {
  // Speed only. Pitch deliberately survives a file change: it is a setting
  // the player has dialled in, not a property of the file.
  speedAnchorRatio = 1.0f;
  speedKnobAnchored = false;
}

// -- SD card and WAV source -----------------------------------------------
// Declared at file scope, not inside setup(): FATFileSystem keeps a
// reference to the block device for as long as any file on it stays
// open, which for this sketch is the entire runtime.

// The faster drop-in for the core's SDCardBlockDevice - see RA6M5FastSD.h.
// What this sketch mostly needs it for is writing the cached copies (see
// MipCache.cpp): the core writes one sector per command, which made building
// a file's copies take about as long as the file plays.
FastSDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                    PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
FATFileSystem sdFs("sd");
MipCache cache;  // builds and tracks the current file's lower-rate copies - see MipCache.h

// volatile because audioCallback() reads it - see the idle branch there.
// Only ever written by loadFile() and unloadFile(), which both disable the
// audio interrupt around the whole state swap, so no further guarding is
// needed.
volatile bool fileLoaded = false;
String currentFilename = "NONE";
float rateFactor = 1.0f;  // wav's native sample rate / 48000 - recomputed on every file load
uint32_t nativeRate = 0;  // the WAV's own sample rate, in Hz

// -- Playback voices ----------------------------------------------------------
// A voice plays one copy of the current file - the WAV itself, or one of the
// filtered, lower-rate copies MipCache keeps on the card - through its own
// ring buffer, at its own position. Normally one voice plays alone. When the
// speed moves far enough that a different copy would serve better, the idle
// voice is opened on that copy, primed at the matching position, and
// crossfaded in over FADE_TICKS while both play; then it becomes the active
// voice and the old one goes idle. See chooseLevel() and startLevelChange().
//
// Positions are kept per voice, in that voice's own copy's samples rather
// than the original file's. That's not just tidiness: at 1000x, a position
// counted in the original file's samples would overflow a 32-bit counter in
// under a minute, but a voice only ever steps through its own copy at
// around one sample per tick, so its counter lasts for hours.

// The Voice struct itself lives in Voice.h - only so Arduino's automatic
// function prototypes, which land near the top of this file, can name it.
const size_t CHUNK_SAMPLES = 1024;     // samples per SD read when refilling a voice
const int32_t REVERSAL_MARGIN = 4096;  // ~93ms at 44.1kHz; must stay < RING_CAPACITY

Voice voices[2];

// Which voice is playing, and which (if either) is fading in over it. loop()
// changes these only with the audio interrupt masked; the interrupt itself
// promotes the fading voice to active when its crossfade completes.
volatile uint8_t activeVoice = 0;
volatile int8_t fadingVoice = -1;    // -1: no crossfade in progress
volatile uint32_t fadeTicks = 0;     // ticks into the current crossfade
const uint32_t FADE_TICKS = 960;      // 20ms at 48kHz

// Written by loop() at control rate, read by the audio interrupt every
// tick - a plain float read/write is atomic on this 32-bit core, the
// same "no lock needed for a single scalar" case AudioKnobsDemo's
// freq/gain pointers rely on (see RingBuffer.h's comment for why the
// ring buffer itself is a different, larger problem that does need
// real atomics).
volatile float currentStep = 1.0f;
volatile int16_t lastGoodSample = 0;
volatile uint32_t underrunTicks = 0;

// -- Pitch shifting -------------------------------------------------------
//
// Speed and pitch are coupled by resampling alone: play a file faster and it
// gets higher, exactly like a tape. Decoupling them needs one extra
// primitive, time-stretching, and then:
//
//   pitch shift = resample by r  +  time-stretch by 1/r
//   speed change = time-stretch alone
//
// The resampler already does the first half. What is added here is the
// second: a two-tap crossfading granular shifter, the cheapest useful
// time-stretcher there is. No FFT, no correlation search - the read position
// is simply held at a slowly-ramping offset behind the nominal playback
// position, and when that offset has ramped a whole grain it wraps, with a
// second tap half a grain out of phase crossfading over the seam.
//
// Effective read rate = speed * pitchRatio, while the nominal position (and
// so the duration, and so everything the SD refill is built around) still
// advances at speed alone. Concretely, with offset o and nominal rate v:
//
//   d(read)/dt = v - d(o)/dt  ==>  d(o)/dt = (1 - pitchRatio) * |v|
//
// Both taps therefore read *behind* the nominal position in the direction of
// travel, never ahead, whichever way the tape is running - which matters,
// because it means the shifter needs history the ring buffer already keeps
// (REVERSAL_MARGIN) and no lookahead the refill would have to be taught.
//
// The artifact is a periodic warble, worst on sustained tones, getting more
// obvious the further pitchRatio is from 1. That is the price of skipping
// the correlation search; WSOLA is the upgrade path if it grates.
const float PITCH_MIN_RATIO = 0.5f;   // one octave down
const float PITCH_MAX_RATIO = 2.0f;   // one octave up

// Grain length, in source samples. ~46ms at 44.1kHz: long enough that the
// grain rate stays below the audible range at moderate shifts, short enough
// to avoid obvious flanging. MUST stay well under REVERSAL_MARGIN (4096 in
// loop()), since that is what guarantees the samples a tap reaches back for
// are still in the ring.
const float GRAIN_LENGTH_SAMPLES = 2048.0f;

// Below this distance from unity the shifter is bypassed entirely and the
// single-tap path runs instead. Not just an optimisation: with the offset
// ramp stopped, the two taps sit at fixed different delays and sum into a
// comb filter, so unity pitch has to be a special case rather than a limit.
const float PITCH_BYPASS_EPSILON = 0.001f;

volatile float pitchRatio = 1.0f;   // written by loop(), read by the audio ISR
volatile float grainOffset = 0.0f;  // ISR-owned; source samples behind nominal

// Hann window, precomputed. At 50% overlap two Hann windows sum to exactly
// 1.0, so the crossfade is unity-gain with no correction term - and each tap
// is silent exactly where its own grain wraps, which is what hides the seam.
const uint16_t HANN_LUT_SIZE = 512;  // power of two, so the index masks
float hannLut[HANN_LUT_SIZE];

inline float hannAt(float phase01) {
  uint16_t i = (uint16_t)(phase01 * (float)HANN_LUT_SIZE) & (HANN_LUT_SIZE - 1);
  return hannLut[i];
}

// True while knob1 is adjusting pitch instead of speed. Toggled by touch1
// (pressed on its own - chorded with touch2 it latches the transport
// instead). Each mode keeps its own virtual knob position, so switching back
// and forth leaves both settings where they were.
bool pitchMode = false;

// Transport state, as of the last control tick. Only loop() touches it -
// the audio ISR has no opinion about it, since stopping is expressed purely
// as a step of zero. Reported over BLE as RUN: so the web page can show it.
bool transportRunning = false;

// Latched transport. Set by pressing touch1 while touch2 is held (or by a
// BLE LATCH: command), cleared by the next press of touch2 (or BLE STOP) -
// at which point playback carries on uninterrupted if touch2 is what cleared
// it, because that same press is also holding the transport up. So the latch
// is engaged and released without playback ever stopping, which is the point.
bool transportLatched = false;

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

// The interpolation kernel's lookup table - see SincInterpolator.h.
float sincTable[SINC_TABLE_LEN];

// The fastest read the kernel stretches itself to filter properly for, in
// samples per tick. Normal playback never reads a copy much faster than
// LEVEL_UP_STEP (2), but for the few milliseconds after a sudden jump in
// speed, before the right copy has faded in, it can be asked for up to
// MAX_VOICE_RATE; past this it aliases rather than grow ever more taps
// and run the audio interrupt out of time.
const float MAX_KERNEL_STRETCH = 3.0f;

// The fastest a voice is allowed to step through its copy outside a
// crossfade - see the comment where it's applied in audioCallback().
const float MAX_VOICE_RATE = 8.0f;

// The bandwidth to read at when moving through a copy at `samplesPerTick`:
// the full band at or below one sample per tick, 1/s of it above - see
// SincInterpolator.h for why.
AUDIO_HOT inline float bandwidthFor(float samplesPerTick) {
  float s = __builtin_fabsf(samplesPerTick);
  if (s <= 1.0f) return 1.0f;
  if (s >= MAX_KERNEL_STRETCH) return 1.0f / MAX_KERNEL_STRETCH;
  return 1.0f / s;
}

// Reads one interpolated sample from a voice, at whole sample idx plus
// fraction frac of its copy, plus an offset in samples (which may be
// fractional or negative). Kept as a whole/fraction pair rather than one
// float position, since a float can't hold a large sample index exactly.
AUDIO_HOT inline bool readAt(Voice &v, int32_t idx, float frac, float offset, float bandwidth,
                             float &out) {
  float rel = frac + offset;
  float whole = __builtin_floorf(rel);
  return readSinc(v.ring, idx + (int32_t)whole, rel - whole, bandwidth, out);
}

// Two-tap granular read - see the pitch shifting comment above for the
// derivation. Both taps sit behind the nominal position in the direction of
// travel; their Hann envelopes are driven by the same ramp that moves them,
// so each tap is silent exactly where its own grain wraps. grainOffset is
// counted in the original file's samples, so it's scaled into this voice's
// copy - which keeps both voices' grains in step through a crossfade.
AUDIO_HOT inline bool readGrains(Voice &v, int32_t idx, float frac, float step, float bandwidth,
                       float &out) {
  float dir = (step < 0.0f) ? -1.0f : 1.0f;

  float offsetA = grainOffset;
  float offsetB = offsetA + GRAIN_LENGTH_SAMPLES * 0.5f;
  if (offsetB >= GRAIN_LENGTH_SAMPLES) offsetB -= GRAIN_LENGTH_SAMPLES;

  float a, b;
  if (!readAt(v, idx, frac, -dir * offsetA * v.levelScale, bandwidth, a)) return false;
  if (!readAt(v, idx, frac, -dir * offsetB * v.levelScale, bandwidth, b)) return false;

  out = a * hannAt(offsetA / GRAIN_LENGTH_SAMPLES) +
        b * hannAt(offsetB / GRAIN_LENGTH_SAMPLES);
  return true;
}

// One voice's output for this tick, pitch-shifted or not.
//
// The pitch shifter's grain taps move through the copy at speed x pitch,
// but they're read at full bandwidth regardless - shifting up can alias,
// as it always has here. Band-limiting them properly would mean up to four
// stretched-kernel reads a tick during a crossfade (two voices, two taps
// each), and that measured well over the whole of a tick. The shifter is a
// lo-fi effect with its own warble anyway.
AUDIO_HOT inline bool renderVoice(Voice &v, float step, float pitch, bool shifting,
                                  float &out) {
  int32_t idx = v.resampler.index;
  float frac = v.resampler.frac;
  float rate = step * v.levelScale;  // samples of this copy per tick
  bool ok = shifting ? readGrains(v, idx, frac, step, 1.0f, out)
                     : readAt(v, idx, frac, 0.0f, bandwidthFor(rate), out);
  // A tap can reach past the start of the buffer in the moments right
  // after a rewind, when there is little history behind position 0 yet.
  // Falling back to the unpitched read costs a brief pitch glitch there
  // and nothing else, which beats reporting an underrun that isn't one.
  if (!ok && shifting) ok = readAt(v, idx, frac, 0.0f, bandwidthFor(rate), out);
  return ok;
}

// -- Audio interrupt timing (diagnostic) -------------------------------------
// The audio interrupt should fire every 1/48000s exactly. If anything keeps
// interrupts masked for longer than a tick, a tick is skipped outright and
// the DAC holds one sample twice - a tiny click. The Serial status line
// reports how many ticks have been dropped, and how much of each tick the
// interrupt itself takes.
//
// Dropped ticks are found by counting: the interrupt counts every tick it
// runs, and loop() compares that with how many there should have been by
// now according to micros(). Timing each gap from inside the interrupt
// doesn't work here, for two reasons found the hard way: FastLED's driver
// for this chip zeroes the CPU's cycle counter on every LED update, and
// micros() itself depends on a lower-priority interrupt, so read from
// inside this one it occasionally jumps by a millisecond. (The interrupt's
// own run time is still safe on the cycle counter - nothing can reset it
// partway through an interrupt.)
uint32_t cyclesPerTick = 0;          // set in setup()
volatile uint32_t audioTicks = 0;    // every tick the interrupt has run
uint32_t dropCountTicks = 0;         // audioTicks when counting (re)started
uint32_t dropCountMicros = 0;        // micros() then
uint64_t dropCountElapsed = 0;       // microseconds counted since
uint32_t dropCountLastMicros = 0;
// The timer's real rate is its clock divided by a whole number of counts
// per tick - about 48008Hz rather than exactly 48000 - which is enough to
// look like 8 ticks a second going missing, so the count uses the real one.
uint64_t tickClockHz = 48000;        // set in setup(), from the timer itself
uint64_t tickClockCountsPerTick = 1;

// Starts counting dropped ticks afresh - after loading a file or rewinding,
// which pause the audio on purpose and so would swamp the count.
void restartDropCount() {
  dropCountTicks = audioTicks;
  dropCountMicros = dropCountLastMicros = micros();
  dropCountElapsed = 0;
}

// Ticks that should have run since restartDropCount() but didn't.
int32_t droppedTicks() {
  uint32_t now = micros();
  dropCountElapsed += (uint32_t)(now - dropCountLastMicros);
  dropCountLastMicros = now;
  uint32_t expected =
      (uint32_t)(dropCountElapsed * tickClockHz / (tickClockCountsPerTick * 1000000));
  return (int32_t)(expected - (audioTicks - dropCountTicks));
}
volatile uint32_t isrCyclesMax = 0;  // since the last status line
volatile uint32_t isrCyclesSum = 0;
volatile uint32_t isrCount = 0;

AUDIO_HOT void audioCallback(timer_callback_args_t *args) {
  uint32_t tickStart = DWT->CYCCNT;
  audioTicks++;

  int16_t outSample;

  if (!fileLoaded) {
    // Nothing loaded: idle, not starved. An underrun means the producer
    // could not keep up with the consumer, and with no file there is no
    // producer and nothing is expected - counting these would (and did)
    // bury the real measurement under 48000 phantom underruns per second
    // for as long as the board sat at the menu. Hold the output steady and
    // leave the resampler alone.
    outSample = lastGoodSample;
  } else {
    float step = currentStep;
    float p = pitchRatio;
    bool shifting = __builtin_fabsf(p - 1.0f) >= PITCH_BYPASS_EPSILON;

    Voice &active = voices[activeVoice];
    int8_t fading = fadingVoice;

    float sample;
    bool ok = renderVoice(active, step, p, shifting, sample);

    // Crossfading to another copy: mix the incoming voice in, rising
    // linearly over FADE_TICKS. Linear (equal-gain) rather than equal-power,
    // because the two voices are playing the same audio at the same instant -
    // filtered versions of one signal, lined up in time - so they add
    // coherently, and an equal-gain fade keeps the level steady.
    if (fading >= 0) {
      float incoming;
      bool okIn = renderVoice(voices[fading], step, p, shifting, incoming);
      if (ok && okIn) {
        sample += ((float)fadeTicks / (float)FADE_TICKS) * (incoming - sample);
      } else if (okIn) {
        sample = incoming;
        ok = true;
      }
    }

    if (ok) {
      if (sample > 32767.0f) sample = 32767.0f;
      if (sample < -32768.0f) sample = -32768.0f;
      outSample = (int16_t)sample;
      lastGoodSample = outSample;

      // Ramp the grain offset by (1 - pitchRatio) * |step| and wrap it into
      // one grain - the wrap is the splice the crossfade exists to hide.
      if (shifting) {
        float o = grainOffset + (1.0f - p) * fabsf(step);
        if (o >= GRAIN_LENGTH_SAMPLES) o -= GRAIN_LENGTH_SAMPLES;
        else if (o < 0.0f) o += GRAIN_LENGTH_SAMPLES;
        grainOffset = o;
      }
      // No ring.release() anymore: the consumer no longer publishes
      // anything back through the ring buffer at all - the producer
      // (loop()) decides reclaim policy itself, based on direction. See
      // RingBuffer.h's header comment and loop()'s refill logic.
      // Each voice steps through its own copy, scaled to that copy's rate.
      //
      // Outside a crossfade, no faster than MAX_VOICE_RATE samples of its
      // copy per tick. Normally a voice reads its copy at no more than 2,
      // but when the speed jumps - SPEED:1000 from 1x, or a fast flick of
      // the knob - the playing voice would suddenly be asked for hundreds,
      // and drain its ring buffer in a few ticks while loop() is still
      // preparing the right copy. So it lags behind for those few
      // milliseconds instead, and the crossfade catches up. (During the
      // crossfade itself both voices run at the true speed, so they stay
      // in step - if the outgoing one runs dry, the incoming one covers.)
      float activeRate = step * active.levelScale;
      if (fading < 0 && __builtin_fabsf(activeRate) > MAX_VOICE_RATE) {
        activeRate = __builtin_copysignf(MAX_VOICE_RATE, activeRate);
      }
      active.resampler.advance(activeRate);
      if (fading >= 0) {
        voices[fading].resampler.advance(step * voices[fading].levelScale);
        if (++fadeTicks >= FADE_TICKS) {
          activeVoice = (uint8_t)fading;  // the old voice goes idle
          fadingVoice = -1;
        }
      }
    } else {
      outSample = lastGoodSample;  // hold the last good sample - see header comment
      underrunTicks++;
    }
  }

  // 16-bit signed -> 12-bit unsigned, same shift-based rescale used
  // throughout this pipeline for cheapness; a few LSBs of resolution
  // given up is inaudible against this DAC's own ~24-LSB absolute
  // accuracy (RA6M5 datasheet, Table 53.48).
  uint16_t dacValue = (uint16_t)(((int32_t)outSample + 32768) >> 4);
  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;

  uint32_t used = DWT->CYCCNT - tickStart;
  if (used > isrCyclesMax) isrCyclesMax = used;
  isrCyclesSum += used;
  isrCount++;
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

// Points a voice at one copy of the named file: level 0 is the WAV itself,
// anything higher one of its cached copies. Leaves the voice untouched if
// the file can't be opened.
bool openVoice(Voice &v, const String &name, uint8_t level) {
  WavReader reader;
  bool ok = (level == 0)
                ? reader.open(("/sd/" + name).c_str())
                : reader.openRaw(MipCache::levelPath(name, level).c_str(), nativeRate >> level);
  if (!ok) return false;
  v.reader = std::move(reader);  // closes whatever the voice had open before
  v.level = level;
  v.levelScale = ldexpf(1.0f, -(int)level);
  return true;
}

// Empties a voice's ring buffer to start at virtual position `from`, then
// fills it forward until it holds `samples` (or is full).
void primeVoice(Voice &v, int32_t from, uint32_t samples) {
  int16_t chunk[CHUNK_SAMPLES];
  v.ring.reset(from);
  while (v.ring.usedSpace() < samples && v.ring.freeSpace() >= CHUNK_SAMPLES) {
    uint32_t offset = fileOffsetFor(v.ring.validHigh(), v.reader.totalSamples());
    size_t got = v.reader.readSamplesAt(offset, chunk, CHUNK_SAMPLES);
    if (got == 0) break;
    v.ring.pushHigh(chunk, got);
  }
}

// How many of the current file's cached copies are ready to play.
uint8_t readyLevels() {
  return (fileLoaded && cache.name() == currentFilename) ? cache.readyLevels() : 0;
}

// -- Choosing a copy ----------------------------------------------------------
// A voice steps through its copy |step| * levelScale samples per tick. The
// interpolator low-passes as it reads (SincInterpolator.h), so any step is
// alias-free - but its cost grows with the step, since a read at 2 samples
// per tick needs twice the taps of one at 1. So the aim is a step between
// about 1 and 2, with hysteresis so that hovering right at a boundary
// doesn't bounce between copies:
//   - above LEVEL_UP_STEP, move to a higher (lower-rate) copy;
//   - below LEVEL_DOWN_STEP, move to a lower one.
// The two must be more than a factor of two apart, or a move one way would
// immediately qualify for a move straight back. Since both sides of a
// switch are band-limited the same way, the crossfade between them should
// be close to inaudible wherever the switch happens.
const float LEVEL_UP_STEP = 2.0f;
const float LEVEL_DOWN_STEP = 0.85f;

uint8_t chooseLevel(float stepMagnitude, uint8_t current, uint8_t maxLevel) {
  if (current > maxLevel) current = maxLevel;
  float s = stepMagnitude * ldexpf(1.0f, -(int)current);
  if (s <= LEVEL_UP_STEP && (s >= LEVEL_DOWN_STEP || current == 0)) return current;
  // Out of range: jump straight to the copy that suits this speed, however
  // many levels away it is - one crossfade, not one per level.
  uint8_t k = 0;
  while (k < maxLevel && stepMagnitude * ldexpf(1.0f, -(int)k) > LEVEL_UP_STEP) k++;
  return k;
}

// Where voice `from` is right now, as a position in voice `to`'s copy.
// Integer arithmetic for the whole-sample part, since this runs with the
// audio interrupt masked and has to be quick - this chip has no hardware
// double precision - and since a float can't hold a large sample position
// exactly anyway.
void positionInCopy(Voice &from, Voice &to, int32_t &idx, float &frac) {
  uint64_t whole = fileOffsetFor(from.resampler.index, from.reader.totalSamples());
  float part = from.resampler.frac;
  int shift = (int)from.level - (int)to.level;
  if (shift >= 0) {
    // To a higher-rate copy: every sample of `from` spans 2^shift of `to`'s.
    whole <<= shift;
    part = ldexpf(part, shift);
    float extra = floorf(part);
    whole += (uint64_t)extra;
    part -= extra;
  } else {
    // To a lower-rate copy: 2^-shift of `from`'s samples make one of `to`'s.
    int down = -shift;
    uint32_t remainder = (uint32_t)(whole & ((1u << down) - 1));
    whole >>= down;
    part = ldexpf((float)remainder + part, -down);
  }
  idx = (int32_t)(whole % to.reader.totalSamples());
  frac = part;
}

// How far each side of a voice's starting position to read before it
// plays: enough history behind it for the pitch shifter's grains (a grain's
// length, plus the interpolator's reach) and for playing backward, and
// enough ahead to cover how far the playing voice moves while this reads.
const int32_t PRIME_BEHIND = (int32_t)GRAIN_LENGTH_SAMPLES + 64;
const int32_t PRIME_AHEAD = 4096;

// After a failed attempt to open a copy, don't retry on every control tick.
unsigned long levelChangeFailedMillis = 0;
const unsigned long LEVEL_CHANGE_RETRY_MS = 1000;

// Crossfades from the playing voice to the idle one, opened on copy
// `target`. Everything slow - opening the copy, reading it from the card -
// happens on the idle voice while the active one keeps playing; interrupts
// are only masked for the microsecond or two it takes to hand over.
//
// That mask is noInterrupts(), not audioTimer.disable_overflow_irq() as
// elsewhere in this sketch. The difference matters here: re-enabling the
// timer's own interrupt (R_BSP_IrqEnable) also *clears* any tick that came
// due while it was off, so even a brief mask that way loses a tick - the
// DAC holds one sample twice, a tiny click - about one time in twenty.
// That's fine around loadFile()'s deliberate pause, but not on every copy
// change. noInterrupts() just delays a pending tick until interrupts()
// lets it through.
void startLevelChange(uint8_t target) {
  uint8_t idle = 1 - activeVoice;
  Voice &playing = voices[activeVoice];
  Voice &incoming = voices[idle];
  if (!openVoice(incoming, currentFilename, target)) {
    levelChangeFailedMillis = millis();
    return;
  }

  int32_t idx;
  float frac;
  positionInCopy(playing, incoming, idx, frac);
  int32_t primedAt = idx;
  primeVoice(incoming, idx - PRIME_BEHIND, PRIME_BEHIND + PRIME_AHEAD);

  noInterrupts();
  // Again, now: the playing voice moved on while the copy was being read.
  positionInCopy(playing, incoming, idx, frac);
  // If it crossed the end of the file meanwhile, the fresh position will
  // have wrapped round to the start - but the ring was primed in one
  // continuous stretch, so unwrap it back alongside where priming began.
  int32_t len = (int32_t)incoming.reader.totalSamples();
  if (idx < primedAt - len / 2) idx += len;
  if (idx > primedAt + len / 2) idx -= len;
  incoming.resampler.index = idx;
  incoming.resampler.frac = frac;
  fadeTicks = 0;
  fadingVoice = (int8_t)idle;
  interrupts();

  Serial.print("Crossfading to ");
  if (target == 0) {
    Serial.println("the original file");
  } else {
    Serial.print("copy ");
    Serial.print(target);
    Serial.print(" (1/");
    Serial.print(1 << target);
    Serial.println(" rate)");
  }
}

// Tops up one voice's ring buffer by at most one chunk, in whichever
// direction it's playing - see the refill comment in loop() for the
// direction-aware margin this keeps.
void refillVoice(Voice &v, float step) {
  if (!v.reader.isOpen()) return;
  int32_t pos = v.resampler.index;
  uint32_t total = v.reader.totalSamples();

  if (step > 0.0f) {
    int32_t wantLow = pos - REVERSAL_MARGIN;
    if (wantLow > v.ring.validLow()) v.ring.releaseLow(wantLow);
    if (v.ring.freeSpace() >= CHUNK_SAMPLES) {
      int16_t chunk[CHUNK_SAMPLES];
      size_t got = v.reader.readSamplesAt(fileOffsetFor(v.ring.validHigh(), total), chunk,
                                          CHUNK_SAMPLES);
      if (got > 0) v.ring.pushHigh(chunk, got);
    }
  } else if (step < 0.0f) {
    int32_t wantHigh = pos + REVERSAL_MARGIN;
    if (wantHigh < v.ring.validHigh()) v.ring.releaseHigh(wantHigh);
    if (v.ring.freeSpace() >= CHUNK_SAMPLES) {
      int16_t chunk[CHUNK_SAMPLES];
      // A "backward" chunk needs no in-memory reversal: it's an
      // ordinary forward read starting earlier in the file, which
      // lands in exactly the ascending order pushLow() expects.
      int32_t startVirtual = v.ring.validLow() - (int32_t)CHUNK_SAMPLES;
      size_t got = v.reader.readSamplesAt(fileOffsetFor(startVirtual, total), chunk,
                                          CHUNK_SAMPLES);
      if (got > 0) v.ring.pushLow(chunk, got);
    }
  }
  // step == 0: paused - deliberately untouched. See Resampler.h's
  // advance() and audioCallback() above: this needs no special case
  // here either, since the consumer's position simply stops moving.
}

// Whether the playing voices have all the data they want for now - the
// cache builder only runs then, so a slow SD write never comes at the
// expense of playback.
bool voicesToppedUp(float step) {
  if (step == 0.0f) return true;
  if (voices[activeVoice].ring.freeSpace() >= CHUNK_SAMPLES) return false;
  int8_t fading = fadingVoice;
  return fading < 0 || voices[fading].ring.freeSpace() < CHUNK_SAMPLES;
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

  // Every file starts on its original WAV, in voice 0, with no crossfade
  // under way; the other voice is closed until a copy change needs it.
  fadingVoice = -1;
  activeVoice = 0;
  voices[1].reader.close();
  voices[1].ring.reset();
  Voice &v = voices[0];
  v.reader = std::move(candidate);  // closes the old file (if any), adopts the new one
  v.level = 0;
  v.levelScale = 1.0f;
  v.resampler.index = 0;
  v.resampler.frac = 0.0f;
  lastGoodSample = 0;  // don't carry a stale sample into the new file
  grainOffset = 0.0f;
  // Per-file, not cumulative: the interesting number is how badly *this*
  // file underruns at the ratio being asked for, which is not comparable
  // across files with different native sample rates anyway. Safe to write
  // bare here - the audio interrupt is disabled for this whole block.
  underrunTicks = 0;
  nativeRate = v.reader.nativeSampleRate();
  rateFactor = nativeRate / 48000.0f;
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
  //
  // Starts PRIME_BEHIND samples before the top of the file - i.e. from the
  // end of it, since the file plays as a loop - so the interpolator has
  // history to reach back into from the very first sample.
  primeVoice(v, -PRIME_BEHIND, RING_CAPACITY);

  audioTimer.enable_overflow_irq();
  restartDropCount();

  currentFilename = filename;
  fileLoaded = true;
  // Check this file's cached copies, and build whatever's missing - in the
  // background, a step per loop() pass. Until they're ready, high speeds are
  // capped (see the control block in loop()).
  cache.start(filename);
  return true;
}

// Returns the playback cursor to the top of the file.
//
// Called when the transport starts, so every press of the pad plays from the
// beginning rather than resuming wherever scrubbing left off - the tape deck
// metaphor's rewind, folded into play.
//
// Takes the same care loadFile() does around the shared playback state: the
// audio interrupt reads resampler.index and the ring buffer continuously, so
// it is disabled across the whole swap rather than trusting any single
// write to be atomic. The buffer is re-primed before the interrupt comes
// back, for the same reason loadFile() primes - so playback does not wake
// into an instant underrun. Priming does block loop() briefly, which is
// acceptable here for the same reason it is in loadFile(): it happens on a
// deliberate, occasional user action, and nothing is playing at that instant
// anyway.
//
// The tape starts at whatever speed the knob is already set to, which may
// be hundreds of times real-time - so this starts it straight on the copy
// that speed calls for, rather than on the original WAV, which couldn't
// be read fast enough to keep up while a crossfade got going.
void rewindToStart() {
  uint8_t level = chooseLevel(fabsf(lastKnownRatio * rateFactor), 0, readyLevels());

  audioTimer.disable_overflow_irq();

  fadingVoice = -1;  // abandon any crossfade - both voices' positions are moot now
  Voice &v = voices[activeVoice];
  if (level != v.level && !openVoice(v, currentFilename, level)) {
    openVoice(v, currentFilename, 0);  // copy unreadable - fall back to the WAV
  }
  v.resampler.index = 0;
  v.resampler.frac = 0.0f;
  lastGoodSample = 0;   // don't carry a stale sample across the jump
  grainOffset = 0.0f;   // start the next grain cycle from the seam, not mid-crossfade
  // Same per-playback logic as loadFile(): the interesting figure is how
  // this pass underruns, not one accumulated across every previous pass.
  underrunTicks = 0;

  primeVoice(v, -PRIME_BEHIND, RING_CAPACITY);  // see loadFile()

  audioTimer.enable_overflow_irq();
  restartDropCount();
}

// Closes the current file, leaving nothing loaded. Needed before deleting,
// renaming or overwriting the file that is playing: FatFs does not support
// changing a file while it is open, and WavReader holds it open the whole
// time it is loaded. Same interrupt masking as loadFile(), for the same
// reason - the audio interrupt reads fileLoaded and the ring buffer.
//
// Also stops building its cached copies, since the builder holds the WAV
// and the copy in progress open too.
void unloadFile() {
  audioTimer.disable_overflow_irq();
  fileLoaded = false;
  fadingVoice = -1;
  for (Voice &v : voices) {
    v.reader.close();
    v.ring.reset();
  }
  audioTimer.enable_overflow_irq();
  cache.stop();
  currentFilename = "NONE";
}

// FAT filenames are case-insensitive, so "a.wav" and "A.WAV" are one file.
bool isCurrentFile(const String &name) {
  return fileLoaded && name.equalsIgnoreCase(currentFilename);
}

// Speed and pitch back to their defaults, from the BLE RESET: command.
// Each sets the live value directly, so it takes effect on the very next
// audio tick, and also drops its virtual knob's anchor, so the control
// block re-anchors that knob at the default the next time it is the active
// mode - otherwise the next turn of the knob would carry on from the old
// setting. Same mechanism loadFile() uses to start every file at 1.0x.
void resetSpeedToNative() {
  lastKnownRatio = 1.0f;
  resetVirtualKnobToNative();
}

// SPEED:<ratio>, from BLE or the USB serial monitor: jumps straight to a
// given speed - negative for backward, 0 to pause - and re-anchors the
// knob there, so turning it carries on from that speed. Mostly a testing
// aid: an exact, repeatable speed is hard to dial in by hand.
void setSpeedRatio(float ratio) {
  ratio = copysignf(fminf(fabsf(ratio), MAX_RATIO), ratio);
  lastKnownRatio = ratio;
  speedAnchorRatio = ratio;
  speedKnobAnchored = false;
}

void resetPitchToUnity() {
  pitchRatio = 1.0f;
  pitchKnobAnchored = false;
}

// Loads the first file on the card this demo can actually play. Returns
// whether anything was cued. See the call site in setup() for why.
bool cueFirstPlayableFile() {
  DIR *dir = opendir("/sd");
  if (!dir) {
    Serial.println("Could not open /sd to look for a file to cue.");
    return false;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_type != DT_REG) continue;
    String name(ent->d_name);
    String upper = name;
    upper.toUpperCase();
    if (!upper.endsWith(".WAV")) continue;

    String err;
    if (loadFile(name, err)) {
      closedir(dir);
      Serial.print("Cued ");
      Serial.println(name);
      return true;
    }
    Serial.print("  skipping ");
    Serial.print(name);
    Serial.print(" - ");
    Serial.println(err);
  }

  closedir(dir);
  Serial.println("No playable file found on the card - waiting for a BLE PLAY command.");
  return false;
}

// -- BLE control -------------------------------------------------------
// CodeLess Binary mode / DSPS bidirectional protocol - see the header
// comment and AudioKnobsWebBLEDemo.ino, whose handshake this mirrors and
// whose header comment documents the whole thing. Once the link is binary,
// Serial2 is a raw pipe: pumpBLEInput() for inbound lines, and a direct
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

// How often the module advertises while nothing is connected, in ms - the
// fastest CodeLess allows (AT+ADVSTART=<ms>, 100-3000). Its default is
// about 690ms, and phones and laptops only listen in short windows, so at
// that rate the board could take anywhere from an instant to 20 seconds to
// show up in a device picker. Seven times as many announcements gives a
// listener seven times the chances. The cost is tens of microamps on the
// module, while nothing is connected - nothing next to the RA6M5's own
// draw.
const unsigned int BLE_ADV_INTERVAL_MS = 100;

// The advertising command, with that interval.
String bleAdvertiseCommand() { return "AT+ADVSTART=" + String(BLE_ADV_INTERVAL_MS); }

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

// -- Card management over BLE (upload, rename, delete) --------------------

// Names the phone may create. Must end in .WAV, since LIST shows nothing
// else - a file uploaded under any other name could never be seen, played or
// deleted again from the page. Also rejects the characters FAT forbids, and
// '/', which would reach outside the card's root (and is the separator in
// the REN: command).
bool isValidWavName(const String &name) {
  if (name.length() < 5 || name.length() > 64) return false;
  for (unsigned int i = 0; i < name.length(); i++) {
    char c = name[i];
    if (c < 32 || strchr("/\\:*?\"<>|", c) != nullptr) return false;
  }
  String upper = name;
  upper.toUpperCase();
  return upper.endsWith(".WAV");
}

bool fileExists(const String &name) {
  FILE *f = fopen(("/sd/" + name).c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

// After the loaded file goes away, cue the next playable one, so the board
// stays usable standing on its own (see setup()), and tell the page.
void recueAfterLosingFile() {
  cueFirstPlayableFile();
  blePrintLine("PLAYING:" + String(fileLoaded ? currentFilename : "NONE"));
}

// Uploading. The page sends PUT:<bytes>:<name>, waits for READY:<name>, then
// sends exactly <bytes> bytes of raw file data - not text, not lines, so for
// the length of an upload loop() stops parsing Serial2 and pumpUpload() reads
// it directly instead.
//
// Flow control is the whole difficulty. The UART between the module and this
// chip has no hardware flow control, and Serial2's receive buffer is only 512
// bytes, so anything the phone sends faster than loop() drains it is simply
// lost. So the board acknowledges every UPLOAD_ACK_BYTES it has written to
// the card with ACK:<total>, and the page never has more than its window
// (see UPLOAD_WINDOW_BYTES in wav-remote.html) unacknowledged. The window is
// what keeps the receive buffer from overflowing, so it must stay below 512.
//
// The data goes to a temporary file first and only replaces the real one at
// the very end. So an upload that fails or is cancelled halfway leaves any
// existing file of that name untouched, and a file being overwritten keeps
// playing right up until the new version is complete.
//
// Speed: measured at about 1.8KB/s from a Mac - the rate the phone's side
// sends Bluetooth writes at, well under the 5.6KB/s that 57600 baud allows.
// A 100KB sample takes about a minute, a 1MB one about ten. Fine for short
// samples; this is not a way to fill a card.
const char *UPLOAD_TEMP_PATH = "/sd/UPLOAD.TMP";  // not .WAV, so LIST never shows it
const size_t UPLOAD_ACK_BYTES = 128;
const unsigned long UPLOAD_IDLE_TIMEOUT = 3000;   // ms without data before giving up
const unsigned long UPLOAD_DRAIN_QUIET = 500;     // ms of silence that ends a failed upload

FILE *uploadFile = nullptr;
String uploadName;
uint32_t uploadSize = 0;
uint32_t uploadWritten = 0;
uint8_t uploadBuffer[UPLOAD_ACK_BYTES];
size_t uploadBuffered = 0;
unsigned long uploadLastDataMillis = 0;

// A failed upload doesn't report straight away. The page may still have up
// to a window of data in flight, and if loop() went back to parsing lines
// those bytes would be read as commands. So the board keeps swallowing input
// until the link has been quiet for UPLOAD_DRAIN_QUIET, and only then sends
// the ERR: - by which point the page has stopped sending.
bool uploadDraining = false;
String uploadDrainError;

bool uploadInProgress() { return uploadFile != nullptr || uploadDraining; }

void startUpload(const String &args) {
  int sep = args.indexOf(':');
  String sizeText = sep == -1 ? "" : args.substring(0, sep);
  String name = sep == -1 ? args : args.substring(sep + 1);
  name.trim();

  long size = sizeText.toInt();
  if (!isValidWavName(name)) {
    blePrintLine("ERR:" + name + ":name must end in .wav, with no / \\ : * ? \" < > |");
    return;
  }
  if (size <= 0) {
    blePrintLine("ERR:" + name + ":empty or unreadable size");
    return;
  }

  remove(UPLOAD_TEMP_PATH);  // a leftover from an upload interrupted by a reset
  uploadFile = fopen(UPLOAD_TEMP_PATH, "wb");
  if (!uploadFile) {
    blePrintLine("ERR:" + name + ":could not create a file on the card");
    return;
  }

  uploadName = name;
  uploadSize = (uint32_t)size;
  uploadWritten = 0;
  uploadBuffered = 0;
  uploadLastDataMillis = millis();
  abortFileList();  // the page doesn't upload mid-list, but don't interleave if it does
  Serial.print("Receiving ");
  Serial.print(name);
  Serial.print(", ");
  Serial.print(uploadSize);
  Serial.println(" bytes");
  blePrintLine("READY:" + name);
}

void failUpload(const String &reason) {
  if (uploadFile) {
    fclose(uploadFile);
    uploadFile = nullptr;
  }
  remove(UPLOAD_TEMP_PATH);
  Serial.print("Upload failed: ");
  Serial.println(reason);
  uploadDraining = true;
  uploadDrainError = reason;
  uploadLastDataMillis = millis();
}

void finishUpload() {
  fclose(uploadFile);
  uploadFile = nullptr;

  // FatFs can neither replace an open file nor rename onto an existing name,
  // so a file that is playing is closed first and the old copy is deleted
  // before the new one moves into place.
  bool wasCurrent = isCurrentFile(uploadName);
  if (wasCurrent) unloadFile();
  String dest = "/sd/" + uploadName;
  remove(dest.c_str());  // fails harmlessly when this is a new name
  // Any cached copies were made from the old version. Removing them here,
  // rather than leaving the fingerprint check to notice, means a stale cache
  // can't outlive the file it was made from even briefly.
  MipCache::removeFor(uploadName);
  if (rename(UPLOAD_TEMP_PATH, dest.c_str()) != 0) {
    remove(UPLOAD_TEMP_PATH);
    blePrintLine("ERR:" + uploadName + ":could not move the upload into place");
    if (wasCurrent) recueAfterLosingFile();
    return;
  }

  Serial.print("Saved ");
  Serial.println(uploadName);
  blePrintLine("SAVED:" + uploadName);

  // Overwrote what was playing: load the new version in its place. Playback
  // restarts from the top, since it is a different file now.
  if (wasCurrent) {
    String err;
    if (loadFile(uploadName, err)) {
      blePrintLine("PLAYING:" + uploadName);
    } else {
      blePrintLine("ERR:" + uploadName + ":" + err);
      recueAfterLosingFile();
    }
  }
}

// Called by loop() instead of the usual line parsing while an upload is in
// progress. Never blocks: takes whatever has arrived, and writes to the card
// only once a full acknowledgement's worth (or the end of the file) is in.
void pumpUpload() {
  if (uploadDraining) {
    while (Serial2.available()) {
      Serial2.read();
      uploadLastDataMillis = millis();
    }
    if (millis() - uploadLastDataMillis >= UPLOAD_DRAIN_QUIET) {
      uploadDraining = false;
      blePrintLine("ERR:" + uploadName + ":" + uploadDrainError);
    }
    return;
  }

  while (Serial2.available() && uploadBuffered < UPLOAD_ACK_BYTES &&
         uploadWritten + uploadBuffered < uploadSize) {
    uploadBuffer[uploadBuffered++] = (uint8_t)Serial2.read();
    uploadLastDataMillis = millis();
  }

  bool complete = uploadWritten + uploadBuffered == uploadSize;
  if (uploadBuffered == UPLOAD_ACK_BYTES || (complete && uploadBuffered > 0)) {
    if (fwrite(uploadBuffer, 1, uploadBuffered, uploadFile) != uploadBuffered) {
      failUpload("write failed - is the card full?");
      return;
    }
    uploadWritten += uploadBuffered;
    uploadBuffered = 0;
    blePrintLine("ACK:" + String(uploadWritten));
  }

  if (uploadWritten == uploadSize) {
    finishUpload();
  } else if (millis() - uploadLastDataMillis >= UPLOAD_IDLE_TIMEOUT) {
    failUpload("timed out waiting for data");
  }
}

// REN:<old>/<new>. '/' can't appear in either name (see isValidWavName()),
// which makes it an unambiguous separator.
void renameOnCard(const String &args) {
  int sep = args.indexOf('/');
  String from = sep == -1 ? args : args.substring(0, sep);
  String to = sep == -1 ? "" : args.substring(sep + 1);
  from.trim();
  to.trim();

  if (!isValidWavName(to)) {
    blePrintLine("ERR:" + from + ":new name must end in .wav, with no / \\ : * ? \" < > |");
    return;
  }
  // A change of case alone is the same file as far as FAT is concerned, so
  // "already exists" would be wrong for it.
  if (!to.equalsIgnoreCase(from) && fileExists(to)) {
    blePrintLine("ERR:" + from + ":" + to + " already exists");
    return;
  }

  // Renaming an open file isn't supported either, so a playing file is
  // closed around the rename and reloaded under its new name - restarting
  // from the top, with the transport's latch left as it was.
  bool wasCurrent = isCurrentFile(from);
  if (wasCurrent) unloadFile();

  bool ok = rename(("/sd/" + from).c_str(), ("/sd/" + to).c_str()) == 0;
  if (ok) {
    // The audio hasn't changed, so its cached copies move with it rather
    // than being rebuilt under the new name.
    MipCache::renameFor(from, to);
    blePrintLine("RENAMED:" + from + "/" + to);
  } else {
    blePrintLine("ERR:" + from + ":rename failed");
  }

  if (wasCurrent) {
    String err;
    String reload = ok ? to : from;
    if (loadFile(reload, err)) {
      blePrintLine("PLAYING:" + reload);
    } else {
      recueAfterLosingFile();
    }
  }
}

// DEL:<name>. Deleting what is playing also clears the latch - carrying on
// latched into whatever file gets cued next would be a surprise.
void deleteFromCard(const String &nameArg) {
  String name = nameArg;
  name.trim();
  if (name.length() == 0 || name.indexOf('/') != -1) {
    blePrintLine("ERR:" + name + ":invalid filename");
    return;
  }

  bool wasCurrent = isCurrentFile(name);
  if (wasCurrent) {
    unloadFile();
    transportLatched = false;
  }

  if (remove(("/sd/" + name).c_str()) == 0) {
    MipCache::removeFor(name);
    Serial.print("Deleted ");
    Serial.println(name);
    blePrintLine("DELETED:" + name);
  } else {
    blePrintLine("ERR:" + name + ":delete failed");
  }

  if (wasCurrent) recueAfterLosingFile();
}

// Whether a line is one of the commands handleBLECommand() takes - the
// web page's words, which can only ever reach this sketch through an open
// Binary mode pipe. See handleBLELine() for why that matters.
bool isBoardCommand(const String &line) {
  static const char *const COMMANDS[] = {
    "LIST", "PLAY:", "LATCH:", "STOP", "RESET:", "PROFILE", "SPEED:", "PITCH:", "PUT:", "REN:", "DEL:"
  };
  for (const char *command : COMMANDS) {
    if (line.startsWith(command)) return true;
  }
  return false;
}

void handleBLECommand(String line) {
  line.trim();
  if (line == "LIST") {
    startFileList();
  } else if (line.startsWith("PLAY:") || line.startsWith("LATCH:")) {
    // PLAY: cues a file, leaving the transport to the touch pads. LATCH:
    // cues it and latches the transport on, as if both pads had been
    // chorded, so it plays from the top and keeps playing. No rewind is
    // needed for that: loading already puts the cursor at the top.
    bool latch = line.startsWith("LATCH:");
    String filename = line.substring(latch ? 6 : 5);
    filename.trim();
    String err;
    if (loadFile(filename, err)) {
      if (latch) transportLatched = true;
      blePrintLine("PLAYING:" + filename);
    } else {
      blePrintLine("ERR:" + filename + ":" + err);
    }
  } else if (line == "STOP") {
    // Clears the latch, exactly as a touch2 press would - so a finger that
    // happens to be holding touch2 keeps the tape running until it lifts.
    transportLatched = false;
  } else if (line == "RESET:SPEED") {
    resetSpeedToNative();
  } else if (line == "PROFILE") {
    printProfile();
  } else if (line.startsWith("SPEED:")) {
    setSpeedRatio(line.substring(6).toFloat());
  } else if (line.startsWith("PITCH:")) {
    // Like SPEED:, an exact pitch ratio for testing - the knob takes over
    // again the next time it's turned in pitch mode.
    pitchRatio = constrain(line.substring(6).toFloat(), 1.0f / 32.0f, 32.0f);
    pitchKnobAnchored = false;
  } else if (line == "RESET:PITCH") {
    resetPitchToUnity();
  } else if (line.startsWith("PUT:")) {
    startUpload(line.substring(4));
  } else if (line.startsWith("REN:")) {
    renameOnCard(line.substring(4));
  } else if (line.startsWith("DEL:")) {
    deleteFromCard(line.substring(4));
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
  // The module restarted itself. It does this every time it leaves Binary
  // mode - i.e. after every working connection - so whatever this sketch
  // believed a moment ago (and while the module reboots, an unanswered
  // probe can look like an open pipe), it's now in command mode with nobody
  // connected. It also comes back advertising at its own default interval,
  // so restart advertising at ours: stop first, since starting while
  // already advertising is an error.
  if (line == "+READY") {
    Serial.println("Module restarted - advertising again.");
    linkMode = LINK_COMMAND;
    awaitingProbeEcho = false;
    lastLineSent = "";
    bleWriteRaw("AT+ADVSTOP\r");
    bleWriteRaw(bleAdvertiseCommand() + "\r");
    return;
  }

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
    // would stall loop() and therefore the BLE input itself for a second
    // or more right as a new connection might already be starting up.
    bleWriteRaw(bleAdvertiseCommand() + "\r");
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
    // An exact match, not indexOf(): in command mode the module echoes back
    // everything this sketch sends it, and the echo of our own reply,
    // "AT+BINREQACK", contains "+BINREQ" too. Matching that would answer
    // the echo, which gets echoed, which gets answered... The old blocking
    // reader's 20ms wait per line hid this by slowing the loop to a crawl
    // until the module stopped echoing; reading lines as fast as they come
    // turned it into a runaway.
    // A command from the page while still waiting to find out whether the
    // pipe is open: it can only have come through the pipe, so it is. The
    // probe below would work that out too, but up to 2.3 seconds later -
    // and the page now reconnects straight into a fresh pipe and starts
    // sending at once (see establishLink() in wav-remote.html), so a
    // command often gets here first. Dropping it, as this branch used to,
    // lost the page's first command after nearly every connect.
    if (linkMode == LINK_PENDING && isBoardCommand(line)) {
      Serial.println("A command came through the pipe - Binary mode is live.");
      linkMode = LINK_BINARY;
      awaitingProbeEcho = false;
      handleBLECommand(line);
      return;
    }

    if (line == "+BINREQ") {
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

// Reads whatever has arrived on Serial2 and, once the module has gone quiet,
// hands each line of it to handleBLELine() - without ever waiting.
//
// This used to be ble.readOutput(), which is Serial2.readString(): it reads
// until the link has been quiet for its timeout (20ms here), so every
// arrival - a command, a status line, a probe echo - stalled loop() for at
// least 20ms, knobs and all. This keeps exactly the same rule, but checks
// it each pass instead of waiting for it: bytes collect in a buffer across
// loop() passes, and once BLE_QUIET_MS go by with nothing more, the whole
// burst is split into lines and handled, just as before.
//
// Why keep the 20ms of quiet at all, rather than acting on each line the
// moment its newline arrives? Two reasons. The web page sends commands with
// no newline, so a gap is the only sign one is over. And the module seems
// to need it: replying the instant a status line ends - while the module
// may still be mid-burst - was tried, and the Binary mode handshake stopped
// working, and the module stopped coming back to advertise after a
// disconnect. Answering only once it has finished talking had always
// worked.
const unsigned long BLE_QUIET_MS = 20;
const unsigned int BLE_BURST_MAX = 1024;  // a runaway burst is cut here rather than growing forever
String bleBurst;
unsigned long bleBurstLastByteMillis = 0;

void pumpBLEInput() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    bleBurstLastByteMillis = millis();
    if (bleBurst.length() < BLE_BURST_MAX) bleBurst += c;
  }
  if (bleBurst.length() == 0 || millis() - bleBurstLastByteMillis < BLE_QUIET_MS) return;

  String burst = bleBurst;
  bleBurst = "";
  bleTrace("mod  ->", burst);

  // A burst is routinely several \r\n-terminated status lines at once
  // ("+CONNECTED\r\n+BINREQ\r\n"), and each must be handled - acting on
  // only the first match once silently lost the +BINREQ and stalled the
  // handshake.
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

// -- Pots and touch pad -------------------------------------------------

RV112FF pot1(A2, A3);
MultiTurnKnob knob1(pot1);
CapTouch touch1(20);
// Transport control: held = running, released = stopped - or latched on, via
// the touch1 chord. See the control block in loop().
CapTouch touch2(21);

bool touch1WasTouched = false;
bool touch2WasTouched = false;

// -- Onboard RGB LED (transport indicator) -------------------------------
// Blinking = running from a held finger, steady = latched, dark = stopped.
// Same WS2812 on D13 that WS2812TouchDemo drives.
const uint8_t RGB_LED_PIN = 13;
const uint8_t NUM_LEDS = 1;
CRGB leds[NUM_LEDS];

// This LED is startlingly bright at full scale, so everything here is
// expressed as a percentage of it and kept low. Adjustable by hand for now.
const uint8_t RGBLED_MAX_BRIGHTNESS = 20;                          // percent
const uint8_t RGBLED_DIM_BRIGHTNESS = RGBLED_MAX_BRIGHTNESS / 2;   // percent
const unsigned long RGBLED_BLINK_INTERVAL = 300;                   // ms on, ms off

inline uint8_t rgbLevel(uint8_t percent) { return (uint8_t)((255 * percent) / 100); }

// When the current blink cycle began. The phase is measured from here rather
// than from absolute millis(), so the blink always starts lit: taking it
// straight from the clock meant a short tap could land in the dark half of
// the cycle and look like nothing had happened, even though audio was
// playing correctly. Re-anchored on entry to the blinking state, which
// covers both starting playback and releasing the latch back to a held
// finger - the latter would otherwise drop straight from steady-on to dark.
unsigned long blinkPhaseStartMillis = 0;
bool wasBlinking = false;

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
// couple of seconds of speech. Still a tuned-by-ear number - raise it for
// coarser scrubbing, lower it for finer. Note it
// scales the wow/flutter warble mode by the same factor, since that is the
// same sum with knob1 left running.
//
// Since halved to 1/30 - two seconds per turn - once the knob's ADC noise
// was fixed (see NOISE_DEBUGGING.md) and scrubbing became precise enough to
// want finer control. The trade-off: the cancellation point against 1x
// forward playback rises back to roughly half a turn per second.
const float SCRUB_SCALE = 1.0f / 30.0f;

const unsigned long CONTROL_INTERVAL = 10;  // ms (~100Hz)
unsigned long previousControlMillis = 0;

const unsigned long STATUS_INTERVAL = 500;  // ms
unsigned long previousStatusMillis = 0;

// -- loop() profiling -----------------------------------------------------
// loop() isn't one job but about ten, each on its own schedule. The PROFILE
// command (USB serial or BLE) prints how often each one actually did work
// since the last PROFILE, how long it took when it did, and its share of
// the CPU - timed with the same cycle counter the audio interrupt uses, so
// no debugger is needed. Times include any audio interrupts that landed
// mid-stage, since the interrupt pre-empts loop() wherever it is - up to
// ~17us each, which is why a stage only counts as "busy" (as having done
// real work rather than just checked whether it had any) past 30us.
//
// One catch: FastLED zeroes the cycle counter on every LED update (see
// "Audio interrupt timing"), so the LED stage is timed with micros()
// instead, and any measurement spanning a reset is thrown away.
enum LoopStage : uint8_t {
  STAGE_BLE, STAGE_USB, STAGE_FILE_LIST, STAGE_REFILL, STAGE_CACHE,
  STAGE_TOUCH, STAGE_CONTROL, STAGE_COPY_CHANGE, STAGE_LED, STAGE_STATUS, STAGE_COUNT
};
const char *const STAGE_NAMES[STAGE_COUNT] = {
  "BLE input", "USB input", "file list", "SD refill", "cache build",
  "touch scan", "knobs+transport", "copy change", "LED", "status lines"
};
struct StageStats {
  uint32_t busyPasses = 0;  // passes where it took long enough to count as work
  uint64_t busyCycles = 0;  // ...and the time those took
  uint64_t cycles = 0;      // total, busy or not
  uint32_t maxCycles = 0;
};
StageStats stageStats[STAGE_COUNT];
uint32_t profilePasses = 0;
uint32_t profileMaxPassCycles = 0;
unsigned long profileStartMillis = 0;
const uint32_t STAGE_BUSY_CYCLES = 6000;   // 30us at 200MHz
const uint32_t STAGE_BOGUS_CYCLES = 1u << 30;  // ~5s: only a counter reset looks like this

// Takes the stage as a plain uint8_t rather than a LoopStage only because
// Arduino's generated prototypes land above the enum's definition.
inline void stageTook(uint8_t stage, uint32_t used) {
  if (used >= STAGE_BOGUS_CYCLES) return;
  StageStats &st = stageStats[stage];
  st.cycles += used;
  if (used > st.maxCycles) st.maxCycles = used;
  if (used >= STAGE_BUSY_CYCLES) {
    st.busyPasses++;
    st.busyCycles += used;
  }
}

inline void stageDone(uint8_t stage, uint32_t startCycles) {
  stageTook(stage, DWT->CYCCNT - startCycles);
}

void printProfile() {
  unsigned long elapsed = millis() - profileStartMillis;
  if (elapsed == 0) elapsed = 1;
  float cyclesPerUs = SystemCoreClock / 1e6f;
  char line[120];
  snprintf(line, sizeof(line), "loop(): %lu passes in %lums (%lu/s), longest pass %lu us",
           (unsigned long)profilePasses, elapsed, (unsigned long)(profilePasses * 1000UL / elapsed),
           (unsigned long)(profileMaxPassCycles / cyclesPerUs));
  Serial.println(line);
  Serial.println("  stage              busy/s   avg us when busy   max us   CPU %");
  for (int i = 0; i < STAGE_COUNT; i++) {
    const StageStats &st = stageStats[i];
    float busyPerSec = st.busyPasses * 1000.0f / elapsed;
    float avgUs = st.busyPasses ? (st.busyCycles / cyclesPerUs) / st.busyPasses : 0.0f;
    float cpu = 100.0f * (st.cycles / cyclesPerUs) / (elapsed * 1000.0f);
    snprintf(line, sizeof(line), "  %-17s %8.1f %13.0f %13.0f %7.1f", STAGE_NAMES[i], busyPerSec,
             avgUs, st.maxCycles / cyclesPerUs, cpu);
    Serial.println(line);
    stageStats[i] = StageStats();
  }
  profilePasses = 0;
  profileMaxPassCycles = 0;
  profileStartMillis = millis();
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("SD resample demo: turn the pot for a continuously-variable playback speed.");
  Serial.println("Touch the pad while turning for fast/coarse sweeps; let go for slow/fine control.");
  Serial.println("Or connect over BLE with web/wav-remote.html to pick a file and watch live stats.");

  touch1.begin();
  touch2.begin();

  // ---- RA6M5-specific ----
  // Tune the ADC for the pots: a longer sampling time and a discharge
  // before each conversion, which between them remove the crosstalk
  // between the two pots (see RA6M5AnalogTuning.h). With that fixed in
  // hardware, RV112FF's portable workaround - a throwaway conversion
  // before each reading - is pure overhead, so switch it off. On any
  // other chip begin() does nothing, and this sketch keeps working as
  // it did before.
  if (RA6M5AnalogTuning::begin(A2)) {
    pot1.setSettlingReads(0);
    pot2.setSettlingReads(0);
  }
  // ---- end RA6M5-specific ----

  // Average four readings per wiper on the scrub knob. Its rpm() drives
  // the audio directly, and at rest its ADC noise was crossing the scrub
  // deadband about 1.5 times a second - each crossing a tiny back-and-forth
  // nudge of the cursor, heard as a "crumb" of garbled audio while stopped.
  // Averaging cut the at-rest noise 2.6x, to well inside the deadband, and
  // adds no lag (unlike a longer smoothTau or a longer differentiation
  // window, which were rejected for adding tens of milliseconds to the
  // feel of a scrub).
  pot2.setOversampling(4);

  for (uint16_t i = 0; i < HANN_LUT_SIZE; i++) {
    hannLut[i] = 0.5f * (1.0f - cosf(2.0f * PI * (float)i / (float)HANN_LUT_SIZE));
  }
  initSincTable();
  initSpeedCurve();

  // ---- RA6M5-specific ----
  // Start the Cortex-M33's cycle counter, which the audio interrupt uses to
  // time itself - see "Audio interrupt timing" above.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cyclesPerTick = SystemCoreClock / 48000;
  // ---- end RA6M5-specific ----

  FastLED.addLeds<WS2812B, RGB_LED_PIN, GRB>(leds, NUM_LEDS);
  leds[0] = CRGB::Black;
  FastLED.show();

  if (sdFs.mount(&sdBlockDevice) != 0) {
    Serial.println("SD card mount failed - check it's inserted and FAT32-formatted.");
    while (1) {}
  }

  // Clear out cached copies of files that are gone - deleted or renamed
  // from a computer, say, rather than through the web page.
  MipCache::removeOrphans();

  // Cue the first file the demo can actually play, so the board is usable
  // standing on its own - hold the transport pad and it makes a sound, with
  // no phone, no browser and no BLE connection needed. Skipping files that
  // fail to load matters on a real card: a sample library is full of stereo,
  // 24-bit, float and WAVE_FORMAT_EXTENSIBLE files this demo cannot open, and
  // stopping at the first *name* rather than the first playable file would
  // leave it silent for no visible reason.
  //
  // This does not reintroduce the SD activity the demo used to avoid: with
  // the transport stopped, step is 0 and loop()'s refill takes its explicit
  // paused branch, so nothing touches the card until playback actually
  // starts. And it all happens long before any BLE connection exists.
  cueFirstPlayableFile();

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
  bleCommand(bleAdvertiseCommand());

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
  tickClockHz = audioTimer.get_freq_hz();
  tickClockCountsPerTick = audioTimer.get_period_raw();
  restartDropCount();
}

void loop() {
  uint32_t passStart = DWT->CYCCNT;
  uint32_t t = passStart;

  // Checked unconditionally every pass, not gated to CONTROL_INTERVAL -
  // a PLAY: request shouldn't wait up to 10ms for no reason, and this
  // is a cheap check when nothing's waiting (see AudioKnobsWebBLEDemo
  // for the same reasoning). During an upload Serial2 carries raw file data
  // rather than lines, and pumpUpload() reads it instead.
  //
  if (uploadInProgress()) {
    pumpUpload();
  } else {
    pumpBLEInput();
  }
  stageDone(STAGE_BLE, t);
  t = DWT->CYCCNT;

  // The same commands also work typed into the USB serial monitor, one per
  // line - handy for testing without a phone (e.g. LATCH:AUDIO.WAV, then
  // SPEED:500). Replies still go out over BLE; the status lines here show
  // the effect.
  static String usbLine;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (usbLine.length() > 0) handleBLECommand(usbLine);
      usbLine = "";
    } else if (usbLine.length() < 80) {
      usbLine += c;
    }
  }
  stageDone(STAGE_USB, t);

  // Emits the next file-list line if one is due - see pumpFileList().
  t = DWT->CYCCNT;
  pumpFileList();
  stageDone(STAGE_FILE_LIST, t);

  // A probe that was never echoed means the module stopped echoing, i.e. the
  // pipe is finally open. Checked every pass so the promotion is prompt.
  if (linkMode == LINK_PENDING && awaitingProbeEcho &&
      millis() - probeSentMillis >= BINARY_PROBE_TIMEOUT) {
    awaitingProbeEcho = false;
    linkMode = LINK_BINARY;
    Serial.println("Probe went unechoed - Binary mode is live.");
  }

  // SD refill, for whichever voices are playing. Direction-aware: each
  // keeps a standing margin (REVERSAL_MARGIN) on whichever edge is
  // currently *trailing* its position, continuously - not just reactively
  // after a reversal - since an ordinary zero-crossing during real-time
  // knob turning must never produce an audible gap, and there's no way to
  // detect a reversal in advance. Only a margin that already exists before
  // the reversal happens can guarantee that. See refillVoice().
  //
  // This used to be where the demo hit its wall: reading the original WAV
  // at 20x meant reading 20 seconds of audio a second, and the card ran
  // out of bandwidth there. With the cached copies, a voice never reads
  // its copy much faster than real time, whatever the speed.
  if (fileLoaded) {
    float step = currentStep;  // snapshot once per pass
    t = DWT->CYCCNT;
    refillVoice(voices[activeVoice], step);
    int8_t fading = fadingVoice;
    if (fading >= 0) refillVoice(voices[fading], step);
    stageDone(STAGE_REFILL, t);

    // Build cached copies in the gaps: only when the playing voices are
    // topped up, and never during an upload, whose flow control needs
    // loop() to keep answering promptly. Each step is a few milliseconds.
    t = DWT->CYCCNT;
    if (cache.building() && !uploadInProgress() && voicesToppedUp(step)) cache.step();
    stageDone(STAGE_CACHE, t);
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    previousControlMillis = currentMillis;
    t = DWT->CYCCNT;
    CapTouch::scan();
    stageDone(STAGE_TOUCH, t);

    t = DWT->CYCCNT;
    float physicalNow = knob1.read();
    if (physicalNow != -1.0f) {
      // Whichever mode is active drives its own accumulator; the other
      // simply holds its value until selected again. lastPhysicalKnobDegrees
      // is shared and updated every tick either way, which is what makes a
      // mode switch seamless instead of jumpy.
      float delta = physicalNow - lastPhysicalKnobDegrees;
      lastPhysicalKnobDegrees = physicalNow;

      if (pitchMode) {
        if (!pitchKnobAnchored) {
          pitchKnobDegrees = PITCH_KNOB_SPAN_DEGREES * 0.5f;  // unity, mid-dial
          pitchKnobAnchored = true;
        } else {
          pitchKnobDegrees += delta;
        }

        // Pitch parks at its extremes instead of wrapping round to the
        // opposite one. Speed wraps deliberately - its dial is continuous
        // through the seam, since fastest-forward and fastest-backward are
        // genuinely adjacent ideas. Pitch has two hard ends and running off
        // one to reappear at the other is just disorienting, so the
        // accumulator is clamped and the dial position computed directly
        // rather than through knobAngle()'s fmod.
        if (pitchKnobDegrees < 0.0f) pitchKnobDegrees = 0.0f;
        if (pitchKnobDegrees > PITCH_KNOB_SPAN_DEGREES) {
          pitchKnobDegrees = PITCH_KNOB_SPAN_DEGREES;
        }

        pitchRatio = angleToPitchRatio(
            (pitchKnobDegrees / PITCH_KNOB_SPAN_DEGREES) * 360.0f);
      } else {
        if (!speedKnobAnchored) {
          // First calibration, or a reset just requested one (new file
          // loaded): anchor the virtual knob at native (1.0x) speed right
          // here, ignoring physicalNow's absolute position entirely - the
          // whole point of relative control is that wherever the real
          // knob happens to be sitting doesn't matter.
          speedKnobDegrees = angleForRatio(speedAnchorRatio) * TURNS_PER_SWEEP * 2.0f;
          speedKnobAnchored = true;
        } else {
          speedKnobDegrees += delta;
        }

        float angle = knobAngle(speedKnobDegrees, TURNS_PER_SWEEP);
        if (angle >= 0) {
          float ratio = angleToSpeedRatio(angle);
          lastKnownRatio = ratio;
          if (ratio < minRatioSeen) minRatioSeen = ratio;
          if (ratio > maxRatioSeen) maxRatioSeen = ratio;
        }
      }
    }
    // else: pot not yet calibrated - lastKnownRatio keeps whatever it
    // already had (the native-speed passthrough default from setup()/
    // loadFile(), until pot1 calibrates for the first time).

    // Tape-scrub: pot2's rotational velocity adds directly onto
    // whatever knob1 already requests, rather than replacing it -
    // with the transport stopped (or knob1 at zero) this alone
    // drives playback, sounding like manually scrubbing a reel of
    // tape; layered on top of ordinary forward/backward playback it
    // adds a wow/flutter-style warble instead; sitting still, rpm() is
    // 0 and it contributes nothing at all - no special-casing needed
    // for any of those three cases, they all fall out of one sum.
    pot2.read();  // drives pot2's own rpm() computation - see RV112FF.h
    // Negated: confirmed live the un-reversed mapping scrubbed opposite
    // the intuitive direction, same as knob1 originally did.
    // Deadband, in RPM, below which the pot is considered to be at rest.
    //
    // pot2 runs a 0.02s rpmSmoothTau (see its declaration) so that scrubbing
    // feels immediate, and the cost of that short a derivative window is
    // at-rest jitter - exactly what that comment predicted. Measured on the
    // bench with both pots calibrated and untouched, pot2's rpm() wandered
    // up to +/-0.36, which is a step of +/-0.024 and creeps the cursor at
    // roughly 1-2% of real-time speed: audible as very slow scrubbing while
    // supposedly stopped.
    //
    // NOTE: this figure is sized to the ADC jitter as it stands today. If
    // that jitter is ever reduced at its source (better filtering, settling
    // time between channels, hardware changes), come back and re-tune this
    // down - a deadband wider than the noise it exists to reject is pure
    // lost sensitivity.
    //
    // 0.5 costs almost nothing in responsiveness: a gentle scrub of a
    // quarter-turn per second is 15 RPM, so this is about 3% of an ordinary
    // gesture, and the smallest deliberate movement is far above it.
    //
    // Subtracted rather than gated, so crossing the threshold ramps up from
    // zero instead of jumping straight to full deadband width - a hard gate
    // would make the quietest real scrub start with an audible lurch.
    const float SCRUB_DEADBAND_RPM = 0.5f;

    float rawRpm = pot2.isCalibrated() ? pot2.rpm() : 0.0f;
    float scrubRpm = fabsf(rawRpm) - SCRUB_DEADBAND_RPM;
    if (scrubRpm < 0.0f) scrubRpm = 0.0f;
    float scrub = -copysignf(scrubRpm, rawRpm) * SCRUB_SCALE;

    // Scale the scrub with playback speed, but never below 1x.
    //
    // A fixed scrub contribution is a shrinking fraction of the motion
    // already happening: at 20x, a scrub worth +/-1 step perturbs the
    // cursor by five percent and is essentially inaudible. Multiplying by
    // the speed magnitude keeps it a constant *proportion* of whatever is
    // playing, so the gesture feels the same at any speed - and it makes
    // the turn rate needed to overcome forward playback speed-independent
    // too, since both sides of the sum scale together.
    //
    // The floor at 1x is what stops this scaling *down*, and it is not
    // optional: the primary tape-scrub case is the transport stopped,
    // where the ratio is 0 - a bare multiply would there
    // multiply the scrub by zero and disable the feature precisely where
    // it matters most. Below 1x the scrub keeps its absolute feel.
    // Transport: touch pad 2 held means running, released means stopped.
    //
    // "Stopped" is implemented as a speed of zero rather than as a mode,
    // because zero is already a fully supported state everywhere downstream:
    // Resampler.h's advance() leaves index and frac frozen exactly when step
    // is 0 (that is the entire implementation of pause - audioCallback()
    // needs no special case), and the direction-aware refill above already
    // has an explicit "step == 0: paused - deliberately untouched" branch.
    // So stopping is just declining to add knob1's contribution to the sum.
    //
    // Scrub deliberately survives, because it is a separate term: a stopped
    // tape still moves when you turn the reel by hand.
    // Transport, with a latch.
    //
    //   touch2 held                -> running, for as long as it is held
    //   touch1 pressed while held  -> latched on; running survives release
    //   touch2 pressed again       -> latch cleared
    //
    // Clearing the latch never interrupts playback: the press that clears it
    // is also holding the transport up by itself, so running only ends when
    // that finger lifts. Both triggers are edge-detected so they fire once
    // per press rather than continuously while held.
    //
    // Note touch1 keeps its existing job of gating the speed knob's
    // fast/slow sweep - the chord only means "latch" when touch2 is also
    // down, so the two uses do not collide.
    bool touch1Now = touch1.isTouched();
    bool touch2Now = touch2.isTouched();

    // touch1 pressed on its own toggles pitch mode; chorded with touch2 it
    // latches the transport instead. One button, two jobs, disambiguated by
    // whether the other pad is down - which is exactly how the latch was
    // already specified, so nothing had to move.
    if (touch1Now && !touch1WasTouched) {
      if (touch2Now) {
        transportLatched = true;
      } else {
        pitchMode = !pitchMode;
        Serial.print("Knob 1 now adjusts ");
        Serial.println(pitchMode ? "PITCH" : "SPEED");
      }
    }
    if (touch2Now && !touch2WasTouched && transportLatched) transportLatched = false;

    touch1WasTouched = touch1Now;
    touch2WasTouched = touch2Now;

    bool nowRunning = touch2Now || transportLatched;

    // Rewind on the rising edge of *running*, not of the pad, so engaging or
    // releasing the latch mid-playback does not jump back to the top.
    if (nowRunning && !transportRunning && fileLoaded) rewindToStart();
    transportRunning = nowRunning;

    float transportRatio = transportRunning ? lastKnownRatio : 0.0f;

    // Scrub gain follows the *effective* ratio, not the knob's. Stopped with
    // the knob parked at 20x would otherwise give 20x scrub sensitivity
    // while nothing is playing, which is both twitchy and wrong for the
    // metaphor - a hand on a stationary reel should feel the same whatever
    // speed the deck would run at. Floored at 1x, so stopping never makes
    // scrubbing less effective than it already was.
    float speedMagnitude = fabsf(transportRatio);
    float scrubGain = (speedMagnitude > 1.0f) ? speedMagnitude : 1.0f;

    float step = transportRatio * rateFactor + scrub * scrubGain;

    // Until the cached copies are built, high speeds are capped at
    // MAX_STEP_UNCACHED samples per tick through the highest ready copy (or
    // the WAV itself). Not because the card couldn't keep up with more on
    // its own - it's the building: a build step occasionally stalls loop()
    // for 30ms+ while the card finishes a write, and the ring buffer has to
    // ride that out. At 2 samples per tick it holds ~130ms of runway; 8 was
    // tried, and underran.
    const float MAX_STEP_UNCACHED = 2.0f;
    uint8_t ready = readyLevels();
    if (ready < cache.levelCount()) {
      float cap = ldexpf(MAX_STEP_UNCACHED, ready);
      if (fabsf(step) > cap) step = copysignf(cap, step);
    }
    currentStep = step;
    stageDone(STAGE_CONTROL, t);

    // Move to a different copy if this speed calls for one. Not while
    // paused - there's nothing to hear, and the tape will most likely
    // start again at the speed it stopped at - and not while a crossfade
    // is already under way.
    if (fileLoaded && fadingVoice < 0 && step != 0.0f &&
        millis() - levelChangeFailedMillis >= LEVEL_CHANGE_RETRY_MS) {
      uint8_t current = voices[activeVoice].level;
      uint8_t target = chooseLevel(fabsf(step), current, ready);
      t = DWT->CYCCNT;
      if (target != current) startLevelChange(target);
      stageDone(STAGE_COPY_CHANGE, t);
    }

    // Transport indicator: steady at full level when latched, blinking at
    // half level when running from a held finger, dark when stopped.
    //
    // FastLED.show() bit-bangs the WS2812's timing and blocks (and on most
    // platforms masks interrupts) for tens of microseconds - long enough to
    // straddle a 48kHz audio tick. So it is called only when the colour
    // actually changes, which during a blink is a handful of times a second
    // rather than a hundred.
    uint32_t ledStartMicros = micros();  // see "loop() profiling" on why not the cycle counter
    bool blinking = transportRunning && !transportLatched;
    if (blinking && !wasBlinking) blinkPhaseStartMillis = currentMillis;
    wasBlinking = blinking;

    // Colour carries the knob's mode, brightness and blink carry the
    // transport: green while knob1 sets speed, blue while it sets pitch.
    // Without this the mode toggle is completely invisible - you would have
    // to turn the knob and listen to find out which one you were in.
    uint8_t level = 0;
    if (transportLatched) {
      level = RGBLED_MAX_BRIGHTNESS;
    } else if (blinking) {
      unsigned long phase = currentMillis - blinkPhaseStartMillis;
      if (((phase / RGBLED_BLINK_INTERVAL) % 2) == 0) level = RGBLED_DIM_BRIGHTNESS;
    }

    CRGB wanted = level == 0
                      ? CRGB::Black
                      : (pitchMode ? CRGB(0, 0, rgbLevel(level))
                                   : CRGB(0, rgbLevel(level), 0));
    if (wanted != leds[0]) {
      leds[0] = wanted;
      FastLED.show();
    }
    stageTook(STAGE_LED, (micros() - ledStartMicros) * (SystemCoreClock / 1000000));
  }

  // Deliberately short and infrequent: Serial here is native USB CDC
  // (see CLAUDE.md), and a blocking print sitting in a hot loop would
  // itself steal cycles from the SD refill above, manufacturing the
  // very underrun this demo is trying to measure honestly.
  t = DWT->CYCCNT;
  if (currentMillis - previousStatusMillis >= STATUS_INTERVAL) {
    previousStatusMillis = currentMillis;
    float ratio = currentStep / rateFactor;
    float fillPercent = 100.0f * voices[activeVoice].ring.usedSpace() / RING_CAPACITY;
    uint8_t level = fileLoaded ? voices[activeVoice].level : 0;
    int cachePercent = (cache.name() == currentFilename) ? cache.percentDone() : 0;
    // Four decimals below 1x, since the knob now slows smoothly right down
    // to zero and two would show anything under 0.005x as "0.00x".
    int speedDecimals = fabsf(ratio) < 1.0f ? 4 : 2;
    Serial.print("speed ");
    Serial.print(ratio, speedDecimals);
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
    Serial.print(underrunTicks);
    Serial.print("  copy ");
    Serial.print(level);
    Serial.print("  cache ");
    Serial.print(cachePercent);
    Serial.print("%  isr avg ");
    uint32_t count = isrCount;
    Serial.print(count ? 100 * (isrCyclesSum / count) / cyclesPerTick : 0);
    Serial.print("% max ");
    Serial.print(100 * isrCyclesMax / cyclesPerTick);
    Serial.print("%  dropped ");
    Serial.println(droppedTicks());
    isrCyclesMax = 0;
    isrCyclesSum = 0;
    isrCount = 0;

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
      // RUN:, not PLAY: - "PLAY:" is already an inbound command meaning
      // "load and play this file", and reusing it for transport state would
      // be ambiguous to anyone reading a raw trace.
      //
      // Note SPD: is the *effective* ratio (currentStep / rateFactor, just
      // above), not the knob's requested one, so it reads 0.00 while stopped
      // and reflects scrub while running. That is the pre-existing meaning
      // of this field and RUN: does not change it - but it does mean a
      // client cannot show "1.5x (stopped)"; the requested ratio would need
      // its own field.
      // REQ: is the speed knob's *requested* ratio, which SPD: cannot show
      // while stopped (SPD: is the effective ratio, so it reads 0.00 then).
      // Lets a client display the speed the transport would run at if
      // started, rather than losing the setting whenever the tape is
      // stopped.
      String stat = "SPD:" + String(ratio, speedDecimals) +
                     ",REQ:" + String(lastKnownRatio, fabsf(lastKnownRatio) < 1.0f ? 4 : 2) +
                     ",PIT:" + String(pitchRatio, 3) +
                     ",BUF:" + String((int)fillPercent) + ",UND:" + String(underrunTicks) +
                     ",RUN:" + String(transportRunning ? 1 : 0) +
                     ",LAT:" + String(transportLatched ? 1 : 0) +
                     ",LVL:" + String(level) + ",CACHE:" + String(cachePercent);
      if (probing) {
        probeSentMillis = millis();
        awaitingProbeEcho = true;
      }
      blePrintLine(stat);
    }

    minRatioSeen = 1e9f;
    maxRatioSeen = -1e9f;
  }
  stageDone(STAGE_STATUS, t);

  uint32_t passCycles = DWT->CYCCNT - passStart;
  if (passCycles < STAGE_BOGUS_CYCLES && passCycles > profileMaxPassCycles) {
    profileMaxPassCycles = passCycles;
  }
  profilePasses++;
}
