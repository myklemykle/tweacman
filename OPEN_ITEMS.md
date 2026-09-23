# Open items

Things discussed but not yet done, as of 2026-09-21. Written for offline
review - nothing here is in progress. Roughly grouped, and within each group
roughly in the order they'd be worth doing.

Design proposals large enough to have their own document are linked rather
than repeated: see `CodelessPipe_design.md`.

## SDResampleDemo - tape transport

- **Spurious transport latching: a common-mode disturbance on TSCAP.**
  PARKED for the PCB, where better shielding should make it straightforward.
  Measured 2026-09-21, and the measurements rule out every software fix tried
  or considered:
  - Touch deltas are sharply *bimodal* - either ~20 (idle) or ~3500-7800.
    Across 209 samples exactly one landed in the whole 100-2999 band. So the
    spurious events are full-strength capacitive signals reaching more than
    twice the 3000 threshold, not marginal noise. **No threshold and no
    debounce can reject them**, because they are larger than a real touch.
  - They are *common-mode*: 8 of 11 events fired on both pads at once, and
    when both fired the two channels agreed to within 163 counts on ~7000
    (2.3%). A finger is local to one pad; this moves both together, which
    points at the TSCAP reference they share on D6.
  - It happens with hands completely away from the board, so it is not
    proximity to the bare-wire pads.
  - It correlates weakly with system load: 340k loop passes/sec during
    events versus 358k without, so supply or reference coupling from CPU and
    SD activity is the likely path.
  - **Rejecting by similarity does not work either.** A deliberate two-pad
    chord was measured at 0.0%-7.9% mismatch (mean 4.0%), overlapping the
    2.3% of the disturbance - with real chord samples *tighter* than it. No
    separating threshold exists.
  - **D31/TS11 is not usable as a common-mode reference.** It is the onboard
    USR button with its own circuitry attached, and swings 0 to ~12000 on its
    own while both real pads sit quiet.
  Remaining avenues are all physical: TSCAP capacitor value, a grounded guard
  between the pads, and supply decoupling - i.e. the carrier board.
  **Probably fixed - re-test.** On 2026-09-23 a separate CapTouch bug was
  found and fixed: under interrupt load, `scan()` could file one pad's result
  under the other (holding D21 put ~4150-count single-scan spikes on D20; 22
  in 15s before the fix, 0 after, with NoiseMeter's DAC/SD/LED loads on).
  The spurious latching only ever happened while touch2 (D21) was held down,
  which is exactly when that bug shows, so this is very likely the same
  thing - the "common-mode" readings above would then have been one pad's
  value filed under both. Confirm in SDResampleDemo before unparking the
  physical avenues.
- **Touch chatter now has consequences.** Latching play is implemented (D20
  chorded with D21). A momentary hold forgives a spurious touch edge as a
  brief dropout, but a latch does not: a stray rising edge on D21 silently
  clears the latch, and you only find out when lifting your finger stops
  playback unexpectedly. If this shows up in use, debounce D21's rising edge
  specifically rather than the pads in general.
- **`FastLED.show()` versus the audio interrupt.** show() bit-bangs WS2812
  timing and blocks for tens of microseconds - long enough to straddle a
  48kHz audio tick, and on most platforms it masks interrupts while doing it.
  Mitigated by only calling it when the colour actually changes (a few times
  a second while blinking, not 100Hz), but it has not been confirmed
  click-free by ear. Listen at blink transitions.
- **Pause versus restart.** Every start rewinds to the top, including after
  the latch is cleared and the transport stopped. Consistent, and chosen
  deliberately, but once a latch exists people tend to expect stop-then-play
  to resume rather than restart. Revisit if it feels wrong in use.
- **Clicks on start/stop.** Deferred until actually heard. Stopping mid-waveform
  holds a DC value on the DAC and resuming jumps back into the waveform; through
  A1's 4.7uF coupling cap that is a click, and A0 has no cap at all. Now that
  stopping is a routine action rather than something that never happened, this
  is much more likely to show up. The fix is to ramp `lastGoodSample` toward
  midscale over a few ms rather than stepping.
- **Scrub: override instead of add.** Scrub is currently summed onto knob1's
  ratio, so forward playback partially cancels a backward scrub. Raising
  `SCRUB_SCALE` outran the cancellation but did not remove it. Overriding
  knob1 while `|scrub|` is above a threshold would remove it properly, and
  would make the pause-deadband parking trick unnecessary.
- **Scrub gain cap.** `scrubGain` is `max(1, |effective ratio|)` with no upper
  limit, so at 40x a small hand movement is a 40x cursor jump - and at those
  speeds the SD card is already near its bandwidth wall, so a big scrub may
  slam the buffer into underruns. Left uncapped deliberately rather than
  guessing a limit; revisit if it feels unmanageable up there.

## ADC noise on the pots

Resolved 2026-09-23 - see `NOISE_DEBUGGING.md` for the whole investigation.
Crosstalk between the pots is gone (`RA6M5AnalogTuning`: longer ADC sampling
time plus discharge-before-sample, 230-280 counts down to about 1), and the
scrub "crumbs" at rest are gone (4x oversampling on the scrub knob, plus
capacitors from the wiper pins to ground). What's left, for the record:

- **~~Differentiate over a longer baseline.~~ Rejected.** It would cut rpm()
  noise, but adds tens of milliseconds of lag to a scrub; the rule for this
  project's controls is no UI artifact longer than 10ms. Oversampling did the
  job with no lag instead.
- **~~Reverse-order double pass.~~ Not possible through `analogRead()`.**
  The core's `analogRead()` converts every pin ever read on that ADC unit, in
  fixed channel order (A5, A4, A3, A2), whatever order the sketch asks in.
- **`SCRUB_DEADBAND_RPM` stays at 0.5.** Its source comment invites re-tuning
  it down once the noise is lower; tried by feel after the fixes and judged
  right as it is - scrub is already very sensitive.
- **Buffer the wipers (hardware)** - op-amp followers, for the `feature/pcb`
  schematic. Probably unnecessary now; the capacitors on the wiper pins and
  the ADC settings cover what they would have. Keep in mind if the PCB's
  pots misbehave.

## BLE / CodeLess

- **`HelloWorldBLE` needs `bleExitBinaryMode()`.** It never uses Binary mode,
  but it gets wedged by whichever sketch ran before it: a module left in
  Binary mode swallows its `ATR`/`AT+ADVSTOP`/`AT+ADVSTART` as raw pipe data,
  so startup appears to succeed and nothing works. The one sketch broken by a
  feature it does not use. Explicitly deferred.
- **The `+++` acknowledgement churn.** The module asks to leave Binary mode by
  sending `+++` every two seconds; both sketches now answer it, but the reply
  (`AT`/`AT+BINREQEXITACK`) goes out *as pipe data* while Binary mode is live,
  so the module likely never sees it and keeps asking. The link recovers every
  time, so this is cosmetic - but clients occasionally see stray `AT+...`
  lines in their data stream. Really a symptom of the open question below.
- **`CodelessPipe` library.** Full proposal in `CodelessPipe_design.md`. Its
  two unresolved questions, both of which change the design:
  - What the mode-detection probe should send when a sketch has no traffic of
    its own (proposed: a lone `"\n"`).
  - Whether the sketch can *force* the mode transition - e.g. `AT+DISCONNECT`
    right after `AT+BINREQACK` - instead of waiting for the central to hang
    up. Untested. If it works, the handshake gets much simpler and the `+++`
    churn above probably disappears with it.

## Toolchain / bench reliability

- **USB and DFU flakiness makes iteration slow.** Raised as a real cost, to be
  tackled deliberately rather than worked around. Observed repeatedly on
  2026-09-21:
  - `make upload` fails with `dfu-util: Cannot set alternate interface zero:
    LIBUSB_ERROR_OTHER` (exit status 74) while the board still enumerates
    perfectly well as a serial port. Retrying sometimes works immediately;
    at one point it failed seven times running and was only cleared by
    physically unplugging, waiting, and replugging.
  - Separately, the serial device node vanished entirely for several minutes
    after a DFU attempt - no `/dev/cu.usbmodem*`, nothing in
    `arduino-cli board list`, and `dfu-util -l` saw no DFU device either -
    until an unplug/replug.
  - An inline USB power monitor was in the chain for most of the session and
    has since been removed; a plausible contributor, not yet confirmed.
  - Our own tooling handles the post-reset re-enumeration only partly: the
    upload/monitor sequence waits for the device *node* to exist, but the
    node can appear before the CDC endpoint is ready, which is what produced
    `Port monitor error: timeout waiting for message` mid-session. A
    readiness check (or a short retry on first read) would be more honest
    than the current existence check plus a fixed sleep.

## Web pages

- **Redeploy the hosted copies.** `mykle.com/webble/oscillator-remote.html` is
  still the pre-fix version, and `wav-remote.html` has changed too (device
  filter, transport pill, potential-speed readout). Neither phone sees any of
  today's work until they are redeployed.

## SDResampleDemo - other

- **`WavReader` only handles mono 16-bit PCM.** Confirmed live: `boom.wav` on
  the test card returns `ERR:boom.wav:this demo only plays mono 16-bit PCM WAV
  files`. Stereo, 8/24-bit, float and WAVE_FORMAT_EXTENSIBLE are all present in
  a real sample library. Failing cleanly over BLE is the correct behaviour and
  not a bug, but broader format support would make the demo far less annoying
  to use with real files.
- **Re-benchmark the underrun ceiling.** The header quotes ~23-25x forward as
  the measured onset point, from before bidirectional support reserved
  `REVERSAL_MARGIN` of the ring as standing reversal headroom. That reservation
  lowers the real forward runway, so the number is stale. Worth re-measuring in
  both directions against whatever card is on the bench.
