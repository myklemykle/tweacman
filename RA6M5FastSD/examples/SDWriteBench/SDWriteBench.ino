// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  SDWriteBench - SparkFun Thing Plus RA6M5

  Measures SD card write throughput, to find out whether slow writes come
  from the card or from the way they're sent to it. SDResampleDemo's cache
  builds write about 0.35MB/s (8KB fwrite()s), and its USB disk mode copies
  onto the card at about 115KB/s, from a card rated for 10MB/s sequential
  writes. Three groups of tests, the companion to SDReadBench:

    1. Raw sector writes, below the filesystem: the same stretch of card
       written sequentially, from one sector to 128KB per command, through
       the core's SDCardBlockDevice (one command per sector) and this
       library's FastSDCardBlockDevice (one command per request).
    2. USB disk mode's pattern: the core's UsbMsd library writes every
       512-byte sector the computer sends as a read, an erase and a write,
       each its own command. Measured with the core's erase, and with this
       library's, which skips it (an SD card doesn't need one).
    3. Through FatFs, the way a sketch writes files: unbuffered fwrite()s to
       a new file in 8KB (MipCache's size), 32KB and 128KB pieces; the same
       with the core's erase, which FATFileSystem asks for before every
       write; and rewriting an existing file, which needs no new clusters,
       to show what the filesystem's own bookkeeping costs.

  Each result shows the speed and the slowest single call - the longest a
  sketch would be stalled waiting for one write.

  Safety: raw writes would wreck the filesystem if they landed anywhere
  FatFs keeps track of, so they only go into the sectors of a scratch file
  this sketch creates (WBENCH.TMP), and only after checking that file sits
  in one unbroken run on the card. To FatFs, that file's contents change
  and nothing else does. Everything the sketch creates is deleted at the
  end. Needs about 3MB free; nothing else on the card is touched.

  Wiring: just an SD card, FAT32-formatted, in the slot.

  How to test: open the serial monitor. The benchmark runs as soon as the
  monitor connects (the USB serial port drops anything printed before
  then), takes about 20 seconds, and runs again whenever you send any
  character. Each test writes for about a second, or until it's written
  its full amount.
*/

#include <RA6M5FastSD.h>
#include "FATFileSystem.h"
#include <stdio.h>
#include <string.h>

// Sits between FatFs and the real SD driver, counting every read and
// write FatFs asks for, so the file-level tests can show how many extra
// sectors the filesystem's own bookkeeping costs.
class CountingBlockDevice : public BlockDevice {
 public:
  explicit CountingBlockDevice(BlockDevice &inner) : inner_(inner) {}

  uint32_t readCalls = 0, sectorsRead = 0;
  uint32_t programCalls = 0, sectorsWritten = 0;
  // Where the first multi-sector read since resetCounts() came from - file
  // data, not one of FatFs's single-sector table reads.
  bool haveDataRead = false;
  bd_addr_t firstDataReadAddr = 0;

  void resetCounts() {
    readCalls = sectorsRead = programCalls = sectorsWritten = 0;
    haveDataRead = false;
  }

  int init() override { return inner_.init(); }
  int deinit() override { return inner_.deinit(); }
  int sync() override { return inner_.sync(); }
  int read(void *buffer, bd_addr_t addr, bd_size_t size) override {
    readCalls++;
    sectorsRead += size / 512;
    if (size > 512 && !haveDataRead) {
      haveDataRead = true;
      firstDataReadAddr = addr;
    }
    return inner_.read(buffer, addr, size);
  }
  int program(const void *buffer, bd_addr_t addr, bd_size_t size) override {
    programCalls++;
    sectorsWritten += size / 512;
    return inner_.program(buffer, addr, size);
  }
  int erase(bd_addr_t addr, bd_size_t size) override { return inner_.erase(addr, size); }
  int trim(bd_addr_t addr, bd_size_t size) override { return inner_.trim(addr, size); }
  bd_size_t get_read_size() const override { return inner_.get_read_size(); }
  bd_size_t get_program_size() const override { return inner_.get_program_size(); }
  bd_size_t get_erase_size() const override { return inner_.get_erase_size(); }
  bd_size_t get_erase_size(bd_addr_t addr) const override { return inner_.get_erase_size(addr); }
  int get_erase_value() const override { return inner_.get_erase_value(); }
  bd_size_t size() const override { return inner_.size(); }
  const char *get_type() const override { return inner_.get_type(); }

 private:
  BlockDevice &inner_;
};

FastSDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                    PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
CountingBlockDevice countingDevice(sdBlockDevice);
FATFileSystem sdFs("sd");

const char *SCRATCH_PATH = "/sd/WBENCH.TMP";  // raw writes land inside this file
const char *NEW_FILE_PATH = "/sd/WBENCH2.TMP";
const uint32_t SCRATCH_BYTES = 1024 * 1024;
const uint32_t NEW_FILE_MAX_BYTES = 2 * 1024 * 1024;
const uint32_t TEST_MICROS = 1000000;  // each test stops after about this long

// 4-byte aligned: the SD host moves data by DMA in 32-bit words, and an
// unaligned buffer has to go through the library's bounce buffer instead.
alignas(4) uint8_t buf[128 * 1024];
alignas(4) uint8_t checkBuf[4096];

bd_addr_t scratchBase = 0;  // the scratch file's first byte on the card
FILE *testFile = nullptr;
bool wasConnected = false;

// -- Timing -------------------------------------------------------------------

struct Timing {
  uint32_t bytes, calls, elapsed, worst;  // elapsed and worst in microseconds
};

// One write of `size` bytes, `offset` bytes into the test. Returns 0 on
// success, like the block device calls.
typedef int (*WriteStep)(uint32_t offset, uint32_t size);

// Calls step() over and over, `size` bytes at a time, until it's written
// maxBytes or about TEST_MICROS have passed.
Timing runTimed(uint32_t size, uint32_t maxBytes, WriteStep step) {
  Timing t = {0, 0, 0, 0};
  uint32_t start = micros();
  while (t.bytes < maxBytes && micros() - start < TEST_MICROS) {
    uint32_t callStart = micros();
    int rv = step(t.bytes, size);
    uint32_t took = micros() - callStart;
    if (rv != 0) {
      Serial.print("  write failed, error ");
      Serial.println(rv);
      break;
    }
    if (took > t.worst) t.worst = took;
    t.bytes += size;
    t.calls++;
  }
  t.elapsed = micros() - start;
  return t;
}

void printResult(const char *label, const Timing &t) {
  if (t.calls == 0) return;
  float mbps = (float)t.bytes / (float)t.elapsed;  // bytes per microsecond == MB/s
  char line[140];
  snprintf(line, sizeof(line), "  %-42s %6.3f MB/s   %6.2f ms per call, worst %6.2f ms", label,
           mbps, t.elapsed / 1000.0f / t.calls, t.worst / 1000.0f);
  Serial.println(line);
}

// Fills buf with a counting pattern, different for each seed, so a
// read-back can tell one write's data from another's.
void fillPattern(uint32_t seed) {
  uint32_t *words = (uint32_t *)buf;
  for (uint32_t i = 0; i < sizeof(buf) / 4; i++) words[i] = seed * 0x9E3779B9u + i;
}

// -- The scratch file -----------------------------------------------------------

// Creates the scratch file, finds where on the card its data lives, and
// checks that it's all one unbroken run of sectors - read back 4KB at a
// time, each piece must come from exactly where the first one did plus its
// offset. Only then is it safe to write raw sectors inside it.
bool createScratchFile() {
  remove(SCRATCH_PATH);
  FILE *f = fopen(SCRATCH_PATH, "wb");
  if (!f) {
    Serial.println("Couldn't create the scratch file - is the card full or write-protected?");
    return false;
  }
  setvbuf(f, nullptr, _IONBF, 0);
  fillPattern(0);
  for (uint32_t done = 0; done < SCRATCH_BYTES; done += sizeof(buf)) {
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
      fclose(f);
      Serial.println("Couldn't write the scratch file - is there 3MB free?");
      return false;
    }
  }
  fclose(f);

  f = fopen(SCRATCH_PATH, "rb");
  setvbuf(f, nullptr, _IONBF, 0);
  bool contiguous = true;
  uint32_t breakOffset = 0;
  for (uint32_t offset = 0; offset < SCRATCH_BYTES && contiguous; offset += sizeof(checkBuf)) {
    countingDevice.resetCounts();
    fread(checkBuf, 1, sizeof(checkBuf), f);
    if (!countingDevice.haveDataRead) {
      contiguous = false;  // clusters under 1KB: can't tell where the data is
    } else if (offset == 0) {
      scratchBase = countingDevice.firstDataReadAddr;
    } else if (countingDevice.firstDataReadAddr != scratchBase + offset) {
      contiguous = false;
    }
    if (!contiguous) breakOffset = offset;
  }
  fclose(f);
  if (!contiguous) {
    char line[120];
    snprintf(line, sizeof(line), "The scratch file (from card sector %lu) breaks %luKB in.",
             (unsigned long)(scratchBase / 512), (unsigned long)(breakOffset / 1024));
    Serial.println(line);
    Serial.println("It isn't in one piece on the card, so raw writes into it");
    Serial.println("wouldn't be safe. Free up some space (or reformat the card) and try again.");
    remove(SCRATCH_PATH);
    return false;
  }
  return true;
}

// Writes a fresh pattern with the fast driver and reads it back with the
// core's own driver: a fast write that wrote the wrong thing would make
// every number below meaningless.
bool fastWriteChecksOut() {
  fillPattern(1);
  sdBlockDevice.program(buf, scratchBase, sizeof(checkBuf));
  sdBlockDevice.setMultiBlock(false);
  sdBlockDevice.read(checkBuf, scratchBase, sizeof(checkBuf));
  sdBlockDevice.setMultiBlock(true);
  return memcmp(buf, checkBuf, sizeof(checkBuf)) == 0;
}

// -- Raw sector writes ----------------------------------------------------------

// Every raw write wraps round inside the scratch file. (Each test's size
// divides SCRATCH_BYTES, so no write ever crosses its end.)
bd_addr_t scratchAddr(uint32_t offset) { return scratchBase + offset % SCRATCH_BYTES; }

int rawWrite(uint32_t offset, uint32_t size) {
  return sdBlockDevice.program(buf, scratchAddr(offset), size);
}

// USB disk mode's way, one sector at a time, as USBMSD::write() in the
// core's UsbMsd library does it: read the sector (in case only part of it
// is being replaced), erase it, write it.
int usbDiskWrite(uint32_t offset, uint32_t size) {
  bd_addr_t addr = scratchAddr(offset);
  int rv = sdBlockDevice.read(checkBuf, addr, 512);
  if (rv == 0) rv = sdBlockDevice.erase(addr, 512);
  if (rv == 0) rv = sdBlockDevice.program(checkBuf, addr, 512);
  return rv;
}

void benchUsbDisk(bool skipErase) {
  sdBlockDevice.setSkipErase(skipErase);
  Timing t = runTimed(512, SCRATCH_BYTES, usbDiskWrite);
  sdBlockDevice.setSkipErase(true);
  printResult(skipErase ? "read + write (erase skipped)" : "read + erase + write (core's erase)", t);
}

void benchRaw(bool multiBlock, uint32_t size) {
  sdBlockDevice.setMultiBlock(multiBlock);
  Timing t = runTimed(size, SCRATCH_BYTES, rawWrite);
  sdBlockDevice.setMultiBlock(true);
  char label[64];
  if (size < 1024) {
    snprintf(label, sizeof(label), "%s, %3lu B per call", multiBlock ? "fast driver" : "core driver",
             (unsigned long)size);
  } else {
    snprintf(label, sizeof(label), "%s, %3luKB per call", multiBlock ? "fast driver" : "core driver",
             (unsigned long)(size / 1024));
  }
  printResult(label, t);
}

// -- Through FatFs ----------------------------------------------------------------

int fileWrite(uint32_t offset, uint32_t size) {
  return fwrite(buf, 1, size, testFile) == size ? 0 : -1;
}

// rewrite: overwrite the scratch file in place, so FatFs never has to
// find a new cluster; otherwise write a new file, which it has to grow.
// coreErase: let FATFileSystem's erase before each write through to the
// card, one command per sector, as the core's driver does it.
void benchFile(uint32_t size, bool rewrite, bool coreErase = false) {
  testFile = fopen(rewrite ? SCRATCH_PATH : NEW_FILE_PATH, rewrite ? "r+b" : "wb");
  if (!testFile) {
    Serial.println("  couldn't open a file to write");
    return;
  }
  setvbuf(testFile, nullptr, _IONBF, 0);
  fillPattern(2);
  countingDevice.resetCounts();
  sdBlockDevice.setSkipErase(!coreErase);
  Timing t = runTimed(size, rewrite ? SCRATCH_BYTES : NEW_FILE_MAX_BYTES, fileWrite);
  // Closing writes FatFs's last table and directory updates - part of the cost.
  uint32_t closeStart = micros();
  fclose(testFile);
  t.elapsed += micros() - closeStart;
  sdBlockDevice.setSkipErase(true);
  if (!rewrite) remove(NEW_FILE_PATH);

  char label[64];
  snprintf(label, sizeof(label), "%s, %3luKB per fwrite%s", rewrite ? "rewrite file" : "new file",
           (unsigned long)(size / 1024), coreErase ? ", core's erase" : "");
  printResult(label, t);
  if (t.calls == 0) return;
  // The data itself is size/512 sectors per fwrite; anything over that is
  // FatFs's file allocation table and directory.
  char line[140];
  snprintf(line, sizeof(line), "  %-42s %5.1f sectors written (%lu data) in %4.1f commands, %4.1f read, per fwrite",
           "", (float)countingDevice.sectorsWritten / t.calls, (unsigned long)(size / 512),
           (float)countingDevice.programCalls / t.calls, (float)countingDevice.sectorsRead / t.calls);
  Serial.println(line);
}

// -- Main -----------------------------------------------------------------------

void runBenchmarks() {
  Serial.println();
  Serial.println("SDWriteBench");
  Serial.println("Creating a 1MB scratch file to write raw sectors into...");
  if (!createScratchFile()) return;
  char line[100];
  snprintf(line, sizeof(line), "Scratch file at card sector %lu, in one piece.",
           (unsigned long)(scratchBase / 512));
  Serial.println(line);
  if (!fastWriteChecksOut()) {
    Serial.println("Fast-driver write read back WRONG - results below are invalid.");
  }

  Serial.println();
  Serial.println("Raw sector writes, sequential, below the filesystem:");
  benchRaw(false, 32768);
  benchRaw(true, 512);
  benchRaw(true, 4096);
  benchRaw(true, 8192);
  benchRaw(true, 32768);
  benchRaw(true, 131072);

  Serial.println();
  Serial.println("USB disk mode's pattern, one sector per call:");
  benchUsbDisk(false);
  benchUsbDisk(true);

  Serial.println();
  Serial.println("Through FatFs, unbuffered fwrite():");
  benchFile(8192, false);
  benchFile(32768, false);
  benchFile(131072, false);
  benchFile(32768, false, true);
  benchFile(131072, false, true);
  benchFile(32768, true);

  remove(SCRATCH_PATH);
  Serial.println();
  Serial.println("Done - send any character to run again.");
}

void setup() {
  Serial.begin(115200);
  if (sdFs.mount(&countingDevice) != 0) {
    while (true) {
      if (Serial) Serial.println("SD card mount failed - check it's inserted and FAT32-formatted.");
      delay(2000);
    }
  }
  // Clear out anything a run that was cut short left behind.
  remove(SCRATCH_PATH);
  remove(NEW_FILE_PATH);
}

void loop() {
  // Native USB serial: wait until a monitor is actually listening.
  bool connected = Serial;
  if (connected && !wasConnected) {
    delay(200);  // let the host finish opening the port
    runBenchmarks();
  }
  wasConnected = connected;

  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    runBenchmarks();
  }
}
