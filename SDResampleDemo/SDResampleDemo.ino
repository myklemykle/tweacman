// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  SDResampleDemo - SparkFun Thing Plus RA6M5

  Streams a WAV file off the onboard SD card and plays it out the DAC,
  with one pot controlling a completely smooth, arbitrary, BIDIRECTIONAL
  playback-speed ratio - not discrete multiples, and slowing down (in
  either direction) genuinely interpolates new sample values rather than
  just repeating or stepping through existing ones. The pot sweeps from
  -MAX_RATIO smoothly through zero at the middle up to +MAX_RATIO;
  negative plays the file backward. A second pot scrubs, like a hand
  on the reel - see "Tape scrub" below. A phone/browser can also pick
  which file plays and watch the same live stats over BLE - see "BLE
  control" below.

  Wiring: an SD card (FAT32, any SDHC card - see the SD note in
  CLAUDE.md, this board's slot is wired to the RA6M5's native SDHI
  peripheral, not SPI) with one or more WAV files at its root - use
  tools/make_test_wav.py to generate a simple one, a slow sine sweep
  that makes speed changes and any resampling artifacts obvious by ear.
  Otherwise same as DACAudioTest (3.5mm jack on A0/A1/GND) plus two pots
  (RV112FF wipers on A2/A3 for speed or pitch, A4/A5 for tape scrub or
  time-stretch, both pots' track ends on 3.3V/GND) and two CapTouch pads
  (D20 steps through the knob modes, and chorded with D21 latches the
  transport;
  D21 is the transport itself - hold it to run, release to stop; both
  share the TSCAP capacitor on D6 - see RA6M5CapTouch.h). The onboard WS2812
  on D13 shows the knob mode and the blue STAT LED the transport - no extra
  wiring. The DA14531MOD Bluetooth
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
  aliasing stays out of the way. Changing copies is a straight swap
  between two playback voices, one playing while the other is readied
  (see "Playback voices" below, and startLevelChange()). Copies are built in the background the first time
  a file is cued - a few seconds for a short sample - and until they're
  ready the fastest speeds are capped; the Serial status and the BLE
  CACHE: field show progress. The copies live as long as their WAV and
  are rebuilt if it changes - see "BLE control" below for what happens to
  them when the web page uploads, renames or deletes a file.

  The copies alone don't make changing between them seamless, though.
  Each copy is free of aliasing only up to one sample per tick, so reading
  one any faster - which happens just below every switch - used to alias,
  and the switch made it suddenly vanish. What fixes that is the interpolator: it
  low-passes as it reads (SincInterpolator.h), so every copy is band-
  limited the same way on both sides of a switch. That costs CPU in the
  audio interrupt - the Serial status line reports how much ("isr avg/max",
  roughly 20-30% for mono and 30-40% for stereo normally, up to about 45%
  and 60% with the pitch shifter on, and about 12% more while the filter
  is in use - see "Filter"), along with "dropped", a count of audio ticks that never ran because
  something held interrupts off for longer than a tick (it restarts at
  each file load and transport start, which pause the audio on purpose).
  It should stay at zero; if it climbs, something is blocking too long.

  Tape scrub: turning the second pot takes over the playback cursor
  outright, like grabbing the reel - the transport waits underneath, and
  when the knob has been still for SCRUB_RELEASE_MS (150ms) playback
  carries on from wherever the knob left the tape. It works the same
  whether the transport runs or not. The knob's change in angle, not its
  speed, sets where the cursor should be, and the cursor is steered onto
  that target - so the knob's position maps exactly onto the file, and a
  small backlash (SCRUB_BACKLASH_DEGREES) keeps a resting knob silent
  without losing any of a long turn. How much of the file a turn covers
  is fitted to each file as it loads: about one second of audio, moved
  to the nearest power-of-two fraction or multiple of the file's length,
  so whole turns line up with the file's loop - see
  SCRUB_SECONDS_PER_TURN and scrubKnob().

  Relative speed control: the first pot is RELATIVE (like a DJ jog wheel
  or a DAW's jog control), not ABSOLUTE (like a normal volume knob, where
  a given shaft position always means the same value). Each file starts
  at its own saved speed - native (1.0x) for a file played for the first
  time, see "Per-file settings" below - regardless of wherever the
  physical pot happens to be sitting; only turning the pot moves speed
  from there, and by however much you turn it, not to wherever the pot's
  absolute position would otherwise map. This matters because the pot
  has no mechanical center detent or home position (see RV112FF.h - it's
  an endless-rotation part with no absolute "zero" a hobbyist could
  return it to), so an absolute mapping would make "start every file at
  native speed" impossible to hit reliably by hand. See knobForSpeed()/resetVirtualKnobToNative() and
  the control-rate block in loop() for the implementation - a small
  virtual position that starts anchored at native speed and accumulates
  the pot's own physical deltas from then on, the same idea as a
  relative/incremental encoder even though the underlying sensor is an
  absolute one.

  Per-file settings: each WAV keeps its own speed, pitch, window (see
  "Start and end points"), echo and filter, in a small text file under the card's settings/ folder (see FileSettings.h), so
  picking it again - or rebooting - brings them back. They're saved two
  seconds after the last change, and at once when another file is picked.
  The same folder remembers which file was cued, and the board cues it
  again at boot instead of the first file on the card. Settings follow a
  file when it's renamed, go when it's deleted, and survive uploading a
  new version of it.

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
  to list the WAV files on the card, pick one, see and change everything
  the knobs set (a card per knob mode, the current one highlighted), and
  watch the same buffer/underrun stats live instead of over USB Serial.
  Reuses the
  CodeLess Binary mode/DSPS bidirectional protocol already validated in
  AudioKnobsWebBLEDemo - see that sketch and the codeless-binary-mode-
  dsps-protocol memory for the handshake itself. On top of that
  transport, this sketch speaks a small plain-text line protocol:
  `LIST` requests a `FILES:BEGIN`/one-name-per-line/`FILES:END` block,
  in the order the file knob steps through them (natural, case-blind -
  see WavList.h), so a client shows them as they come;
  `PLAY:<name>` requests a switch, answered with `PLAYING:<name>` on
  success or `ERR:<name>:<reason>` on failure - a bad or unsupported
  file is a normal, reported failure, not a hang. The periodic stats line
  carries `RUN:0|1` alongside `SPD:`/`BUF:`/`UND:`, so a client can show
  whether the transport is running. `SPD:` is the effective ratio, so it
  reads 0.00 while stopped and follows the scrub knob while it's turning; `REQ:` is the
  speed knob's requested ratio, which survives being stopped, so a client
  can show what the transport *would* run at; `PIT:` is the pitch ratio,
  1.000 meaning unshifted; `ECT:` and `ECA:` are the echo's delay in seconds
  and its amount; `FLC:` and `FLW:` the filter's centre in Hz and its width
  in octaves, 0 to 10 (10.00 is wide open, no filter; see setFilterBand()); `LAT:0|1` is whether the transport is latched;
  `LVL:` is which copy is playing (0 the original, k the cached copy at
  1/2^k its rate); `CACHE:` is how far building those copies has got, in
  percent. For the page's tweaks tab there are also `MODE:`, the knob mode
  (0 for the first row of MODES); `HRD:`, the pitch you hear (the shift
  times the speed, which is what the pitch knob sets); and `WS:`, `WE:` and
  `LEN:`, the window's start and end and the file's length, in seconds.

  The page can also drive the transport and manage the card:
    `LATCH:<name>`       load a file and latch the transport on - plays from
                         the top and keeps playing, answered like PLAY:
    `STOP`               clear the latch, as a touch2 press would
    `RESET:SPEED`        speed back to 1.0x
    `RESET:ALL`          everything saved per file back to its default, as
                         the triple tap does - see "Reset"
    `MODE:<n>`           select a knob mode, 0 for the first, as stepping
                         to it with D20 would
    `HEARD:<ratio>`      set the pitch you hear, as the pitch knob does:
                         the shift becomes whatever makes the current
                         speed sound at it
    `STRETCH:<ratio>`    set the speed with the pitch you hear held, as
                         the time-stretch knob does
    `TAPE:<ratio>`       set the speed and let the pitch follow, as the
                         speed knob does. (`SPEED:` is one or the other,
                         depending on the knob mode; these two say which.)
    `RESET:PITCH`        pitch back to unshifted
    `SPEED:<ratio>`      jump to an exact speed (negative is backward, 0
                         pauses); the knob carries on from there
    `PITCH:<ratio>`      set an exact pitch ratio (1 is unshifted)
    `WINDOW:<s>,<e>`     play only from <s> to <e> seconds into the file -
                         see "Start and end points"; `RESET:WINDOW` goes
                         back to the whole file
    `ECHO:<s>,<amount>`  set the echo: a delay of <s> seconds, and an amount
                         from 0 (none) to 0.95 - see "Echo"; `RESET:ECHO`
                         turns it off
    `FILTER:<low>,<high>` set the filter's bottom and top cutoffs, in Hz,
                         from 20 to 20480 (both at the ends is wide open,
                         no filter; both together, it screams) - see
                         "Filter"; `RESET:FILTER` opens it
    `BAND:<centre>,<oct>` the same, as the knobs set it: the band's centre
                         in Hz and its width in octaves, 0 to 10 (10 is
                         wide open)
    `RESET:CACHE`        throw away the current file's cached copies and
                         build them again - for a copy suspected of being
                         damaged, since they're otherwise kept up to date
                         on their own
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
    `CAL`                print, over USB Serial, each knob's saved and live
                         calibration - see "Knob calibration" further down
    `RECAL`              forget both knobs' saved calibration and learn it
                         again
    `DISK:ON`            make the card a USB disk on the computer the board
                         is plugged into - answered `DISKMODE:ON`, repeated
                         in place of the status line until it's ejected
                         there, then `DISKMODE:OFF`; see "USB disk mode".
                         (Not DISK:ON back: in command mode the module
                         echoes everything, and the board would take its
                         own reply for the command again)
    `DISK:OFF`           take the card back without waiting for the eject -
                         an emergency exit only: the computer isn't told,
                         keeps the disk mounted with its own (soon stale)
                         copy of the file table, and could corrupt the card
                         with it the next time DISK:ON opens it up. Eject
                         it on the computer first, or unplug the cable
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
  D20 while D21 is held - or D21 while D20 is held, the chord works in
  either order - latches it on, so playback continues after the fingers
  lift; the next press of D21 on its own clears the latch, without interrupting
  playback, since that press is holding the transport up by itself. Starting
  rewinds to the top of the file, so each start plays from the beginning
  rather than resuming where scrubbing left off - engaging or clearing the
  latch mid-playback does not rewind, because playback never stops. Scrubbing
  still works while stopped - that is the point of expressing "stopped" as a
  speed of zero.

  Reset: tapping both pads together three times in a row puts the current
  file's speed, pitch, start and end points, echo and filter back to their
  defaults, and the RGB LED flashes white to say so. The first of the
  three taps latches the transport like any other chord, and it stays
  latched. The taps have to be taps - each under 0.4 seconds, and no more
  than that apart - so latching and holding never counts toward one. See
  "Triple tap" in the code.

  Knob modes: tapping D20 on its own (not chorded with D21, which latches)
  steps to the next knob mode, round and round. Holding it turns the knobs
  into selectors: knob1 steps through the modes, forward or back, one every
  60 degrees, so any mode is one gesture away; knob2 steps through the WAVs
  on the card, one every 30 degrees, and plays each one it lands on from
  its start, until D20 is released - or on past that, if the transport was
  already latched on. Each mode gives the two
  knobs a different job, and is one row of the MODES table - see "Knob
  modes" further down. Five are built so far. In speed mode knob1 sets the
  speed, tape-style (any pitch shift stays a ratio on top), and knob2
  scrubs. In pitch mode knob1 sets the pitch you hear and knob2 stretches
  time - see below. Switching never changes the sound by itself. Pitch is saved per file, like
  speed (see "Per-file settings"). The pitch knob is relative, at
  PITCH_OCTAVES_PER_TURN octaves per turn from wherever the pitch is, out to
  PITCH_OCTAVE_LIMIT octaves either way, where it stops. Speed
  and pitch are genuinely independent: the nominal playback position still
  advances at the speed ratio alone - which is what the SD refill is built
  around - while a two-tap granular shifter reads around it at speed * pitch.
  See the pitch shifting comment further down for how that works and what
  it costs.

  Time-stretch: in pitch mode, knob2 turns the same speed dial knob1 does in
  speed mode - the full range, through zero and backward - while the pitch
  you hear stays where knob1 set it. The pitch shifter's grains read at that
  pitch whatever the speed (see grainTapRate), which also works at a speed of
  zero: the grains keep playing in place, a granular freeze. To keep the
  treble, the cached copy is chosen by the grains' read rate rather than the
  speed (see the copy change in loop()), which keeps full quality up to about
  8x; beyond that the copies take over and the treble drops away. Expect a
  sound rather than a studio stretcher: clean around 0.5x-2x, more grainy
  further out.

  Start and end points: in the third mode (purple), knob1 moves the start of
  the stretch of file that plays and knob2 its end - the "window". The other
  modes then treat the window as the whole tape: it loops, the transport
  rewinds to its start (so tapping D21 auditions the start point), and the
  scrub goes round it. Both knobs are relative, at the scrub knob's rate.
  The two ends can't come closer than WINDOW_MIN_SECONDS (20ms): push one
  into the other and both move, so at that width either knob slides the
  window along the file. An end stops at the end of the file, rather than
  carrying on round it. The join from the window's end back to its start is
  crossfaded over a few milliseconds, so it doesn't click (the whole file,
  with nothing either side to fade with, loops as it always did). If an end
  is moved past the playing position,
  playback carries on from the window's start. Moving the window never
  interrupts the sound - see "The window" further down for how, given that
  audio is read from the card well ahead of where it's playing.

  Echo: in the fourth mode (pink), knob1 sets an echo's delay time and
  knob2 how much of it there is - one amount for both its level and its
  feedback, so more means louder repeats and more of them. It's off until
  knob2 is turned up. The delay runs from a millisecond (not an echo at that
  length but a metallic ring) to 2.7 seconds, three octaves per turn, and
  turning it bends the pitch of the echoes already on their way, as moving
  the head of a tape echo would. The echoes are mono and duller than the
  sound itself, increasingly so as they repeat: the delay is kept in RAM,
  and making 2.7 seconds of it fit meant keeping less of the signal - see
  EchoDelay.h, which explains all of this. The echo applies to whatever
  comes out, in every mode. When the transport stops, the echoes neither
  ring on for ever nor are cut dead: they carry on repeating while fading
  out smoothly over three delay times (half a second at least), and then the
  feedback is taken away. Scrubbing a stopped tape brings the echo back for
  as long as the knob keeps moving, and it fades the same way when the knob
  comes to rest. Both settings are saved per file.

  Filter: in the fifth mode (orange), the knobs play a resonant band-pass
  filter, in the spirit of a Korg MS-20's: knob1 sets the band's width, from
  ten octaves - wide open, the whole audible range, no filter at all - down
  to nothing, and knob2 its centre - five octaves per turn round the
  middle, 640Hz, faster further out, two turns each way. Wide open is the
  top of a hill for knob1: turning either way from it narrows the band,
  quickly at first, so a quarter turn
  is plain to hear, then more slowly, over a turn and a half in all;
  turning back stops at wide open. Narrowing the band, the sound thins to
  it, with a bump at each edge, and over the last couple of octaves the
  resonance rises until the filter screams by itself, as an analog one
  does - and knob2 then plays the scream's pitch. Moving the centre slides
  both edges of the band together, keeping the width; even wide open, that
  brings one edge into hearing. At the ends of its travel the whole band
  has slid out past what you can hear - which a digital filter can't
  really do, so the sound is faded instead as the filter's own slope would
  fade it - and it's silent - see setFilterBand() and BandFilter.h. The
  scream is kept in
  hand by soft clipping, as the MS-20's is by diodes, so it settles at a
  steady level rather than running away. When the transport stops it fades
  out with the echo, over three echo delay times - otherwise someone outside
  filter mode could be left with a scream they couldn't find the knob for -
  and it comes back when the tape plays again. The filter comes before the
  echo, so the echoes are filtered too. It costs about 12% of the audio
  interrupt's time while in use, and nothing wide open. See BandFilter.h,
  which explains how, and tools/filter_render.cpp, which runs the same
  filter on a computer for measuring and listening. Both settings are saved
  per file.

  Two LEDs show the state. The onboard WS2812 on D13 is always lit, in the
  colour of the knob mode: green for speed mode, aqua for pitch mode, purple
  for the window, pink for the echo, orange for the filter (no red or blue, which the board's other LEDs already use) - at RGBLED_BRIGHTNESS or a little
  less, 10% of what the LED can actually do, since it is startlingly bright
  otherwise. The blue STAT LED shows the transport: dark when stopped,
  blinking while running from a held finger, steady when latched. (One LED used to do both jobs, by colour and by blinking, which
  left the mode invisible whenever the transport was stopped.)

  USB disk mode: DISK:ON (the web page's "USB disk" button) hands the SD
  card to the computer at the other end of the USB cable, where it appears
  as a drive - far quicker than uploading over BLE for moving WAVs on and
  off. The board stops playing, saves the file's settings, unmounts the
  card and then opens UsbDiskGate (UsbDiskGate.h), which until then has
  told the computer there's no card in the drive. Ejecting the drive on the
  computer, or unplugging the cable (or the computer going to sleep - see
  watchUsbHost()), closes the gate again; the sketch
  mounts the card, tidies up after any files deleted or renamed there (as
  at boot) and cues the remembered file. (DISK:OFF also takes it back, but
  the computer isn't told, so it's an emergency exit only. Dropping off USB
  and reconnecting, to make the computer look again, was tried - with
  tud_disconnect()/tud_connect() - and hung the board.) Only one side may
  ever have the card: each keeps its own copy of the file table in memory,
  and two writers would corrupt it. The USB serial port keeps working
  throughout. It needs the core's UsbMsd
  library plus a workaround for a bug in it on this board's full-speed USB
  port, without which the board doesn't come up on USB at all - see
  fixUsbDiskPacketSize(). macOS leaves a hidden "._" file beside every file
  it copies onto a FAT card; names starting with a dot are never listed.

  File formats: mono or stereo, at any sample rate, in 8-, 16-, 24- or
  32-bit PCM or 32- or 64-bit float, with plain or WAVE_FORMAT_EXTENSIBLE
  headers - see WavReader.h, which converts each to the 16 bits everything
  else here works in. Stereo plays left out of A0 (the jack's tip) and right
  out of A1 (its ring); mono plays the same signal out of both. Files with
  more channels, and compressed ones (ADPCM, A-law...), are refused with a
  clear ERR: over BLE. tools/make_format_test_wavs.py writes a set of test
  files covering all of it, each playing the same three notes (an octave
  lower on the right, for the stereo ones), so at 1x they should all sound
  alike.

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
       to 1024x - underrunTicks should stay at zero, and "Switched to
       copy N" lines should appear as the speed passes each power of two.
       Listen at those moments for anything untoward.
       Then reboot and confirm the copies are reused ("building" doesn't
       appear again), and delete the card's cache/ folder from a computer
       to confirm it's rebuilt.
    6. Open web/wav-remote.html, connect, confirm LIST populates real
       filenames from the card, PLAY: on a supported file switches
       cleanly, and PLAY: on anything else (a stereo file, say) returns a
       clean ERR: instead of hanging. Play each of the
       make_format_test_wavs.py files at 1x and confirm they all sound
       the same - the same notes, at the same tempo - and that the stereo
       ones (t2_...) are an octave lower in the right ear.
    7. With the transport stopped (pad released), twist the second pot
       back and forth - confirm it sounds like manually scrubbing tape
       (pitch and direction tracking the twist, silent when you stop), and
       that a knob left alone makes no sound at all. Serial says how much
       of the file one turn covers ("Scrub: one turn = ..."); turn that
       many turns and confirm you land back on the same sound. Then play
       at some steady speed and twist it again: it takes over completely,
       and playback carries on from where you left the tape once the knob
       has been still for a moment.
    8. Confirm relative control: with the first pot sitting anywhere at
       all, load a file that has never been played and confirm playback
       starts at native (1.0x) speed regardless of the pot's physical
       position. Then turn the pot a small amount
       and confirm speed changes by a correspondingly small amount from
       wherever it just was - not by jumping to whatever an absolute
       mapping of the pot's new position would imply.
    9. From the page: play a file with its play button and confirm it
       latches (steady STAT LED, keeps playing with no pad touched), and
       that Stop releases it; the status line says which, with the file's
       name. On the Tweaks tab, tap each mode's card and confirm the RGB
       LED changes colour to match, then step the mode with D20 and
       confirm the page's highlight follows. Drag each slider and confirm
       you hear it (and that Speed bends the pitch where Stretch doesn't);
       turn a knob and confirm its slider follows. Tap a value, type a
       number, and confirm it's set. Reset all tweaks (two taps) puts
       everything back.
    10. Upload a short WAV, then upload it again to confirm the overwrite
       prompt, rename it, and delete it - each time checking the list
       updates. Repeat the overwrite, rename and delete on the file that is
       playing, and cancel an upload halfway to confirm the old file
       survives. Watch the underrun count while uploading during playback.
       Check the card's cache/ folder follows along: the renamed file's
       folder renamed, the deleted file's gone, and the overwritten
       file's copies rebuilt (Serial says so) the next time it's cued.
    11. Knob calibration: send RECAL, then reset the board. Serial says
       neither knob has a saved calibration, and neither knob does
       anything yet. Turn each about two full turns until it responds,
       then play and stop - Serial reports each calibration saved. Reset
       again: Serial says both were loaded, and the knobs respond at once.
       CAL shows the saved ranges alongside the live ones.
    12. Per-file settings: set a file's speed and pitch with the knobs,
       wait two seconds (Serial says "Settings saved"), pick another file
       and then the first again - its speed and pitch come back. Reboot:
       the same file is cued, with the same settings. In pitch mode, turn
       knob 1 and confirm pitch carries on from the restored value rather
       than jumping back to unshifted.
    13. Time-stretch: play a file, switch to pitch mode (D20), and turn
       knob 2 - the tempo changes and the pitch doesn't. Through zero it
       freezes into a granular drone, and past it plays backward, still at
       the same pitch. Turn knob 1 meanwhile: pitch moves, tempo doesn't,
       at two octaves per turn from wherever it is - even at hundreds of
       times real speed, where the pitch you hear starts octaves up.
       Switch back to speed mode: nothing changes until a knob is turned.
    14. Start and end points: latch a file playing, press D20 until the LED
       is purple, and turn knob 2 - the loop gets shorter from its end.
       Turn knob 1 - it gets shorter from its start. Push either end into
       the other and the 20ms window slides along the file as a buzz.
       None of it should click or drop out beyond the loop's own seam, and
       the underrun count should stay put. Serial prints the window on
       entering the mode; WINDOW:1,2 sets one exactly and RESET:WINDOW
       gives the whole file back. Tap D21 unlatched: it plays from the
       window's start each time. In speed mode the scrub stays inside the
       window. Pick another file and come back, or reboot: the window is
       restored.
    15. Selecting with the knobs: tap D20 - the mode steps on as the finger
       lifts. Hold D20 alone and turn knob 1 - the LED steps through the
       mode colours, one every 60 degrees, forward and back, and letting go
       doesn't step it again. Still holding it, turn knob 2 - each 30
       degrees cues the next or previous WAV on the card and plays it
       (Serial says "Loaded"). Holding D20 without turning plays nothing.
       Release it: playback stops, and both knobs do their jobs in the mode
       shown, with no jump. Latch the transport first and do the same: the
       file picked keeps playing after D20 is released.
    16. Echo: latch a file playing, press D20 until the LED is pink, and
       turn knob 2 forward - echoes appear and, further round, take longer
       to die away, without ever building up. Turn knob 1: the echoes move
       apart or together, bending in pitch as they go. All the way back on
       knob 1 they close up into a metallic ring. Stop the transport: the
       echoes keep repeating as they fade smoothly away over three delay
       times, and the output doesn't thump or stick. Back in speed mode,
       scrub the stopped tape with knob 2: it echoes in full, and when the
       knob comes to rest the echoes fade away the same way.
       ECHO:0.5,0.6 sets one exactly (Serial prints it, and the status line
       shows it) and RESET:ECHO turns it off. The "isr" figures on the
       status line should be only a few percent higher than before. Pick
       another file and come back, or reboot: the echo is restored.
    17. Reset: change the speed, pitch, window, echo and filter of a test file,
       then tap both pads together three times, about as fast as knocking
       on a door. The RGB LED flashes white, Serial prints "Reset: ...",
       the file plays as it did when new, and the transport is latched.
       Then confirm what doesn't reset: two taps; three slow ones (a
       second apart); three presses each held for a second; and a chord
       held while the other hand does anything at all. RESET:ALL does the
       same from Serial.
    18. Filter: latch a file playing, press D20 until the LED is orange,
       and turn knob 1 a quarter turn either way - the bass and treble go,
       leaving a band around 640Hz (PINK.WAV, from tools/make_test_wav.py
       --pink, makes it easiest to hear). Turn knob 2 either way and the
       band moves; the width the page shows doesn't change. Turn knob 2 two
       turns either way and the sound fades away to nothing; bring it back
       to 640Hz (RESET:FILTER, or tap Centre's reset on the page). Keep turning
       knob 1 the same way: the band rings, then screams by itself, and knob
       2 plays the scream's pitch.
       Stop the transport: the scream fades away over three
       echo delay times (with the echo off, its delay is still set: 0.3
       seconds by default, so about a second); play again and it comes
       back. Turn knob 1 back until it stops at wide open and the sound is
       untouched, wherever knob 2 is. FILTER:500,1000 sets one exactly and RESET:FILTER opens
       it again; the "isr" figures
       rise about 12% while the filter is narrowed, and drop back a moment
       after it's wide open. Pick another file and come back, or reboot:
       the filter is restored.
    19. USB disk mode: with the board's USB cable in a computer, tap "USB
       disk" on the web page's Files tab (or type DISK:ON in the Serial
       Monitor). Playback stops, the LED goes a dim white, and within a
       few seconds the card appears as a drive. Copy a WAV onto it and
       eject it: the LED comes back in the mode's colour, the remembered
       file is cued again, and the new WAV is in the page's file list
       (but no "._" twin of it). Do it again and unplug the cable instead
       of ejecting: about two seconds later the board comes back the
       same way. DISK:ON with no
       computer on the USB port is refused.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <RA6M5AnalogTuning.h>
#include <RA6M5CapTouch.h>
#include <FastLED.h>
#include <codelessBLEPeripheral.h>
#include <EEPROM.h>       // the Data Flash, for saving the knobs' calibration
#include <RA6M5FastSD.h>  // the core's SD driver, but reading and writing many sectors per command
#include "FATFileSystem.h"
#include "UsbMsd.h"
#include "tusb.h"  // tud_mounted(), tud_suspended()
#include "UsbDiskGate.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR
#include "RingBuffer.h"
#include "Resampler.h"
#include "WavReader.h"
#include "MipCache.h"
#include "FileSettings.h"
#include "Voice.h"
#include "SincInterpolator.h"
#include "EchoDelay.h"
#include "BandFilter.h"
#include "WavList.h"  // the card's WAVs, in the order the page and the file knob share
#include <utility>  // for std::move in loadFile()

// -- Speed range and knob mapping ----------------------------------------
// The speed dial: a virtual dial the speed knob turns (see "Relative speed
// control" in the header comment), running from +MAX_RATIO at one end,
// smoothly through zero (stopped) in the middle, to -MAX_RATIO (full speed
// backward) at the other. It's a loop - turning on past either end hops
// straight to the other - which is the only discontinuity anywhere on it.
// Positions on it are measured in degrees of knob rotation out from the
// middle: positive forward, negative backward.

const float MAX_RATIO = 1024.0f;  // magnitude at both extremes - enough to sweep a 20Hz tone up
                                  // past 20kHz, which the cached copies make possible (MipCache.h)

// The curve has two pieces, meeting at 1x.
//
// Above 1x the speed doubles every SPEED_DEGREES_PER_DOUBLING of rotation:
// from 1x, half a turn reaches 2x, a turn 4x, two turns 16x, and five turns
// MAX_RATIO. It used to climb about three and a half times as steeply
// (roughly 7 octaves a turn, so one turn from 1x was already about 140x),
// which was confirmed live to be too twitchy at high speeds.
//
// Below 1x: ratio = SPEED_CURVE_R0 * (e^(SPEED_CURVE_K * degrees) - 1).
// Mostly exponential - a linear version was tried and confirmed live to
// feel too twitchy below 1x, since a linear mapping spends the same
// rotation on every equal-sized ratio step, leaving only a sliver of the
// knob's travel for everything under 1x. An exponential spends equal
// rotation on equal *ratios* instead, which is what fine control at low
// speed needs.
//
// But a pure exponential never reaches zero, which is why this used to need
// a dead band in the middle: a flat "paused" stretch filling the gap between
// -0.1x and +0.1x, with a jump from 0 straight to 0.1x at its edges. The
// "- 1" fixes that: near the middle the curve flattens into a straight line
// through zero, so the speed slows smoothly through 0.05x, 0.01x, 0.001x...
// to exactly 0 in the middle and on into reverse, with no jump anywhere.
// (Stopping outright is the transport's job now - release the touch pad.)
// SPEED_CURVE_K sets how sharply it bends (it's the value an older,
// single-curve dial used, kept because it felt right); SPEED_CURVE_R0 is
// whatever puts 1x at NATIVE_SPEED_DEGREES, worked out by initSpeedCurve().
const float NATIVE_SPEED_DEGREES = 194.4f;       // how far out from the middle 1x sits
const float SPEED_DEGREES_PER_DOUBLING = 180.0f;
const float SPEED_CURVE_K = 0.01303f;            // per degree
float SPEED_CURVE_R0 = 1.0f;
// How far out from the middle each end of the dial is, also worked out by
// initSpeedCurve(): 194.4 + 10 doublings * 180 = 1994.4 degrees, about 5.5
// turns.
float speedDialEndDegrees = 1.0f;

void initSpeedCurve() {
  SPEED_CURVE_R0 = 1.0f / (expf(SPEED_CURVE_K * NATIVE_SPEED_DEGREES) - 1.0f);
  speedDialEndDegrees = NATIVE_SPEED_DEGREES + log2f(MAX_RATIO) * SPEED_DEGREES_PER_DOUBLING;
}

// The speed at a position on the dial, in degrees from the middle.
float speedForDial(float degrees) {
  float d = fabsf(degrees);
  float magnitude = (d <= NATIVE_SPEED_DEGREES)
      ? SPEED_CURVE_R0 * (expf(SPEED_CURVE_K * d) - 1.0f)
      : exp2f((d - NATIVE_SPEED_DEGREES) / SPEED_DEGREES_PER_DOUBLING);
  return copysignf(fminf(magnitude, MAX_RATIO), degrees);
}

// The inverse - where a given speed lives on the dial, so the virtual knob
// below can (re-)anchor itself there: native speed on every file load, or
// whatever SPEED: asked for.
float dialForSpeed(float ratio) {
  float r = fminf(fabsf(ratio), MAX_RATIO);
  float d = (r <= 1.0f)
      ? logf(1.0f + r / SPEED_CURVE_R0) / SPEED_CURVE_K
      : NATIVE_SPEED_DEGREES + log2f(r) * SPEED_DEGREES_PER_DOUBLING;
  return copysignf(d, ratio);
}

// The virtual knob below keeps an unbounded running total of degrees turned;
// this folds that onto the dial's loop. A total of 0 is the +MAX_RATIO end,
// and turning the knob up (a growing total) lowers the speed - confirmed
// live that the opposite sense spun the wrong way.
float dialForKnob(float knobDegrees) {
  float loop = 2.0f * speedDialEndDegrees;
  float wrapped = fmodf(knobDegrees, loop);
  if (wrapped < 0) wrapped += loop;
  return speedDialEndDegrees - wrapped;
}

// The knob total that puts the dial at a given speed.
float knobForSpeed(float ratio) {
  return speedDialEndDegrees - dialForSpeed(ratio);
}

// The pitch knob is relative, like the speed and scrub knobs: each turn
// moves the pitch you hear by PITCH_OCTAVES_PER_TURN octaves from wherever
// it is, in whatever range. It used to be a dial covering 5 octaves either
// side of the file's own pitch, at one octave per turn - but at high speeds
// the pitch you hear is far outside that (200x is 7.6 octaves up), so the
// dial anchored at its end, snapped there on the first turn, and took many
// turns to come back.
const float PITCH_OCTAVES_PER_TURN = 2.0f;

// How far the pitch you hear can go, either way, in octaves from the file's
// own - as far as the speed dial goes (1024x is 10 octaves). Nothing in the
// signal path limits it: both grain taps trail the cursor by at most a
// grain, whatever the pitch, so the buffer, the card and the CPU don't care.
// What changes is the character: shifting far up moves most of the sound
// above hearing (the copy is chosen to suit the grains' rate, so it's
// filtered out rather than aliased - see levelRateFor()), and as the shift
// grows the grains splice faster, until the crossfade seam is audible and
// it behaves like a ring modulator. That's a musical effect rather than a
// malfunction, so the limit is taste.
const float PITCH_OCTAVE_LIMIT = 10.0f;

// -- Relative ("virtual knob") speed control -------------------------------
// The physical knob is absolute (its position directly maps to a
// point on the dial) but the speed control is relative: pot1's
// MOVEMENT, not its position, is what moves a separate virtual dial
// position - speedKnobDegrees below - through the speed dial above.
// This means the real knob's physical position at any moment is
// meaningless on its own; only how far it's turned since the last tick
// matters. speedKnobAnchored tracks "no baseline yet" explicitly,
// rather than testing lastPhysicalKnobDegrees's sign for MultiTurnKnob's
// -1 not-ready sentinel - unlike a single-turn [0,360) reading, MultiTurnKnob::read()'s
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
// In pitch mode, the pitch you hear (relative to the file's own), which
// knob1 sets and knob2's time-stretch holds steady - see "Time-stretch" in
// the header comment. Kept in step with pitchRatio and the speed by
// holdPitch() and syncHeldPitch().
float heldPitch = 1.0f;
float lastPhysicalKnobDegrees = 0.0f;

// knob1's position after a small backlash - see the control-rate block.
float knob1Degrees = 0.0f;
const float KNOB1_BACKLASH_DEGREES = 0.3f;

// The ratio the speed knob re-anchors at the next time it finds no
// baseline - native speed, except after a SPEED: command picks another.
float speedAnchorRatio = 1.0f;

// Doesn't touch speedKnobDegrees directly - just clears the baseline
// so the control-rate block re-anchors it at native speed on its very
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

// The card as a USB disk, behind a gate that keeps it hidden from the
// computer except in USB disk mode - see "USB disk mode" and UsbDiskGate.h.
// (The list form of USBMSD's constructor: its single-device one writes past
// the end of a zero-length array.)
UsbDiskGate usbDiskGate(sdBlockDevice);
USBMSD usbDisk({&usbDiskGate});
// Runs fixUsbDiskPacketSize() before main() starts USB - see UsbDiskGate.h.
struct UsbDiskPacketSizeFix {
  UsbDiskPacketSizeFix() { fixUsbDiskPacketSize(); }
} usbDiskPacketSizeFix;
bool usbDiskMode = false;
volatile bool usbDiskEjected = false;  // set by the USB interrupt
MipCache cache;  // builds and tracks the current file's lower-rate copies - see MipCache.h

// volatile because audioCallback() reads it - see the idle branch there.
// Only ever written by loadFile() and unloadFile(), which both disable the
// audio interrupt around the whole state swap, so no further guarding is
// needed.
volatile bool fileLoaded = false;
String currentFilename = "NONE";
float rateFactor = 1.0f;  // wav's native sample rate / 48000 - recomputed on every file load
uint32_t fileFrames = 1;  // the WAV's length in frames, at its own rate - set on every file load
uint32_t nativeRate = 0;  // the WAV's own sample rate, in Hz

// The window: the stretch of the file that plays, round and round, in
// frames of the WAV - the whole file unless mode 3's knobs have moved its
// ends. See "The window" further down. Fractional, so that slow turns of a
// knob add up.
double windowStart = 0.0;
double windowEnd = 1.0;  // one past the last frame played
uint8_t fileChannels = 1; // 1 for mono, 2 for stereo

// -- Playback voices ----------------------------------------------------------
// A voice plays one copy of the current file - the WAV itself, or one of the
// filtered, lower-rate copies MipCache keeps on the card - through its own
// ring buffer, at its own position. Only one ever plays. When the speed moves
// far enough that a different copy would serve better, the idle voice is
// opened on that copy and primed at the matching position while the active
// one keeps playing, and then the two simply swap - see chooseLevel() and
// startLevelChange().
//
// The swap is a hard cut, no crossfade. It used to be a 20ms crossfade, back
// when each copy was read in a way that aliased just below a change and not
// just above it, so the change was audible and needed smoothing over. Now
// both sides of a change are filtered the same way (SincInterpolator.h), and
// measured on the board the two copies differ by -60dB or less at the
// moment of a change - and no artifact was audible with a hard cut on pure
// tones, a bright sawtooth, a sine sweep or a drum loop. Dropping the fade
// means the audio interrupt only ever renders one voice, which roughly
// halves its worst case.
//
// Positions are kept per voice, in that voice's own copy's samples rather
// than the original file's. That's not just tidiness: at 1000x, a position
// counted in the original file's samples would overflow a 32-bit counter in
// under a minute, but a voice only ever steps through its own copy at
// around one sample per tick, so its counter lasts for hours.

// The Voice struct itself lives in Voice.h - only so Arduino's automatic
// function prototypes, which land near the top of this file, can name it.
const size_t CHUNK_FRAMES = 1024;      // frames per SD read when refilling a voice

// Where each chunk lands on its way from the card into a ring buffer -
// room for a stereo chunk. Static rather than on the stack, which a 4KB
// buffer would strain; only loop() ever reads the card, one chunk at a time.
alignas(4) int16_t chunkBuffer[CHUNK_FRAMES * 2];
const int32_t REVERSAL_MARGIN = 4096;  // frames, ~93ms at 44.1kHz; must stay well under
                                       // a ring's frameCapacity()

Voice voices[2];

// Which voice is playing. loop() changes it only with interrupts masked.
volatile uint8_t activeVoice = 0;

// The step the audio interrupt is using right now, in samples of the
// original file per output tick. loop() works out a new step 100 times a
// second, at control rate. While the scrub knob is driving, the interrupt
// glides to each new step in a straight line over the next control
// interval rather than jumping: targetStep is where it's headed and
// stepSlope how far it moves each tick. Jumping made a scrub a staircase of
// tiny pitch steps a hundred times a second, heard as a warble. The glide
// costs at most one control interval (10ms, about 5ms on average) before a
// change fully arrives - noticeable as lag on the speed knob, which didn't
// warble to begin with, so its steps still jump. Set through rampStepTo()
// or setStepNow(), never directly, so the three always agree.
volatile float currentStep = 1.0f;
volatile float targetStep = 1.0f;
volatile float stepSlope = 0.0f;
const float STEP_RAMP_TICKS = 480.0f;  // one control interval, CONTROL_INTERVAL, at 48kHz
volatile int16_t lastGoodLeft = 0;   // the last frame played, held through an underrun
volatile int16_t lastGoodRight = 0;

// Rounds toward zero and clips to the 16-bit range.
AUDIO_HOT inline int16_t clampToSample(float x) {
  if (x > 32767.0f) return 32767;
  if (x < -32768.0f) return -32768;
  return (int16_t)x;
}
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

// The shifter doesn't switch in and out of that bypass instantly: the grains
// trail the cursor while the single tap reads right at it, so an instant
// switch jumps the sound back or forward by up to a grain - a glitch heard
// every time a time-stretch passed through the held pitch (1x, unless the
// pitch knob has moved it). shiftMix glides between the two over
// SHIFT_FADE_TICKS instead - 0 the single tap, 1 the grains - reading both
// only while it's in between.
const float SHIFT_FADE_TICKS = 480.0f;  // 10ms at 48kHz
float shiftMix = 0.0f;                  // ISR-owned

volatile float pitchRatio = 1.0f;   // written by loop(), read by the audio ISR

// For time-stretch: when zero or more, the grains read at exactly this many
// source samples per tick - a fixed pitch - whatever the step is, instead
// of at pitchRatio times the step. -1 when not time-stretching. Written by
// loop() each control tick.
volatile float grainTapRate = -1.0f;
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

// Which knob mode is selected: a row of MODES - see "Knob modes". Advanced
// by touch1 (pressed on its own - chorded with touch2 it latches the
// transport instead).
uint8_t knobMode = 0;

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

// The fastest a voice is allowed to step through its copy - see the comment
// where it's applied in audioCallback().
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

// Reads one interpolated frame from a voice - out[0] left, out[1] right,
// the same for mono - at whole frame idx plus fraction frac of its copy,
// plus an offset in frames (which may be fractional or negative). Kept as a
// whole/fraction pair rather than one float position, since a float can't
// hold a large frame index exactly.
AUDIO_HOT inline bool readAt(Voice &v, int32_t idx, float frac, float offset, float bandwidth,
                             float out[2]) {
  float rel = frac + offset;
  float whole = __builtin_floorf(rel);
  return readSinc(v.ring, idx + (int32_t)whole, rel - whole, bandwidth, out);
}

// Two-tap granular read - see the pitch shifting comment above for the
// derivation. Both taps sit behind the nominal position in the direction of
// travel; their Hann envelopes are driven by the same ramp that moves them,
// so each tap is silent exactly where its own grain wraps. grainOffset is
// counted in the original file's samples, so it's scaled into this voice's
// copy - which keeps the grains in step when playback moves to another copy.
AUDIO_HOT inline bool readGrains(Voice &v, int32_t idx, float frac, float step, float bandwidth,
                                 float out[2]) {
  float dir = (step < 0.0f) ? -1.0f : 1.0f;

  float offsetA = grainOffset;
  float offsetB = offsetA + GRAIN_LENGTH_SAMPLES * 0.5f;
  if (offsetB >= GRAIN_LENGTH_SAMPLES) offsetB -= GRAIN_LENGTH_SAMPLES;

  float a[2], b[2];
  if (!readAt(v, idx, frac, -dir * offsetA * v.levelScale, bandwidth, a)) return false;
  if (!readAt(v, idx, frac, -dir * offsetB * v.levelScale, bandwidth, b)) return false;

  float gainA = hannAt(offsetA / GRAIN_LENGTH_SAMPLES);
  float gainB = hannAt(offsetB / GRAIN_LENGTH_SAMPLES);
  out[0] = a[0] * gainA + b[0] * gainB;
  out[1] = a[1] * gainA + b[1] * gainB;
  return true;
}

// One voice's output for this tick: the single tap, the pitch shifter's
// grains, or a blend of the two by `mix` while gliding between them (see
// shiftMix).
//
// The pitch shifter's grain taps move through the copy at speed x pitch,
// but they're read at full bandwidth regardless. The copy is chosen to keep
// that rate under about 2 samples per tick (see levelRateFor()), so only
// the top octave can alias, between 1 and 2. Band-limiting them properly
// would mean two stretched-kernel reads a tick, each up to twice the cost of
// an ordinary one - and the audio interrupt's budget is being kept free for
// other processing. The shifter is a lo-fi effect with its own warble anyway.
AUDIO_HOT inline bool renderVoice(Voice &v, float step, float mix, float out[2]) {
  int32_t idx = v.resampler.index;
  float frac = v.resampler.frac;
  float rate = step * v.levelScale;  // samples of this copy per tick
  if (mix <= 0.0f) return readAt(v, idx, frac, 0.0f, bandwidthFor(rate), out);

  // A tap can reach past the start of the buffer in the moments right
  // after a rewind, when there is little history behind position 0 yet.
  // Falling back to the unpitched read costs a brief pitch glitch there
  // and nothing else, which beats reporting an underrun that isn't one.
  if (!readGrains(v, idx, frac, step, 1.0f, out)) {
    return readAt(v, idx, frac, 0.0f, bandwidthFor(rate), out);
  }
  if (mix >= 1.0f) return true;

  float plain[2];
  if (!readAt(v, idx, frac, 0.0f, bandwidthFor(rate), plain)) return true;
  out[0] = plain[0] + mix * (out[0] - plain[0]);
  out[1] = plain[1] + mix * (out[1] - plain[1]);
  return true;
}

// -- Echo --------------------------------------------------------------------
// A feedback echo on the output, set by the fourth knob mode - see
// EchoDelay.h for how it works and why it sounds as it does. It runs in the
// audio interrupt on every tick, whatever the mode and whether or not
// anything is playing. With the amount at zero, as it is until that knob is
// turned, it adds nothing to the sound.
//
// When the sound stops, the tail doesn't ring on for ever: loop() fades it
// out (EchoDelay's setFade()) over a few delay times (ECHO_TAIL_REPEATS) -
// long enough for the last thing played to come round that many times, each
// quieter - and then damps it (setDamped()), taking the feedback away. The
// fade follows (1 - t) cubed rather than a straight line: a straight line
// in level is only 30dB down at 97% of the way, and drops to nothing over
// the last 3% - which is heard as a cut. The cube is 60dB down by 90% and
// arrives at nothing gently. "The sound" is the transport running, or the scrub
// knob moving a stopped tape; the fade starts when the transport stops or
// that knob comes to rest. A very short delay still gets
// ECHO_TAIL_MIN_SECONDS, so a stop is never an abrupt cut.
EchoDelay echo;
float echoSeconds = 0.3f;  // the delay time; loop()'s copy of what `echo` was told
float echoAmount = 0.0f;   // level and feedback, 0 (off) to ECHO_MAX_AMOUNT
const float ECHO_TAIL_REPEATS = 3.0f;
const float ECHO_TAIL_MIN_SECONDS = 0.5f;
bool echoTailing = false;            // whether the echo is fading out after the sound...
unsigned long soundStoppedMillis = 0;  // ...and when that sound was last heard

// The delay knob is relative and exponential, like the pitch knob: a turn
// multiplies or divides the delay by 2 to the power of this, wherever it
// is. All the way from ECHO_MIN_SECONDS to ECHO_MAX_SECONDS is a little
// under four turns.
const float ECHO_OCTAVES_PER_TURN = 3.0f;
// The amount knob goes from nothing to ECHO_MAX_AMOUNT in this many turns.
const float ECHO_AMOUNT_TURNS = 1.0f;

// -- Filter ------------------------------------------------------------------
// A resonant band-pass filter, set by the fifth knob mode - see BandFilter.h
// for how it works and how it's played. It runs in the audio interrupt just
// before the echo, so the echoes are of the filtered sound. Wide open, as it
// is until a knob is turned, it's out of the circuit and costs nothing.
//
// Narrowed far enough it oscillates by itself, and an oscillating filter
// would carry on screaming when the sound stops - hard to silence for anyone
// not in filter mode. So loop() fades it out along with the echo's tail,
// over a few echo delay times - see "Echo". Playing again brings the scream
// back.
BandFilter filter;
float filterLowHz = FILTER_MIN_HZ;   // the band's bottom; loop()'s copy of what `filter` was told
float filterHighHz = FILTER_MAX_HZ;  // and its top: both at the ends is wide open, no filter

// The knobs play the band by its centre and width - see setFilterBand().
// The centre starts in the middle of the range, in octaves: 640Hz.
const float FILTER_CENTRE_HZ = 640.0f;
float filterCentreHz = FILTER_CENTRE_HZ;
float filterWidthOctaves = FILTER_OPEN_OCTAVES;  // 0 to 10, and 10 is wide open

// The centre reaches this many octaves either way from FILTER_CENTRE_HZ:
// far enough that even the widest band slides out past the end of what you
// can hear, and fades away to silence - see BandFilter.h. That's from a
// fiftieth of a Hz to 21MHz, which only means anything as a place to put
// the band where it can't be heard.
const float FILTER_CENTRE_REACH_OCTAVES = 15.0f;
const float FILTER_CENTRE_LOWEST_HZ = FILTER_CENTRE_HZ / 32768.0f;   // 2^15
const float FILTER_CENTRE_HIGHEST_HZ = FILTER_CENTRE_HZ * 32768.0f;

// The centre knob is relative, on a curve: FILTER_OCTAVES_PER_TURN in the
// middle, speeding up toward the ends, so the reach takes
// FILTER_CENTRE_REACH_TURNS either way - see filterCentreOctaves().
const float FILTER_OCTAVES_PER_TURN = 5.0f;
const float FILTER_CENTRE_REACH_TURNS = 2.0f;

// The width knob runs from wide open to closed in this many turns, either
// way from open, on a curve - see turnFilterWidth().
const float FILTER_WIDTH_TURNS = 1.5f;
int8_t filterWidthSide = 1;  // which way from open the width knob was last turned

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

// Starts the audio interrupt gliding from its current step to `target`
// over the next control interval - see currentStep.
void rampStepTo(float target) {
  noInterrupts();
  targetStep = target;
  stepSlope = (target - currentStep) / STEP_RAMP_TICKS;
  if (stepSlope == 0.0f) currentStep = target;  // no glide would ever get there
  interrupts();
}

// Jumps straight to a step, with no glide - for the speed knob, and for a
// new file, where there's nothing to glide from.
void setStepNow(float step) {
  noInterrupts();
  currentStep = targetStep = step;
  stepSlope = 0.0f;
  interrupts();
}

AUDIO_HOT void audioCallback(timer_callback_args_t *args) {
  uint32_t tickStart = DWT->CYCCNT;
  audioTicks++;

  int16_t outLeft, outRight;

  if (!fileLoaded) {
    // Nothing loaded: idle, not starved. An underrun means the producer
    // could not keep up with the consumer, and with no file there is no
    // producer and nothing is expected - counting these would (and did)
    // bury the real measurement under 48000 phantom underruns per second
    // for as long as the board sat at the menu. Hold the output steady and
    // leave the resampler alone.
    outLeft = lastGoodLeft;
    outRight = lastGoodRight;
  } else {
    // Glide toward loop()'s latest step - see currentStep.
    float step = currentStep;
    float target = targetStep;
    if (step != target) {
      step += stepSlope;
      if ((stepSlope > 0.0f) ? (step > target) : (step < target)) step = target;
      currentStep = step;
    }
    float p = pitchRatio;
    float tapRate = grainTapRate;
    bool stretching = tapRate >= 0.0f;
    float stepSize = __builtin_fabsf(step);
    bool shifting = stretching
        ? __builtin_fabsf(tapRate - stepSize) >= PITCH_BYPASS_EPSILON * tapRate
        : __builtin_fabsf(p - 1.0f) >= PITCH_BYPASS_EPSILON;

    float mix = shiftMix;
    if (shifting ? mix < 1.0f : mix > 0.0f) {
      mix += (shifting ? 1.0f : -1.0f) / SHIFT_FADE_TICKS;
      mix = (mix > 1.0f) ? 1.0f : (mix < 0.0f) ? 0.0f : mix;
      shiftMix = mix;
    }

    Voice &active = voices[activeVoice];

    float frame[2];
    bool ok = renderVoice(active, step, mix, frame);

    if (ok) {
      outLeft = clampToSample(frame[0]);
      outRight = clampToSample(frame[1]);
      lastGoodLeft = outLeft;
      lastGoodRight = outRight;

      // Ramp the grain offset and wrap it into one grain - the wrap is the
      // splice the crossfade exists to hide. The grains trail the cursor by
      // the offset, so they read at |step| minus its rate of growth: at
      // pitchRatio times the step, or at a fixed rate when time-stretching -
      // which works even at a step of zero, where the grains keep playing in
      // place (a freeze). Kept moving while the grains fade out too.
      if (mix > 0.0f) {
        float o = grainOffset + (stretching ? stepSize - tapRate : (1.0f - p) * stepSize);
        if (o >= GRAIN_LENGTH_SAMPLES) o -= GRAIN_LENGTH_SAMPLES;
        else if (o < 0.0f) o += GRAIN_LENGTH_SAMPLES;
        grainOffset = o;
      }
      // No ring.release() anymore: the consumer no longer publishes
      // anything back through the ring buffer at all - the producer
      // (loop()) decides reclaim policy itself, based on direction. See
      // RingBuffer.h's header comment and loop()'s refill logic.
      // The voice steps through its own copy, scaled to that copy's rate -
      // but no faster than MAX_VOICE_RATE samples of it per tick. Normally a
      // voice reads its copy at no more than 2, but when the speed jumps -
      // SPEED:1000 from 1x, or a fast flick of the knob - the playing voice
      // would suddenly be asked for hundreds, and drain its ring buffer in a
      // few ticks while loop() is still preparing the right copy. So it lags
      // behind for those few milliseconds instead, and catches up when the
      // right copy takes over.
      float activeRate = step * active.levelScale;
      if (__builtin_fabsf(activeRate) > MAX_VOICE_RATE) {
        activeRate = __builtin_copysignf(MAX_VOICE_RATE, activeRate);
      }
      active.resampler.advance(activeRate);
    } else {
      outLeft = lastGoodLeft;  // hold the last good frame - see header comment
      outRight = lastGoodRight;
      underrunTicks++;
    }
  }

  // The filter and then the echo, last: they hear exactly what goes out,
  // including a held frame. (lastGoodLeft/Right above stay as they were,
  // without them.)
  float effected[2] = {(float)outLeft, (float)outRight};
  filter.process(effected);
  echo.process(effected);
  outLeft = clampToSample(effected[0]);
  outRight = clampToSample(effected[1]);

  // 16-bit signed -> 12-bit unsigned, same shift-based rescale used
  // throughout this pipeline for cheapness; a few LSBs of resolution
  // given up is inaudible against this DAC's own ~24-LSB absolute
  // accuracy (RA6M5 datasheet, Table 53.48). Left to DAC0 (A0, the jack's
  // tip), right to DAC1 (A1, its ring) - a mono file's frames carry the
  // same value in both.
  R_DAC->DADR[0] = (uint16_t)(((int32_t)outLeft + 32768) >> 4);
  R_DAC->DADR[1] = (uint16_t)(((int32_t)outRight + 32768) >> 4);

  uint32_t used = DWT->CYCCNT - tickStart;
  if (used > isrCyclesMax) isrCyclesMax = used;
  isrCyclesSum += used;
  isrCount++;
}

// -- File switching (BLE PLAY: command, and the boot-time default) -------

// The crossfade that joins the end of the window to its start - see "The
// window" further down. This long, though never more than a quarter of the
// window, nor than fits the buffer it's worked out in.
const float WINDOW_FADE_SECONDS = 0.005f;
const uint32_t WINDOW_FADE_MAX_FRAMES = 256;
alignas(4) int16_t fadeBuffer[WINDOW_FADE_MAX_FRAMES * 2];  // room for stereo

// The window (windowStart/windowEnd), in frames of one voice's copy. Never
// empty, however small the window or coarse the copy.
CopyWindow windowInCopy(const Voice &v) {
  uint32_t total = v.reader.totalFrames();
  uint32_t start = (uint32_t)windowStart >> v.level;
  uint32_t end = (uint32_t)windowEnd >> v.level;
  if (end > total) end = total;
  if (start >= end) {
    start = (end > 0) ? end - 1 : 0;
    end = start + 1;
  }
  CopyWindow w;
  w.start = start;
  w.length = end - start;

  // The crossfade needs the same length of audio from outside the window:
  // the lead-in before its start, or failing that what follows its end.
  // The whole file has neither, and loops as it always did.
  uint32_t fade = (uint32_t)(WINDOW_FADE_SECONDS * (nativeRate >> v.level));
  if (fade > WINDOW_FADE_MAX_FRAMES) fade = WINDOW_FADE_MAX_FRAMES;
  if (fade > w.length / 4) fade = w.length / 4;
  w.fadeAtEnd = start >= fade;
  if (!w.fadeAtEnd && end + fade > total) fade = 0;
  w.fade = fade;
  return w;
}

// Gives a voice the window afresh, with its position `window.start` at the
// start of the window - for a voice that's about to be primed from
// scratch. moveVoiceWindow() is the one for a voice that's playing.
void resetVoiceWindow(Voice &v) {
  v.window = windowInCopy(v);
  v.windowBase = (int32_t)v.window.start;
}

// The only place a virtual (signed, possibly-wrapped-many-times-over)
// sample position gets mapped onto a real, in-range file sample offset:
// round and round the voice's window, with position windowBase at its
// start. AudioRingBuffer and the audio interrupt stay ignorant of
// wraparound in either direction - the buffer simply holds the window end
// to end, as many times over as fit.
uint32_t fileOffsetFor(const Voice &v, int32_t virtualPos) {
  int32_t m = (virtualPos - v.windowBase) % (int32_t)v.window.length;
  if (m < 0) m += (int32_t)v.window.length;
  return v.window.start + (uint32_t)m;
}

// Crossfades a run of frames just read from the card - `frames` of them
// in dst, from file offset `pos` - wherever it overlaps the window's fade:
// see "The window". Each frame there is mixed with the one a whole window
// away, outside the window, which is read into fadeBuffer.
void fadeSeam(Voice &v, uint32_t pos, int16_t *dst, size_t frames) {
  const CopyWindow &w = v.window;
  if (w.fade == 0) return;
  uint32_t zone = w.fadeAtEnd ? w.start + w.length - w.fade : w.start;
  uint32_t from = (pos > zone) ? pos : zone;
  uint32_t to = (pos + frames < zone + w.fade) ? pos + frames : zone + w.fade;
  if (from >= to) return;

  uint32_t other = w.fadeAtEnd ? from - w.length : from + w.length;
  if (v.reader.readFramesAt(other, fadeBuffer, to - from) != to - from) return;

  uint8_t channels = v.reader.channels();
  int16_t *mine = dst + (from - pos) * channels;
  for (uint32_t i = 0; i < to - from; i++) {
    // How much of the other audio: rising to all of it by the window's last
    // frame, or falling from all of it at the window's first.
    uint32_t n = from + i - zone;
    float mix = w.fadeAtEnd ? (float)(n + 1) / w.fade : (float)(w.fade - n) / w.fade;
    for (uint8_t c = 0; c < channels; c++) {
      int16_t &sample = mine[i * channels + c];
      sample = (int16_t)lroundf(sample + mix * (fadeBuffer[i * channels + c] - sample));
    }
  }
}

// Reads `frames` frames of a voice's copy into dst, starting at file offset
// `offset` (which must be inside its window) and carrying on round the
// window from its end back to its start, crossfaded there.
//
// A window can be much shorter than what's asked for - the minimum is a few
// frames of a low-rate copy - so once one whole lap is in dst, the rest is
// copied from there rather than read from the card again and again.
size_t readLooped(Voice &v, uint32_t offset, int16_t *dst, size_t frames) {
  uint8_t channels = v.reader.channels();
  uint32_t end = v.window.start + v.window.length;
  uint32_t pos = offset;
  size_t done = 0;
  while (done < frames) {
    if (pos >= end) pos = v.window.start;
    size_t n = frames - done;
    if (n > end - pos) n = end - pos;
    if (done >= v.window.length) {
      memcpy(dst + done * channels, dst + (done - v.window.length) * channels,
             n * channels * sizeof(int16_t));
    } else {
      n = v.reader.readFramesAt(pos, dst + done * channels, n);
      if (n == 0) break;  // shouldn't happen, but avoid spinning forever
      fadeSeam(v, pos, dst + done * channels, n);
    }
    done += n;
    pos += n;
  }
  return done;
}

// Points a voice at one copy of the named file: level 0 is the WAV itself,
// anything higher one of its cached copies. Leaves the voice untouched if
// the file can't be opened.
bool openVoice(Voice &v, const String &name, uint8_t level) {
  WavReader reader;
  bool ok = (level == 0)
                ? reader.open(("/sd/" + name).c_str())
                : reader.openRaw(MipCache::levelPath(name, level).c_str(), nativeRate >> level,
                                 fileChannels);
  if (!ok) return false;
  v.reader = std::move(reader);  // closes whatever the voice had open before
  v.ring.setChannels(v.reader.channels());
  v.level = level;
  v.levelScale = ldexpf(1.0f, -(int)level);
  resetVoiceWindow(v);
  return true;
}

// Empties a voice's ring buffer to start at virtual position `from`, then
// fills it forward until it holds `frames` (or is full).
void primeVoice(Voice &v, int32_t from, uint32_t frames) {
  v.ring.reset(from);
  while (v.ring.usedSpace() < frames && v.ring.freeSpace() >= CHUNK_FRAMES) {
    size_t got = readLooped(v, fileOffsetFor(v, v.ring.validHigh()), chunkBuffer, CHUNK_FRAMES);
    if (got == 0) break;
    v.ring.pushHigh(chunkBuffer, got);
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
// switch are band-limited the same way, the switch itself is inaudible
// wherever it happens (see "Playback voices" above).
const float LEVEL_UP_STEP = 2.0f;
const float LEVEL_DOWN_STEP = 0.85f;

uint8_t chooseLevel(float stepMagnitude, uint8_t current, uint8_t maxLevel) {
  if (current > maxLevel) current = maxLevel;
  float s = stepMagnitude * ldexpf(1.0f, -(int)current);
  if (s <= LEVEL_UP_STEP && (s >= LEVEL_DOWN_STEP || current == 0)) return current;
  // Out of range: jump straight to the copy that suits this speed, however
  // many levels away it is - one switch, not one per level.
  uint8_t k = 0;
  while (k < maxLevel && stepMagnitude * ldexpf(1.0f, -(int)k) > LEVEL_UP_STEP) k++;
  return k;
}

// The rate to choose a copy by, for a step through the original file.
//
// Unshifted, that's just the step. But the pitch shifter's grains read at
// their own rate - the step times pitchRatio, or grainTapRate when
// time-stretching - and readGrains() doesn't low-pass them, so it's the copy
// that has to: one filtered for the step alone would alias when the pitch is
// shifted far up, and lose its treble when it's shifted far down. So choose
// by the grains' rate instead - as long as the cursor stays within the
// voice's MAX_VOICE_RATE through it (the level choice keeps a step of up to
// LEVEL_UP_STEP, so a quarter of the step keeps it within 4x that).
//
// The same rule in both knob modes, so the same speed and pitch sound the
// same in either: it used to apply only while time-stretching, and
// switching modes at a large shift switched the aliasing on and off.
float levelRateFor(float step) {
  float s = fabsf(step);
  float grains = (grainTapRate >= 0.0f) ? grainTapRate : s * pitchRatio;
  return fmaxf(grains, s / 4.0f);
}

// Where voice `from` is right now, as a position in voice `to`'s copy.
// Integer arithmetic for the whole-sample part, since this runs with the
// audio interrupt masked and has to be quick - this chip has no hardware
// double precision - and since a float can't hold a large sample position
// exactly anyway.
void positionInCopy(Voice &from, Voice &to, int32_t &idx, float &frac) {
  uint64_t whole = fileOffsetFor(from, from.resampler.index);
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
  // A file offset in `to`'s copy, which is also a position in it: a freshly
  // opened voice's positions start out equal to its file offsets (see
  // resetVoiceWindow()).
  idx = (int32_t)whole;
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

// Switches playback to the idle voice, opened on copy `target`. Everything
// slow - opening the copy, reading it from the card - happens on the idle
// voice while the active one keeps playing; interrupts are only masked for
// the microsecond or two it takes to hand over.
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

  // Arrive at the new step now rather than gliding to it (see currentStep),
  // before priming: the new copy is primed only so far around where the
  // cursor will be, which assumes the cursor moves at the speed the copy
  // was chosen for while it's read. Gliding down from 200x instead carried
  // it through several more loops of a short file meanwhile, far from the
  // primed stretch, and playback never recovered.
  noInterrupts();
  currentStep = targetStep;
  stepSlope = 0.0f;
  interrupts();

  int32_t idx;
  float frac;
  positionInCopy(playing, incoming, idx, frac);
  int32_t primedAt = idx;
  primeVoice(incoming, idx - PRIME_BEHIND, PRIME_BEHIND + PRIME_AHEAD);

  noInterrupts();
  // Again, now: the playing voice moved on while the copy was being read.
  positionInCopy(playing, incoming, idx, frac);
  // If it crossed the end of the window meanwhile, the fresh position will
  // have wrapped round to the start - but the ring was primed in one
  // continuous stretch, so unwrap it back alongside where priming began.
  int32_t len = (int32_t)incoming.window.length;
  if (idx < primedAt - len / 2) idx += len;
  if (idx > primedAt + len / 2) idx -= len;
  incoming.resampler.index = idx;
  incoming.resampler.frac = frac;
  activeVoice = idle;  // the old voice goes idle, ready for the next change
  interrupts();

  Serial.print("Switched to ");
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
// Normally the cursor can't leave the stretch of file held in its voice's
// ring buffer: when a read would reach past either end, the audio
// interrupt reports an underrun and stays put until the refill catches up.
// Only code that sets the position directly can strand it outside - a copy
// change, which hands the cursor to a copy that was read in around where
// the cursor was a moment earlier, is the one that can. If the cursor has
// moved further than that while the copy was read (a slow card, or a bug -
// see startLevelChange()), the refill can't recover: it grows the buffer
// from its edges, and once it's full there's nowhere left to grow toward
// the cursor. Playback would stay silent until a rewind or a new file. So
// loop() checks every pass, and reloads the buffer around the cursor if it
// ever finds it outside - a dropout of a few milliseconds instead.
//
// How far past an edge the cursor can legitimately be: an advance only
// happens after a successful read, so at most one step's worth beyond.
const int32_t STRANDED_SLACK = 64;  // frames
uint32_t strandedRepairs = 0;

void repairStrandedCursor(Voice &v, float step) {
  int32_t pos = v.resampler.index;
  if (pos >= v.ring.validLow() - STRANDED_SLACK && pos < v.ring.validHigh() + STRANDED_SLACK) return;

  // Same care as rewindToStart(): the interrupt reads the position and the
  // buffer, so it's held off across the reload.
  audioTimer.disable_overflow_irq();
  pos = v.resampler.index;
  // More runway on whichever side the cursor is heading toward.
  int32_t from = (step < 0.0f) ? pos - PRIME_AHEAD : pos - PRIME_BEHIND;
  primeVoice(v, from, PRIME_BEHIND + PRIME_AHEAD);
  audioTimer.enable_overflow_irq();
  restartDropCount();

  strandedRepairs++;
  Serial.print("Playback cursor was stranded outside its buffer - reloaded it there (");
  Serial.print(strandedRepairs);
  Serial.println(" so far). This shouldn't happen; please report it.");
}

void refillVoice(Voice &v, float step) {
  if (!v.reader.isOpen()) return;
  int32_t pos = v.resampler.index;

  if (step > 0.0f) {
    int32_t wantLow = pos - REVERSAL_MARGIN;
    if (wantLow > v.ring.validLow()) v.ring.releaseLow(wantLow);
    if (v.ring.freeSpace() >= CHUNK_FRAMES) {
      size_t got = readLooped(v, fileOffsetFor(v, v.ring.validHigh()), chunkBuffer, CHUNK_FRAMES);
      if (got > 0) v.ring.pushHigh(chunkBuffer, got);
    }
  } else if (step < 0.0f) {
    int32_t wantHigh = pos + REVERSAL_MARGIN;
    if (wantHigh < v.ring.validHigh()) v.ring.releaseHigh(wantHigh);
    if (v.ring.freeSpace() >= CHUNK_FRAMES) {
      // A "backward" chunk needs no in-memory reversal: it's an
      // ordinary forward read starting earlier in the file, which
      // lands in exactly the ascending order pushLow() expects.
      int32_t startVirtual = v.ring.validLow() - (int32_t)CHUNK_FRAMES;
      size_t got = readLooped(v, fileOffsetFor(v, startVirtual), chunkBuffer, CHUNK_FRAMES);
      if (got == CHUNK_FRAMES) v.ring.pushLow(chunkBuffer, got);
    }
  }
  // step == 0: paused - deliberately untouched. See Resampler.h's
  // advance() and audioCallback() above: this needs no special case
  // here either, since the consumer's position simply stops moving.
}

// -- The window ---------------------------------------------------------------
// Mode 3's two knobs move the start and the end of the stretch of file that
// plays. Everything else then treats that stretch as the whole tape: it
// loops, the transport rewinds to its start, and the scrub goes round it.
//
// That takes very little, because looping only ever happened in one place:
// fileOffsetFor(), which maps a voice's ever-advancing position onto the
// file, and now maps it onto the voice's window instead. The audio
// interrupt knows nothing about it.
//
// The catch is that a voice reads ahead - its ring buffer holds a third of
// a second at 1x, and many laps of the whole file at high speeds - and
// whatever is already buffered was read for the old window. So when the
// window moves under a playing voice (moveVoiceWindow()), the buffer is cut
// back to a short runway ahead of the cursor, and the window's mapping is
// re-anchored so that reading carries on seamlessly from there, round the
// new window. No dropout, and the change is heard within the runway's
// length.
//
// The join from the window's end back to its start is crossfaded, or it
// would click on every lap - the audio either side of it has nothing to do
// with each other. Over the window's last few milliseconds its own audio
// fades out while the lead-in to its start (the audio just before the
// window) fades in, so that by the last frame the signal is exactly what
// comes before the first, and carries straight on into it. A window at the
// very start of the file has no lead-in, so it's done the other way round
// there: the window's first few milliseconds fade in from the audio that
// follows its end. It's worked into the audio as it's read from the card
// (readLooped() and fadeSeam()), so it costs the audio interrupt nothing,
// and it's as smooth played backward or scrubbed across. It can't change
// what a short window is, though: a 20ms loop is a 50Hz buzz however
// neatly it's joined - only the click goes.
//
// If the cut would land outside the new window - an end has been
// moved past the playing position - reading carries on from the window's
// start instead (or from its end, playing backward).
//
// An end stops at the end of the file rather than carrying on round it,
// and the two ends can't come closer than WINDOW_MIN_SECONDS: push one
// into the other and both move.
const float WINDOW_MIN_SECONDS = 0.02f;

// How much already-buffered audio is left ahead of the cursor when the
// window moves, in frames of the voice's copy - around 5-10ms. Long enough
// that loop() refills it well before the cursor gets there.
const int32_t WINDOW_RUNWAY = 512;

double windowMinFrames() {
  return fmin((double)fileFrames, fmax(1.0, (double)WINDOW_MIN_SECONDS * nativeRate));
}

// Wraps a position in frames of the WAV into the window.
double wrapToWindow(double frames) {
  double length = windowEnd - windowStart;
  double wrapped = fmod(frames - windowStart, length);
  return windowStart + ((wrapped < 0.0) ? wrapped + length : wrapped);
}

// Brings a playing voice into line with a window that has just moved - see
// above. `step` only says which way it's playing.
void moveVoiceWindow(Voice &v, float step) {
  CopyWindow was = v.window;
  CopyWindow now = windowInCopy(v);
  if (now.start == was.start && now.length == was.length) return;

  // If everything buffered in the direction of travel lies inside both the
  // old window and the new one, in one piece and clear of both crossfades,
  // it's all still right and nothing need be thrown away. Otherwise keep
  // only a runway.
  int32_t cursor = v.resampler.index;
  int32_t limit = (step >= 0.0f) ? v.ring.validHigh() : v.ring.validLow();
  int64_t at = fileOffsetFor(v, cursor);
  int64_t reach = at + (limit - cursor);  // the far end of what's buffered that way
  int64_t lowest = (reach < at) ? reach : at;
  int64_t highest = (reach < at) ? at : reach;
  int64_t safeLow = was.start + was.fade;
  if (safeLow < now.start + now.fade) safeLow = now.start + now.fade;
  int64_t safeHigh = was.start + was.length - was.fade;
  if (safeHigh > now.start + now.length - now.fade) safeHigh = now.start + now.length - now.fade;
  bool stillGood = lowest >= safeLow && highest <= safeHigh;

  int32_t edge = cursor;  // the position the next read will join on to
  if (!stillGood && step >= 0.0f) {
    edge = cursor + WINDOW_RUNWAY;
    if (edge < limit) v.ring.releaseHigh(edge);
    else edge = limit;
  } else if (!stillGood) {
    edge = cursor - WINDOW_RUNWAY;
    if (edge > limit) v.ring.releaseLow(edge);
    else edge = limit;
  }

  // Re-anchor: the edge keeps the file offset it had if that's still in
  // the window, and otherwise becomes the window's start.
  uint32_t edgeAt = fileOffsetFor(v, edge);  // by the old window
  bool inside = edgeAt >= now.start && edgeAt < now.start + now.length;
  v.window = now;
  v.windowBase = inside ? edge - (int32_t)(edgeAt - now.start) : edge;
}

// Keeps a window legal: inside the file, and no shorter than the minimum.
void clampWindow(double &start, double &end) {
  double minimum = windowMinFrames();
  end = constrain(end, minimum, (double)fileFrames);
  start = constrain(start, 0.0, end - minimum);
}

// Sets the window, in frames of the WAV, and brings playback into line.
void setWindow(double start, double end) {
  clampWindow(start, end);
  windowStart = start;
  windowEnd = end;
  if (fileLoaded) moveVoiceWindow(voices[activeVoice], currentStep);
}

// WINDOW:<start>,<end>, in seconds - an exact window, mostly for testing.
void setWindowSeconds(const String &args) {
  int comma = args.indexOf(',');
  if (comma == -1 || !fileLoaded) return;
  setWindow(args.substring(0, comma).toFloat() * (double)nativeRate,
            args.substring(comma + 1).toFloat() * (double)nativeRate);
  printWindow();
}

void resetWindowToWholeFile() {
  setWindow(0.0, (double)fileFrames);
}

void printWindow() {
  Serial.print("Window: ");
  Serial.print(windowStart / nativeRate, 3);
  Serial.print("s to ");
  Serial.print(windowEnd / nativeRate, 3);
  Serial.print("s of ");
  Serial.print((double)fileFrames / nativeRate, 3);
  Serial.println("s");
}

// Whether the playing voice has all the data it wants for now - the cache
// builder only runs then, so a slow SD write never comes at the expense of
// playback.
bool voiceToppedUp(float step) {
  if (step == 0.0f) return true;
  return voices[activeVoice].ring.freeSpace() < CHUNK_FRAMES;
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

  flushSettings();  // the outgoing file's, before it's replaced

  audioTimer.disable_overflow_irq();

  // Every file starts on its original WAV, in voice 0; the other voice is
  // closed until a copy change needs it.
  activeVoice = 0;
  voices[1].reader.close();
  voices[1].ring.reset();
  Voice &v = voices[0];
  v.reader = std::move(candidate);  // closes the old file (if any), adopts the new one
  v.level = 0;
  v.levelScale = 1.0f;
  endScrub();
  lastGoodLeft = lastGoodRight = 0;  // don't carry a stale frame into the new file
  grainOffset = 0.0f;
  // Per-file, not cumulative: the interesting number is how badly *this*
  // file underruns at the ratio being asked for, which is not comparable
  // across files with different native sample rates anyway. Safe to write
  // bare here - the audio interrupt is disabled for this whole block.
  underrunTicks = 0;
  nativeRate = v.reader.nativeSampleRate();
  fileChannels = v.reader.channels();
  v.ring.setChannels(fileChannels);
  voices[1].ring.setChannels(fileChannels);
  rateFactor = nativeRate / 48000.0f;
  fileFrames = v.reader.totalFrames();
  fitScrubToFile();
  // Each file starts at its own saved speed, pitch and window (native speed,
  // unshifted and the whole file if it has none), regardless of wherever
  // the (relative, not absolute - see resetVirtualKnobToNative()'s comment)
  // knobs currently sit: set directly rather than waiting for the next
  // control-rate tick to catch up, so there's no gap where the previous
  // file's settings apply.
  applySettings(FileSettings::load(filename));
  // And cued at the start of its window.
  resetVoiceWindow(v);
  v.resampler.index = (int32_t)v.window.start;
  v.resampler.frac = 0.0f;
  // Only moving if the transport is running. Starting at 1x regardless
  // played a fragment of every file picked while stopped: the audio comes
  // back on below, and cache.start() can take tens of milliseconds before
  // the next control tick notices the transport is stopped.
  setStepNow(transportRunning ? lastKnownRatio * rateFactor : 0.0f);

  // Prime the buffer before resuming, same as the original boot-time
  // pre-fill, so playback doesn't wake into an instant underrun. Always
  // fills forward from the start of the window regardless of which direction
  // the knob currently requests - if it's already parked at a negative
  // ratio the instant a file loads, a brief low-side underrun is
  // possible for a fraction of a second until loop()'s normal backward
  // refill catches up. Same accepted trade-off as the file-switch click
  // this function already causes - see the header comment.
  //
  // Starts PRIME_BEHIND samples before the start of the window - i.e. from
  // the end of it, since the window plays as a loop - so the interpolator
  // has history to reach back into from the very first sample.
  primeVoice(v, v.resampler.index - PRIME_BEHIND, v.ring.frameCapacity());

  audioTimer.enable_overflow_irq();
  restartDropCount();

  currentFilename = filename;
  fileLoaded = true;
  rememberCurrentFile();
  Serial.print("Loaded ");
  Serial.print(filename);
  Serial.print(": ");
  Serial.print(nativeRate);
  Serial.print(" Hz, ");
  Serial.print(v.reader.formatName());
  Serial.print(fileChannels == 2 ? " stereo, " : " mono, ");
  Serial.print(v.reader.totalFrames());
  Serial.println(" frames");
  // Check this file's cached copies, and build whatever's missing - in the
  // background, a step per loop() pass. Until they're ready, high speeds are
  // capped (see the control block in loop()).
  cache.start(filename);
  return true;
}

// Returns the playback cursor to the top of the file - or of the window,
// if mode 3's knobs have moved its start.
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
// be read fast enough to keep up while the change to the right copy was
// being prepared.
void rewindToStart() {
  uint8_t level = chooseLevel(levelRateFor(lastKnownRatio * rateFactor), 0, readyLevels());

  audioTimer.disable_overflow_irq();

  Voice &v = voices[activeVoice];
  if (level != v.level && !openVoice(v, currentFilename, level)) {
    openVoice(v, currentFilename, 0);  // copy unreadable - fall back to the WAV
  }
  resetVoiceWindow(v);
  v.resampler.index = (int32_t)v.window.start;
  v.resampler.frac = 0.0f;
  endScrub();
  lastGoodLeft = lastGoodRight = 0;  // don't carry a stale frame across the jump
  grainOffset = 0.0f;   // start the next grain cycle from the seam, not mid-crossfade
  // Same per-playback logic as loadFile(): the interesting figure is how
  // this pass underruns, not one accumulated across every previous pass.
  underrunTicks = 0;

  primeVoice(v, v.resampler.index - PRIME_BEHIND, v.ring.frameCapacity());  // see loadFile()

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
  flushSettings();
  audioTimer.disable_overflow_irq();
  fileLoaded = false;
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

// Turns the virtual speed dial by `delta` degrees and sets the speed from
// it - knob1 in speed mode, knob2 (time-stretch) in pitch mode. The first
// call after anything clears the anchor (a new file, SPEED:) anchors the
// dial at speedAnchorRatio instead, ignoring the knob's absolute position
// entirely - the whole point of relative control is that wherever the
// real knob happens to be sitting doesn't matter.
void turnSpeedDial(float delta) {
  if (!speedKnobAnchored) {
    speedKnobDegrees = knobForSpeed(speedAnchorRatio);
    speedKnobAnchored = true;
  } else {
    speedKnobDegrees += delta;
  }

  float ratio = speedForDial(dialForKnob(speedKnobDegrees));
  lastKnownRatio = ratio;
  if (ratio < minRatioSeen) minRatioSeen = ratio;
  if (ratio > maxRatioSeen) maxRatioSeen = ratio;
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
  if (modeHoldsPitch()) holdPitch();  // as knob2's time-stretch does
}

// Below this speed the pitch you hear can't be worked out from the shift
// (it's the shift times the speed), so the shift is left alone there.
const float STRETCH_MIN_SPEED = 0.001f;

// The pitch you hear, relative to the file's own: the shift times the speed.
float perceivedPitch() {
  float speed = fabsf(lastKnownRatio);
  return (speed >= STRETCH_MIN_SPEED) ? pitchRatio * speed : pitchRatio;
}

// Makes heldPitch match the current speed and shift again - after anything
// sets one of them directly.
void syncHeldPitch() {
  heldPitch = perceivedPitch();
}

// The other way round, for pitch mode: sets the shift so that the current
// speed sounds at heldPitch.
void holdPitch() {
  float speed = fabsf(lastKnownRatio);
  if (speed >= STRETCH_MIN_SPEED) pitchRatio = heldPitch / speed;
}

void resetPitchToUnity() {
  pitchRatio = 1.0f;
  syncHeldPitch();
}

// PITCH:<ratio> - an exact pitch shift.
void setPitchRatio(float ratio) {
  pitchRatio = constrain(ratio, 1.0f / 1024.0f, 1024.0f);
  syncHeldPitch();
}

// HEARD:<ratio> - the pitch you hear, set the way the pitch knob sets it:
// the speed stays, and the shift makes up the difference.
void setHeardPitch(float ratio) {
  float limit = exp2f(PITCH_OCTAVE_LIMIT);
  heldPitch = constrain(ratio, 1.0f / limit, limit);
  holdPitch();
}

// STRETCH:<ratio> - the speed, set the way the time-stretch knob sets it:
// the pitch you hear stays where it was. Whatever the mode, unlike SPEED:.
//
// heldPitch is only kept up to date in pitch mode, so in any other it's
// worked out here first - but not at a standstill, where the pitch can't be
// worked out from the shift (see STRETCH_MIN_SPEED). Doing that anyway took
// the shift itself for the pitch every time a stretch passed through zero,
// and sent the pitch up by the hundredfold each time.
void stretchToSpeed(float ratio) {
  if (!modeHoldsPitch() && fabsf(lastKnownRatio) >= STRETCH_MIN_SPEED) syncHeldPitch();
  float held = heldPitch;
  setSpeedRatio(ratio);
  heldPitch = held;
  holdPitch();
}

// The pitch you hear, for the stats line. At a standstill it's the pitch
// being held for when the tape moves again, not the bare shift.
float reportedPitch() {
  return (fabsf(lastKnownRatio) >= STRETCH_MIN_SPEED) ? perceivedPitch() : heldPitch;
}

// TAPE:<ratio> - the speed, set the way the speed knob sets it: the shift
// stays, so the pitch you hear goes up and down with the speed. Whatever
// the mode - in pitch mode SPEED: would hold the pitch instead.
void tapeToSpeed(float ratio) {
  float shift = pitchRatio;
  setSpeedRatio(ratio);
  pitchRatio = shift;
  syncHeldPitch();
}

// Loads the first file on the card this demo can actually play. Returns
// whether anything was cued. See the call site in setup() for why.
// Remembers which file is cued, so a reboot comes back to it (see
// cueRememberedFile()). Written only when it changes.
String rememberedFile;

// Sets the echo: its delay in seconds and its amount (level and feedback),
// each held within its range.
void setEcho(float seconds, float amount) {
  echoSeconds = constrain(seconds, ECHO_MIN_SECONDS, ECHO_MAX_SECONDS);
  echoAmount = constrain(amount, 0.0f, ECHO_MAX_AMOUNT);
  echo.setTime(echoSeconds);
  echo.setAmount(echoAmount);
}

void printEcho() {
  Serial.print("Echo: ");
  Serial.print(echoSeconds, 3);
  Serial.print("s, amount ");
  Serial.println(echoAmount, 2);
}

// ECHO:<seconds>,<amount> - an exact echo, mostly for testing.
void setEchoFromCommand(const String &args) {
  int comma = args.indexOf(',');
  if (comma == -1) return;
  setEcho(args.substring(0, comma).toFloat(), args.substring(comma + 1).toFloat());
  printEcho();
}

// Echo off. The delay time stays where it was, ready for next time.
void resetEcho() {
  setEcho(echoSeconds, 0.0f);
}

// The centre knob's curve: from the knob's position, in turns either way
// from the middle, to the centre in octaves either way from
// FILTER_CENTRE_HZ. A straight line plus a cube: the line, at
// FILTER_OCTAVES_PER_TURN, is all there is near the middle, so the audible
// range plays much as it would without the curve; the cube takes over
// further out, to cover the reach in the turns allowed.
const float FILTER_CENTRE_CUBE =
    (FILTER_CENTRE_REACH_OCTAVES - FILTER_OCTAVES_PER_TURN * FILTER_CENTRE_REACH_TURNS) /
    (FILTER_CENTRE_REACH_TURNS * FILTER_CENTRE_REACH_TURNS * FILTER_CENTRE_REACH_TURNS);

float filterCentreOctaves(float turns) {
  return FILTER_OCTAVES_PER_TURN * turns + FILTER_CENTRE_CUBE * turns * turns * turns;
}

// ...and back: the knob position for a centre, solving the cubic directly
// (Cardano's formula - this cubic always has exactly one real answer).
float filterCentreTurns(float octaves) {
  float p = FILTER_OCTAVES_PER_TURN / FILTER_CENTRE_CUBE / 3.0f;
  float q = octaves / FILTER_CENTRE_CUBE / 2.0f;
  float d = sqrtf(q * q + p * p * p);
  return cbrtf(q + d) + cbrtf(q - d);
}

// Sets the filter from its centre (Hz) and width (octaves, 0 to 10), as
// the knobs play it. Think of the band as two shelves - the high-pass below
// it and the low-pass above it - with the width the space between them:
// half of it either side of the centre. Moving the centre slides both
// shelves together, and the width stays what it was set to. A shelf
// outside the audible range does nothing you can hear; slid all the way
// past the far end, it takes the sound with it - BandFilter.h fades it out
// as the filter's slope would. At the full ten octaves, centred, the
// filter is wide open, out of the circuit.
void setFilterBand(float centreHz, float widthOctaves) {
  filterCentreHz = constrain(centreHz, FILTER_CENTRE_LOWEST_HZ, FILTER_CENTRE_HIGHEST_HZ);
  filterWidthOctaves = constrain(widthOctaves, 0.0f, FILTER_OPEN_OCTAVES);
  float half = exp2f(filterWidthOctaves / 2.0f);
  filterLowHz = filterCentreHz / half;
  filterHighHz = filterCentreHz * half;
  filter.setCutoffs(filterLowHz, filterHighHz);
}

// Hz to print: whole Hz, or a few decimals where the centre has gone far
// below anything audible.
int hzDecimals(float hz) { return hz < 10.0f ? 3 : 0; }

// Sets the filter by its bottom and top cutoffs in Hz instead: the band
// between them, centred.
void setFilter(float lowHz, float highHz) {
  lowHz = constrain(lowHz, FILTER_MIN_HZ, FILTER_MAX_HZ);
  highHz = constrain(highHz, lowHz, FILTER_MAX_HZ);
  setFilterBand(sqrtf(lowHz * highHz), log2f(highHz / lowHz));
}

void printFilter() {
  Serial.print("Filter: ");
  Serial.print(filterLowHz, hzDecimals(filterLowHz));
  Serial.print("Hz to ");
  Serial.print(filterHighHz, hzDecimals(filterHighHz));
  Serial.print("Hz (centre ");
  Serial.print(filterCentreHz, hzDecimals(filterCentreHz));
  Serial.print("Hz), ");
  Serial.print(filterWidthOctaves, 2);
  Serial.println(filter.isOpen() ? " octaves wide (open: no filter)" : " octaves wide");
}

// FILTER:<low Hz>,<high Hz> - an exact band, mostly for testing.
void setFilterFromCommand(const String &args) {
  int comma = args.indexOf(',');
  if (comma == -1) return;
  setFilter(args.substring(0, comma).toFloat(), args.substring(comma + 1).toFloat());
  printFilter();
}

// BAND:<centre Hz>,<width octaves> - the filter as the knobs play it; the
// web page's way of setting it.
void setFilterBandFromCommand(const String &args) {
  int comma = args.indexOf(',');
  if (comma == -1) return;
  setFilterBand(args.substring(0, comma).toFloat(), args.substring(comma + 1).toFloat());
  printFilter();
}

// Filter off: wide open, centred.
void resetFilter() {
  setFilterBand(FILTER_CENTRE_HZ, FILTER_OPEN_OCTAVES);
}

void rememberCurrentFile() {
  if (currentFilename == rememberedFile) return;
  FileSettings::saveCurrentName(currentFilename);
  rememberedFile = currentFilename;
}

// At boot: cues the file that was cued last time, if it's still on the
// card and plays - otherwise the first playable one, as before.
bool cueRememberedFile() {
  rememberedFile = FileSettings::loadCurrentName();
  if (rememberedFile.length() > 0) {
    String err;
    if (loadFile(rememberedFile, err)) {
      Serial.print("Cued ");
      Serial.print(rememberedFile);
      Serial.println(", the file cued last time");
      return true;
    }
    Serial.print("Couldn't cue ");
    Serial.print(rememberedFile);
    Serial.print(", the file cued last time - ");
    Serial.println(err);
  }
  return cueFirstPlayableFile();
}

// The first playable WAV in the list's order (see WavList.h).
bool cueFirstPlayableFile() {
  for (const String &name : sortedWavNames()) {
    String err;
    if (loadFile(name, err)) {
      Serial.print("Cued ");
      Serial.println(name);
      return true;
    }
    Serial.print("  skipping ");
    Serial.print(name);
    Serial.print(" - ");
    Serial.println(err);
  }

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
// So: one name per pumpFileList() call, at the same 40ms spacing. Same rate
// on the wire, no blocking. The names are read and sorted up front (see
// WavList.h) - reading the folder takes a few milliseconds, not 40 per name -
// so the page gets them in the order the file knob steps through them.
// Telemetry is suppressed for the duration rather than relying on a fixed
// cooldown afterwards, which is also strictly more correct - a long list can
// legitimately take longer to drain than any fixed window.
const unsigned long FILE_LIST_LINE_INTERVAL = 40;  // ms between names

void finishFileList();

std::vector<String> fileListNames;  // what's left to send, in order
size_t fileListNext = 0;
bool fileListSending = false;
unsigned long nextFileLineMillis = 0;

bool fileListInProgress() { return fileListSending; }

void abortFileList() {
  fileListSending = false;
  fileListNames.clear();
  fileListNames.shrink_to_fit();  // give the names' memory back between lists
}

void startFileList() {
  abortFileList();  // a second LIST mid-send restarts cleanly
  blePrintLine("FILES:BEGIN");
  fileListNames = sortedWavNames();
  fileListNext = 0;
  fileListSending = true;
  nextFileLineMillis = millis() + FILE_LIST_LINE_INTERVAL;
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
  if (!fileListSending) return;
  if ((long)(millis() - nextFileLineMillis) < 0) return;

  if (fileListNext < fileListNames.size()) {
    blePrintLine(fileListNames[fileListNext++]);
    nextFileLineMillis = millis() + FILE_LIST_LINE_INTERVAL;
    return;
  }
  finishFileList();  // all sent
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
  return isWavName(name);
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
  // Its saved settings are kept, though: a new version of the same file is
  // most likely the same sample, re-edited.
  //
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
    FileSettings::renameFor(from, to);
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
    FileSettings::removeFor(name);
    Serial.print("Deleted ");
    Serial.println(name);
    blePrintLine("DELETED:" + name);
  } else {
    blePrintLine("ERR:" + name + ":delete failed");
  }

  if (wasCurrent) recueAfterLosingFile();
}

// RESET:CACHE - deletes the current file's cached copies and loads the
// file again, which starts building them afresh, just as the first time it
// was played. The file has to be unloaded first: the builder holds its
// copies open. Playback goes back to the top, still latched if it was.
void rebuildCache() {
  if (!fileLoaded) return;
  String name = currentFilename;
  bool latched = transportLatched;
  unloadFile();
  MipCache::removeFor(name);
  Serial.print("Deleted the cached copies of ");
  Serial.print(name);
  Serial.println(" - building them again.");
  String err;
  if (loadFile(name, err)) {
    transportLatched = latched;
  } else {
    Serial.println("...but it wouldn't load again: " + err);
    recueAfterLosingFile();
  }
}

// Whether a line is one of the commands handleBLECommand() takes - the
// web page's words, which can only ever reach this sketch through an open
// Binary mode pipe. See handleBLELine() for why that matters.
bool isBoardCommand(const String &line) {
  static const char *const COMMANDS[] = {
    "LIST", "PLAY:", "LATCH:", "STOP", "RESET:", "PROFILE", "SPEED:", "PITCH:", "WINDOW:", "ECHO:", "FILTER:", "BAND:", "PUT:", "REN:", "DEL:", "DISK:",
    "MODE:", "HEARD:", "STRETCH:", "TAPE:",
    "CAL", "RECAL"
  };
  for (const char *command : COMMANDS) {
    if (line.startsWith(command)) return true;
  }
  return false;
}

void handleBLECommand(String line) {
  line.trim();
  if (line == "DISK:ON") {
    enterUsbDiskMode();
    return;
  }
  if (line == "DISK:OFF") {
    leaveUsbDiskMode();
    return;
  }
  // Everything else touches the card or the playback that reads it, and the
  // card belongs to the computer for now.
  if (usbDiskMode) {
    blePrintLine("ERR:DISK:the card is a USB disk until it's ejected");
    return;
  }

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
  } else if (line == "RESET:ALL") {
    resetAllTweaks();
  } else if (line == "RESET:SPEED") {
    resetSpeedToNative();
  } else if (line == "PROFILE") {
    printProfile();
  } else if (line.startsWith("SPEED:")) {
    setSpeedRatio(line.substring(6).toFloat());
  } else if (line.startsWith("PITCH:")) {
    // Like SPEED:, an exact pitch ratio for testing - the knob carries on
    // from there the next time it's turned in pitch mode.
    setPitchRatio(line.substring(6).toFloat());
  } else if (line == "RESET:PITCH") {
    resetPitchToUnity();
  } else if (line.startsWith("HEARD:")) {
    setHeardPitch(line.substring(6).toFloat());
  } else if (line.startsWith("STRETCH:")) {
    stretchToSpeed(line.substring(8).toFloat());
  } else if (line.startsWith("TAPE:")) {
    tapeToSpeed(line.substring(5).toFloat());
  } else if (line.startsWith("MODE:")) {
    selectModeFromCommand(line.substring(5).toInt());
  } else if (line.startsWith("WINDOW:")) {
    setWindowSeconds(line.substring(7));
  } else if (line == "RESET:WINDOW") {
    resetWindowToWholeFile();
  } else if (line.startsWith("ECHO:")) {
    setEchoFromCommand(line.substring(5));
  } else if (line == "RESET:ECHO") {
    resetEcho();
  } else if (line.startsWith("FILTER:")) {
    setFilterFromCommand(line.substring(7));
  } else if (line.startsWith("BAND:")) {
    setFilterBandFromCommand(line.substring(5));
  } else if (line == "RESET:FILTER") {
    resetFilter();
  } else if (line == "RESET:CACHE") {
    rebuildCache();
  } else if (line.startsWith("PUT:")) {
    startUpload(line.substring(4));
  } else if (line.startsWith("REN:")) {
    renameOnCard(line.substring(4));
  } else if (line.startsWith("DEL:")) {
    deleteFromCard(line.substring(4));
  } else if (line == "CAL") {
    printKnobCalibration();
  } else if (line == "RECAL") {
    forgetKnobCalibration();
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

// -- Triple tap ---------------------------------------------------------------
// Both pads tapped together three times resets the current file's settings
// (resetAllTweaks()). A "tap" here is the chord - both pads down at once -
// and then both pads up again, all inside TAP_MAX_MS; anything shorter than
// TAP_MIN_MS is taken for a pad's reading flickering rather than a finger.
// Each tap has to start within TAP_GAP_MS of the last one ending.
//
// Nothing waits on this. The chord still latches the transport the instant
// it lands, as it always did, and the reset happens the instant the third
// chord lands - there is no pause to see whether another tap is coming,
// because no other gesture uses a second or third chord.
//
// Two fingers never land in the same instant, so each tap really goes one
// pad, both, one, none. The lone pad at either end is ignored here (though
// not by the transport or the mode selector: fingers that miss each other
// entirely are two separate taps to them). But a pad touched and let go on
// its own, with no chord, is some other gesture, and starts the count again.
const unsigned long TAP_MIN_MS = 30;
const unsigned long TAP_MAX_MS = 400;
const unsigned long TAP_GAP_MS = 400;
uint8_t chordTapCount = 0;          // taps so far toward the three
bool chordHeld = false;             // from the chord landing until both pads are up
bool padAlone = false;              // a pad is down that hasn't been part of a chord
unsigned long chordStartMillis = 0;
unsigned long chordEndMillis = 0;

// The white flash that says a reset happened.
const unsigned long RESET_FLASH_MS = 150;
bool resetFlashing = false;
unsigned long resetFlashStartMillis = 0;

// Everything that's saved per file back to its default - what a file that
// has never been played gets. From the triple tap and RESET:ALL.
void resetAllTweaks() {
  if (!fileLoaded) return;
  FileSettings defaults;
  resetSpeedToNative();
  resetPitchToUnity();
  resetWindowToWholeFile();
  setEcho(defaults.echoTime, defaults.echoAmount);
  setFilterBand(defaults.filterCentre, defaults.filterWidth);
  resetFlashStartMillis = millis();
  resetFlashing = true;
  Serial.print("Reset: speed, pitch, window, echo and filter back to defaults for ");
  Serial.println(currentFilename);
}

// Call every control tick with the two pads' states.
void watchForTripleTap(bool pad1, bool pad2, unsigned long now) {
  bool both = pad1 && pad2;
  bool neither = !pad1 && !pad2;

  if (both && !chordHeld) {
    // A chord lands.
    chordHeld = true;
    padAlone = false;
    chordStartMillis = now;
    if (chordTapCount > 0 && now - chordEndMillis > TAP_GAP_MS) chordTapCount = 0;
    if (chordTapCount == 2) {
      chordTapCount = 0;
      chordStartMillis = now - TAP_MAX_MS - 1;  // this one isn't the first tap of another three
      resetAllTweaks();
    }
  } else if (chordHeld && neither) {
    // ...and has been let go. Was it a tap?
    chordHeld = false;
    unsigned long held = now - chordStartMillis;
    if (held >= TAP_MIN_MS && held <= TAP_MAX_MS) {
      chordTapCount++;
      chordEndMillis = now;
    } else {
      chordTapCount = 0;
    }
  } else if (!chordHeld) {
    if (!neither) {
      padAlone = true;
    } else if (padAlone) {
      padAlone = false;
      chordTapCount = 0;
    }
  }
}

// -- Onboard LEDs (mode and transport indicators) -------------------------
// The RGB LED - the same WS2812 on D13 that WS2812TouchDemo drives - is
// always lit, in the colour of the knob mode. The blue STAT LED
// (LED_BUILTIN) shows the transport: blinking = running from a held finger,
// steady = latched, dark = stopped.
const uint8_t RGB_LED_PIN = 13;
const uint8_t NUM_LEDS = 1;
CRGB leds[NUM_LEDS];

// This LED is startlingly bright at full scale, so everything here is
// expressed as a percentage of it and kept low. Adjustable by hand for now.
const uint8_t RGBLED_BRIGHTNESS = 10;                    // percent
const unsigned long TRANSPORT_BLINK_INTERVAL = 300;      // ms on, ms off

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

// The second pot: tape scrub. While it's being turned it takes over the
// playback cursor outright, like a hand on the reel - see "Tape scrub" in
// the header comment, and scrubKnob() below.
RV112FF pot2(A4, A5);

// -- Tape scrub -------------------------------------------------------------
// How much of the file one turn of the scrub knob covers, before it's
// fitted to the file: one second of audio. fitScrubToFile() then moves it
// to the nearest power-of-two fraction or multiple of the file's length,
// so that a whole number of turns - or of files per turn - lines up with
// the file's loop. Nearest in the log domain, so that's never more than a
// factor of the square root of 2 away from this: a 3-second file gets a
// quarter of the file per turn (0.75s), a 1.4-second one the whole file,
// a 3-minute one 1/128th of it (1.4s).
const float SCRUB_SECONDS_PER_TURN = 1.0f;

// How far the knob has to move from where it last came to rest to start a
// scrub. A resting knob's reading jitters by a fraction of a degree, and
// a scrub pauses the transport - so a threshold any tighter turns that
// jitter into random dropouts in playback (found on the bench: a 0.3-degree
// threshold, measured from wherever the last turn left off, triggered
// constantly). The degree that starts a scrub still moves the cursor, so
// nothing is lost; at one second per turn it's about 3ms of audio.
const float SCRUB_START_DEGREES = 1.0f;

// Once scrubbing, the cursor follows the knob through a much smaller
// backlash, like the slack in a gear: it only moves once the knob has moved
// this far past where it last turned round. That keeps the jitter of a
// hand resting on the knob from nudging the cursor back and forth, heard
// as "crumbs" of audio. Unlike a deadband on speed it loses nothing over a
// long turn: four turns still moves the cursor four turns' worth, less one
// backlash at each change of direction.
const float SCRUB_BACKLASH_DEGREES = 0.3f;

// How long the knob has to sit still before scrubbing hands the cursor
// back to the transport. A real scrub pauses briefly at every change of
// direction, so too short and playback lurches forward at each reversal;
// too long and it's slow to resume. A starting point to tune by feel.
const unsigned long SCRUB_RELEASE_MS = 150;

// The cursor chases the knob with this time constant, in output ticks
// (960 = 20ms) - the same smoothing the speed-based scrub this replaced
// used. The knob's position arrives 100 times a second, in steps, and
// following it any tighter is heard as a buzz at that rate.
const float SCRUB_FOLLOW_TICKS = 960.0f;

float scrubFramesPerDegree = 48000.0f / 360.0f;  // set per file by fitScrubToFile()
bool scrubbing = false;             // whether the scrub knob owns the cursor right now
float scrubPhysicalDegrees = 0.0f;  // pot2's position, accumulated across turns
float scrubKnobDegrees = 0.0f;      // the same, after the backlash
float scrubRestDegrees = 0.0f;      // where pot2 last came to rest (physical)
float scrubAnchorDegrees = 0.0f;    // scrubKnobDegrees when it did
double scrubAnchorFrames = 0.0;     // where the cursor was when a scrub started
float scrubPrevAngle = -1.0f;       // pot2's last reading, or -1 if it had none
unsigned long scrubLastMoveMillis = 0;

// Set to 1 to print when each scrub starts and ends - handy for tuning
// SCRUB_BACKLASH_DEGREES (a resting knob should never start one) and
// SCRUB_RELEASE_MS.
#define SCRUB_TRACE 0

// Fits the scrub to a newly loaded file - see SCRUB_SECONDS_PER_TURN.
void fitScrubToFile() {
  float defaultFrames = SCRUB_SECONDS_PER_TURN * nativeRate;
  int k = (int)lroundf(log2f((float)fileFrames / defaultFrames));
  float framesPerTurn = ldexpf((float)fileFrames, -k);
  scrubFramesPerDegree = framesPerTurn / 360.0f;

  Serial.print("Scrub: one turn = ");
  if (k >= 0) {
    Serial.print("1/");
    Serial.print(1L << k);
    Serial.print(" of the file");
  } else {
    Serial.print(1L << -k);
    Serial.print(" times through it");
  }
  Serial.print(" (");
  Serial.print(framesPerTurn / nativeRate, 2);
  Serial.println("s of audio).");
}

// The playback cursor, in frames of the WAV, within the window. The voice's
// own position is in its copy's units, so it's scaled back up.
double cursorFrames() {
  noInterrupts();
  Voice &v = voices[activeVoice];
  int32_t index = v.resampler.index;
  float frac = v.resampler.frac;
  uint8_t level = v.level;
  interrupts();
  return wrapToWindow(ldexp((double)fileOffsetFor(v, index) + frac, level));
}

// Hands the cursor back to the transport - also used when something else
// moves the cursor (a rewind, a new file), so a scrub in progress doesn't
// drag it back to where the knob had it. The knob's position now becomes
// its resting point, which the next scrub has to move clearly away from.
void endScrub() {
#if SCRUB_TRACE
  if (scrubbing) Serial.println("Scrub: released");
#endif
  scrubbing = false;
  scrubRestDegrees = scrubPhysicalDegrees;
  scrubAnchorDegrees = scrubKnobDegrees;
}

// Reads knob2. Returns false until it's calibrated; after that, `turned`
// is how far it has turned since the last call, in degrees after the
// backlash - 0 at rest. It also keeps the positions the scrub works from
// (scrubPhysicalDegrees, scrubKnobDegrees), whichever mode is using the knob.
bool readKnob2(float &turned) {
  turned = 0.0f;
  float angle = pot2.read();
  if (angle < 0.0f) {
    scrubPrevAngle = -1.0f;
    if (scrubbing) endScrub();
    return false;
  }

  if (scrubPrevAngle < 0.0f) {
    // First reading (at boot, or after RECAL): wherever the knob is, it's
    // at rest there.
    scrubPrevAngle = angle;
    endScrub();
    return true;
  }

  // The short way round, across the 0/360 seam.
  float delta = angle - scrubPrevAngle;
  if (delta > 180.0f) delta -= 360.0f;
  if (delta < -180.0f) delta += 360.0f;
  scrubPrevAngle = angle;
  scrubPhysicalDegrees += delta;

  float slack = scrubPhysicalDegrees - scrubKnobDegrees;
  float moved = 0.0f;
  if (slack > SCRUB_BACKLASH_DEGREES) {
    moved = slack - SCRUB_BACKLASH_DEGREES;
  } else if (slack < -SCRUB_BACKLASH_DEGREES) {
    moved = slack + SCRUB_BACKLASH_DEGREES;
  }
  scrubKnobDegrees += moved;
  turned = moved;
  return true;
}

// The step that moves the cursor toward where the scrub knob has put it,
// for loop() to use while `scrubbing` is true.
float scrubStep = 0.0f;

// Knob2 in speed mode: tape scrub. Called every control tick, turned or
// not, since it's the knob coming to rest that ends a scrub. While the knob
// owns the cursor, `scrubbing` is true and scrubStep is the step to take.
//
// The knob sets a target position in the file and the cursor is steered
// onto it, rather than the knob setting a speed: steering corrects for
// anything that holds the cursor back (the speed cap while copies are
// built, a moment's lag in a copy change), so the cursor always ends up
// exactly where the knob says - N turns covers the same stretch of the
// file every time. That's also why this works from the knob's accumulated
// position (see readKnob2()) rather than from `turned`.
void scrubKnob(float turned) {
  if (!fileLoaded) {
    if (scrubbing) endScrub();
    return;
  }

  // Movement is measured in clear steps from a resting point that's moved
  // up each time, so the release timer below only restarts for real motion
  // and not for jitter.
  unsigned long now = millis();
  if (fabsf(scrubPhysicalDegrees - scrubRestDegrees) > SCRUB_START_DEGREES) {
    scrubRestDegrees = scrubPhysicalDegrees;
    scrubLastMoveMillis = now;
    if (!scrubbing) {
      // A hand on the reel: start from wherever the tape is now.
      scrubbing = true;
      scrubAnchorFrames = cursorFrames();
#if SCRUB_TRACE
      Serial.println("Scrub: started");
#endif
    }
  }
  if (!scrubbing) return;

  if (now - scrubLastMoveMillis >= SCRUB_RELEASE_MS) {
    endScrub();
    return;
  }

  // Where the knob has moved the tape to since the scrub started. Negated:
  // confirmed live the un-reversed mapping scrubbed opposite the intuitive
  // direction, same as knob1 originally did.
  double scrubTarget = wrapToWindow(
      scrubAnchorFrames - (scrubKnobDegrees - scrubAnchorDegrees) * scrubFramesPerDegree);

  // The short way round to the target, in case it lies across the loop
  // point from the cursor.
  double lap = windowEnd - windowStart;
  double error = scrubTarget - cursorFrames();
  if (error > lap / 2.0) error -= lap;
  if (error < -(lap / 2.0)) error += lap;
  scrubStep = (float)error / SCRUB_FOLLOW_TICKS;
}

// -- Knob modes -------------------------------------------------------------
// The two knobs do different jobs depending on the mode, and touch1 steps
// through the modes. Each mode is one row of MODES, below: a name, the
// colour the RGB LED shows while it's selected, and a function for each
// knob. Every control tick, loop() reads both knobs and hands however far
// each has turned to the current mode's function for it - so adding a mode
// means writing its two knob functions and adding a row.
//
// A knob function is called every tick its knob is calibrated, whether it
// moved or not (`turned` is 0 at rest), because some need that: the speed
// dial anchors itself on its first call, and the scrub ends when its knob
// stops. A mode that leaves a knob unused has nullptr there.

// Set by RECAL: the next real knob1 reading becomes the new baseline
// instead of being diffed against the last one from before - otherwise
// the knob's position when it finishes recalibrating would register as
// one sudden turn, and jump the speed.
bool knob1NeedsBaseline = true;  // and at boot, before knob1's first reading

// Reads knob1. Returns false until it's calibrated; after that, `turned`
// is how far it has turned since the last call, in degrees after a small
// backlash - 0 at rest.
bool readKnob1(float &turned) {
  turned = 0.0f;
  float physicalNow = knob1.read();
  if (physicalNow == -1.0f) return false;

  if (knob1NeedsBaseline) {
    knob1Degrees = physicalNow;
    lastPhysicalKnobDegrees = physicalNow;
    knob1NeedsBaseline = false;
  }

  // A small backlash, as on the scrub knob (see SCRUB_BACKLASH_DEGREES):
  // the knob's reading jitters by a fraction of a degree at rest, which
  // made the speed flicker between 1.00x and 0.9999x on the web page.
  float slack = physicalNow - knob1Degrees;
  if (slack > KNOB1_BACKLASH_DEGREES) {
    knob1Degrees = physicalNow - KNOB1_BACKLASH_DEGREES;
  } else if (slack < -KNOB1_BACKLASH_DEGREES) {
    knob1Degrees = physicalNow + KNOB1_BACKLASH_DEGREES;
  }

  // Measured from the last tick's position whatever mode that tick was in,
  // which is what makes a mode switch seamless instead of jumpy.
  turned = knob1Degrees - lastPhysicalKnobDegrees;
  lastPhysicalKnobDegrees = knob1Degrees;
  return true;
}

// Knob1 in pitch mode: moves the pitch you hear - relative, see
// PITCH_OCTAVES_PER_TURN. Turning the knob up lowers the pitch, the same
// sense as the speed dial.
void turnPitch(float turned) {
  if (turned == 0.0f) return;
  float octaves = log2f(heldPitch) - turned / 360.0f * PITCH_OCTAVES_PER_TURN;
  octaves = constrain(octaves, -PITCH_OCTAVE_LIMIT, PITCH_OCTAVE_LIMIT);
  heldPitch = exp2f(octaves);
  holdPitch();
}

// Knob2 in pitch mode: time-stretch. Turns the same speed dial knob1 does
// in speed mode, with the pitch you hear held where knob1 set it.
void stretchTime(float turned) {
  if (turned == 0.0f) return;
  turnSpeedDial(turned);
  holdPitch();
}

// Knob1 in window mode: moves the start of the window (see "The window"),
// at the scrub knob's rate and in its sense - a turn covers the same stretch
// of file either way. Pushed up against the end, it moves that too.
void turnWindowStart(float turned) {
  if (turned == 0.0f || !fileLoaded) return;
  double minimum = windowMinFrames();
  double start = constrain(windowStart - turned * scrubFramesPerDegree, 0.0, fileFrames - minimum);
  setWindow(start, fmax(windowEnd, start + minimum));
}

// Knob2 in window mode: moves the end, and pushes the start the same way.
void turnWindowEnd(float turned) {
  if (turned == 0.0f || !fileLoaded) return;
  double minimum = windowMinFrames();
  double end = constrain(windowEnd - turned * scrubFramesPerDegree, minimum, (double)fileFrames);
  setWindow(fmin(windowStart, end - minimum), end);
}

// Knob1 in echo mode: the delay time - relative, see ECHO_OCTAVES_PER_TURN.
// Forward lengthens it. The echoes bend in pitch while it turns, like a
// tape echo's - see EchoDelay.h.
void turnEchoTime(float turned) {
  if (turned == 0.0f) return;
  float octaves = log2f(echoSeconds) - turned / 360.0f * ECHO_OCTAVES_PER_TURN;
  setEcho(exp2f(octaves), echoAmount);
}

// Knob2 in echo mode: how much echo - its level and its feedback together.
// Forward is more; it stops at none and at ECHO_MAX_AMOUNT.
void turnEchoAmount(float turned) {
  if (turned == 0.0f) return;
  setEcho(echoSeconds, echoAmount - turned / 360.0f * ECHO_MAX_AMOUNT / ECHO_AMOUNT_TURNS);
}

// Knob1 in filter mode: the band's width. Wide open is the top of a hill:
// turning either way from it narrows the band, down to nothing - and the
// filter screaming - FILTER_WIDTH_TURNS later. Turned back, it stops at wide
// open, and turning on from there narrows it again, on the other side.
//
// The knob's distance d from closed (0 to 1) sets the width as d squared:
// fast at first, so the first quarter turn takes it from ten octaves to
// about seven - 60Hz to 7kHz around the middle of the range, plain to hear
// - and slower as it closes in, where the scream is played.
void turnFilterWidth(float turned) {
  if (turned == 0.0f) return;
  float fromOpen = filterWidthSide * (1.0f - sqrtf(filterWidthOctaves / FILTER_OPEN_OCTAVES));
  float next = constrain(fromOpen - turned / 360.0f / FILTER_WIDTH_TURNS, -1.0f, 1.0f);
  if (fromOpen != 0.0f && (next > 0.0f) != (fromOpen > 0.0f)) next = 0.0f;  // stop at open
  if (next != 0.0f) filterWidthSide = next > 0.0f ? 1 : -1;
  float d = 1.0f - fabsf(next);
  setFilterBand(filterCentreHz, FILTER_OPEN_OCTAVES * d * d);
}

// Knob2 in filter mode: the band's centre - relative, on the curve of
// filterCentreOctaves(). Forward raises it, sliding both edges of the band
// along; the width stays as it is. With the band narrowed to a scream,
// this plays its pitch; turned far enough either way, the band slides out
// of hearing and the sound fades to silence.
void turnFilterCentre(float turned) {
  if (turned == 0.0f) return;
  float turns = filterCentreTurns(log2f(filterCentreHz / FILTER_CENTRE_HZ)) - turned / 360.0f;
  turns = constrain(turns, -FILTER_CENTRE_REACH_TURNS, FILTER_CENTRE_REACH_TURNS);
  setFilterBand(FILTER_CENTRE_HZ * exp2f(filterCentreOctaves(turns)), filterWidthOctaves);
}

struct KnobMode {
  const char *name;             // printed to Serial when the mode is selected
  float hue;                    // the RGB LED's colour, in degrees round the colour
                                // wheel (0 red, 120 green, 240 blue) - see modeColour()
  uint8_t brightness;           // and its brightness, as a percentage of all it can do
  bool holdsPitch;              // speed changes leave the pitch you hear alone
                                // (time-stretch) rather than moving it, tape-style
  void (*enter)();              // called when the mode is selected, or nullptr
  void (*knob1)(float turned);  // degrees knob1 turned this tick, or nullptr
  void (*knob2)(float turned);  // the same for knob2
};

// The hues stay clear of red and blue, which the board's other LEDs already
// use. Most are at RGBLED_BRIGHTNESS; green and aqua are turned down
// to look about as bright as the rest - the eye is most sensitive to green,
// and aqua has two of the LED's three colours lit. One more colour was
// picked to go with these, for a mode still to come: yellow (60, at 6).
const KnobMode MODES[] = {
  // name, hue, brightness, holdsPitch, enter, knob1, knob2
  {"speed / scrub", 120.0f, 7, false, nullptr, turnSpeedDial, scrubKnob},             // green
  {"pitch / time-stretch", 180.0f, 6, true, syncHeldPitch, turnPitch, stretchTime},  // aqua
  {"window start / end", 268.0f, RGBLED_BRIGHTNESS, false, printWindow, turnWindowStart, turnWindowEnd},  // purple
  {"echo time / amount", 335.0f, RGBLED_BRIGHTNESS, false, printEcho, turnEchoTime, turnEchoAmount},  // pink
  {"filter width / centre", 30.0f, RGBLED_BRIGHTNESS, false, printFilter, turnFilterWidth, turnFilterCentre},  // orange
};
const uint8_t MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

bool modeHoldsPitch() {
  return MODES[knobMode].holdsPitch;
}

// A fully saturated colour at `hueDegrees` round the colour wheel, at
// `percent` of the LED's full brightness - the standard HSV conversion: the wheel is six 60-degree
// sectors, and across each one a single channel ramps up or down while the
// other two sit at full and at zero.
CRGB modeColour(float hueDegrees, uint8_t percent) {
  float sector = fmodf(hueDegrees, 360.0f) / 60.0f;
  uint8_t full = rgbLevel(percent);
  uint8_t rising = (uint8_t)lroundf(full * (sector - floorf(sector)));
  uint8_t falling = full - rising;
  switch ((int)sector) {
    case 0:  return CRGB(full, rising, 0);   // red to yellow
    case 1:  return CRGB(falling, full, 0);  // yellow to green
    case 2:  return CRGB(0, full, rising);   // green to cyan
    case 3:  return CRGB(0, falling, full);  // cyan to blue
    case 4:  return CRGB(rising, 0, full);   // blue to magenta
    default: return CRGB(full, 0, falling);  // magenta to red
  }
}

// -- Selecting with the knobs ---------------------------------------------------
// While touch1 is held on its own, the knobs select instead of doing their
// jobs in the current mode: knob1 picks the mode and knob2 picks the file.
// Each steps on, or back, every so many degrees it turns, round and round,
// so any mode or file is one gesture away. The knobs go back to their jobs,
// in whichever mode that leaves, when the pad is released.
//
// Touching the pad and letting go without selecting anything is a tap,
// which steps to the next mode.
const float MODE_SELECT_DEGREES = 60.0f;
const float FILE_SELECT_DEGREES = 30.0f;
float modeSelectDegrees = 0.0f;  // how far each knob has turned toward its next step
float fileSelectDegrees = 0.0f;
bool wasSelecting = false;
bool modeTapPending = false;     // touch1 is down, and so far it could be a tap

// True while a file picked with knob2 is playing for as long as touch1
// stays held - an audition. It runs the transport the same way a finger on
// touch2 does, so if the transport was already latched on, letting go
// changes nothing and the new file plays on.
bool auditioning = false;

// Adds a knob's movement to `degrees`, and takes out and returns however
// many whole steps of `perStep` that makes - negative for backward.
int selectorSteps(float &degrees, float turned, float perStep) {
  // Negated, like the scrub: the knob's readings run opposite to the way
  // that feels like "forward".
  degrees -= turned;
  int steps = (int)(degrees / perStep);  // rounds toward zero
  degrees -= steps * perStep;
  return steps;
}

void turnModeSelector(float turned) {
  int steps = selectorSteps(modeSelectDegrees, turned, MODE_SELECT_DEGREES);
  if (steps == 0) return;
  modeTapPending = false;
  selectMode((knobMode + MODE_COUNT + steps % MODE_COUNT) % MODE_COUNT);
}

// The WAV `steps` places on from the current file (back, if negative) in
// the list's order - the order LIST gives, see WavList.h - going round at
// the ends. Empty if the card has none.
String wavAtOffset(int steps) {
  std::vector<String> names = sortedWavNames();
  int count = names.size();
  if (count == 0) return "";
  int current = 0;
  for (int i = 0; i < count; i++) {
    if (isCurrentFile(names[i])) current = i;
  }
  return names[((current + steps) % count + count) % count];
}

// Knob2 while selecting: steps through the card's WAVs. Each one it lands
// on is cued and plays from its start for as long as the pad stays held -
// an audition. Only turning does that: holding the pad plays nothing by
// itself.
//
// A fast turn can cover several steps in one tick, and they're taken in one
// go - loading a file takes tens of milliseconds, far too long to load
// each one passed on the way.
void turnFileSelector(float turned) {
  int steps = selectorSteps(fileSelectDegrees, turned, FILE_SELECT_DEGREES);
  if (steps == 0 || uploadInProgress()) return;
  modeTapPending = false;
  int direction = (steps > 0) ? 1 : -1;
  // A file that won't play is stepped over, a few times at most.
  for (int tries = 0; tries < 8; tries++, steps += direction) {
    String name = wavAtOffset(steps);
    if (name.length() == 0 || isCurrentFile(name)) return;
    String err;
    if (loadFile(name, err)) {
      auditioning = true;
      blePrintLine("PLAYING:" + name);
      return;
    }
    Serial.print("  skipping ");
    Serial.print(name);
    Serial.print(" - ");
    Serial.println(err);
  }
}

// Switches to a knob mode. A scrub in progress ends, since its knob may be
// about to mean something else.
void selectMode(uint8_t index) {
  knobMode = index % MODE_COUNT;
  endScrub();
  if (MODES[knobMode].enter) MODES[knobMode].enter();
  Serial.print("Knob mode ");
  Serial.print(knobMode + 1);
  Serial.print(" of ");
  Serial.print(MODE_COUNT);
  Serial.print(": ");
  Serial.println(MODES[knobMode].name);
}

// MODE:<n>, from the web page. Nothing happens for the mode it's already
// in, so a repeated command doesn't end a scrub or reprint the mode.
void selectModeFromCommand(int index) {
  if (index >= 0 && index < MODE_COUNT && index != knobMode) selectMode(index);
}

// -- Knob calibration -----------------------------------------------------
// RV112FF learns each pot's range by watching it turn, and reports nothing
// until it has seen all of it - about two full turns per pot. Rather than
// ask for that at every power-on, this sketch saves each pot's calibration
// to the Data Flash (through the EEPROM library) and restores it at boot.
//
// A pot is saved only if nothing valid was found for it at boot, the first
// time the transport stops after its calibration completes - so in
// practice each pot is saved once, ever. That matters because the Data
// Flash doesn't spread writes around to limit wear. Stopping is chosen as
// a quiet moment: the write pauses loop() briefly, and with the tape
// stopped there's little going on for that to disturb.
//
// RECAL (over BLE or the USB serial monitor) forgets both calibrations and
// starts learning again - after swapping a pot, say, or to test this.
const int CAL_EEPROM_ADDR[2] = {0, sizeof(RV112FFCalibration)};
RV112FF *const calPots[2] = {&pot1, &pot2};
bool calStored[2] = {false, false};

void loadKnobCalibration() {
  for (int i = 0; i < 2; i++) {
    RV112FFCalibration cal;
    EEPROM.get(CAL_EEPROM_ADDR[i], cal);
    // setCalibration() checks the data itself and refuses anything it
    // can't trust: never-written flash, a partial write, an old format.
    calStored[i] = calPots[i]->setCalibration(cal) && calPots[i]->isCalibrated();
    Serial.print("Knob ");
    Serial.print(i + 1);
    Serial.println(calStored[i]
        ? ": calibration loaded."
        : ": no saved calibration - turn it about two full turns to calibrate.");
  }
}

// Saves any pot that has finished calibrating and has nothing stored yet.
void saveNewKnobCalibration() {
  for (int i = 0; i < 2; i++) {
    if (calStored[i] || !calPots[i]->isCalibrated()) continue;
    RV112FFCalibration cal = calPots[i]->getCalibration();
    unsigned long start = micros();
    EEPROM.put(CAL_EEPROM_ADDR[i], cal);
    calStored[i] = true;
    // The wiper ranges are fractions of full scale - handy for spotting a
    // pot whose range looks short.
    Serial.print("Knob ");
    Serial.print(i + 1);
    Serial.print(": calibration saved in ");
    Serial.print(micros() - start);
    Serial.print("us - wiper ranges ");
    printWiperRanges(cal);
    Serial.println();
  }
}

// Prints one calibration's two wiper ranges, as fractions of full scale.
void printWiperRanges(const RV112FFCalibration &cal) {
  Serial.print(cal.minVal[0], 3);
  Serial.print("-");
  Serial.print(cal.maxVal[0], 3);
  Serial.print(", ");
  Serial.print(cal.minVal[1], 3);
  Serial.print("-");
  Serial.print(cal.maxVal[1], 3);
}

// The CAL command: what's saved for each knob, and what it's using now.
// The live ranges can be a little wider than the saved ones, since the
// library keeps widening them if a wiper ever reads past them.
void printKnobCalibration() {
  for (int i = 0; i < 2; i++) {
    RV112FFCalibration saved;
    EEPROM.get(CAL_EEPROM_ADDR[i], saved);
    // A throwaway RV112FF is the simplest way to apply the library's own
    // checks to what's stored, without disturbing the real pot. Its
    // constructor doesn't touch any hardware.
    RV112FF checker(A2, A3);
    bool savedOk = checker.setCalibration(saved) && checker.isCalibrated();

    Serial.print("Knob ");
    Serial.print(i + 1);
    Serial.print(calPots[i]->isCalibrated() ? ": calibrated" : ": still calibrating");
    Serial.print(". Saved: ");
    if (savedOk) {
      printWiperRanges(saved);
    } else {
      Serial.print("nothing");
    }
    Serial.print(". Live: ");
    printWiperRanges(calPots[i]->getCalibration());
    Serial.println(".");
  }
}

// -- Per-file settings -------------------------------------------------------
// Each WAV keeps its own speed, pitch, window, echo and filter on the card (see FileSettings.h):
// picking a file again, or rebooting, brings them back. They're saved a
// couple of seconds after the last change rather than on every tick of a
// knob - an SD write can pause loop() for tens of milliseconds - and at
// once when the file is switched away from.
const unsigned long SETTINGS_SAVE_DELAY_MS = 2000;
FileSettings savedSettings;  // what's on the card for the current file
FileSettings seenSettings;   // the settings as of the last change
unsigned long settingsChangedMillis = 0;

FileSettings currentSettings() {
  FileSettings s;
  s.speed = lastKnownRatio;
  s.pitch = pitchRatio;
  s.windowStart = (uint32_t)windowStart;
  s.windowEnd = (windowEnd >= fileFrames) ? 0 : (uint32_t)windowEnd;  // 0: the file's end
  s.echoTime = echoSeconds;
  s.echoAmount = echoAmount;
  s.filterCentre = filterCentreHz;
  s.filterWidth = filterWidthOctaves;
  return s;
}

// Makes a file's settings the live ones. Both knobs carry on from them the
// next time they're turned, the way they do after SPEED: and PITCH:.
void applySettings(const FileSettings &s) {
  lastKnownRatio = copysignf(fminf(fabsf(s.speed), MAX_RATIO), s.speed);
  speedAnchorRatio = lastKnownRatio;
  speedKnobAnchored = false;
  setPitchRatio(s.pitch);
  // Only the numbers: this is for a file that's being loaded, whose voice
  // loadFile() sets up from them. (setWindow() is the one for a file
  // that's playing.)
  windowStart = s.windowStart;
  windowEnd = s.windowEnd ? (double)s.windowEnd : (double)fileFrames;
  clampWindow(windowStart, windowEnd);
  setEcho(s.echoTime, s.echoAmount);
  setFilterBand(s.filterCentre, s.filterWidth);
  savedSettings = seenSettings = currentSettings();
}

// Writes the current file's settings now, if they've changed since the
// last save. Called before the file goes away, and by trackSettings().
void flushSettings() {
  if (!fileLoaded) return;
  FileSettings now = currentSettings();
  if (now == savedSettings) return;
  if (now.save(currentFilename)) {
    savedSettings = seenSettings = now;
    Serial.print("Settings saved for ");
    Serial.println(currentFilename);
  }
}

// Called each control tick: saves once the settings have sat unchanged
// for SETTINGS_SAVE_DELAY_MS. Not during an upload, whose flow control
// needs loop() to keep answering promptly - it just waits until after.
void trackSettings() {
  FileSettings now = currentSettings();
  if (now != seenSettings) {
    seenSettings = now;
    settingsChangedMillis = millis();
  } else if (now != savedSettings && !uploadInProgress() &&
             millis() - settingsChangedMillis >= SETTINGS_SAVE_DELAY_MS) {
    flushSettings();
  }
}

void forgetKnobCalibration() {
  RV112FFCalibration blank = {};  // magic 0, which setCalibration() refuses
  for (int i = 0; i < 2; i++) {
    EEPROM.put(CAL_EEPROM_ADDR[i], blank);
    calPots[i]->resetCalibration();
    calStored[i] = false;
  }
  knob1NeedsBaseline = true;
  Serial.println("Knob calibration forgotten - turn each knob about two full turns to calibrate again.");
}

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

// -- USB disk mode -------------------------------------------------------------
// DISK:ON hands the card to the computer at the other end of the USB cable,
// where it appears as a drive; ejecting it there (or DISK:OFF) hands it
// back. See "USB disk mode" in the header comment, and UsbDiskGate.h for why
// the two can never share it.

// Whether the computer has gone - the cable pulled out. USB has no "unplugged"
// signal this sketch can use: the core's driver never passes one to TinyUSB,
// and the RA6M5's VBUS sense (P407) stayed high with the cable out on
// battery, measured. What does change, within a fraction of a second, is
// that the bus goes quiet and TinyUSB reports it suspended - which is
// also what a computer going to sleep looks like, so a sleeping computer
// counts as gone too. That's safe for the card: once the gate closes,
// nothing the computer does can reach it, and when the computer wakes its
// requests fail and it drops the disk (complaining it wasn't ejected).
//
// Serial needs nothing extra: the core's Serial counts a suspended bus as
// nobody listening, and drops what's printed rather than waiting for room.
// (Telling TinyUSB the cable was unplugged, by raising the event and
// running tud_task() with interrupts held off, was tried - and hung the
// board on the spot.)
const unsigned long USB_GONE_MS = 2000;  // longer than a passing glitch
unsigned long usbSeenMillis = 0;
bool usbGone = false;

// Called every pass of loop() to keep usbGone up to date.
void watchUsbHost() {
  unsigned long now = millis();
  if (tud_mounted() && !tud_suspended()) {
    usbSeenMillis = now;
    usbGone = false;
  } else if (now - usbSeenMillis >= USB_GONE_MS) {
    usbGone = true;
  }
}

// DISK:ON. Stops everything that uses the card, lets go of it, and opens the
// gate - the computer notices within a second or two and mounts it.
void enterUsbDiskMode() {
  if (usbDiskMode) {
    blePrintLine("DISKMODE:ON");
    return;
  }
  // Without a computer, nobody could ever eject it - only DISK:OFF would
  // bring the board back.
  if (!tud_mounted() || tud_suspended()) {
    blePrintLine("ERR:DISK:no computer on the USB port");
    return;
  }
  if (uploadInProgress()) {
    blePrintLine("ERR:DISK:an upload is in progress");
    return;
  }

  abortFileList();
  transportLatched = false;
  unloadFile();  // saves the file's settings and stops building its copies
  // Silence the echo and the filter's scream: loop() stops looking after
  // their fade-out while in this mode.
  echo.setFade(0.0f);
  echo.setDamped(true);
  filter.setLevel(0.0f);

  if (sdFs.unmount() != 0) {
    Serial.println("USB disk: couldn't unmount the card - staying put.");
    blePrintLine("ERR:DISK:couldn't let go of the card");
    recueAfterLosingFile();
    return;
  }
  // Unmounting shut the card down; start it up again here, in loop(), since
  // the gate won't - see UsbDiskGate.h.
  if (sdBlockDevice.init() != 0) {
    Serial.println("USB disk: couldn't start the card up again.");
    blePrintLine("ERR:DISK:couldn't start the card up");
    sdFs.mount(&sdBlockDevice);
    recueAfterLosingFile();
    return;
  }
  usbDiskEjected = false;
  usbDiskGate.setOpen(true);
  usbDiskMode = true;
  Serial.println("USB disk mode: the card is on the computer now. Eject it there to come back.");
  blePrintLine("DISKMODE:ON");
}

// The computer ejected the disk: Finder's eject button, or the equivalent.
// Called by TinyUSB, inside the USB interrupt, so it only raises a flag for
// loop(). Overrides TinyUSB's weak default.
extern "C" bool tud_msc_start_stop_cb(uint8_t lun, uint8_t powerCondition, bool start,
                                      bool loadEject) {
  (void)lun;
  (void)powerCondition;
  if (loadEject && !start) usbDiskEjected = true;
  return true;
}

// After an eject, the cable coming out, or DISK:OFF: closes the gate, mounts the card again and
// picks up whatever changed on it - files deleted or renamed on the computer
// leave cached copies and settings behind, cleared here just as at boot.
void leaveUsbDiskMode() {
  if (!usbDiskMode) {
    blePrintLine("DISKMODE:OFF");
    return;
  }
  usbDiskGate.setOpen(false);
  usbDiskMode = false;
  usbDiskEjected = false;
  if (sdFs.mount(&sdBlockDevice) != 0) {
    Serial.println("USB disk: couldn't mount the card again - is it still FAT32?");
    blePrintLine("ERR:DISK:couldn't mount the card again");
    return;
  }
  MipCache::removeOrphans();
  FileSettings::removeOrphans();
  cueRememberedFile();
  // The knobs went unread meanwhile: whatever they turned through then
  // shouldn't arrive all at once, as one big turn.
  knob1NeedsBaseline = true;
  scrubPrevAngle = -1.0f;
  Serial.println("USB disk mode over: the card is back.");
  blePrintLine("DISKMODE:OFF");
}

// loop()'s whole job while in USB disk mode: notice the eject (or the
// computer going - see watchUsbHost() - after which nobody is left to eject
// it, and the card is in the same state as any USB stick pulled out without
// ejecting), light the LED
// a dim white, and tell the web page - in place of the usual status line,
// and doubling as the Binary mode probe it normally is (see the status
// block in loop()).
void serviceUsbDiskMode() {
  unsigned long now = millis();
  if (usbDiskEjected || usbGone) {
    leaveUsbDiskMode();
    return;
  }

  uint8_t white = (uint8_t)(255u * RGBLED_BRIGHTNESS / 100u / 2u);
  CRGB wanted(white, white, white);
  if (wanted != leds[0]) {
    leds[0] = wanted;
    FastLED.show();
  }
  digitalWrite(LED_BUILTIN, LOW);

  if (now - previousStatusMillis < STATUS_INTERVAL) return;
  previousStatusMillis = now;
  bool probing = (linkMode == LINK_PENDING) &&
                 (now - probeSentMillis >= BINARY_PROBE_INTERVAL);
  if (linkMode == LINK_BINARY || probing) {
    if (probing) {
      probeSentMillis = now;
      awaitingProbeEcho = true;
    }
    blePrintLine("DISKMODE:ON");
  }
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

  loadKnobCalibration();

  for (uint16_t i = 0; i < HANN_LUT_SIZE; i++) {
    hannLut[i] = 0.5f * (1.0f - cosf(2.0f * PI * (float)i / (float)HANN_LUT_SIZE));
  }
  initSincTable();
  initSpeedCurve();
  echo.begin();
  setEcho(echoSeconds, echoAmount);
  filter.begin();
  setFilterBand(filterCentreHz, filterWidthOctaves);

  // ---- RA6M5-specific ----
  // Start the Cortex-M33's cycle counter, which the audio interrupt uses to
  // time itself - see "Audio interrupt timing" above.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cyclesPerTick = SystemCoreClock / 48000;
  // ---- end RA6M5-specific ----

  FastLED.addLeds<WS2812B, RGB_LED_PIN, GRB>(leds, NUM_LEDS);
  leds[0] = CRGB::Black;  // loop() lights it in the mode's colour
  FastLED.show();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  if (sdFs.mount(&sdBlockDevice) != 0) {
    Serial.println("SD card mount failed - check it's inserted and FAT32-formatted.");
    while (1) {}
  }

  // Clear out cached copies of files that are gone - deleted or renamed
  // from a computer, say, rather than through the web page.
  MipCache::removeOrphans();
  FileSettings::removeOrphans();

  // Cue the first file the demo can actually play, so the board is usable
  // standing on its own - hold the transport pad and it makes a sound, with
  // no phone, no browser and no BLE connection needed. Skipping files that
  // fail to load matters on a real card: a sample library is full of stereo
  // and compressed files this demo cannot open, and
  // stopping at the first *name* rather than the first playable file would
  // leave it silent for no visible reason.
  //
  // This does not reintroduce the SD activity the demo used to avoid: with
  // the transport stopped, step is 0 and loop()'s refill takes its explicit
  // paused branch, so nothing touches the card until playback actually
  // starts. And it all happens long before any BLE connection exists.
  cueRememberedFile();

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
  // with nothing loaded yet - audioCallback() just holds lastGoodLeft/Right
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

  watchUsbHost();

  // In USB disk mode the card belongs to the computer, so nothing below -
  // playback, the knobs, saving settings - can run. See "USB disk mode".
  if (usbDiskMode) {
    serviceUsbDiskMode();
    return;
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
    repairStrandedCursor(voices[activeVoice], step);
    refillVoice(voices[activeVoice], step);
    stageDone(STAGE_REFILL, t);

    // Build cached copies in the gaps: only when the playing voices are
    // topped up, and never during an upload, whose flow control needs
    // loop() to keep answering promptly. Each step is a few milliseconds.
    t = DWT->CYCCNT;
    if (cache.building() && !uploadInProgress() && voiceToppedUp(step)) cache.step();
    stageDone(STAGE_CACHE, t);
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    float controlSeconds = (currentMillis - previousControlMillis) / 1000.0f;  // usually CONTROL_INTERVAL
    previousControlMillis = currentMillis;
    t = DWT->CYCCNT;
    CapTouch::scan();
    stageDone(STAGE_TOUCH, t);

    t = DWT->CYCCNT;
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
    // Scrub works whether the transport runs or not: a stopped tape still
    // moves when you turn the reel by hand.
    // Transport, with a latch.
    //
    //   touch2 held                -> running, for as long as it is held
    //   touch1 pressed while held  -> latched on; running survives release
    //   touch2 pressed, touch1 held -> the same, the other way round
    //   touch2 pressed again       -> latch cleared
    //
    // Clearing the latch never interrupts playback: the press that clears it
    // is also holding the transport up by itself, so running only ends when
    // that finger lifts. Both triggers are edge-detected so they fire once
    // per press rather than continuously while held.
    bool touch1Now = touch1.isTouched();
    bool touch2Now = touch2.isTouched();

    // Three taps of both pads together reset the file - see "Triple tap".
    watchForTripleTap(touch1Now, touch2Now, currentMillis);

    // touch1 pressed while touch2 is down latches the transport. On its own
    // it's the selector: tapped, it steps to the next knob mode; held, it
    // turns the knobs into selectors - see "Selecting with the knobs". A
    // tap is a press and release with no selecting in between, so the step
    // comes on the release.
    if (touch1Now && !touch1WasTouched) {
      if (touch2Now) {
        transportLatched = true;
      } else {
        modeTapPending = true;
      }
    }
    if (touch2Now) modeTapPending = false;
    if (!touch1Now) {
      if (touch1WasTouched && modeTapPending) selectMode(knobMode + 1);
      modeTapPending = false;
    }

    // Selecting: while touch1 is held on its own. A file picked then plays
    // only until it's released (see turnFileSelector()).
    bool selecting = touch1Now && !touch2Now;
    if (!selecting) auditioning = false;

    // touch2 pressed while touch1 is down latches too - the chord works in
    // either order. Pressed on its own, it clears the latch.
    if (touch2Now && !touch2WasTouched) transportLatched = touch1Now;

    touch1WasTouched = touch1Now;
    touch2WasTouched = touch2Now;

    bool nowRunning = touch2Now || transportLatched || auditioning;

    // Rewind on the rising edge of *running*, not of the pad, so engaging or
    // releasing the latch mid-playback does not jump back to the top.
    if (nowRunning && !transportRunning && fileLoaded) rewindToStart();
    // And a stop is when newly learned knob calibrations get saved.
    if (!nowRunning && transportRunning) {
      saveNewKnobCalibration();
    }
    transportRunning = nowRunning;

    // The knobs: each hands however far it has turned since the last tick to
    // the current mode's function for it - see "Knob modes". Both are read
    // every tick whatever the mode does with them, so that a knob turned in
    // one mode doesn't arrive as a jump in the next. Until a pot is
    // calibrated its function isn't called, and lastKnownRatio keeps
    // whatever it had (native speed, from setup()/loadFile()).
    //
    // Except while touch1 is held on its own: then knob1 picks the mode and
    // knob2 the file instead - see "Selecting with the knobs".
    if (selecting && !wasSelecting) modeSelectDegrees = fileSelectDegrees = 0.0f;
    wasSelecting = selecting;

    float turned;
    if (selecting) {
      if (readKnob1(turned)) turnModeSelector(turned);
      if (readKnob2(turned)) turnFileSelector(turned);
      // So that knob2's turning isn't taken for a scrub once the pad is
      // released: wherever it is now is where it's resting.
      endScrub();
    }
    const KnobMode &activeMode = MODES[knobMode];
    if (!selecting) {
      if (readKnob1(turned) && activeMode.knob1) activeMode.knob1(turned);
      if (readKnob2(turned) && activeMode.knob2) activeMode.knob2(turned);
    }

    // While the scrub knob is moving it owns the cursor outright, like a
    // hand on the reel, and the transport waits underneath - when the knob
    // comes to rest, playback carries on from wherever it left the tape.
    bool scrubDriven = scrubbing;
    float step = scrubDriven ? scrubStep
                             : (transportRunning ? lastKnownRatio : 0.0f) * rateFactor;

    // While there's sound the echo is at full; after it stops the echo
    // fades out over a few delay times, and then is damped - see "Echo".
    bool sounding = transportRunning || scrubDriven;
    float tailFade = 0.0f;
    if (sounding) {
      echoTailing = true;
      soundStoppedMillis = millis();
      tailFade = 1.0f;
    } else if (echoTailing) {
      float tailSeconds = fmaxf(echoSeconds * ECHO_TAIL_REPEATS, ECHO_TAIL_MIN_SECONDS);
      float faded = (float)(millis() - soundStoppedMillis) / (tailSeconds * 1000.0f);
      if (faded >= 1.0f) echoTailing = false;
      else tailFade = (1.0f - faded) * (1.0f - faded) * (1.0f - faded);  // see "Echo"
    }
    echo.setFade(tailFade);
    echo.setDamped(!echoTailing);
    // The filter fades out along with the echo - see "Filter".
    filter.setLevel(tailFade);
    filter.update(controlSeconds);

    // While time-stretching, the grains read at the held pitch whatever the
    // speed - see grainTapRate. Only while running: stopped is silent.
    grainTapRate = (activeMode.holdsPitch && transportRunning) ? heldPitch * rateFactor : -1.0f;

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
      // ...but never below native speed: a 192kHz file needs 4 samples per
      // tick just to play at 1x, and capping that at 2 would play it at
      // half speed until its copies were built.
      float cap = fmaxf(ldexpf(MAX_STEP_UNCACHED, ready), rateFactor);
      if (fabsf(step) > cap) step = copysignf(cap, step);
    }
    // Glide only the scrub's steps - see currentStep.
    if (scrubDriven) {
      rampStepTo(step);
    } else {
      setStepNow(step);
    }
    trackSettings();
    stageDone(STAGE_CONTROL, t);

    // Move to a different copy if this speed calls for one. Not while
    // paused - there's nothing to hear, and the tape will most likely
    // start again at the speed it stopped at.
    if (fileLoaded && step != 0.0f &&
        millis() - levelChangeFailedMillis >= LEVEL_CHANGE_RETRY_MS) {
      uint8_t current = voices[activeVoice].level;
      // By the grains' rate when the pitch is shifted - see levelRateFor().
      uint8_t target = chooseLevel(levelRateFor(step), current, ready);
      t = DWT->CYCCNT;
      if (target != current) startLevelChange(target);
      stageDone(STAGE_COPY_CHANGE, t);
    }

    // Transport indicator, on the blue STAT LED: steady when latched,
    // blinking when running from a held finger, dark when stopped.
    uint32_t ledStartMicros = micros();  // see "loop() profiling" on why not the cycle counter
    bool blinking = transportRunning && !transportLatched;
    if (blinking && !wasBlinking) blinkPhaseStartMillis = currentMillis;
    wasBlinking = blinking;

    bool statOn = transportLatched;
    if (blinking) {
      unsigned long phase = currentMillis - blinkPhaseStartMillis;
      statOn = ((phase / TRANSPORT_BLINK_INTERVAL) % 2) == 0;
    }
    digitalWrite(LED_BUILTIN, statOn ? HIGH : LOW);

    // Mode indicator, on the RGB LED: always lit, in the current mode's
    // colour. Without this the mode is completely invisible - you would
    // have to turn a knob and listen to find out which one you were in.
    //
    // FastLED.show() bit-bangs the WS2812's timing and masks interrupts for
    // long enough to drop an audio tick. So it is called only when the
    // colour actually changes - which is only when the mode does, or for
    // the white flash that marks a reset (twice then: on and off).
    CRGB wanted = modeColour(activeMode.hue, activeMode.brightness);
    if (resetFlashing) {
      if (currentMillis - resetFlashStartMillis < RESET_FLASH_MS) {
        uint8_t white = (uint8_t)(255u * RGBLED_BRIGHTNESS / 100u);
        wanted = CRGB(white, white, white);
      } else {
        resetFlashing = false;
      }
    }
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
    float fillPercent = 100.0f * voices[activeVoice].ring.usedSpace() /
                        voices[activeVoice].ring.frameCapacity();
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
    Serial.print(")  pitch ");
    Serial.print(pitchRatio, 3);
    Serial.print("  echo ");
    Serial.print(echoSeconds, 3);
    Serial.print("s x");
    Serial.print(echoAmount, 2);
    Serial.print("  filter ");
    Serial.print(filterLowHz, 0);
    Serial.print("-");
    Serial.print(filterHighHz, 0);
    Serial.print("Hz  buffer ");
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
                     ",ECT:" + String(echoSeconds, 3) + ",ECA:" + String(echoAmount, 2) +
                     ",FLC:" + String(filterCentreHz, hzDecimals(filterCentreHz)) + ",FLW:" + String(filterWidthOctaves, 2) +
                     ",BUF:" + String((int)fillPercent) + ",UND:" + String(underrunTicks) +
                     ",RUN:" + String(transportRunning ? 1 : 0) +
                     ",LAT:" + String(transportLatched ? 1 : 0) +
                     ",LVL:" + String(level) + ",CACHE:" + String(cachePercent) +
                     ",MODE:" + String(knobMode) + ",HRD:" + String(reportedPitch(), 3);
      if (fileLoaded) {
        stat += ",WS:" + String(windowStart / nativeRate, 3) +
                ",WE:" + String(windowEnd / nativeRate, 3) +
                ",LEN:" + String((double)fileFrames / nativeRate, 3);
      }
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
