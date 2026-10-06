/*
  SDReadBench - SparkFun Thing Plus RA6M5

  Measures SD card read throughput, to find where SDResampleDemo's speed
  ceiling comes from (~20x forward, ~13x backward on a 44.1kHz mono 16-bit
  file - about 1.76MB/s and 1.15MB/s - from a card rated far faster) and
  how much each fix buys back. Two levels of the storage stack:

    1. Raw sector reads, below the filesystem: the core's own
       SDCardBlockDevice::read(), which sends one command per 512-byte
       sector, against this library's FastSDCardBlockDevice, which sends
       one multi-block command for the whole request.
    2. SDResampleDemo's file access pattern, through fopen/fseek/fread and
       FatFs: chunks read forward, then chunks read backward (each one an
       ordinary forward read starting one chunk below the last), exactly
       as WavReader::readSamplesAt() does it. This is run step by step
       from the demo's original setup to the fully fixed one, since each
       fix only pays off once the layers above it stop chopping requests
       into small pieces.

  Backward reads are slower for a reason that has nothing to do with the
  card: FatFs only knows each cluster's *next* cluster, and the core builds
  it without its fast-seek option, so every backward seek re-walks the
  file's cluster chain from its first cluster. The deeper into the file,
  the more FAT sectors that walk reads - which is why backward is measured
  near the file's end, its worst case. Bigger chunks don't shorten the walk,
  but they do it less often.

  A counting wrapper sits between FatFs and the SD driver, so each result
  also shows how many driver calls and sectors one chunk actually cost.
  Speeds are also shown as the playback ratio they'd support for a 44.1kHz
  mono 16-bit file, to compare directly against SDResampleDemo's knob.

  Wiring: just an SD card with at least one .WAV file at its root of 2MB or
  more. The largest .WAV is used; its format doesn't matter, since only its
  sample data's position is used. Nothing is written.

  How to test: open the serial monitor. The benchmark runs as soon as the
  monitor connects (the USB serial port drops anything printed before
  then), takes about 10 seconds, and runs again whenever you send any
  character.

  This example code is in the public domain.
*/

#include <RA6M5FastSD.h>
#include "FATFileSystem.h"
#include <stdio.h>
#include <string.h>

// Sits between FatFs and the real SD driver, counting every read FatFs
// asks for - how many calls, how many sectors, and the address of the
// most recent one (used to find where the test file lives on the card).
class CountingBlockDevice : public BlockDevice {
 public:
  explicit CountingBlockDevice(BlockDevice &inner) : inner_(inner) {}

  uint32_t readCalls = 0;
  uint32_t sectorsRead = 0;
  uint32_t unalignedSectors = 0;  // sectors read into a buffer the fast path can't DMA into
  bd_addr_t lastReadAddr = 0;

  void resetCounts() { readCalls = 0; sectorsRead = 0; unalignedSectors = 0; }

  int init() override { return inner_.init(); }
  int deinit() override { return inner_.deinit(); }
  int sync() override { return inner_.sync(); }
  int read(void *buffer, bd_addr_t addr, bd_size_t size) override {
    readCalls++;
    sectorsRead += size / 512;
    if ((uintptr_t)buffer & 3) unalignedSectors += size / 512;
    lastReadAddr = addr;
    return inner_.read(buffer, addr, size);
  }
  int program(const void *buffer, bd_addr_t addr, bd_size_t size) override {
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

const uint32_t RAW_TEST_BYTES = 2 * 1024 * 1024;  // per raw-driver test
const uint32_t FILE_TEST_BYTES = 1024 * 1024;     // per file-level test
const float BYTES_PER_SEC_1X = 44100.0f * 2;      // 44.1kHz mono 16-bit at native speed

// 4-byte aligned: the SD host moves data by DMA in 32-bit words, and an
// unaligned buffer has to go through the library's bounce buffer instead.
alignas(4) uint8_t buf[32768 + 4];  // +4: see the alignment note in benchFilePattern()
alignas(4) uint8_t checkBuf[32768];

char testPath[96] = "";
long testFileSize = 0;
long dataStart = 0;
long dataBytes = 0;
uint32_t fileFirstSector = 0;  // where the test file's data starts on the card

bool wasConnected = false;

// -- Reporting --------------------------------------------------------------

void printResult(const char *label, uint32_t bytes, uint32_t elapsedMicros) {
  float mbps = (float)bytes / (float)elapsedMicros;  // bytes per microsecond == MB/s
  char line[120];
  snprintf(line, sizeof(line), "  %-40s %6.2f MB/s  (= %5.1fx)", label, mbps,
           mbps * 1e6f / BYTES_PER_SEC_1X);
  Serial.println(line);
}

// -- Finding a test file ----------------------------------------------------

// Picks the largest .WAV at the card's root and finds its data chunk the
// same way WavReader::open() does, without caring about the audio format.
bool findTestFile() {
  DIR *dir = opendir("/sd");
  if (!dir) return false;
  struct dirent *ent;
  testFileSize = 0;
  while ((ent = readdir(dir)) != nullptr) {
    size_t len = strlen(ent->d_name);
    if (len < 4 || strcasecmp(ent->d_name + len - 4, ".WAV") != 0) continue;
    char path[96];
    snprintf(path, sizeof(path), "/sd/%s", ent->d_name);
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size > testFileSize) {
      testFileSize = size;
      strncpy(testPath, path, sizeof(testPath) - 1);
    }
  }
  closedir(dir);
  if (testFileSize == 0) return false;

  FILE *f = fopen(testPath, "rb");
  fseek(f, 12, SEEK_SET);  // skip "RIFF", size, "WAVE"
  while (true) {
    char id[4];
    uint32_t size;
    if (fread(id, 1, 4, f) != 4 || fread(&size, 4, 1, f) != 1) {
      fclose(f);
      return false;
    }
    if (memcmp(id, "data", 4) == 0) {
      dataStart = ftell(f);
      dataBytes = size;
      break;
    }
    fseek(f, size + (size & 1), SEEK_CUR);
  }

  // Read the first sector-aligned block of sample data and note where on
  // the card FatFs fetched it from - that's where the raw tests read from,
  // so they measure real file data rather than empty space.
  long alignedStart = (dataStart + 511) & ~511L;
  fseek(f, alignedStart, SEEK_SET);
  fread(buf, 1, 4096, f);
  fileFirstSector = countingDevice.lastReadAddr / 512;
  fclose(f);
  return true;
}

// -- Raw sector reads ---------------------------------------------------------

void benchRawRead(bool multiBlock, uint32_t bytesPerCall, bool backward = false) {
  sdBlockDevice.setMultiBlock(multiBlock);
  bd_addr_t base = (bd_addr_t)fileFirstSector * 512;

  // A fast wrong answer would be worse than useless - check the fast path
  // returns exactly what the core's own read does.
  if (multiBlock) {
    sdBlockDevice.read(buf, base, bytesPerCall);
    sdBlockDevice.setMultiBlock(false);
    sdBlockDevice.read(checkBuf, base, bytesPerCall);
    sdBlockDevice.setMultiBlock(true);
    if (memcmp(buf, checkBuf, bytesPerCall) != 0) {
      Serial.println("  multi-block data MISMATCH vs core read() - result below is invalid");
    }
  }

  uint32_t start = micros();
  for (uint32_t done = 0; done < RAW_TEST_BYTES; done += bytesPerCall) {
    // Backward: the same chunks, last one first.
    bd_addr_t addr = backward ? base + RAW_TEST_BYTES - bytesPerCall - done : base + done;
    int rv = sdBlockDevice.read(buf, addr, bytesPerCall);
    if (rv != 0) {
      Serial.print("  read failed, error ");
      Serial.println(rv);
      sdBlockDevice.setMultiBlock(true);
      return;
    }
  }
  uint32_t elapsed = micros() - start;
  sdBlockDevice.setMultiBlock(true);

  char label[64];
  snprintf(label, sizeof(label), "%s, %2luKB per call%s", multiBlock ? "fast driver" : "core driver",
           (unsigned long)(bytesPerCall / 1024), backward ? ", backward" : "");
  printResult(label, RAW_TEST_BYTES, elapsed);
}

// -- File-level tests (SDResampleDemo's access pattern) ---------------------

// Reads FILE_TEST_BYTES of sample data in chunks the way
// WavReader::readSamplesAt() does: a fresh fseek(SEEK_SET) before every
// chunk. Forward walks up from the start of the sample data; backward
// walks down from near its end. If the file's sample data happens to start
// on a sector boundary, reads are skewed to where a plain 44-byte header
// would put them, so this measures the common, unaligned case.
void benchFilePattern(bool multiBlock, bool unbuffered, uint32_t chunkBytes, bool alignDst,
                      bool backward) {
  FILE *f = fopen(testPath, "rb");
  if (!f) {
    Serial.println("  couldn't reopen test file");
    return;
  }
  if (unbuffered) setvbuf(f, nullptr, _IONBF, 0);
  sdBlockDevice.setMultiBlock(multiBlock);

  long skew = (dataStart % 512 == 0) ? 44 : 0;
  long fromByte = backward ? dataBytes - 32768 : 0;  // stay clear of the file's last partial chunk
  uint32_t chunks = FILE_TEST_BYTES / chunkBytes;

  countingDevice.resetCounts();
  uint32_t seekMicros = 0;
  uint32_t start = micros();
  for (uint32_t i = 0; i < chunks; i++) {
    long offset = backward ? fromByte - (long)(i + 1) * chunkBytes
                           : fromByte + (long)i * chunkBytes;
    uint32_t seekStart = micros();
    long pos = dataStart + skew + offset;
    fseek(f, pos, SEEK_SET);
    seekMicros += micros() - seekStart;
    // FatFs DMAs whole sectors straight into the destination, starting
    // (512 - pos % 512) bytes in - so that address is only 4-byte aligned
    // (and fast) if the buffer's address matches pos mod 4. Offsetting the
    // buffer by pos & 3 guarantees it for any file position.
    fread(alignDst ? buf + (pos & 3) : buf, 1, chunkBytes, f);
  }
  uint32_t elapsed = micros() - start;
  fclose(f);
  sdBlockDevice.setMultiBlock(true);

  char label[64];
  snprintf(label, sizeof(label), "  %s", backward ? "backward, near file end" : "forward");
  printResult(label, FILE_TEST_BYTES, elapsed);
  char line[120];
  snprintf(line, sizeof(line), "  %-40s %5.2f driver calls, %5.2f sectors (%5.2f unaligned), %6lu us (%lu us in fseek) per chunk", "",
           (float)countingDevice.readCalls / chunks, (float)countingDevice.sectorsRead / chunks,
           (float)countingDevice.unalignedSectors / chunks,
           (unsigned long)(elapsed / chunks), (unsigned long)(seekMicros / chunks));
  Serial.println(line);
}

void benchFileStep(const char *title, bool multiBlock, bool unbuffered, uint32_t chunkBytes,
                   bool alignDst = false) {
  Serial.println(title);
  benchFilePattern(multiBlock, unbuffered, chunkBytes, alignDst, false);
  benchFilePattern(multiBlock, unbuffered, chunkBytes, alignDst, true);
}

// -- Main -----------------------------------------------------------------

void runBenchmarks() {
  Serial.println();
  Serial.println("SDReadBench");
  char line[160];
  snprintf(line, sizeof(line), "Test file: %s, %ld bytes, sample data at byte %ld (%ld bytes)",
           testPath, testFileSize, dataStart, dataBytes);
  Serial.println(line);
  if (dataBytes < (long)FILE_TEST_BYTES * 2) {
    Serial.println("Test file is too short - needs at least 2MB of sample data.");
    return;
  }

  Serial.println();
  Serial.println("Raw sector reads, sequential, below the filesystem:");
  benchRawRead(false, 32768);
  benchRawRead(true, 2048);
  benchRawRead(true, 16384);
  benchRawRead(true, 32768);
  benchRawRead(true, 2048, true);
  benchRawRead(true, 32768, true);

  Serial.println();
  Serial.println("SDResampleDemo's pattern through fseek/fread, one fix at a time:");
  benchFileStep(" core driver, 2KB chunks (the demo as it was)", false, false, 2048);
  benchFileStep(" + fast driver", true, false, 2048);
  benchFileStep(" + unbuffered FILE", true, true, 2048);
  benchFileStep(" + 16KB chunks", true, true, 16384);
  benchFileStep(" + 32KB chunks", true, true, 32768);
  benchFileStep(" + buffer offset to match file position (16KB)", true, true, 16384, true);
  benchFileStep(" + buffer offset to match file position (32KB)", true, true, 32768, true);

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
  findTestFile();
}

void loop() {
  // Native USB serial: wait until a monitor is actually listening.
  bool connected = Serial;
  if (connected && !wasConnected) {
    delay(200);  // let the host finish opening the port
    if (testFileSize == 0) {
      Serial.println("No .WAV file found at the card's root.");
    } else {
      runBenchmarks();
    }
  }
  wasConnected = connected;

  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    if (testFileSize > 0) runBenchmarks();
  }
}
