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

// -- Waiting for a transfer ---------------------------------------------------
// The core's wait_for_completition() can report a failed transfer as a good
// one. It marks the transfer "in progress" only once it's already running,
// so a result that arrives quickly - success or failure - is overwritten;
// it then falls back on the host's busy flag, which clears either way, and
// returns success. The caller gets whatever was in its buffer as if it had
// been read. And it gives up only after about 2.5 seconds. So this class
// marks the transfer before starting it, waits for the driver's own
// verdict, and gives up after about half a second.

const uint32_t TRANSFER_TIMEOUT_POLLS = 200000;  // ~2.5us each: about half a second
const uint32_t MISSED_EVENT_POLLS = 100;         // grace for a verdict after the host goes idle

int FastSDCardBlockDevice::finishTransfer(fsp_err_t started) {
  if (started != FSP_SUCCESS) {
    refused_++;  // e.g. the card was marked uninitialised, or a transfer is stuck
    return (int)started;
  }
  uint32_t idlePolls = 0;
  for (uint32_t i = 0; i < TRANSFER_TIMEOUT_POLLS; i++) {
    CmdStatus s = SDCardBlockDevice::st;
    if (s == CmdStatus::SUCCESS) return 0;
    if (s == CmdStatus::FAILED) {
      transferErrors_++;
      return FSP_ERR_TRANSFER_ABORTED;
    }
    // The host went idle but no verdict came. Not expected - the driver
    // reports every transfer's end - so count it, and trust the host.
    sdmmc_status_t status;
    if (R_SDHI_StatusGet(&ctrl, &status) == FSP_SUCCESS && !status.transfer_in_progress &&
        ++idlePolls > MISSED_EVENT_POLLS) {
      missedEvents_++;
      return 0;
    }
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MICROSECONDS);
  }
  timeouts_++;
  return FSP_ERR_TIMEOUT;
}

void FastSDCardBlockDevice::resetErrorCounts() {
  transferErrors_ = timeouts_ = refused_ = missedEvents_ = 0;
}

int FastSDCardBlockDevice::readAligned(void *buffer, bd_addr_t addr, bd_size_t size) {
  if (!multiBlock_) return SDCardBlockDevice::read(buffer, addr, size);

  if (!is_valid_read(addr, size)) return FSP_ERR_INVALID_ADDRESS;
  if (open() != BLOCK_DEVICE_OK) return FSP_ERR_NOT_OPEN;
  if (!initialized) {
    refused_++;  // marked uninitialised: card-detect blinked, or it was never started
    return FSP_ERR_NOT_INITIALIZED;
  }

  // One command for the whole request, instead of one per sector.
  SDCardBlockDevice::st = CmdStatus::IN_PROGRESS;  // before starting - see finishTransfer()
  return finishTransfer(R_SDHI_Read(&ctrl, (uint8_t *)buffer, addr / read_block_size,
                                    size / read_block_size));
}

int FastSDCardBlockDevice::programAligned(const void *buffer, bd_addr_t addr, bd_size_t size) {
  // Writes gain even more than reads from going multi-block: a card has to
  // program flash after every write command, so one command per sector
  // pays that cost per sector.
  if (!multiBlock_) return SDCardBlockDevice::program(buffer, addr, size);

  if (!is_valid_program(addr, size)) return FSP_ERR_INVALID_ADDRESS;
  if (open() != BLOCK_DEVICE_OK) return FSP_ERR_NOT_OPEN;
  if (!initialized) {
    refused_++;  // marked uninitialised: card-detect blinked, or it was never started
    return FSP_ERR_NOT_INITIALIZED;
  }

  SDCardBlockDevice::st = CmdStatus::IN_PROGRESS;
  return finishTransfer(R_SDHI_Write(&ctrl, (const uint8_t *)buffer, addr / write_block_size,
                                     size / write_block_size));
}

int FastSDCardBlockDevice::erase(bd_addr_t addr, bd_size_t size) {
  // An SD card's write replaces what was there, so FatFs's erase before
  // every write (and UsbMsd's before every sector) only costs time - an
  // erase command per sector, through the core's driver.
  if (!skipErase_) {
    dropFromCache(addr, size);
    return SDCardBlockDevice::erase(addr, size);
  }
  return is_valid_erase(addr, size) ? 0 : FSP_ERR_INVALID_ADDRESS;
}

// -- The sector cache --------------------------------------------------------
// FatFs reads directories and its allocation table one sector at a time,
// and (built in tiny mode, as the core builds it) keeps none of them, so a
// path lookup re-reads the same few directory sectors every time. Those
// single-sector reads are answered from a handful of 8-sector blocks
// instead - see RA6M5FastSD.h.

void FastSDCardBlockDevice::clearSectorCache() {
  for (uint8_t i = 0; i < LINES; i++) lines_[i].sectors = 0;
  cacheHits_ = 0;
  cacheMisses_ = 0;
}

void FastSDCardBlockDevice::setSectorCache(bool on) {
  cacheOn_ = on;
  clearSectorCache();
}

int FastSDCardBlockDevice::readCachedSector(void *buffer, bd_addr_t addr) {
  if (!is_valid_read(addr, SECTOR)) return FSP_ERR_INVALID_ADDRESS;  // also catches no card yet
  uint32_t sector = addr / SECTOR;
  uint32_t first = sector - sector % LINE_SECTORS;
  CacheLine *line = nullptr;
  for (uint8_t i = 0; i < LINES; i++) {
    if (lines_[i].sectors && sector >= lines_[i].first &&
        sector < lines_[i].first + lines_[i].sectors) {
      line = &lines_[i];
      break;
    }
  }
  if (line) {
    cacheHits_++;
  } else {
    // Replace the least recently used block with the one around this sector.
    line = &lines_[0];
    for (uint8_t i = 1; i < LINES; i++) {
      if (lines_[i].lastUsed < line->lastUsed) line = &lines_[i];
    }
    uint32_t cardSectors = size() / SECTOR;
    uint32_t count = LINE_SECTORS;
    if (first + count > cardSectors) count = cardSectors - first;
    line->sectors = 0;
    int rv = readAligned(line->data, (bd_addr_t)first * SECTOR, count * SECTOR);
    if (rv != 0) return rv;
    line->first = first;
    line->sectors = count;
    cacheMisses_++;
  }
  line->lastUsed = ++useClock_;
  memcpy(buffer, line->data + (sector - line->first) * SECTOR, SECTOR);
  return 0;
}

// After a write: copy the new data into any cached block it overlaps, so
// the cache always matches the card.
void FastSDCardBlockDevice::updateCache(const void *buffer, bd_addr_t addr, bd_size_t size) {
  uint32_t from = addr / SECTOR, to = (addr + size) / SECTOR;  // [from, to)
  for (uint8_t i = 0; i < LINES; i++) {
    CacheLine &l = lines_[i];
    if (!l.sectors) continue;
    uint32_t lo = from > l.first ? from : l.first;
    uint32_t hi = to < l.first + l.sectors ? to : l.first + l.sectors;
    if (lo < hi) {
      memcpy(l.data + (lo - l.first) * SECTOR, (const uint8_t *)buffer + (lo - from) * SECTOR,
             (hi - lo) * SECTOR);
    }
  }
}

// After an erase or a failed write, when what's on the card isn't known.
void FastSDCardBlockDevice::dropFromCache(bd_addr_t addr, bd_size_t size) {
  uint32_t from = addr / SECTOR, to = (addr + size) / SECTOR;
  for (uint8_t i = 0; i < LINES; i++) {
    CacheLine &l = lines_[i];
    if (l.sectors && from < l.first + l.sectors && to > l.first) l.sectors = 0;
  }
}

int FastSDCardBlockDevice::init() {
  clearSectorCache();
  return SDCardBlockDevice::init();
}

int FastSDCardBlockDevice::deinit() {
  clearSectorCache();
  return SDCardBlockDevice::deinit();
}

// -- Reads and writes -------------------------------------------------------

int FastSDCardBlockDevice::read(void *buffer, bd_addr_t addr, bd_size_t size) {
  if (cacheOn_ && size == SECTOR) return readCachedSector(buffer, addr);
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
  int rv = 0;
  if (isAligned(buffer)) {
    rv = programAligned(buffer, addr, size);
  } else {
    const uint8_t *src = (const uint8_t *)buffer;
    for (bd_size_t done = 0; done < size && rv == 0;) {
      bd_size_t n = size - done < sizeof(bounce) ? size - done : sizeof(bounce);
      memcpy(bounce, src + done, n);
      rv = programAligned(bounce, addr + done, n);
      done += n;
    }
  }
  if (rv == 0) {
    updateCache(buffer, addr, size);
  } else {
    dropFromCache(addr, size);
  }
  return rv;
}
