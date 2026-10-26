# Open items

Things discussed but not yet done. Updated alongside the commits that finish
or add items, so this should match the code on `main`; last reviewed
2026-10-16. Roughly grouped, and within each group roughly in the order
they'd be worth doing.

Design proposals large enough to have their own document are linked rather
than repeated: see `CodelessPipe_design.md`. The investigation behind the pot
and touch noise fixes is in `NOISE_DEBUGGING.md`.

## SDResampleDemo - planned features

Modes are rows of the `MODES` table in the sketch, stepped through by
touch1 (or picked with knob1 while it's held). Five are built: speed mode
(knob1 speed, knob2 scrub), pitch mode (knob1 pitch, knob2 time-stretch),
window mode (knob1 start, knob2 end), echo mode (knob1 delay time, knob2
amount) and filter mode (knob1 the band's width, knob2 its centre). Each further mode gets its own feature branch, and its
parameters should be added to the per-file settings (`FileSettings.h`),
which hold speed, pitch, window, echo and filter so far, and to the web
page's tweaks tab.

### Modes

- **Mode colours.** Six hues clear of the board's red and blue LEDs: green
  120, aqua 180, purple 268, pink 335 and orange 30 degrees are in use;
  yellow 60 is picked and waiting for mode 6 (it was tried on the LED as a
  placeholder mode, then taken out). Green, aqua and yellow are dimmed
  to match the rest. Revisit if any pair is hard to tell apart.
- **One description of the modes, not two.** The web page has its own list
  of the modes and their settings (`MODES` and `PARAMS` in
  `wav-remote.html`), which has to be kept in step with the sketch's `MODES`
  table by hand. The board could describe them instead (name, hue, each
  setting's range and units) for the page to build itself from.
- **Time-stretch: fixed-length grains at high speeds.** Above about 8
  samples per tick, pitch mode's time-stretch goes dull and then nearly
  silent - congaTam.wav (a 22kHz file, almost all its energy at 155-310Hz)
  vanishes when the 1/64 copy takes over, and t_08000_16.wav's tune when
  the 1/8 and 1/16 do. Two causes, both by design rather than bugs. The
  grains stream the copy past at the playback speed, and a voice can only
  read about 8 samples of its copy per tick, so the copy choice
  (`levelRateFor()`, the `s / 4` floor) keeps moving to lower-rate copies,
  each with half the bandwidth - fine in speed mode, where what a copy
  drops would be above hearing anyway, but in time-stretch the pitch is
  held, so it's sound you'd still hear. And the grains restart every time
  the cursor moves a grain length (2048 samples), so at high speed they
  are a few ticks long and mostly heard as a buzz at that rate, which
  follows the knob. The fix: above about 8x, every ~23ms load a fresh
  46ms grain straight from the WAV (or a low copy) at the cursor, in
  `loop()` - one ~4KB card read per grain whatever the speed - and play
  it at the held pitch, two overlapping with Hann windows as now. No copy
  switching, full treble, and a steady grain rate that could become a
  setting. Needs grain buffers, a new read path in the audio interrupt,
  and a bench check of the card's timing. (The cheap alternative,
  letting the cursor read each copy twice as fast before switching, only
  moves the problem up an octave and keeps the buzz.)
- **Window that wraps round the file's ends.** Mode 3's start and end stop
  at the ends of the file; letting the window carry on round from the end
  to the beginning would be cool.
- **Shorter windows.** The minimum is 20ms (`WINDOW_MIN_SECONDS`). Much
  shorter and the loop becomes a pitched buzz - worth trying.
- **Echo: a longer delay.** Mode 4's delay line is in RAM (`EchoDelay.h`):
  mono, 24kHz, 2.7 seconds. Longer means keeping it on the SD card - a
  pre-allocated file used as a circle, written behind the write head and
  read ahead of the tap like a third voice. Mono is comfortable on average
  (about 96KB/s of writes against the card's 350KB/s); the risks are the
  card's occasional long write stalls underrunning playback, and 25ms
  writes stuttering the control tick. Measure the worst-case write time
  during playback first. Kept as a file, it's also most of a
  sound-on-sound looper.
- **Echo: runaway feedback.** The amount stops at 0.95. Past 1 the echo
  feeds itself for ever, tape-echo style, but needs a soft clipper in the
  loop rather than today's hard limit. Loud material with a lot of echo
  already clips at the output - the same clipper would help there.
- **Echo: finish the feedback trial.** Knob2 sets one amount, which is
  both how loud the echoes are and how much they feed back. Play with it
  for a while and decide whether that's right or the two want separate
  controls. `EchoDelay` already keeps them as separate gains inside (it
  needed to, to damp the echo on a stop), so splitting them is only a
  question of what sets the second one - there's no third knob.
- **Echo: delay time locked to the window**, so the repeats fall in step
  with the loop.
- **Filter: tune it by ear.** Mode 5's character is in a few constants in
  `BandFilter.h`, set from measurements (`make filter-render`) rather than
  some listening: the bump at each edge (`FILTER_BUMP`, k = 1.5, about
  +5dB - raised from 1.2 after the first listen), how hard it screams
  (`FILTER_SCREAM`), where in the narrowing the scream starts
  (`FILTER_RESONANCE_OCTAVES`, at about 0.5 octaves wide now), and how hard
  the resonance clipping bites (the `1 +` in the stage's `a`). The scream
  sits at about -11dBFS. Each knob moves its cutoff 5 octaves per turn.
- **Filter: its CPU cost.** About 570 cycles per stereo sample, 12% of the
  audio interrupt (56% to 68% on a stereo file with pitch shifting),
  while narrowed; nothing wide open. Written in Faust first, it measured
  925 - Faust's per-call overhead with one sample per call - so it's plain
  C++. Most of what's left is chained floating-point arithmetic and four
  divisions. If the planned post-processing needs the room: a mono filter
  would halve it, or the divisions could be traded for an approximation.
- **Filter: other shapes.** It's a band-pass only (a high-pass and a
  low-pass in series). Low-pass-only or a notch could be further modes, or
  a third parameter.

### Web UI (`web/wav-remote.html`)

Redesigned 2026-10-13 (`feature/web-redesign`): a one-line status bar, a
full-screen Connect button until there's a board, and three tabs at the
bottom - Tweaks (a card per knob mode, its two settings side by side like
the knobs, each with a slider and a reset; the board's current mode
highlighted, tap a card to change mode), Files (the list scrolls under a
fixed header), and Info (playback stats and the link log, folded away).

- **Try the redesign on a phone.** It has only been run in desktop Chrome
  against a stand-in for the board. Things to check on the real thing:
  sliders against the half-second stats (do they fight the thumb?), the
  slider ranges (speed -4x to 4x, pitch two octaves either way; typing
  reaches the rest), the decimal keypad for typed values, and the layout
  under the notch and home bar.
- **Status line: a logo and a playing animation.** "TWEACMAN" is text for
  now, and playing/stopped is a word.
- **Bottom tab bar: on trial.** Move it to the top if it doesn't suit.
- **Normalize and compress recordings.** Audio recorded on the phone comes
  out too quiet.

## SDResampleDemo - known issues

- **USB disk mode: speed.** With the core's `UsbMsd` library as it is,
  copying onto the card runs at about 115KB/s and reading off it about
  550KB/s (4MB each way, 2026-10-16); full-speed USB allows about 1MB/s.
  A Finder copy of 12MB of files off the card took about 30s (~400KB/s),
  so many files cost a little more than one big one. Copying that same
  folder back onto the card took 4:17 (~47KB/s) - under half the
  single-file write speed, since every file adds scattered single-sector
  writes (file table, directory entry, macOS's "._" file), each of which
  the library turns into a read and a write. Spotlight's share is
  unmeasured. Not copying `cache/`
  avoids most of that - the board rebuilds it - and marking it hidden on
  the card would keep Finder from showing or copying it (untested whether
  the core's FATFileSystem lets the sketch set that attribute).
  Writes are slow because the library gets 512 bytes at a time from the
  computer and, for each sector, reads it back from the card and writes it
  again as its own command - the worst case for an SD card. The plan, if
  it's worth it: our own `tud_msc_*` handlers that gather consecutive
  sectors into a ~32KB buffer and write them as one multi-sector command
  through `RA6M5FastSD`, flushed when the computer jumps elsewhere, asks
  for a sync, or ejects. Reads could go through the multi-sector path too.
  Test first, to see where the time really goes: Finder copies of a real
  sample folder both ways (many small files as well as a few big ones),
  the card's own write speed in large pieces (a small benchmark sketch -
  the 0.35MB/s figure in CLAUDE.md was measured with 8KB writes), and how
  long the mount takes, including macOS's checks.
- **USB disk mode: Spotlight holds up ejecting.** Soon after the card
  mounts, macOS's Spotlight indexer (`mds_stores`) starts reading it, and
  ejecting is refused until it's done - 40 seconds once, at USB disk mode's
  ~550KB/s. A `.metadata_never_index` file in the card's root used to stop
  that; whether current macOS still honours it is untested (the file list
  would skip it, being a dot-file). Otherwise it's `mdutil -i off` on the
  Mac, per volume.
- **USB disk mode: try it on Windows (and Linux).** Only tested on macOS
  so far. Worth checking: how long the card takes to appear, that "Safely
  remove" brings the board back as Finder's eject does, and what hidden
  files Windows leaves on the card (names starting with a dot are already
  skipped; folders such as `System Volume Information` aren't listed
  anyway, since only files are).
- **USB disk mode: the BLE link hiccups while the computer is busy with the
  card.** The computer's requests are served inside the USB interrupt, so
  loop() is starved while it reads or writes; a Binary mode probe's echo
  then arrives after `BINARY_PROBE_TIMEOUT`, the link is wrongly promoted,
  and the next echo demotes it again (seen as "Unrecognized BLE command"
  lines on Serial). It recovers by itself, and only matters with no phone
  connected; the real fix is the probe noticing bytes waiting before it
  gives up.
- **Other pot sketches relearn calibration every boot.** Since 2026-10-09
  `RV112FF` reports nothing until a pot's calibration is complete (about two
  turns). SDResampleDemo saves it; AudioKnobsDemo, AudioKnobsWebBLEDemo,
  NoiseMeter and the library examples don't, so their knobs need two turns
  after every power-on. Each could save it the same way if wanted.
- **Cache-build stalls.** While `MipCache` is building, a single build step
  can stall `loop()` for up to ~35ms on an SD write - over this project's
  10ms limit for control latency, so the knobs and pads feel it during a
  build. Card writes are slow (~0.35MB/s on the bench card) whatever the
  chunk size, so the fix is probably splitting the work into smaller steps
  rather than writing faster.

## BLE / CodeLess

- **`HelloWorldBLE` needs `bleExitBinaryMode()`.** It never uses Binary mode,
  but it gets wedged by whichever sketch ran before it: a module left in
  Binary mode swallows its `ATR`/`AT+ADVSTOP`/`AT+ADVSTART` as raw pipe data,
  so startup appears to succeed and nothing works. The one sketch broken by a
  feature it does not use.
- **The `+++` acknowledgement churn.** The module asks to leave Binary mode by
  sending `+++`; the sketches answer it, but the reply
  (`AT`/`AT+BINREQEXITACK`) may go out *as pipe data* while Binary mode is
  live, so clients occasionally see stray `AT+...` lines in their data
  stream. Cosmetic - the link recovers every time. Really a symptom of the
  open question below.
- **`CodelessPipe` library.** Full proposal in `CodelessPipe_design.md`.
  Agreed sequencing: port the fixes into each sketch as a straight copy
  first, then extract the library from the working call sites. Its two
  unresolved questions, both of which change the design:
  - What the mode-detection probe should send when a sketch has no traffic of
    its own (proposed: a lone `"\n"`).
  - Whether the sketch can *force* the mode transition - e.g. `AT+DISCONNECT`
    right after `AT+BINREQACK` - instead of the central hanging up and
    reconnecting. Untested. If it works, the handshake gets much simpler and
    the `+++` churn above probably disappears with it.
- **Faster BLE (maybe).** Uploads top out at ~1.8KB/s, and this CodeLess
  firmware has no commands to raise the baud rate, MTU or data length. The
  only real lever is replacing CodeLess with custom DA14531 firmware (SDK6
  DSPS with a bigger MTU, data length extension and a faster UART) - a large
  project.
- **Does the module's Bluetooth address change when it restarts?** Once
  (2026-10-16), desktop Safari's pairing chooser listed two `CLv2-CodeLess`
  entries with completely different addresses, though there's only one board
  in range. The likely cause: the DA14531 SDK can generate a fresh "static
  random" address at each boot, and the module restarts on every power cycle
  and every Binary-mode disconnect, so the Mac briefly still lists the old
  one. (macOS shows browsers a made-up identifier per device rather than
  the real address, so the listed addresses may not be the real ones.) To test:
  `AT+BDADDR`, restart (`AT+R` or a reset), `AT+BDADDR` again. A small `BDADDR`
  passthrough in `SDResampleDemo`'s Serial console would do it. Harmless for
  the web page, which finds the board by name and service, but it would break
  anything that remembers a device (saved Web Bluetooth permissions,
  automatic reconnection). If it does change, check CodeLess's AT command
  reference for a way to set a fixed address.

## PCB / hardware

- **When the OSH Park boards arrive:** 1:1 fit check of the parts, re-run
  ERC (now with the PWR_FLAG), build one, recalibrate the touch thresholds
  for real pads instead of wires, re-test touch noise, and check BLE range
  with the lid on.
- **BOM, after the fit check** (`feature/pcb-bom`, not merged yet):
  - Feet (3M SJ5023) - height unverified.
  - Check the M-F standoff's male thread bites into the F-F standoff above.
  - TS header pin diameter versus the SL socket's contact range.
  - C4 (axial, laid flat) fit on its footprint.
  - The LCSC columns are stale - refresh or drop them.
  - Low Mouser stock on the SL-112 socket and TS-116 header matters for kits.
- **Rev 2 (maybe):** the I2S mic and OLED, preserved as designed on
  `feature/pcb-mic-oled`.
- **Before publishing the board:** fab notes (ENIG preferred, for the exposed
  touch pads and edge plating), a `datasheets/` README with URLs instead of
  the PDFs, and a build README. (Licensing and the SparkFun credit are done -
  see `LICENSE`.)
- **Next board revision: put a source location on the silkscreen.**
  CERN-OHL-S-2.0 asks for a "Source Location" on the product - the repo's
  URL - and the license name would fit next to the existing "this hardware
  is open source" text on the back.

## Arduino libraries

- **Publish the libraries to the Library Manager, so the Arduino IDE can
  find and install them.**
  Plan approved 2026-09-26, postponed until after a code review of the
  libraries. `RV112FF` and `MultiTurnKnob` were reviewed on 2026-10-26:
  their comments no longer refer to this project, and both build for
  RP2040, Teensy and AVR as well (step 1's `architectures=*` is done). Written for `RV112FF`, `RA6M5CapTouch`, `MultiTurnKnob` and
  `RA6M5AnalogTuning`; `RA6M5FastSD` came later and is experimental (it
  reaches into the core class's private members), so it probably shouldn't
  be published. The Library Manager needs each library in its own repository
  with `library.properties` at the top, so the plan keeps developing
  everything in this repo and publishes each library folder from it with
  `git subtree split`:
  1. Prep: ~~set `architectures=*` for `RV112FF` and `MultiTurnKnob`~~
     (done); run `arduino-lint --library-manager submit` on each; merge to
     main.
  2. Create a public GitHub repo per library, then fill in each library's
     empty `url=`.
  3. For each library: `git subtree split --prefix=<Library> main`, push the
     result to its repo's `main`, and tag it `1.0.0` to match
     `library.properties`. Splitting only reads history, so it's safe to run
     from any checkout.
  4. Open a pull request against `arduino/library-registry` adding the repo
     URLs. Publish `RV112FF` first: `MultiTurnKnob` has `depends=RV112FF`,
     which the Library Manager resolves automatically once both are listed.
  - The four names were free in the registry when checked on 2026-09-25, but
    names are first-come, first-served.
  - After publishing, a new release is just the same split-push-tag, with the
    version bumped in `library.properties`; the registry picks up new tags by
    itself.
- **WS2812 hardware offload.** Drive the D13 LED from GPT1 PWM with the DTC
  loading each bit's duty (or SCI8 as SPI plus DMA) instead of FastLED, so
  LED updates never mask interrupts. FastLED's ~30us `cli()` per update drops
  one audio tick, which turned out to be inaudible - so this is for tidiness,
  and possibly its own tutorial demo of hardware-offloaded LED driving. Would
  ship as a small local library in the style of `RA6M5CapTouch`. Watch for
  the SD driver's DMAC channel.

## Toolchain / bench reliability

- **USB and DFU flakiness makes iteration slow.** `make upload` sometimes
  fails with `dfu-util: Cannot set alternate interface zero:
  LIBUSB_ERROR_OTHER` while the board still enumerates as a serial port, and
  the serial device node has vanished for minutes after a DFU attempt; both
  cleared only by unplugging and replugging. An inline USB power monitor was
  in the chain at the time and is a plausible contributor, not confirmed.
- **A real readiness check before opening the monitor.** The upload/monitor
  sequence waits for the device *node* to exist, but the node can appear
  before the CDC endpoint is ready (`Port monitor error: timeout waiting for
  message`). A readiness check, or a short retry on the first read, would be
  more honest than the existence check plus a fixed sleep.

## Web pages

- **Redeploy the hosted copies after changing them.** `oscillator-remote.html`
  and `wav-remote.html` are self-hosted on mykle.com; the phone only sees a
  change once it's redeployed. Up to date as of 2026-10-09.

## Documentation

- **The tutorial.** The long-term goal: a hobbyist tutorial built from these
  demos.
