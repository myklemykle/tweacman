# Hello World over USB Serial — Setup Summary

How `HelloWorld.ino` was brought up on the SparkFun Thing Plus RA6M5, from a
bare Mac to a blinking LED and serial output. Written for reuse in a
tutorial; the research/investigation that led to these steps is omitted —
just the steps themselves.

## Environment setup

1. Confirmed existing tools (`arduino-cli`, Faust, Homebrew) were already
   installed; left Faust at its current version since nothing yet needs the
   upgrade.
2. Installed the official `arduino:renesas_portenta` Arduino core via
   `arduino-cli core install`. The RA6M5 Thing Plus isn't in this core's
   board list, but installing it pulls down the toolchain the board actually
   needs (arm-none-eabi-gcc, dfu-util, bossac, openocd).
3. Downloaded SparkFun's unofficial ported core (a zip from their GitHub
   docs) and installed it manually as a second, higher-numbered version
   (`1.6.1`) alongside the official `1.6.0`, so it takes precedence. This is
   what actually adds the `SparkFun Thing Plus RA6M5` board (FQBN
   `arduino:renesas_portenta:thingplus_ra6m5`) — its tool requirements are
   identical to the official core's, so no extra toolchain install was
   needed.
4. Installed Arduino IDE 2.3.10 as a GUI option, separate from the CLI
   workflow.
5. Wrote a `Makefile` wrapping `arduino-cli compile/upload/monitor`, with
   the port auto-detected from `arduino-cli board list` and overridable
   `SKETCH`/`FQBN`/`PORT`/`BAUD` variables.

## The sketch

6. Wrote `HelloWorld.ino`: blinks the onboard LED (`LED_BUILTIN`, pin 14)
   using non-blocking `millis()` timing, printing `"Hello, world!"` and a
   running `"blink N"` counter over `Serial`.
7. Handled one board-specific quirk: `Serial` on this board is native USB,
   not a UART, so anything printed before a monitor attaches is silently
   discarded rather than buffered. Fixed by watching for `Serial`'s
   connected state to flip from false→true and printing the greeting at
   that moment, so it always appears regardless of when you open the
   monitor.
8. Built and flashed via `make upload` (compiles, then uploads over DFU
   using the board's built-in bootloader), and confirmed over the serial
   monitor that the blink counter and greeting both appear correctly,
   including on a late attach.
