#!/usr/bin/env python3
"""
ble_reset.py - forces the SparkFun Thing Plus RA6M5's DA14531 CodeLess
module out of a stuck Binary mode / DSPS state, without needing to
physically power-cycle the board.

The DA14531 is a separate, always-on coprocessor - reflashing or
resetting the RA6M5 main MCU does not reset it. If it was ever granted
Binary mode and the local host (the RA6M5 sketch) then reset without a
clean exit, the module can be left thinking a connection is still
transparently bridged when it isn't, silently swallowing anything sent
to it until it's explicitly told to exit and reset.

Sequence, per Renesas' UM-140 CodeLess manual
(https://lpccs-docs.renesas.com/UM-140-DA145x-CodeLess/binarymode.html):
    1. "+++"            - escape sequence (no CR), suspends Binary mode locally
    2. "AT+BINREQEXIT\r" - completes the exit handshake
    3. "ATR\r"           - full module reset
All three go over the AT command characteristic (the same one AT+BINREQ
uses), not the DSPS data characteristic.

Usage:
    tools/venv/bin/python3 tools/ble_reset.py
"""

import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "CLv2-CodeLess"

AT_INBOUND_UUID = "914f8fb9-e8cd-411d-b7d1-14594de45425"
AT_OUTBOUND_UUID = "3bb535aa-50b2-4fbe-aa09-6b06dc59a404"
AT_FLOWCTRL_UUID = "e2048b39-d4f9-4a45-9f25-1856c10d5639"

SCAN_TIMEOUT_S = 10.0


async def main() -> int:
    print(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT_S:.0f}s)...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT_S)
    if device is None:
        print(f"Could not find a device advertising as '{DEVICE_NAME}'.")
        return 1

    print(f"Found {device.address}. Connecting...")
    async with BleakClient(device) as client:
        def on_at_flow(_handle, data: bytearray) -> None:
            print(f"< AT flow-control notify: {bytes(data)!r}")

        await client.start_notify(AT_FLOWCTRL_UUID, on_at_flow)

        print('Sending "+++" (escape, no CR)...')
        await client.write_gatt_char(AT_INBOUND_UUID, b"+++", response=True)
        await asyncio.sleep(1.0)

        print("Sending AT+BINREQEXIT...")
        await client.write_gatt_char(AT_INBOUND_UUID, b"AT+BINREQEXIT\r", response=True)
        await asyncio.sleep(1.0)
        try:
            reply = await client.read_gatt_char(AT_OUTBOUND_UUID)
            print(f"AT_OUTBOUND read: {bytes(reply)!r}")
        except Exception as e:
            print(f"AT_OUTBOUND read raised: {e!r}")

        print("Sending ATR (full reset)...")
        await client.write_gatt_char(AT_INBOUND_UUID, b"ATR\r", response=True)
        await asyncio.sleep(1.0)

        print("\nDone. The module should now be back to a clean, non-Binary-mode state.")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
