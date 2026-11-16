// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  SDBurnIn - SparkFun Thing Plus RA6M5

  Reads an SD card over and over, checking every byte, and reports every
  way the card's connection fails - to find out whether a board's SD slot,
  wiring or power is sound, and if not, what goes wrong.

  Why: a card can work for minutes or days and then stop answering. The
  core's SD driver never recovers from that by itself - every later read
  fails until the card is started up again (see RA6M5FastSD.h) - so a
  sketch just sees files that won't open, with no hint of why. This one
  tells the possible causes apart:

    card-detect   the slot's card-detect switch reported the card removed.
                  The driver refuses everything at once afterwards - look
                  at the slot, the card's seating, anything pressing on it.
    timeout       commands go out but the card stops answering. A card that
                  browned out (a dip on its 3.3V supply) does exactly this:
                  it resets itself and ignores reads until it's started up
                  again. So does a marginal bus.
    error         the SD host reported a failed transfer (a CRC error, say):
                  usually signal trouble.
    mismatch      a read came back "good" but with the wrong bytes.

  After any failure it starts the card up again (unmounting and remounting
  the filesystem), logs how long that took, and carries on - so one run
  records every failure, not just the first.

  It can also play a test tone through both DACs while it reads ('t'),
  with the DACs' output amplifiers on or off ('a') - to see whether
  failures follow the load of the audio output, as a power problem would.

  What it reads: up to MAX_FILES .wav files anywhere on the card (hidden
  folders skipped). At startup it reads each one once and records a
  checksum of its contents. Then, forever, it picks one at random and
  either reads it all in 32KB unbuffered reads (like playing it) and checks
  the whole checksum, or opens it, checks its first 4KB and closes it (like
  opening files, which walks the folders on the way). A mismatch is read
  again, to tell a bad read from a bad first pass. Nothing is written.

  Reports, over USB serial: an event line the moment anything goes wrong,
  stamped with the time since boot; a status line every 10 seconds with
  the totals. The USB serial port drops whatever's printed while no
  monitor is open, so the last 32 events are kept, and printed again
  whenever a monitor connects. Commands (send a letter):
    s  status line now        e  the kept events again
    t  test tone on/off       a  DAC output amplifiers on/off

  Wiring: just a FAT32 card with some .wav files in the board's slot. For
  the test tone, whatever is normally on A0/A1 (the amplifiers want a load
  of 5k or more - leave them off for headphones wired straight to the pins).

  How to test: open the serial monitor, or better, leave a logger running
  (`make serial-log` saves the output to logs/ with the time of day on
  each line). Note the status line's numbers, and leave it running - with
  and without the tone. To compare boards, run the same card in each.
  While it runs, tap the slot or flex the board, and watch for events. Stop
  the logger before unplugging the board: the core's serial port can hang
  the board if the cable comes out while a computer has the port open.
*/

#include <RA6M5FastSD.h>
#include "FATFileSystem.h"
#include "FspTimer.h"
#include "bsp_api.h"  // for R_DAC - the tone and the output amplifiers
#include <stdio.h>
#include <string.h>

FastSDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                    PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
FATFileSystem sdFs("sd");

const int MAX_FILES = 128;
const int MAX_PATH = 160;
const uint32_t CHUNK = 32768;      // per read, like SDResampleDemo's playback
const uint32_t HEAD_BYTES = 4096;  // checked by the open-and-peek test
const unsigned long STATUS_INTERVAL = 10000;

struct TestFile {
  char path[MAX_PATH];
  uint32_t size;
  uint32_t sum;      // checksum of the whole file
  uint32_t headSum;  // ...and of its first HEAD_BYTES
};
TestFile files[MAX_FILES];
int fileCount = 0;

alignas(4) uint8_t buf[CHUNK];  // aligned, so the card's DMA goes straight into it

// Declared up here, before Arduino's generated function prototypes use it.
enum ReadResult { READ_OK, READ_NO_OPEN };

// -- Counting ---------------------------------------------------------------

uint64_t bytesRead = 0;
uint32_t fileReads = 0, headReads = 0;
uint32_t openFailures = 0, readFailures = 0;
uint32_t mismatches = 0, mismatchesTwice = 0;
uint32_t cardRemovedEvents = 0, cardUninitialisedEvents = 0;
uint32_t recoveries = 0, recoveryFailures = 0;
bool lastPresent = true, lastInitialised = true;

// Time since boot, as h:mm:ss.
String uptime() {
  unsigned long s = millis() / 1000;
  char text[16];
  snprintf(text, sizeof(text), "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(text);
}

// -- Events -------------------------------------------------------------------

const int KEPT_EVENTS = 32;
String events[KEPT_EVENTS];
int eventCount = 0;  // ever logged; the last KEPT_EVENTS are kept

void logEvent(const String &what) {
  String line = uptime() + "  " + what;
  events[eventCount % KEPT_EVENTS] = line;
  eventCount++;
  Serial.println(line);
}

void printEvents() {
  int first = eventCount > KEPT_EVENTS ? eventCount - KEPT_EVENTS : 0;
  Serial.print(eventCount);
  Serial.println(" events since boot" + String(first ? ", the last few:" : (eventCount ? ":" : ".")));
  for (int i = first; i < eventCount; i++) Serial.println("  " + events[i % KEPT_EVENTS]);
}

// The driver's own error counts, for event lines.
String driverCounts() {
  return "errors " + String(sdBlockDevice.transferErrors()) + ", timeouts " +
         String(sdBlockDevice.transferTimeouts()) + ", refused " +
         String(sdBlockDevice.refusedRequests());
}

// Watches the card-detect switch and whether the driver still counts the
// card as started. Called between every read, so a blink between two of
// them is still seen as long as it leaves the card marked uninitialised -
// which the core's driver always does.
void watchCard() {
  bool present = sdBlockDevice.is_card_present();
  bool initialised = sdBlockDevice.is_card_initialized();
  if (present != lastPresent) {
    if (!present) cardRemovedEvents++;
    logEvent(present ? "card-detect: card back in" : "CARD-DETECT: card reported removed");
  }
  if (initialised != lastInitialised && !initialised) {
    cardUninitialisedEvents++;
    logEvent("DRIVER: card marked uninitialised - it refuses every request now");
  }
  lastPresent = present;
  lastInitialised = initialised;
}

// -- Reading ------------------------------------------------------------------

// FNV-1a: quick, and enough to notice any change.
uint32_t addToSum(uint32_t sum, const uint8_t *data, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) sum = (sum ^ data[i]) * 16777619u;
  return sum;
}

// Reads `limit` bytes of a file (or all of it) and checksums them. Unbuffered,
// so each 32KB fread() is one multi-block command - see RA6M5FastSD.h.
ReadResult readFile(const char *path, uint32_t limit, uint32_t &sum, uint32_t &got) {
  sum = 2166136261u;
  got = 0;
  FILE *f = fopen(path, "rb");
  watchCard();
  if (!f) return READ_NO_OPEN;
  setvbuf(f, nullptr, _IONBF, 0);
  while (got < limit) {
    uint32_t want = limit - got < CHUNK ? limit - got : CHUNK;
    size_t n = fread(buf, 1, want, f);
    watchCard();
    sum = addToSum(sum, buf, n);
    got += n;
    bytesRead += n;
    if (n < want) break;  // the end, or a failed read - the caller checks got
  }
  fclose(f);
  return READ_OK;
}

// Starts the card up again, as a reset would: unmount (which shuts the card
// down) and mount (which starts it). Retries until it works.
void recoverCard(const String &why) {
  logEvent("recovering after " + why + " (" + driverCounts() + ")");
  unsigned long start = millis();
  for (int attempt = 1;; attempt++) {
    sdFs.unmount();
    sdBlockDevice.deinit();
    if (sdFs.mount(&sdBlockDevice) == 0) {
      recoveries++;
      logEvent("recovered in " + String(millis() - start) + "ms" +
               (attempt > 1 ? " (attempt " + String(attempt) + ")" : String()));
      lastPresent = sdBlockDevice.is_card_present();
      lastInitialised = sdBlockDevice.is_card_initialized();
      return;
    }
    recoveryFailures++;
    if (attempt == 1) logEvent("RECOVERY FAILED - retrying every 2 seconds");
    delay(2000);
  }
}

// One test: a whole file checked, or one file's first 4KB.
void runOneTest() {
  TestFile &t = files[random(fileCount)];
  bool whole = random(2) == 0;
  uint32_t limit = whole ? t.size : min(t.size, HEAD_BYTES);
  uint32_t expected = whole ? t.sum : t.headSum;
  uint32_t sum, got;

  ReadResult r = readFile(t.path, limit, sum, got);
  if (r == READ_NO_OPEN) {
    openFailures++;
    recoverCard("OPEN FAILED: " + String(t.path));
    return;
  }
  if (got < limit) {
    readFailures++;
    recoverCard("READ FAILED: " + String(t.path) + " at byte " + String(got));
    return;
  }
  whole ? fileReads++ : headReads++;
  if (sum == expected) return;

  // Wrong bytes from a read that claimed to succeed. Read it again.
  mismatches++;
  uint32_t sum2, got2;
  readFile(t.path, limit, sum2, got2);
  if (sum2 == expected) {
    logEvent("MISMATCH: bad data from one read of " + String(t.path) + "; read again, it matched");
  } else if (sum2 == sum) {
    logEvent("mismatch: " + String(t.path) + " reads the same twice, but not as in the first "
             "pass - taking the new reading as right");
    if (whole) t.sum = sum; else t.headSum = sum;
  } else {
    mismatchesTwice++;
    logEvent("MISMATCH: " + String(t.path) + " read wrong twice, differently each time");
  }
}

// -- Finding files ------------------------------------------------------------

void findFiles(const String &folder) {
  DIR *dir = opendir(folder.c_str());
  if (!dir) return;
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr && fileCount < MAX_FILES) {
    if (ent->d_name[0] == '.') continue;  // hidden: .Trashes, .Spotlight-V100...
    String path = folder + "/" + ent->d_name;
    if (ent->d_type == DT_DIR) {
      findFiles(path);
    } else if (path.length() < MAX_PATH && (path.endsWith(".wav") || path.endsWith(".WAV"))) {
      strcpy(files[fileCount].path, path.c_str());
      fileCount++;
    }
  }
  closedir(dir);
}

// The first pass: every file's size and checksums, read once. A file that
// can't be read now is left out, rather than tested against a bad reading.
void measureFiles() {
  int kept = 0;
  for (int i = 0; i < fileCount; i++) {
    TestFile &t = files[i];
    uint32_t got;
    if (readFile(t.path, HEAD_BYTES, t.headSum, got) != READ_OK ||
        readFile(t.path, UINT32_MAX, t.sum, t.size) != READ_OK || t.size == 0) {
      logEvent("left out (couldn't read it in the first pass): " + String(t.path));
      continue;
    }
    files[kept++] = t;
  }
  fileCount = kept;
}

// -- The test tone --------------------------------------------------------------

FspTimer toneTimer;
bool toneOn = false;
bool ampsOn = false;
volatile uint32_t tonePhase = 0;
int16_t sine[64];

// A 750Hz sine (48kHz / 64) on both DACs, at 7/8 of full scale - all the
// output amplifiers can swing, and as loud as SDResampleDemo plays.
void toneTick(timer_callback_args_t *) {
  uint16_t value = toneOn ? (uint16_t)(2048 + sine[tonePhase++ & 63]) : 2048;
  R_DAC->DADR[0] = value;
  R_DAC->DADR[1] = value;
}

// ---- RA6M5-specific ----
// The DACs' output amplifiers, which the Arduino core leaves off - the same
// register sequence as SDResampleDemo's enableDacOutputAmps(); see
// DACAudioTest for the background.
void setDacOutputAmps(bool on) {
  R_DAC->DACR_b.DAOE0 = 0;
  R_DAC->DACR_b.DAOE1 = 0;
  R_DAC->DAASWCR_b.DAASW0 = on;
  R_DAC->DAASWCR_b.DAASW1 = on;
  R_DAC->DAAMPCR_b.DAAMP0 = on;
  R_DAC->DAAMPCR_b.DAAMP1 = on;
  R_DAC->DACR_b.DAOE0 = 1;
  R_DAC->DACR_b.DAOE1 = 1;
  delayMicroseconds(4);
  R_DAC->DAASWCR_b.DAASW0 = 0;
  R_DAC->DAASWCR_b.DAASW1 = 0;
  ampsOn = on;
}
// ---- end RA6M5-specific ----

void startToneTimer() {
  for (int i = 0; i < 64; i++) sine[i] = (int16_t)(sinf(i * 2 * PI / 64) * 2047 * 7 / 8);
  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  toneTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex, 48000.0f, 50.0f, toneTick, nullptr);
  toneTimer.setup_overflow_irq(1);
  toneTimer.open();
  toneTimer.start();
}

// -- Status ---------------------------------------------------------------------

uint64_t bytesAtLastStatus = 0;
unsigned long lastStatusMillis = 0;

void printStatus() {
  unsigned long now = millis();
  float mbps = (float)(bytesRead - bytesAtLastStatus) / 1000.0f / (float)(now - lastStatusMillis);
  bytesAtLastStatus = bytesRead;
  lastStatusMillis = now;
  char line[300];
  snprintf(line, sizeof(line),
           "%s  read %.1fGB (%.1fMB/s)  files %lu  heads %lu  | open fails %lu  read fails %lu  "
           "mismatches %lu (%lu twice)  | driver: errors %lu  timeouts %lu  refused %lu  missed %lu  "
           "| card-detect %lu  uninitialised %lu  | recoveries %lu (%lu failed)  | tone %s amps %s",
           uptime().c_str(), bytesRead / 1e9, mbps, (unsigned long)fileReads,
           (unsigned long)headReads, (unsigned long)openFailures, (unsigned long)readFailures,
           (unsigned long)mismatches, (unsigned long)mismatchesTwice,
           (unsigned long)sdBlockDevice.transferErrors(), (unsigned long)sdBlockDevice.transferTimeouts(),
           (unsigned long)sdBlockDevice.refusedRequests(), (unsigned long)sdBlockDevice.missedEvents(),
           (unsigned long)cardRemovedEvents, (unsigned long)cardUninitialisedEvents,
           (unsigned long)recoveries, (unsigned long)recoveryFailures, toneOn ? "on" : "off",
           ampsOn ? "on" : "off");
  Serial.println(line);
}

void printIntro() {
  Serial.println();
  Serial.println("SDBurnIn: " + String(fileCount) + " files, reading at random. Commands: "
                 "s status, e events, t tone, a amplifiers.");
  printEvents();
}

// -- Main -----------------------------------------------------------------------

bool wasConnected = false;

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  startToneTimer();
  randomSeed(micros());
  while (sdFs.mount(&sdBlockDevice) != 0) delay(1000);  // no card yet
  findFiles("/sd");
  measureFiles();
  sdBlockDevice.resetErrorCounts();  // count from here, not the first pass
  logEvent("started: " + String(fileCount) + " files");
  lastStatusMillis = millis();
}

void loop() {
  // Native USB serial: greet whoever opens the port.
  bool connected = Serial;
  if (connected && !wasConnected) {
    delay(200);  // let the host finish opening the port
    printIntro();
  }
  wasConnected = connected;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's') printStatus();
    if (c == 'e') printEvents();
    if (c == 't') {
      toneOn = !toneOn;
      logEvent(toneOn ? "test tone on" : "test tone off");
    }
    if (c == 'a') {
      setDacOutputAmps(!ampsOn);
      logEvent(ampsOn ? "DAC output amplifiers on" : "DAC output amplifiers off");
    }
  }

  if (fileCount == 0) {
    if (connected && millis() - lastStatusMillis >= STATUS_INTERVAL) {
      Serial.println("No .wav files found on the card.");
      lastStatusMillis = millis();
    }
    return;
  }

  runOneTest();
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // a heartbeat, one blink per test

  if (millis() - lastStatusMillis >= STATUS_INTERVAL) printStatus();
}
