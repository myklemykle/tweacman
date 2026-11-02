// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  RA6M5FastSD - faster reads and writes for the Renesas Arduino core's SD
  driver.

  The core's SDCardBlockDevice::read() splits every request into 512-byte
  sectors and sends the card one command per sector, waiting for each to
  finish before sending the next. Per-command overhead dominates, so reads
  top out around 2.2MB/s however much you ask for at once - on a card that
  can go much faster. FastSDCardBlockDevice asks the SD host peripheral for
  the whole request in a single multi-block command instead. Measured on a
  SanDisk Ultra card with this library's SDReadBench example:

    one sector per command (the core)      2.2 MB/s
    4 sectors per command                  5.4 MB/s
    8 sectors per command                  8.9 MB/s
    64 sectors per command                18.5 MB/s

  Writes get the same treatment: the core writes one sector per command
  too, and since a card programs its flash after every write command, that
  pays the card's programming time once per 512 bytes. Writing a run of
  sectors in one command pays it once.

    one sector per command (the core)      0.44 MB/s
    8 sectors per command                  3.2 MB/s
    64 sectors per command                 8 MB/s
    256 sectors per command               14 MB/s

  And erases are skipped. The core's FATFileSystem erases every run of
  sectors before writing it (as does its UsbMsd library, every sector),
  and the core's driver does that with one erase command per sector -
  which an SD card doesn't need (a write replaces whatever was there), and
  which costs far more than the write itself. Measured with this library's
  SDWriteBench example, writing a new file in 32KB fwrite()s:

    erase per sector, then write (the core)   0.36 MB/s
    write only                                 4.4 MB/s

  (FatFs writes at most one cluster per command - 16KB on the bench card -
  so files don't reach the raw speeds above.)

  And single-sector reads are cached. The core builds FatFs in its "tiny"
  mode, which keeps just one sector in memory, so every time a file is
  opened, created, renamed or deleted it reads every directory on the way
  to it again from the card, one sector per command - about 0.25ms each.
  For a file a few folders down with long names that's ~36 reads, ~9ms per
  lookup. This class keeps the last few blocks of 8 sectors it was asked
  for one sector of (32KB in all), reading each whole block in one command
  - about the cost of two single reads - since directories are read
  sector after sector. Every write updates whatever it overlaps in the
  cache, so it never goes stale, whoever does the writing through this
  object (FatFs, or the core's UsbMsd library). Bigger reads, which are
  file data, aren't cached.

  It's a drop-in replacement: same constructor, and FATFileSystem mounts it
  the same way.

  Two things the caller controls decide whether reads and writes actually
  get faster:
  - Request size. FatFs passes a run of whole sectors straight through, so a
    16KB fread() or fwrite() becomes one 32-sector command - but only if
    nothing above FatFs chops it up first. The C library's FILE buffering
    does exactly that, splitting every call into 1KB pieces; turn it off
    with setvbuf(file, nullptr, _IONBF, 0) right after fopen(). FatFs also
    only does this for whole sectors, so writes should stay sector-aligned
    within the file (e.g. always write multiples of 512 bytes).
  - Buffer alignment. The SD host moves data by DMA in 32-bit words, and
    data bound for a buffer that isn't 4-byte aligned comes back garbled -
    through the core's own driver too, which is a real bug there (see the
    comment on `bounce` in RA6M5FastSD.cpp). This class routes misaligned
    transfers through an aligned buffer of its own, so they're always
    correct, just a copy slower. For full speed, keep buffers aligned
    (alignas(4)) - and remember FatFs puts whole sectors at an offset set by
    the file position, so a buffer only stays aligned for them if its
    address matches the file position mod 4.

  Experimental: the core keeps its driver handle private, so this reaches
  in with a #define in RA6M5FastSD.cpp rather than replacing the driver
  outright. Fine for trying out; a cleaner version would be a standalone
  driver written against the Renesas FSP API directly.
*/

#pragma once

#include "SDCardBlockDevice.h"

class FastSDCardBlockDevice : public SDCardBlockDevice {
 public:
  using SDCardBlockDevice::SDCardBlockDevice;  // same pins, same constructor

  int read(void *buffer, bd_addr_t addr, bd_size_t size) override;
  int program(const void *buffer, bd_addr_t addr, bd_size_t size) override;
  int erase(bd_addr_t addr, bd_size_t size) override;
  int init() override;    // both empty the sector cache: it may be a
  int deinit() override;  // different card next time

  // Turns the multi-block path off (or back on) for both reads and writes,
  // so a sketch can compare against the core's original
  // one-sector-per-command behavior. (Misaligned buffers are still handled
  // correctly either way.)
  void setMultiBlock(bool on) { multiBlock_ = on; }

  // Turns skipping erases off (or back on), so erase() erases the way the
  // core's driver does - one command per sector - for comparison.
  void setSkipErase(bool on) { skipErase_ = on; }

  // Turns the sector cache off (or back on), for comparison. Turning it
  // either way empties it.
  void setSectorCache(bool on);
  // Empties the sector cache - only needed if something changed the card
  // other than through this object.
  void clearSectorCache();
  // How many single-sector reads the cache answered, and how many it had
  // to read from the card, since the last clearSectorCache().
  uint32_t sectorCacheHits() const { return cacheHits_; }
  uint32_t sectorCacheMisses() const { return cacheMisses_; }

 private:
  int readAligned(void *buffer, bd_addr_t addr, bd_size_t size);
  int programAligned(const void *buffer, bd_addr_t addr, bd_size_t size);
  int readCachedSector(void *buffer, bd_addr_t addr);
  void updateCache(const void *buffer, bd_addr_t addr, bd_size_t size);
  void dropFromCache(bd_addr_t addr, bd_size_t size);

  static const uint32_t SECTOR = 512;
  static const uint32_t LINE_SECTORS = 8;  // read in one command
  static const uint8_t LINES = 8;          // 8 x 4KB = 32KB

  struct CacheLine {
    alignas(4) uint8_t data[LINE_SECTORS * SECTOR];
    uint32_t first = 0;      // its first sector
    uint32_t sectors = 0;    // how many it holds (fewer at the card's end); 0 = empty
    uint32_t lastUsed = 0;   // for replacing the least recently used
  };
  CacheLine lines_[LINES];
  uint32_t useClock_ = 0;
  uint32_t cacheHits_ = 0;
  uint32_t cacheMisses_ = 0;
  bool cacheOn_ = true;

  bool multiBlock_ = true;
  bool skipErase_ = true;
};
