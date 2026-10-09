#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mykle Hansen
# Specified by Mykle Hansen, implemented by Claude (Anthropic).

"""
ble_test.py - live text monitor for HelloWorldBLE's Bluetooth LE mirror.

Connects to the SparkFun Thing Plus RA6M5's DA14531 BLE coprocessor
(advertised as "CLv2-CodeLess") and prints, in real time, every line of
text the board pushes over Bluetooth - the same "Hello, world!" and
"blink N" messages you'd see on the USB serial monitor.

This is the terminal equivalent of connecting with a phone app like
nRF Connect or LightBlue: it does the same "subscribe to a doorbell
notification, then go read the message" dance that the CodeLess firmware
uses to push unsolicited text over BLE. See the header comment in
HelloWorldBLE.ino for what these two characteristics are.

Usage:
    tools/venv/bin/python3 tools/ble_test.py
    (or just: make ble-test)

Press Ctrl-C to stop.
"""

import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "CLv2-CodeLess"

# CodeLess AT-command service, from Renesas' UM-140 manual.
FLOW_CONTROL_CHAR = "e2048b39-d4f9-4a45-9f25-1856c10d5639"  # notify: "data ready" doorbell
OUTBOUND_CHAR = "3bb535aa-50b2-4fbe-aa09-6b06dc59a404"      # read: the actual pushed text

SCAN_TIMEOUT_S = 10.0


async def main() -> int:
    print(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT_S:.0f}s)...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT_S)
    if device is None:
        print(f"Could not find a device advertising as '{DEVICE_NAME}'.")
        print("Checklist:")
        print("  - Is the board powered on and running a sketch that calls ble.setupDefault()?")
        print("  - Is it already connected to something else (a phone, another terminal)?")
        print("    The module stops advertising while connected.")
        print("  - macOS: does Terminal have Bluetooth permission?")
        print("    System Settings -> Privacy & Security -> Bluetooth")
        return 1

    print(f"Found {device.address}. Connecting...")

    async with BleakClient(device) as client:
        print("Connected. Waiting for messages (Ctrl-C to stop)...\n")

        async def on_data_ready(_sender, _data: bytearray) -> None:
            # The notification itself carries no payload - it's a doorbell
            # telling us new text is sitting in the Outbound characteristic.
            raw = await client.read_gatt_char(OUTBOUND_CHAR)
            text = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
            print(text, flush=True)

        await client.start_notify(FLOW_CONTROL_CHAR, on_data_ready)

        try:
            while True:
                await asyncio.sleep(3600)
        except asyncio.CancelledError:
            pass

    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)
