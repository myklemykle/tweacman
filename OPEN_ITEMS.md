# Open items

Things discussed but not yet done, as of 2026-09-21. Written for offline
review - nothing here is in progress. Roughly grouped, and within each group
roughly in the order they'd be worth doing.

Design proposals large enough to have their own document are linked rather
than repeated: see `CodelessPipe_design.md`.

## SDResampleDemo - tape transport

- **Latching play.** The transport is currently momentary: touch pad D21 held
  runs the tape, released stops it. A real deck's Play button latches. Agreed
  to add "soon". Worth knowing before then: if the touch pad chatters at all,
  a latching toggle amplifies that far more than a momentary hold does, since
  every spurious edge flips state instead of causing a brief dropout.
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

The throwaway-conversion fix (RV112FF, commit `cc33586`) removed the
*directional* bias one pot induced in the other. What remains is ordinary
random jitter, amplified about a hundredfold by differentiating angle into
RPM over a 10ms tick.

- **Differentiate over a longer baseline.** Compute `Δangle` over ~50-100ms
  while still updating position every tick. This attacks the 1/dt
  amplification directly, and is meaningfully different from raising
  `rpmSmoothTau` - post-filtering an already-noisy derivative adds the
  "momentum" feel that was deliberately tuned out, whereas a longer
  differentiation window reduces the noise before it exists. **Judged the
  highest-value remaining change for scrub feel.**
- **Oversample / median.** Averaging N reads per channel cuts random noise by
  sqrt(N); a median-of-3 or -5 kills impulsive outliers without averaging's
  lag. Worth doing after the baseline change, if anything is left.
- **Reverse-order double pass.** Read A2,A3,A4,A5 then A5,A4,A3,A2 and average,
  cancelling any residual linear ordering bias. More complex; only if the
  throwaway read turns out to be insufficient.
- **Re-tune `SCRUB_DEADBAND_RPM` downward.** Currently 0.5, sized to the ADC
  jitter as it stood before any of the above. A deadband wider than the noise
  it rejects is pure lost sensitivity. The constant carries this note in the
  source too.
- **Buffer the wipers (hardware).** An op-amp follower per wiper lowers source
  impedance and is the real fix for settling. Relevant to the `feature/pcb`
  branch while the schematic is still editable - four buffered wipers would
  make all of the software mitigations above easier or unnecessary.

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
