#pragma once

#include <stdint.h>

// RTC CNT is a wrapping 16-bit clock at 32 Hz. Every scheduled delay must be
// below its half range (1024 seconds), so signed subtraction orders deadlines
// correctly across the wrap without an overflow interrupt or a 1 Hz CPU wake.
namespace AcDcSleep {
constexpr uint16_t TICKS_PER_SECOND = 32;
constexpr uint16_t MAX_DELAY_TICKS = 32767;

inline bool reached(uint16_t now, uint16_t deadline) {
  return static_cast<int16_t>(now - deadline) >= 0;
}

inline uint16_t earlier(uint16_t now, uint16_t first, uint16_t second) {
  if (reached(now, first)) return first;
  if (reached(now, second)) return second;
  return static_cast<uint16_t>(first - now) <=
                 static_cast<uint16_t>(second - now) ? first : second;
}

class Schedule {
 public:
  void begin(uint16_t now, uint16_t startupDelay) {
    regularDeadline_ = static_cast<uint16_t>(now + startupDelay);
    remainingRetries_ = 0;
    regularPending_ = false;
    retryPending_ = false;
  }

  // Latch overdue work while a failed radio is retried. It must remain due even
  // if initialization takes longer than the clock's signed half range.
  void observe(uint16_t now) {
    regularPending_ = regularDue(now);
    retryPending_ = retryDue(now);
  }
  bool regularDue(uint16_t now) const {
    return regularPending_ || reached(now, regularDeadline_);
  }
  bool retryDue(uint16_t now) const {
    return remainingRetries_ != 0 &&
           (retryPending_ || reached(now, retryDeadline_));
  }
  uint8_t remainingRetries() const { return remainingRetries_; }
  uint16_t regularDeadline() const { return regularDeadline_; }
  uint16_t retryDeadline() const { return retryDeadline_; }

  void regularSent(uint16_t startedAt, uint16_t interval) {
    regularDeadline_ = static_cast<uint16_t>(startedAt + interval);
    regularPending_ = false;
  }

  // The caller owns the cached on-air packet. Repeats never create a new packet
  // or allocate another nonce, and a newer FAIL event replaces the old burst.
  void beginFailRetries(uint16_t finishedAt, uint8_t count, uint16_t delay) {
    remainingRetries_ = count;
    retryDeadline_ = static_cast<uint16_t>(finishedAt + delay);
    retryPending_ = false;
  }
  void retrySent(uint16_t finishedAt, uint16_t delay) {
    if (remainingRetries_ == 0) return;
    --remainingRetries_;
    retryDeadline_ = static_cast<uint16_t>(finishedAt + delay);
    retryPending_ = false;
  }

  uint16_t nextDeadline(uint16_t now) const {
    if (regularDue(now) || retryDue(now)) return now;
    if (remainingRetries_ == 0) return regularDeadline_;
    return earlier(now, regularDeadline_, retryDeadline_);
  }

 private:
  uint16_t regularDeadline_ = 0;
  uint16_t retryDeadline_ = 0;
  uint8_t remainingRetries_ = 0;
  bool regularPending_ = false;
  bool retryPending_ = false;
};
}  // namespace AcDcSleep
