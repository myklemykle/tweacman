# Open items

Things discussed but not yet done. Updated alongside the commits that finish
or add items, so this should match the code on `main`; last reviewed
2026-10-09. Roughly grouped, and within each group roughly in the order
they'd be worth doing.

Design proposals large enough to have their own document are linked rather
than repeated: see `CodelessPipe_design.md`. The investigation behind the pot
and touch noise fixes is in `NOISE_DEBUGGING.md`.

## SDResampleDemo - planned features

Captured 2026-10-09. Several depend on each other: the general modes
framework comes before modes 3-5, and the web UI's mode display builds on
it. Modes are rows of the `MODES` table in the sketch, stepped through by
touch1. Three are built so far: speed mode (knob1 speed, knob2 scrub),
pitch mode (knob1 pitch, knob2 time-stretch) and window mode (knob1 start,
knob2 end); three more are placeholders with a colour and no knob functions. Per-file
settings exist too (speed and pitch, see `FileSettings.h`); each new mode's
parameters should be added to them.

### Modes

- **Mode colours.** Trying six hues clear of the board's red and blue LEDs:
  green 120, aqua 180, purple 268, pink 335, orange 30, yellow 60 degrees,
  with green, aqua and yellow dimmed to match the rest. Revisit if any pair
  is hard to tell apart on the real LED.
- **Describe each mode's parameters in the table** (name, value, units), for
  the web page's tiles and the per-file settings - when the web UI needs it.
- **Window that wraps round the file's ends.** Mode 3's start and end stop
  at the ends of the file; letting the window carry on round from the end
  to the beginning would be cool.
- **Shorter windows.** The minimum is 20ms (`WINDOW_MIN_SECONDS`). Much
  shorter and the loop becomes a pitched buzz - worth trying.
- **Show the window** on the web page; it's only on Serial so far
  (`WINDOW:<s>,<e>`, `RESET:WINDOW`).
- **Mode 4: digital feedback echo** - delay time and feedback/volume.
- **Mode 5: filter on the output** - low/band/high-pass, with cutoff(s) and
  width/Q.

### Web UI (`web/wav-remote.html`)

- **Full-screen CONNECT while disconnected.** The button fills most of the
  phone screen, in front of the rest of the interface, which means nothing
  until connected.
- **Reorganize into tabs.** A small status line on top, then three tabs:
  parameters (speed, pitch, ...), SD file management, and diagnostics (link
  log, buffer and cache info). Phone-first.
- **Show the current mode.** Probably two tiles per mode showing its
  parameters, with the current mode highlighted.
- **One "reset tweaks" button** for all knob tweaks on the current file,
  replacing today's separate speed and pitch resets - back to the defaults,
  and saved that way in its per-file settings. It's also the way back to
  the file's own pitch, now that the pitch knob is relative with no
  click-stop at unity.
- **Pitch tile shows the shift, not the pitch you hear.** `PIT:` reports
  `pitchRatio`, the shift on top of the speed. In pitch mode the knob sets
  the pitch you hear (shift times speed), so at 2x unshifted-sounding audio
  reads 0.5. Report and show the heard pitch when the page shows modes.
- **Normalize and compress recordings.** Audio recorded on the phone comes
  out too quiet.

## SDResampleDemo - known issues

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

## Arduino libraries - publishing to the Library Manager

- **Publish the libraries so the Arduino IDE can find and install them.**
  Plan approved 2026-09-26, postponed until after a code review of the
  libraries. Written for `RV112FF`, `RA6M5CapTouch`, `MultiTurnKnob` and
  `RA6M5AnalogTuning`; `RA6M5FastSD` came later and is experimental (it
  reaches into the core class's private members), so it probably shouldn't
  be published. The Library Manager needs each library in its own repository
  with `library.properties` at the top, so the plan keeps developing
  everything in this repo and publishes each library folder from it with
  `git subtree split`:
  1. Prep: set `architectures=*` for `RV112FF` and `MultiTurnKnob`, which
     aren't chip-specific; run `arduino-lint --library-manager submit` on
     each; merge to main.
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
