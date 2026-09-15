Hello Claude! We are going to develop some microcontroller demo projects together based on various MCUs from the Renesas RA family.

Our first project runs on the Sparkfun Thing Plus RA6M5. We're writing in Arduino and C++, and also using Faust for audio synthesis.

Our code is intended for hobbyists to learn from and extend, so it should be clear, well commented and concise, and follow Arduino conventions. We're building a series of small demo sketches - Hello World, Bluetooth, DAC audio output, potentiometer input, Faust-based synthesis - and will eventually write a tutorial from this material, so each demo should stand on its own as something a hobbyist could read and follow.

## Hardware: SparkFun Thing Plus RA6M5

- MCU: Renesas RA6M5, Arm Cortex-M33 @ 200MHz.
- Memory: 2MB code flash; ~512KB RAM; separate 8KB Data Flash region (`BSP_DATA_FLASH_SIZE_BYTES`) meant for small persistent data, not general code/heap - erased in 64-byte blocks, written in 4-byte granularity, backed by a `DataFlashBlockDevice` and reachable through the bundled `EEPROM` library. Doesn't wear-level; `EEPROM.put()`'s per-byte `update()` skips a write only when the new byte is identical to what's already there.
- Analog I/O: A0/A1 (pins 7/8) are true 12-bit hardware DACs, not PWM - `DAC`/`DAC1` in the core. A2-A5 (pins 9-12) are ADC inputs. The ADC hardware maxes out at 12 bits but `analogRead()` defaults to 10-bit scaling until `analogReadResolution(12)` is called; `analogWrite()` similarly defaults to 8-bit until `analogWriteResolution(12)` is called - both are process-wide settings, not per-pin.
- DAC output amplifier: the DAC has a real internal output amplifier that lowers its output impedance for driving an external load, but the Arduino core hardcodes it off with no exposed API. Enabling it requires reaching directly into `R_DAC`'s registers (`DAAMPCR_b`/`DAASWCR_b`/`DACR_b`) after the channel is already open - see `DACAudioTest.ino` for the working sequence and why it's a deliberate hack rather than something to build on.
- DAC signal characteristics: DAC output rides on a DC bias rather than swinging symmetrically around 0V - a series coupling capacitor (several uF) is needed between a DAC pin and any downstream audio jack/amp input to get a clean AC signal without a click at the start/end of playback.
- `LED_BUILTIN` = pin 14, the blue STAT LED. A WS2812 RGB LED is also on D13 (P105) - driven via FastLED (`FastLED.addLeds<WS2812B, 13, GRB>(...)`), see `WS2812TouchDemo.ino`.
- CTSU (Capacitive Touch Sensing Unit): three real touch-capable pins on this board, decoded via this project's own `CapTouch` library (see below) - D20 (TS05), D21 (TS04), D31 (TS11, also the onboard USR button - usable but not a great choice for a breadboard wire). All three share one analog reference pin, D6 (TSCAP), which needs an external capacitor to GND (~10nF is a good starting point) or nothing will work. See `CapTouch.h`'s class comment for the register-level bring-up details (several real, non-obvious bugs were found debugging this from scratch against the RA6M5 Hardware User's Manual).
- `Serial` is native USB CDC, not a UART - text printed before a monitor/host opens the port is discarded outright, not buffered. Sketches should watch for the connected state to flip from false to true and (re-)greet at that point, rather than printing once in `setup()`.
- Also on board, per SparkFun's docs but not yet used in this project: a MAX17048 fuel gauge, a 16MB external QSPI flash chip, an SD card slot, and a Qwiic I2C connector.

## Attached peripherals (current bench setup)

- **Renesas DA14531MOD Bluetooth LE module** - on-board, a separate coprocessor from the RA6M5 itself, not a library running on the main chip. Wired to the RA6M5 over `Serial2` at 57600 baud, 8N1, no flow control. Runs Renesas' "CodeLess" AT-command firmware and advertises as `CLv2-CodeLess`. Controlled via SparkFun's unofficial `CodelessBLE` Arduino library (manually installed, not in the Library Manager). See `HelloWorldBLE.ino` for outbound-only push text (the undocumented-upstream, reverse-engineered-here GATT notify-then-read protocol) and `AudioKnobsWebBLEDemo.ino` for genuine bidirectional data - CodeLess's AT-command link has no inbound equivalent of its outbound "|" pipe convention (arbitrary writes get parsed as AT commands and rejected), so real two-way traffic needs CodeLess's separate "Binary mode"/DSPS path instead, which turns out to need the *local* UART host's active approval, not just the remote BLE central's request - see that sketch's header comment for the full handshake, and the `codeless-binary-mode-dsps-protocol` memory for the complete writeup (including phone-side WebBLE-app gotchas: Claude Artifacts can't host the companion web page at all - a Permissions-Policy blocks `navigator.bluetooth` outright - and Bluefy's `writeValueWithoutResponse()` silently drops writes, fixed by using plain `writeValue()`).
- **3.5mm TRS audio jack** - tip and ring wired to A0 and A1 (DAC0/DAC1), sleeve to GND. The right channel (A1) currently has a 4.7uF coupling capacitor for DC-blocking; the left (A0) doesn't, kept that way deliberately for A/B comparison.
- **Two Taiwan Alpha RV112FF-40 dual potentiometers** - absolute single-turn 360-degree pots, each with two wipers mechanically offset 90 degrees, giving a pair of triangle-wave signals an absolute shaft angle can be decoded from (no interrupts, no external reference needed). Confirmed via the manufacturer's own datasheet: an "endless rotary" type with no mechanical stop, rated 15,000 rotation cycles - so it really can be spun multiple times, which `MultiTurnKnob` (see below) takes advantage of. Wired: pot 1's wipers to A2/A3, pot 2's wipers to A4/A5, both pots' track ends to 3.3V/GND. Driven by this project's own `RV112FF` library (see below) - ported from a prior, more thoroughly-tested implementation on a different platform (Bela); see `../bela/gemtest.git` (symlinked into this repo as `gemtest.git`) for that project's own notes if the decode math or a hardware quirk needs re-deriving.
- **Two CapTouch pads** - D20 (touch1) and D21 (touch2), each just a wire or touch pad; see the CTSU note above for the shared TSCAP capacitor both depend on. In `AudioKnobsDemo`/`AudioKnobsWebBLEDemo`, touching one gates its knob's sweep speed (fast, single-turn, matching the original mapping) versus released (slow, two full turns per sweep, via `MultiTurnKnob`).
- **Onboard WS2812 RGB LED** (D13) - see the Hardware section above; driven in `WS2812TouchDemo.ino`.

## Toolchain and environment

- Arduino core: SparkFun's unofficial port of `arduino:renesas_portenta`, manually installed as a version-bumped overlay (`1.6.1`) so it takes precedence over the official `1.6.0`. This is fragile - `arduino-cli core upgrade` (or reinstalling the core) will silently wipe it, since it isn't tracked by Arduino's own board manager. Re-extract from SparkFun's core zip to recover.
- Faust 2.70.3, installed at `/opt/faust`. First actually exercised in `AudioKnobsDemo` - see its `synth.dsp`/`faust_support.h` for the embedded-target integration pattern (a trimmed, vendored copy of Faust's own C++ architecture headers, a custom-sized wavetable to fit this chip's RAM, and a hand-rolled audio-rate `FspTimer` interrupt in place of Faust's usual audio-driver architecture files).
- Everything builds and flashes through the project `Makefile` (`arduino-cli` underneath) - see `make help`. Local libraries (`RV112FF`, `CapTouch`, `MultiTurnKnob`) are passed via `--library` rather than installed into the global sketchbook, so they stay versioned with this repo - this is also the Makefile's `LIBRARIES` default, so any new sketch gets all three automatically. `FastLED` is a normal Library Manager install (global, not versioned with this repo).
- `tools/` holds small Python helpers (a Bluetooth LE test monitor, using `bleak`) with their own isolated venv, also Makefile-driven (`make ble-test`).

## Project structure and conventions

- One sketch per feature/demo, each its own top-level folder (`HelloWorld`, `HelloWorldBLE`, `DACAudioTest`, `AudioKnobsDemo`, `AudioKnobsWebBLEDemo`, `WS2812TouchDemo`, ...) so features can be merged together without one displacing another - see the working conventions below on how that maps to git branches. A library's own example sketches live in its `examples/` folder instead (see `RV112FF/examples/`, `CapTouch/examples/`, `MultiTurnKnob/examples/`).
- Reusable code that more than one sketch might use gets its own library folder at the project root, structured as a normal Arduino library (`library.properties`, `keywords.txt`, `src/`, `examples/`) even though it lives in this repo rather than the global sketchbook. Current libraries: `RV112FF` (potentiometer decoding), `CapTouch` (CTSU touch sensing), `MultiTurnKnob` (wraps an `RV112FF` to add Encoder-style multi-turn counting, since the pot itself is a confirmed endless-rotation part but its own sensor has no turn-counting memory).
- A sketch with a browser-based companion UI (currently just `AudioKnobsWebBLEDemo`) keeps that page in its own `web/` subfolder, checked into this repo - it can't be published as a Claude Artifact (see the DA14531MOD note above), so it has to be self-hosted with real HTTPS somewhere the phone can reach it.
- Each demo sketch's header comment is written to stand alone - hardware/wiring, why any non-obvious implementation choice was made, and how to test it - since these are meant to be read individually by a hobbyist, not just as a diff against the previous demo.

## Working conventions

- If I phrase a request as "can we do such-and-such" (or similarly tentative
  phrasing), treat it as a request to discuss, not to implement. Describe
  the work as best you can - approach, what would change, what's involved -
  and stop there. Don't start coding until I explicitly ask you to proceed.

- New features should be developed on their own feature branch and merged
  in when done, rather than committed straight to main. I tend to forget
  this - when we're about to check work in, remind me to branch first if
  we haven't already.
