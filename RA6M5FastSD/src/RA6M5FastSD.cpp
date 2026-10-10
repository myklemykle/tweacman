// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

#include <Arduino.h>
#include "BlockDevice.h"
#include "storage_retarget.h"
#include "r_sdhi.h"
#include "r_dmac.h"

// The core's SDCardBlockDevice keeps its FSP driver handle (ctrl) and its
// completion-wait helper private. Every header that class depends on is
// already included above, so this #define opens up that one class and
// nothing else, in this file only. A deliberate shortcut, not a pattern to
// copy - see RA6M5FastSD.h.
#define private public
#include "SDCardBlockDevice.h"
#undef private

#include "RA6M5FastSD.h"

// The SD host moves data by DMA in 32-bit words, and a buffer that isn't
// 4-byte aligned comes back garbled - measured: nearly every byte wrong,
// through the core's own driver just as through the fast path. That's a
// real trap, not a corner case: FatFs hands a caller's buffer straight to
// the driver for whole sectors, at an offset set by the file position, so
// an unbuffered fread() from an odd 16-bit sample position lands its
// sectors misaligned. (Buffered stdio hides it, by always reading into its
// own aligned buffer.) So any misaligned transfer goes through this
// aligned bounce buffer instead, a few sectors at a time - correct for any
// buffer, and only a memcpy slower.
alignas(4) static uint8_t bounce[4096];

static bool isAligned(const void *p) { return ((uintptr_t)p & 3) == 0; }

int FastSDCardBlockDevice::readAligned(void *buffer, bd_addr_t addr, bd_size_t size) {
  if (!multiBlock_) return SDCardBlockDevice::read(buffer, addr, size);

  if (!is_valid_read(addr, size)) return FSP_ERR_INVALID_ADDRESS;
  if (open() != BLOCK_DEVICE_OK) return FSP_ERR_NOT_OPEN;
  if (!initialized) return FSP_ERR_NOT_INITIALIZED;

  // One command for the whole request, instead of one per sector.
  fsp_err_t rv = R_SDHI_Read(&ctrl, (uint8_t *)buffer, addr / read_block_size,
                             size / read_block_size);
  if (rv == FSP_SUCCESS) rv = wait_for_completition();
  return (int)rv;
}

int FastSDCardBlockDevice::programAligned(const void *buffer, bd_addr_t addr, bd_size_t size) {
  // Writes gain even more than reads from going multi-block: a card has to
  // program flash after every write command, so one command per sector
  // pays that cost per sector.
  if (!multiBlock_) return SDCardBlockDevice::program(buffer, addr, size);

  if (!is_valid_program(addr, size)) return FSP_ERR_INVALID_ADDRESS;
  if (open() != BLOCK_DEVICE_OK) return FSP_ERR_NOT_OPEN;
  if (!initialized) return FSP_ERR_NOT_INITIALIZED;

  fsp_err_t rv = R_SDHI_Write(&ctrl, (const uint8_t *)buffer, addr / write_block_size,
                              size / write_block_size);
  if (rv == FSP_SUCCESS) rv = wait_for_completition();
  return (int)rv;
}

int FastSDCardBlockDevice::read(void *buffer, bd_addr_t addr, bd_size_t size) {
  if (isAligned(buffer)) return readAligned(buffer, addr, size);
  uint8_t *dst = (uint8_t *)buffer;
  for (bd_size_t done = 0; done < size;) {
    bd_size_t n = size - done < sizeof(bounce) ? size - done : sizeof(bounce);
    int rv = readAligned(bounce, addr + done, n);
    if (rv != 0) return rv;
    memcpy(dst + done, bounce, n);
    done += n;
  }
  return 0;
}

int FastSDCardBlockDevice::program(const void *buffer, bd_addr_t addr, bd_size_t size) {
  if (isAligned(buffer)) return programAligned(buffer, addr, size);
  const uint8_t *src = (const uint8_t *)buffer;
  for (bd_size_t done = 0; done < size;) {
    bd_size_t n = size - done < sizeof(bounce) ? size - done : sizeof(bounce);
    memcpy(bounce, src + done, n);
    int rv = programAligned(bounce, addr + done, n);
    if (rv != 0) return rv;
    done += n;
  }
  return 0;
}
