// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  SDCardInfo - SparkFun Thing Plus RA6M5

  Prints an SD card's identity, straight from the card: its CID register
  (manufacturer, OEM, product name, revision, serial number and month of
  manufacture, all set at the factory) and the capacity in its CSD
  register. Useful for spotting a counterfeit: a genuine card's maker ID
  matches its brand (SanDisk is 0x03 with OEM "SD", Samsung 0x1B "SM",
  Kingston 0x41 "42", Lexar 0x9F, among others), and its real capacity
  matches the label. Fakes often carry another maker's ID, zeros, or a
  capacity that doesn't match.

  How: the Renesas FSP's SD driver reads neither register for the
  application, and has no way to send a card a command of your own. But
  every card hands over its CID while it's being started up, before it's
  given an address. So this lets the core start the card (which sets up
  the pins), closes the driver, starts the card again by hand through the
  SD host's registers at the slow identification speed - CMD0, CMD8,
  ACMD41, then CMD2 for the CID, CMD3 for an address, CMD9 for the CSD -
  and then gives the card back to the core. Only read commands; nothing
  is written to the card.

  Wiring: just a card in the board's slot.

  How to test: open the serial monitor; it reads the card when the
  monitor connects, and again whenever you send any character.
*/

#include "SDCardBlockDevice.h"
#include "bsp_api.h"  // R_SDHI0, and starting the SD host's module clock

SDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);

// ---- RA6M5-specific ----
// One command through the SD host, waiting for its response. In the host's
// "normal mode" it works out each command's response type itself; SD_CMD
// just takes the command index, plus 0x40 for an application command
// (ACMD). Returns false, with the error flags, if it fails.
uint32_t lastErrors = 0;

bool sendCommand(uint32_t cmd, uint32_t arg) {
  unsigned long start = millis();
  while (R_SDHI0->SD_INFO2 & (1u << 14)) {  // CBSY: the previous command still on the line
    if (millis() - start > 100) return false;
  }
  R_SDHI0->SD_INFO1 = 0;  // flags clear by writing 0
  R_SDHI0->SD_INFO2 = 0;
  R_SDHI0->SD_ARG = arg;
  R_SDHI0->SD_CMD = cmd;
  start = millis();
  while (true) {
    uint32_t errors = R_SDHI0->SD_INFO2 & 0x807F;  // CMDE CRCE ENDE DTO ILW ILR RSPTO, ILA
    if (errors) {
      lastErrors = errors;
      return false;
    }
    if (R_SDHI0->SD_INFO1 & 1) return true;  // RSPEND
    if (millis() - start > 100) {
      lastErrors = 0xFFFF;
      return false;
    }
  }
}

// A 136-bit response (CID or CSD), as the host keeps it: the card's bits
// 127..8 (its CRC dropped) across four registers.
uint32_t resp[4];

void saveLongResponse() {
  resp[0] = R_SDHI0->SD_RSP10;  // bits 39..8
  resp[1] = R_SDHI0->SD_RSP32;  // bits 71..40
  resp[2] = R_SDHI0->SD_RSP54;  // bits 103..72
  resp[3] = R_SDHI0->SD_RSP76;  // bits 127..104
}

// Bits msb..lsb of the saved register, numbered as in the SD spec.
uint32_t bits(int msb, int lsb) {
  uint32_t value = 0;
  for (int b = msb; b >= lsb; b--) {
    int i = b - 8;
    value = (value << 1) | ((resp[i / 32] >> (i % 32)) & 1);
  }
  return value;
}

// Starts the card from scratch and reads its registers. Leaves the SD host
// for the core to set up again.
bool readCardRegisters(uint32_t cid[4], uint32_t csd[4]) {
  // Let the core start the card, which configures the SD pins, then close
  // its driver (that stops the SD host's interrupts and module clock).
  if (sdBlockDevice.init() != 0) {
    Serial.println("The core couldn't start the card - is one in the slot?");
    return false;
  }
  sdBlockDevice.deinit();
  R_BSP_MODULE_START(FSP_IP_SDHIMMC, 0);

  // A clean host, a slow clock - identification runs at 400kHz or less -
  // and the 74 idle clocks a card wants before its first command.
  R_SDHI0->SOFT_RST = 0;
  R_SDHI0->SOFT_RST = 1;
  R_SDHI0->SD_INFO1_MASK = 0xFFFFFFFF;  // no interrupts: this polls
  R_SDHI0->SD_INFO2_MASK = 0xFFFFFFFF;
  R_SDHI0->SD_CLK_CTRL = 0x80;          // the host's clock / 512: under 400kHz
  R_SDHI0->SD_CLK_CTRL = 0x80 | (1u << 8);  // and on
  delay(2);

  bool ok = sendCommand(0, 0);                  // CMD0: back to idle
  ok = ok && sendCommand(8, 0x1AA);             // CMD8: voltage check, needed for SDHC/SDXC
  for (int i = 0; ok && i < 200; i++) {         // ACMD41 until the card says it's ready
    ok = sendCommand(55, 0) && sendCommand(0x40 | 41, 0x40FF8000);  // high capacity, 2.7-3.6V
    if (ok && (R_SDHI0->SD_RSP10 & 0x80000000)) break;
    delay(5);
  }
  ok = ok && (R_SDHI0->SD_RSP10 & 0x80000000);
  ok = ok && sendCommand(2, 0);                 // CMD2: all send CID
  if (ok) {
    saveLongResponse();
    memcpy(cid, resp, sizeof(resp));
  }
  ok = ok && sendCommand(3, 0);                 // CMD3: the card picks an address
  uint32_t rca = R_SDHI0->SD_RSP10 >> 16;
  ok = ok && sendCommand(9, rca << 16);         // CMD9: send CSD
  if (ok) {
    saveLongResponse();
    memcpy(csd, resp, sizeof(resp));
  }
  if (!ok) {
    Serial.print("A command failed - SD_INFO2 error flags 0x");
    Serial.println(lastErrors, HEX);
  }

  // Back to the core: it starts the card from scratch on its next init().
  R_SDHI0->SD_CLK_CTRL = 0;
  return ok;
}
// ---- end RA6M5-specific ----

void printCardInfo() {
  uint32_t cid[4], csd[4];
  Serial.println();
  if (!readCardRegisters(cid, csd)) return;

  memcpy(resp, cid, sizeof(resp));
  char oem[3] = {(char)bits(119, 112), (char)bits(111, 104), 0};
  char name[6] = {(char)bits(103, 96), (char)bits(95, 88), (char)bits(87, 80),
                  (char)bits(79, 72), (char)bits(71, 64), 0};
  char line[160];
  snprintf(line, sizeof(line), "CID  %06lX %08lX %08lX %08lX", (unsigned long)cid[3],
           (unsigned long)cid[2], (unsigned long)cid[1], (unsigned long)cid[0]);
  Serial.println(line);
  snprintf(line, sizeof(line), "  manufacturer ID  0x%02lX", (unsigned long)bits(127, 120));
  Serial.println(line);
  Serial.println("  OEM ID           \"" + String(oem) + "\"");
  Serial.println("  product name     \"" + String(name) + "\"");
  snprintf(line, sizeof(line), "  revision         %lu.%lu", (unsigned long)bits(63, 60),
           (unsigned long)bits(59, 56));
  Serial.println(line);
  snprintf(line, sizeof(line), "  serial number    0x%08lX", (unsigned long)bits(55, 24));
  Serial.println(line);
  snprintf(line, sizeof(line), "  manufactured     %lu-%02lu", 2000 + (unsigned long)bits(19, 12),
           (unsigned long)bits(11, 8));
  Serial.println(line);

  memcpy(resp, csd, sizeof(resp));
  snprintf(line, sizeof(line), "CSD  %06lX %08lX %08lX %08lX", (unsigned long)csd[3],
           (unsigned long)csd[2], (unsigned long)csd[1], (unsigned long)csd[0]);
  Serial.println(line);
  uint32_t structure = bits(127, 126);
  if (structure == 1 || structure == 2) {  // SDHC/SDXC (v2) or SDUC (v3) layout
    uint64_t sectors = ((uint64_t)bits(69, 48) + 1) * 1024;
    snprintf(line, sizeof(line), "  capacity         %llu sectors = %llu bytes (%.2f GB)",
             (unsigned long long)sectors, (unsigned long long)(sectors * 512),
             sectors * 512 / 1e9);
  } else {  // standard capacity (v1) layout
    uint64_t blocks = ((uint64_t)bits(73, 62) + 1) << (bits(49, 47) + 2);
    uint64_t bytes = blocks << bits(83, 80);
    snprintf(line, sizeof(line), "  capacity         %llu bytes (%.2f GB, standard capacity)",
             (unsigned long long)bytes, bytes / 1e9);
  }
  Serial.println(line);

  // Hand the card back, and check the core can still use it.
  Serial.println(sdBlockDevice.init() == 0 ? "The core restarted the card fine."
                                           : "The core couldn't restart the card - reset the board.");
}

bool wasConnected = false;

void setup() {
  Serial.begin(115200);
}

void loop() {
  bool connected = Serial;
  if (connected && !wasConnected) {
    delay(200);  // let the host finish opening the port
    printCardInfo();
  }
  wasConnected = connected;
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    printCardInfo();
  }
}
