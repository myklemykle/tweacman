# Hello World over Bluetooth LE — Setup Summary

How Bluetooth LE was added to `HelloWorld.ino` on the SparkFun Thing Plus
RA6M5. Written for reuse in a tutorial; the research/investigation that led
to these steps is omitted — just the steps themselves. Assumes the USB
serial version is already working (see `HelloWorld_setup.md`).

## Key concept to introduce first

The board's Bluetooth radio (a Renesas DA14531MOD module) is a **separate
coprocessor**, not something added to the sketch via a BLE library on the
main chip. It's wired to the RA6M5 over a second internal UART (`Serial2`,
fixed at 57600 baud) and runs its own firmware that's controlled with plain
AT-style text commands — closer to talking to an old modem than to using
`ArduinoBLE`.

## Steps

1. Installed SparkFun's unofficial `CodelessBLE` Arduino library by hand (it
   isn't in the Library Manager) — copied from their GitHub repo into the
   sketchbook's `libraries/` folder.
2. In `setup()`, used the library's `CodelessPeripheralDevice` class to open
   `Serial2`, reset the BLE module, and start it advertising (this is one
   call, `setupDefault()`).
3. In `loop()`, watched for two plain-text status lines the module sends on
   its own — `"+CONNECTED"` and `"+DISCONNECTED"` — to track whether a phone
   is currently linked.
4. Added a small helper that sends a line of text out over BLE by prefixing
   it with `"|"` — the module's firmware treats a `|`-prefixed write as data
   to forward to the connected phone, rather than as a command.
5. Wired that helper into the existing logic: whenever a phone is connected,
   the same `"Hello, world!"` and `"blink N"` text already going to USB
   serial is also pushed out over BLE.
6. Handled one more module quirk: it stops advertising the moment something
   connects, and doesn't resume on its own after a disconnect — so the
   sketch re-issues the "start advertising" command each time it sees
   `"+DISCONNECTED"`.
7. Verified the result by connecting a generic BLE tool to the board and
   confirming it can retrieve the mirrored text.
8. Added a terminal-based test tool (`tools/ble_test.py`, run via
   `make ble-test`) that does the same notify-then-read dance a phone app
   would, so the Bluetooth side can be checked from the command line without
   a phone. It runs in its own Python virtual environment
   (`tools/venv`, created automatically on first run) using the `bleak`
   library. One install gotcha: `pip install bleak` tries to build
   `pyobjc-core` from source, which fails on current Xcode/clang - forcing a
   prebuilt wheel with `pip install --only-binary=:all: bleak` avoids it.
   Also requires a one-time macOS permission grant: System Settings ->
   Privacy & Security -> Bluetooth -> enable for Terminal.

## What "testing it" actually involves

This is the part worth weighing for the tutorial: retrieving the pushed
text isn't a simple "subscribe and watch it stream in." It's a two-step
dance:

- Subscribe to notifications on one characteristic (a plain "something
  arrived" doorbell, no content).
- Each time it fires, do a separate GATT *read* of a second characteristic
  to get the actual text.

Any BLE explorer app (nRF Connect, LightBlue) can do this, but it's manual,
per-message tapping — there's no polished "chat" UI for it. It's a good
demonstration that the board *is* accessible over Bluetooth, but it doesn't
feel like a plug-and-play consumer BLE experience the way a UART-bridge app
would.

## Complexity note

The serial path is one self-contained concept (USB CDC quirks). The BLE
path introduces a second onboard computer, a second protocol, and a
two-step read pattern to test it — noticeably more conceptual surface area
for a "Hello World." Might work well as a clearly-separated "bonus/advanced"
section rather than the main walkthrough.
