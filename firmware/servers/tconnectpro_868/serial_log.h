#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// USB diagnostics are optional. The session log remains the authoritative copy.
// Only the receiver loop accesses this mirror; callbacks must not write to it.
class WspdSerialLogMirror {
 public:
  static constexpr size_t CAPACITY = 4096;
  static constexpr size_t WRITE_BUDGET = 64;

  template <typename Port> void begin(Port &port) {
    // Both supported ESP32 cores otherwise wait for a connected USB host that
    // has stopped reading. Never let diagnostics delay monitoring or controls.
    port.setTxTimeoutMs(0);
  }

  void enqueue(const char *text) {
    if (!text) return;
    const size_t length = strnlen(text, CAPACITY);
    if (length > CAPACITY - 2 || length + 2 > CAPACITY - used_) {
      if (dropped_ != UINT32_MAX) ++dropped_;
      return;
    }
    for (size_t i = 0; i < length; ++i) push(text[i]);
    push('\r'); push('\n');
  }

  template <typename Port> void pump(Port &port) {
    if (!used_) return;
    const int available = port.availableForWrite();
    // Leave space for ring-buffer bookkeeping/alignment in the USB driver.
    if (available <= 8) return;
    size_t count = size_t(available - 8);
    if (count > WRITE_BUDGET) count = WRITE_BUDGET;
    if (count > used_) count = used_;
    if (count > CAPACITY - head_) count = CAPACITY - head_;
    size_t sent = port.write(reinterpret_cast<const uint8_t *>(bytes_ + head_), count);
    if (sent > count) sent = count;
    head_ = (head_ + sent) % CAPACITY;
    used_ -= sent;
  }

  size_t pending() const { return used_; }
  uint32_t dropped() const { return dropped_; }

 private:
  void push(char value) {
    bytes_[(head_ + used_) % CAPACITY] = value;
    ++used_;
  }
  char bytes_[CAPACITY] = {};
  size_t head_ = 0, used_ = 0;
  uint32_t dropped_ = 0;
};
