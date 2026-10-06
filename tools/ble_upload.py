#!/usr/bin/env python3
"""
ble_upload.py - uploads files to SDResampleDemo's SD card over BLE, from the
laptop: the same PUT: protocol web/wav-remote.html uses, via the connection
and upload code in ble_card_test.py.

Slow - about 1.8KB/s, the ceiling of the board's Bluetooth module - so this
suits short samples and test files, not filling a card. Files go to the
card's root under their own names (which must end in .wav, and existing
files of the same name are replaced).

Usage:
    tools/venv/bin/python3 tools/ble_upload.py FILE [FILE ...]
"""

import asyncio
import os
import sys
import time

from ble_card_test import PipeLink, find_board, log


async def main(paths):
    device = await find_board()
    async with PipeLink(device) as board:
        total = len(paths)
        for i, path in enumerate(paths, 1):
            name = os.path.basename(path)
            with open(path, "rb") as f:
                data = f.read()
            started = time.time()
            await board.upload(name, data)
            took = time.time() - started
            log(f"[{i}/{total}] {name}: {len(data) / 1024:.1f} KB in {took:.0f}s "
                f"({len(data) / 1024 / took:.1f} KB/s)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    try:
        sys.exit(asyncio.run(main(sys.argv[1:])))
    except RuntimeError as err:
        log(f"FAIL: {err}")
        sys.exit(1)
