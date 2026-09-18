#!/usr/bin/env python3
"""
ble_dsps_test.py - one-off diagnostic: performs the CodeLess Binary mode/
DSPS handshake against the SparkFun Thing Plus RA6M5's DA14531 module
(same as SDResampleDemo/web/wav-remote.html) using bleak instead of a
browser, then sends a test command and prints anything that comes back.

Not a permanent project tool - used to isolate whether an inbound-command
problem is in the browser/Web Bluetooth layer or somewhere below it
(module/UART/sketch), by bypassing the browser entirely.

Usage:
    tools/venv/bin/python3 tools/ble_dsps_test.py
"""

import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "CLv2-CodeLess"

AT_SERVICE_UUID = "866d3b04-e674-40dc-9c05-b7f91bec6e83"
AT_INBOUND_UUID = "914f8fb9-e8cd-411d-b7d1-14594de45425"
AT_FLOWCTRL_UUID = "e2048b39-d4f9-4a45-9f25-1856c10d5639"

DSPS_TX_UUID = "0783b03e-8535-b5a0-7140-a304d2495cb8"
DSPS_FLOWCTRL_UUID = "0783b03e-8535-b5a0-7140-a304d2495cb9"
DSPS_RX_UUID = "0783b03e-8535-b5a0-7140-a304d2495cba"

SCAN_TIMEOUT_S = 10.0


def on_dsps_tx(_handle, data: bytearray) -> None:
    print(f"< DSPS TX: {bytes(data)!r}")


def on_at_flow(_handle, data: bytearray) -> None:
    print(f"< AT flow-control notify: {bytes(data)!r}")


def on_dsps_flow(_handle, data: bytearray) -> None:
    print(f"< DSPS flow-control notify: {bytes(data)!r}")


async def main() -> int:
    print(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT_S:.0f}s)...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT_S)
    if device is None:
        print(f"Could not find a device advertising as '{DEVICE_NAME}'.")
        return 1

    print(f"Found {device.address}. Connecting...")
    async with BleakClient(device) as client:
        print("Connected. Starting notifications...")
        await client.start_notify(AT_FLOWCTRL_UUID, on_at_flow)
        await client.start_notify(DSPS_TX_UUID, on_dsps_tx)
        await client.start_notify(DSPS_FLOWCTRL_UUID, on_dsps_flow)

        print("Writing 0x01 to DSPS flow-control...")
        await client.write_gatt_char(DSPS_FLOWCTRL_UUID, bytes([0x01]), response=False)
        await asyncio.sleep(1.0)

        print("Requesting Binary mode (AT+BINREQ), with response=True...")
        await client.write_gatt_char(AT_INBOUND_UUID, b"AT+BINREQ\x00", response=True)

        print("Waiting 3s for the board to ack BINREQ...")
        await asyncio.sleep(3.0)

        for payload in (b"LIST", b"LIST\n", b"LIST\r\n"):
            print(f"\nSending {payload!r} to DSPS RX (write-without-response)...")
            await client.write_gatt_char(DSPS_RX_UUID, payload, response=False)
            await asyncio.sleep(2.0)

        print("\nSending LIST once more, with response=True this time...")
        try:
            await client.write_gatt_char(DSPS_RX_UUID, b"LIST", response=True)
        except Exception as e:
            print(f"write with response=True raised: {e!r}")
        await asyncio.sleep(2.0)

        print("\nDone. Watch the board's own Serial trace for [BLE raw] lines during this run.")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
