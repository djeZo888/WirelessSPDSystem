#pragma once

#include <stdint.h>

// EEPROM-only high-water journal. No keys, IDs or measurements are stored here.
// Kept byte-identical in both sketch folders so each Arduino sketch is standalone.
// Storage must provide length(), read(address), update(address, byte), and wait().
template <class Storage>
class PersistentNonceJournal {
 public:
  explicit PersistentNonceJournal(Storage& storage) : storage_(storage) {}

  // Scan only; booting without transmitting does not consume a nonce or write.
  // An erased journal starts at initial. Nonblank but unrecoverable storage must
  // be serviced, never silently reset to zero or to the configured initial value.
  bool begin(uint64_t initial = 1) {
    ready_ = false;
    activeSlot_ = -1;
    next_ = initial;
    if (storage_.length() < kBytes || initial == 0 || initial == UINT64_MAX) return false;
    storage_.wait();
    bool blank = true;
    for (uint8_t slot = 0; slot < kSlots; ++slot) {
      uint8_t record[kRecordBytes];
      readRecord(slot, record);
      bool recordBlank = true;
      for (uint8_t i = 0; i < kRecordBytes; ++i) {
        if (record[i] != 0xFF) recordBlank = false;
      }
      if (!recordBlank) blank = false;
      if (record[0] == kCommitted) {
        uint64_t value;
        if (!decode(record, value)) return false;
        if (activeSlot_ < 0 || value > next_) {
          next_ = value;
          activeSlot_ = slot;
        }
      } else if (record[0] != kWriting && record[0] != 0xFF) {
        return false;
      } else if (record[0] == 0xFF && !recordBlank) {
        return false;
      }
    }
    if (activeSlot_ < 0 && !blank) return false;
    ready_ = true;
    return true;
  }

  // Commit the NEXT value before returning the current one for packet creation.
  // A cut between commit and TX may skip one event, but can never replay it.
  // The caller must not transmit if this returns false, even after retry/reboot.
  bool allocate(uint64_t& nonce) {
    if (!ready_ || next_ == UINT64_MAX) return false;
    const uint8_t slot = activeSlot_ < 0 ? 0 : (activeSlot_ + 1) % kSlots;
    const uint16_t base = (uint16_t)slot * kRecordBytes;
    uint8_t record[kRecordBytes];
    record[0] = kCommitted;
    record[1] = 0x4E;  // N
    record[2] = 0x43;  // C
    record[3] = 1;     // Journal format, independent of the on-air FW version.
    const uint64_t savedNext = next_ + 1;
    for (uint8_t i = 0; i < 8; ++i) record[4 + i] = (uint8_t)(savedNext >> (8 * i));
    const uint32_t checksum = crc32(record + 1, 11);
    for (uint8_t i = 0; i < 4; ++i) record[12 + i] = (uint8_t)(checksum >> (8 * i));

    // Reuse the oldest slot while preserving the latest committed slot. Confirm
    // invalidation before touching the body; a failed byte write must stop TX.
    storage_.update(base, kWriting);
    storage_.wait();
    if (storage_.read(base) != kWriting) return fail();
    for (uint8_t i = 1; i < kRecordBytes; ++i) storage_.update(base + i, record[i]);
    storage_.wait();
    for (uint8_t i = 1; i < kRecordBytes; ++i) {
      if (storage_.read(base + i) != record[i]) return fail();
    }
    storage_.update(base, kCommitted);
    storage_.wait();  // DxCore byte writes return before EEPROM becomes ready.
    for (uint8_t i = 0; i < kRecordBytes; ++i) {
      if (storage_.read(base + i) != record[i]) return fail();
    }
    nonce = next_;
    next_ = savedNext;
    activeSlot_ = slot;
    return true;
  }

 private:
  enum : uint16_t { kBytes = 512, kRecordBytes = 16, kSlots = kBytes / kRecordBytes };
  enum : uint8_t { kCommitted = 0xA5, kWriting = 0x00 };
  Storage& storage_;
  uint64_t next_ = 1;
  int8_t activeSlot_ = -1;
  bool ready_ = false;

  bool fail() {
    ready_ = false;
    return false;
  }

  void readRecord(uint8_t slot, uint8_t* record) {
    const uint16_t base = (uint16_t)slot * kRecordBytes;
    for (uint8_t i = 0; i < kRecordBytes; ++i) record[i] = storage_.read(base + i);
  }

  static uint32_t crc32(const uint8_t* data, uint8_t length) {
    uint32_t crc = UINT32_MAX;
    while (length--) {
      crc ^= *data++;
      for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
      }
    }
    return ~crc;
  }

  static bool decode(const uint8_t* record, uint64_t& value) {
    if (record[1] != 0x4E || record[2] != 0x43 || record[3] != 1) return false;
    uint32_t checksum = 0;
    for (uint8_t i = 0; i < 4; ++i) checksum |= (uint32_t)record[12 + i] << (8 * i);
    if (checksum != crc32(record + 1, 11)) return false;
    value = 0;
    for (uint8_t i = 0; i < 8; ++i) value |= (uint64_t)record[4 + i] << (8 * i);
    return value >= 2;
  }
};
