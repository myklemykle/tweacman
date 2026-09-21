#!/usr/bin/env python3
"""
ble_binary_test.py - drives CodeLess "Binary mode" from the laptop.

A line-for-line replica of what AudioKnobsWebBLEDemo's companion web page
(web/oscillator-remote.html) does over Web Bluetooth, but running here in
Python with bleak, and logging every single GATT operation and notification
with timestamps.

Why this exists: the web page runs on a phone, inside a third-party Web
Bluetooth browser, with no usable console - so when the Binary mode handshake
fails there is no way to see which step actually failed. This tool removes the
phone, the app and the browser from the experiment entirely. Run it alongside
`make monitor` (the sketch's own Serial2 wire trace) and you have both halves
of the conversation, timestamped, side by side.

If this succeeds where the phone fails, the bug is in the phone's Web
Bluetooth implementation. If it fails here the same way, the bug is in the
handshake itself or in the sketch - and the trace will say which.

See AudioKnobsWebBLEDemo.ino's header comment for the protocol, and Renesas'
UM-140 manual section 8.6 for the handshake this is replicating.

Usage:
    tools/venv/bin/python3 tools/ble_binary_test.py
    (or just: make ble-binary-test)

Press Ctrl-C to stop.
"""

import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "CLv2-CodeLess"
SCAN_TIMEOUT_S = 10.0

# CodeLess AT-command service - used only to negotiate Binary mode.
AT_SERVICE = "866d3b04-e674-40dc-9c05-b7f91bec6e83"
AT_INBOUND = "914f8fb9-e8cd-411d-b7d1-14594de45425"   # write: AT+BINREQ goes here
AT_OUTBOUND = "3bb535aa-50b2-4fbe-aa09-6b06dc59a404"  # read: the module's AT replies
AT_FLOWCTRL = "e2048b39-d4f9-4a45-9f25-1856c10d5639"  # notify: "AT reply ready" doorbell

# DSPS (Direct Serial Port Service) - the real data pipe, once Binary mode is on.
DSPS_SERVICE = "0783b03e-8535-b5a0-7140-a304d2495cb7"
DSPS_TX = "0783b03e-8535-b5a0-7140-a304d2495cb8"        # notify: board -> here
DSPS_FLOWCTRL = "0783b03e-8535-b5a0-7140-a304d2495cb9"  # notify + write
DSPS_RX = "0783b03e-8535-b5a0-7140-a304d2495cba"        # write: here -> board

_start = time.monotonic()


def log(msg: str) -> None:
    print(f"[{time.monotonic() - _start:7.3f}s] {msg}", flush=True)


def pretty(data: bytes) -> str:
    """Printable text plus a hex dump - a stray NUL or high byte matters here."""
    text = "".join(
        chr(b) if 32 <= b <= 126 else {13: "\\r", 10: "\\n"}.get(b, f"\\x{b:02x}")
        for b in data
    )
    return f'"{text}" | {data.hex(" ")}'


async def main() -> int:
    # find_device_by_name() proved unreliable on CoreBluetooth here - it
    # returned None on runs where a plain discover() saw the board with a
    # healthy -58 dBm the moment afterwards. Scan the full list and match on
    # the name ourselves, with a couple of retries.
    device = None
    for attempt in range(1, 4):
        log(f"Scanning for '{DEVICE_NAME}' (attempt {attempt}, {SCAN_TIMEOUT_S:.0f}s)...")
        for found in await BleakScanner.discover(timeout=SCAN_TIMEOUT_S):
            if found.name == DEVICE_NAME:
                device = found
                break
        if device is not None:
            break
    if device is None:
        log(f"Could not find '{DEVICE_NAME}'.")
        log("  - Is the board powered and advertising? It stops while connected,")
        log("    so disconnect the phone first.")
        log("  - macOS: Terminal needs Bluetooth permission (System Settings >")
        log("    Privacy & Security > Bluetooth).")
        return 1

    log(f"Found {device.address}. Connecting...")
    async with BleakClient(device) as client:
        log("Connected (GATT).")

        # Dump the whole attribute table once. Which characteristics really
        # exist, and what each one's properties actually are, is worth having
        # on the record rather than assumed from the manual.
        log("--- GATT table ---")
        for service in client.services:
            log(f"  service {service.uuid}")
            for char in service.characteristics:
                log(f"    char {char.uuid}  {','.join(char.properties)}")
        log("--- end GATT table ---")

        async def on_at_doorbell(_sender, data: bytearray) -> None:
            # The AT flow-control notification is a doorbell with no payload:
            # the actual reply text is sitting in the Outbound characteristic.
            # The web page subscribes to this and then ignores it, which is
            # exactly why the module's side of the handshake is invisible there.
            log(f"NOTIFY at-flowctrl  {pretty(bytes(data))}")
            try:
                raw = await client.read_gatt_char(AT_OUTBOUND)
                log(f"  READ at-outbound  {pretty(bytes(raw))}")
            except Exception as err:  # noqa: BLE001 - report, don't die
                log(f"  READ at-outbound failed: {err}")
                return

            # The module relays the local host's Binary mode approval to us
            # here. That is the point at which the pipe is meant to open - and
            # the XON we sent back in step 4 was answered with 0x02 (flow off),
            # because at that moment there was nothing to flow. Re-assert it
            # now that the module has actually granted the mode.
            if b"AT+BINREQACK" in bytes(raw):
                log("  -> saw the relayed AT+BINREQACK; re-asserting XON on dsps-flowctrl")
                try:
                    await client.write_gatt_char(DSPS_FLOWCTRL, bytes([0x01]), response=False)
                except Exception as err:  # noqa: BLE001
                    log(f"  XON write failed: {err}")

        async def on_dsps_tx(_sender, data: bytearray) -> None:
            log(f"NOTIFY dsps-tx      {pretty(bytes(data))}")

        async def on_dsps_flow(_sender, data: bytearray) -> None:
            # This is the signal that the pipe is actually open. The web page
            # subscribes and ignores this one too.
            log(f"NOTIFY dsps-flow    {pretty(bytes(data))}")

        # Steps 1-4: all of the notification/flow-control setup, before the
        # request itself. Order matters - see the sketch's header comment.
        log("step 1: start_notify at-flowctrl")
        await client.start_notify(AT_FLOWCTRL, on_at_doorbell)
        log("step 2: start_notify dsps-tx")
        await client.start_notify(DSPS_TX, on_dsps_tx)
        log("step 3: start_notify dsps-flowctrl")
        await client.start_notify(DSPS_FLOWCTRL, on_dsps_flow)
        # response=False is not a preference here: the GATT table above shows
        # dsps-flowctrl as write-without-response only, and asking CoreBluetooth
        # for an acknowledged write on it fails outright with
        # "Writing is not permitted" rather than falling back.
        log("step 4: write 0x01 -> dsps-flowctrl")
        await client.write_gatt_char(DSPS_FLOWCTRL, bytes([0x01]), response=False)

        # Step 5: the request. The module is expected to relay this to the
        # sketch over Serial2, and to wait for the sketch's AT+BINREQACK.
        log('step 5: write "AT+BINREQ\\0" -> at-inbound')
        await client.write_gatt_char(AT_INBOUND, b"AT+BINREQ\x00", response=True)

        log("Handshake requested. Watching for notifications...")
        log("(the board should start sending F1:...,F2:... telemetry on dsps-tx")
        log(" within a second or two if Binary mode was actually granted)")

        # Give the handshake a moment, then exercise the inbound direction too,
        # so a one-way failure can be told apart from a total one.
        await asyncio.sleep(5)
        for cmd in ("T1:1", "T1:0", "T2:1", "T2:0"):
            log(f"WRITE dsps-rx       {pretty(cmd.encode())}")
            try:
                await client.write_gatt_char(DSPS_RX, cmd.encode(), response=False)
            except Exception as err:  # noqa: BLE001
                log(f"  write failed: {err}")
            await asyncio.sleep(2)

        log("Idling - Ctrl-C to stop.")
        while True:
            await asyncio.sleep(3600)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)
