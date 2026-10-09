#pragma once

#include <EEPROM.h>
#include "persistent_nonce.h"

#ifndef WSPD_NONCE_START
#define WSPD_NONCE_START 1ULL
#endif
static_assert(WSPD_NONCE_START > 0 && WSPD_NONCE_START < UINT64_MAX,
              "WSPD_NONCE_START must be 1..UINT64_MAX-1, for erased EEPROM only.");
static_assert(EEPROM_SIZE >= 512, "The persistent nonce journal requires 512 EEPROM bytes.");

struct AvrNonceStorage {
  uint16_t length() const { return EEPROM.length(); }
  void wait() { eeprom_busy_wait(); }
  uint8_t read(uint16_t address) {
    wait();
    return EEPROM.read(address);
  }
  void update(uint16_t address, uint8_t value) {
    wait();
    // Refresh every cell when its slot rotates, including stable format/high
    // counter bytes. AVR Dx specifies an array refresh limit as well as wear.
    EEPROM.write(address, value);
    wait();
  }
};
