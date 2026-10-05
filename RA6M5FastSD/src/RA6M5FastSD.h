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

  // Turns the multi-block path off (or back on) for both reads and writes,
  // so a sketch can compare against the core's original
  // one-sector-per-command behavior. (Misaligned buffers are still handled
  // correctly either way.)
  void setMultiBlock(bool on) { multiBlock_ = on; }

 private:
  int readAligned(void *buffer, bd_addr_t addr, bd_size_t size);
  int programAligned(const void *buffer, bd_addr_t addr, bd_size_t size);

  bool multiBlock_ = true;
};
