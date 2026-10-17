// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

/*
  UsbDiskGate - lets SDResampleDemo hand its SD card to a computer over USB,
  and take it back, without ever letting both use it at once.

  The Arduino core's UsbMsd library makes the board a USB disk as well as a
  serial port: whatever block device it's given appears on the computer as
  a drive. But a FAT card can only have one owner at a time. The computer
  and this sketch's own FATFileSystem would each keep their own idea of the
  card's file table in memory, and each would overwrite the other's changes
  - a corrupted card, sooner or later. And the computer's requests arrive
  inside the USB interrupt, which can land in the middle of one of loop()'s
  own card reads.

  So the disk is given this gate rather than the card. Closed, it reports
  that no card is inserted - like an empty card reader, which a computer
  quietly ignores - and refuses every read and write. The sketch opens it
  only after unmounting its own filesystem, and closes it before mounting
  again. Open, every call goes straight through to the card.

  UsbMsd calls init() before every single request, so that's where a closed
  gate says no; it never reaches the card at all.

  Also here: fixUsbDiskPacketSize(), a workaround for a bug in the core
  that otherwise stops the board working over USB at all - see below.
*/

#pragma once

#include "BlockDevice.h"
#include "tusb.h"

// The core describes the disk's two data endpoints with 512-byte packets.
// That's right for high-speed USB, which the Portenta C33 the library was
// written for uses - but this board's USB port is full speed, where a bulk
// packet can be at most 64 bytes. macOS (rightly) refuses a configuration
// like that outright, serial port and all: linking UsbMsd unmodified left
// the board with no serial port and no disk. The serial port's packets are
// sized for the port's speed; the disk's are hard-coded (USB.cpp,
// USBD_MSD_IN_OUT_SIZE).
//
// The core builds its descriptor once, the first time __SetupUSBDescriptor()
// runs, and keeps it. So this builds it early - from a constructor, before
// main() starts USB - and corrects the disk's endpoints to 64 before any
// computer ever sees it. Harmless on a high-speed port that has no such bug:
// it only touches endpoints that are larger than the port allows.
void __SetupUSBDescriptor();  // the core's, in USB.cpp

inline void fixUsbDiskPacketSize() {
  __SetupUSBDescriptor();
  uint8_t *desc = (uint8_t *)tud_descriptor_configuration_cb(0);
  if (desc == nullptr) return;
  const uint16_t FULL_SPEED_BULK_MAX = 64;
  uint16_t total = desc[2] | (desc[3] << 8);
  bool inDisk = false;
  for (uint16_t i = 0; i + 1 < total && desc[i] > 0; i += desc[i]) {
    uint8_t type = desc[i + 1];
    if (type == TUSB_DESC_INTERFACE) inDisk = (desc[i + 5] == TUSB_CLASS_MSC);
    if (type == TUSB_DESC_ENDPOINT && inDisk && (desc[i + 3] & 0x03) == TUSB_XFER_BULK &&
        (desc[i + 4] | (desc[i + 5] << 8)) > FULL_SPEED_BULK_MAX) {
      desc[i + 4] = FULL_SPEED_BULK_MAX;
      desc[i + 5] = 0;
    }
  }
}

class UsbDiskGate : public BlockDevice {
 public:
  explicit UsbDiskGate(BlockDevice &card) : card_(card) {}

  void setOpen(bool open) { open_ = open; }
  bool isOpen() const { return open_; }

  int init() override { return open_ ? card_.init() : BD_ERROR_DEVICE_ERROR; }
  int deinit() override { return 0; }  // the sketch decides when the card closes
  bool available() override { return open_; }

  int read(void *buffer, bd_addr_t addr, bd_size_t size) override {
    return open_ ? card_.read(buffer, addr, size) : BD_ERROR_DEVICE_ERROR;
  }
  int program(const void *buffer, bd_addr_t addr, bd_size_t size) override {
    return open_ ? card_.program(buffer, addr, size) : BD_ERROR_DEVICE_ERROR;
  }
  int erase(bd_addr_t addr, bd_size_t size) override {
    return open_ ? card_.erase(addr, size) : BD_ERROR_DEVICE_ERROR;
  }

  bd_size_t get_read_size() const override { return card_.get_read_size(); }
  bd_size_t get_program_size() const override { return card_.get_program_size(); }
  bd_size_t get_erase_size() const override { return card_.get_erase_size(); }
  bd_size_t size() const override { return card_.size(); }
  const char *get_type() const override { return "USBDISKGATE"; }

 private:
  BlockDevice &card_;
  volatile bool open_ = false;  // read by the USB interrupt
};
