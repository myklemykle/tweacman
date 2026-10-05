#!/usr/bin/env python3
"""
ble_card_test.py - exercises SDResampleDemo's card management over BLE,
from the laptop instead of the phone.

Does what web/wav-remote.html does, with bleak, and checks the board's
answers: uploads a short test tone, cues it, overwrites it with a different
tone while it's playing, renames it, and deletes it - checking the stats
line's LVL:/CACHE: fields along the way. Every file it touches is one it
created itself (CARDTEST.WAV / CARDTST2.WAV), so it's safe on a real
sample card.

What it can't see from here is the board's cache folder: run the board's
Serial monitor alongside to watch the "Cache: ..." lines - an overwrite
should rebuild the copies, a rename shouldn't.

Optional: --leave NAME uploads a tone under NAME, cues it so its copies are
built, and leaves both on the card - for making an orphaned cache to test
the boot-time cleanup with.

Usage:
    tools/venv/bin/python3 tools/ble_card_test.py [--leave NAME]
"""

import argparse
import asyncio
import io
import math
import struct
import sys
import time
import wave

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "CLv2-CodeLess"

# Same UUIDs as ble_binary_test.py - see there for what each one does.
AT_INBOUND = "914f8fb9-e8cd-411d-b7d1-14594de45425"
AT_FLOWCTRL = "e2048b39-d4f9-4a45-9f25-1856c10d5639"
DSPS_TX = "0783b03e-8535-b5a0-7140-a304d2495cb8"
DSPS_FLOWCTRL = "0783b03e-8535-b5a0-7140-a304d2495cb9"
DSPS_RX = "0783b03e-8535-b5a0-7140-a304d2495cba"

# The page's upload pacing - see UPLOAD_WINDOW_BYTES in wav-remote.html for
# why the window has to stay under the board's 512-byte receive buffer.
UPLOAD_PACKET_BYTES = 60
UPLOAD_WINDOW_BYTES = 384

t0 = time.time()


def log(msg):
    print(f"{time.time() - t0:7.2f}  {msg}", flush=True)


def tone_wav(hz, seconds=0.3, rate=11025):
    """A short mono 16-bit sine, as WAV file bytes."""
    n = int(rate * seconds)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(
            struct.pack("<h", int(0.5 * 32767 * math.sin(2 * math.pi * hz * i / rate)))
            for i in range(n)))
    return buf.getvalue()


class Board:
    """The DSPS pipe as a stream of lines, plus the latest stats fields."""

    def __init__(self, client):
        self.client = client
        self.partial = b""
        self.lines = asyncio.Queue()
        self.stats = {}
        self.acked = 0

    def on_tx(self, _sender, data):
        self.partial += bytes(data)
        while b"\n" in self.partial:
            raw, self.partial = self.partial.split(b"\n", 1)
            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            if line.startswith("SPD:"):
                self.stats = dict(p.split(":", 1) for p in line.split(",") if ":" in p)
            elif line.startswith("ACK:"):
                self.acked = int(line[4:])
            else:
                log(f"< {line}")
                self.lines.put_nowait(line)

    async def send(self, text):
        log(f"> {text}")
        await self.client.write_gatt_char(DSPS_RX, text.encode(), response=False)

    async def expect(self, prefix, timeout=10.0):
        """Waits for the next non-stats line starting with prefix (or ERR:)."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = await asyncio.wait_for(self.lines.get(), end - time.time())
            except asyncio.TimeoutError:
                break
            if line.startswith(prefix):
                return line
            if line.startswith("ERR:"):
                raise RuntimeError(line)
        raise RuntimeError(f"timed out waiting for {prefix}")

    async def upload(self, name, data):
        self.acked = 0
        await self.send(f"PUT:{len(data)}:{name}")
        await self.expect("READY:")
        sent = 0
        while sent < len(data):
            if sent - self.acked >= UPLOAD_WINDOW_BYTES:
                await asyncio.sleep(0.01)
                continue
            n = min(UPLOAD_PACKET_BYTES, UPLOAD_WINDOW_BYTES - (sent - self.acked), len(data) - sent)
            await self.client.write_gatt_char(DSPS_RX, data[sent:sent + n], response=False)
            sent += n
        await self.expect("SAVED:", timeout=15)

    async def wait_cache_ready(self, timeout=20.0):
        # Only a stats line sent after this call counts - an older one
        # describes whatever was cued before.
        self.stats = {}
        end = time.time() + timeout
        while time.time() < end:
            if self.stats.get("CACHE") == "100":
                return
            await asyncio.sleep(0.2)
        raise RuntimeError(f"cache never reached 100% (stats: {self.stats})")


async def connect():
    for found in await BleakScanner.discover(timeout=10.0):
        if found.name == DEVICE_NAME:
            return found
    raise RuntimeError(f"couldn't find {DEVICE_NAME} - is a phone connected to it?")


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--leave", metavar="NAME")
    args = parser.parse_args()

    device = await connect()
    log(f"Connecting to {device.address}...")
    async with BleakClient(device) as client:
        board = Board(client)

        async def on_at_flow(_sender, _data):
            pass

        # The Binary mode handshake, in the order the page does it - see
        # ble_binary_test.py and SDResampleDemo.ino's BLE section.
        await client.start_notify(AT_FLOWCTRL, on_at_flow)
        await client.start_notify(DSPS_TX, board.on_tx)
        await client.start_notify(DSPS_FLOWCTRL, on_at_flow)
        await client.write_gatt_char(DSPS_FLOWCTRL, bytes([0x01]), response=False)
        await client.write_gatt_char(AT_INBOUND, b"AT+BINREQ\x00", response=True)
        log("Waiting for the pipe to open (first stats line)...")
        for _ in range(100):
            if board.stats:
                break
            await asyncio.sleep(0.1)
        if not board.stats:
            raise RuntimeError("no stats line - Binary mode didn't come up")
        missing = [k for k in ("LVL", "CACHE") if k not in board.stats]
        log(f"Stats line OK: {board.stats}" + (f"  MISSING {missing}" if missing else ""))

        if args.leave:
            await board.upload(args.leave, tone_wav(330))
            await board.send(f"PLAY:{args.leave}")
            await board.expect("PLAYING:")
            await board.wait_cache_ready()
            log(f"Left {args.leave} and its cached copies on the card.")
            return 0

        log("--- upload a new file ---")
        await board.upload("CARDTEST.WAV", tone_wav(440))
        await board.send("PLAY:CARDTEST.WAV")
        await board.expect("PLAYING:CARDTEST.WAV")
        await board.wait_cache_ready()
        log(f"cued, cache built: LVL={board.stats.get('LVL')} CACHE={board.stats.get('CACHE')}")

        log("--- overwrite it while it's playing (copies should be rebuilt) ---")
        await board.upload("CARDTEST.WAV", tone_wav(880))
        await board.expect("PLAYING:CARDTEST.WAV")
        await board.wait_cache_ready()

        log("--- rename it (copies should move, not rebuild) ---")
        await board.send("REN:CARDTEST.WAV/CARDTST2.WAV")
        await board.expect("RENAMED:CARDTEST.WAV/CARDTST2.WAV")
        await board.expect("PLAYING:CARDTST2.WAV")
        await board.wait_cache_ready()

        log("--- delete it (copies should go too) ---")
        await board.send("DEL:CARDTST2.WAV")
        await board.expect("DELETED:CARDTST2.WAV")
        await board.expect("PLAYING:")

        await board.send("LIST")
        names = []
        await board.expect("FILES:BEGIN")
        while True:
            line = await board.expect("")
            if line == "FILES:END":
                break
            names.append(line)
        leftovers = [n for n in names if n.upper() in ("CARDTEST.WAV", "CARDTST2.WAV")]
        log(f"LIST: {len(names)} files, test files left: {leftovers or 'none'}")
        log("PASS" if not leftovers and not missing else "FAIL")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except RuntimeError as err:
        log(f"FAIL: {err}")
        sys.exit(1)
