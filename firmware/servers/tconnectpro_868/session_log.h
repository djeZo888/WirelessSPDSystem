#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Return the size of a complete valid UTF-8 character, or zero for an invalid
// byte. Inspect successive bytes only until the string terminator.
inline size_t wspdLogUtf8Size(const unsigned char *p) {
  const unsigned char c = p[0];
  if (c < 128) return 1;
  const size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
  if (!n) return 0;
  for (size_t i = 1; i < n; ++i) if (!p[i] || (p[i] & 0xC0) != 0x80) return 0;
  if ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0) ||
      (c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90)) return 0;
  return n;
}

// Owned by the sketch loop. No flash writes, per-entry allocation or worker calls.
class WspdSessionLog {
public:
  struct Record { uint64_t seq, bootMs; char message[496]; };
  static_assert(sizeof(Record) == 512, "Log records must remain exactly 512 bytes.");
  void begin(Record *storage, size_t count) {
    records_ = storage; capacity_ = count; used_ = head_ = 0; next_ = 1;
    epochOffset_ = 0; clockSynced_ = false;
  }
  void append(uint64_t bootMs, const char *message) {
    if (!records_ || !capacity_) return;
    Record &r = records_[head_]; r.seq = next_++; r.bootMs = bootMs;
    size_t used = 0;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(message);
    while (p && *p && used < sizeof(r.message) - 1) {
      const size_t n = wspdLogUtf8Size(p);
      if (!n) { r.message[used++] = '?'; ++p; continue; }
      if (n > sizeof(r.message) - 1 - used) break; // Never split a character.
      if (n == 1) r.message[used++] = *p < 32 || *p == 127 ? ' ' : char(*p);
      else { memcpy(r.message + used, p, n); used += n; }
      p += n;
    }
    r.message[used] = 0;
    head_ = (head_ + 1) % capacity_; if (used_ < capacity_) ++used_;
  }
  const Record *get(uint64_t seq) const {
    if (seq < oldest() || seq >= next_ || !used_) return nullptr;
    const size_t first = (head_ + capacity_ - used_) % capacity_;
    return &records_[(first + size_t(seq - oldest())) % capacity_];
  }
  void syncClock(uint64_t bootMs, uint64_t unixMs) {
    if (unixMs > bootMs) { epochOffset_ = unixMs - bootMs; clockSynced_ = true; }
  }
  uint64_t unixMs(uint64_t bootMs) const { return clockSynced_ ? epochOffset_ + bootMs : 0; }
  bool clockSynced() const { return clockSynced_; }
  size_t capacity() const { return capacity_; }
  size_t used() const { return used_; }
  uint64_t next() const { return next_; }
  uint64_t oldest() const { return next_ - used_; }
  uint64_t dropped() const { return oldest() - 1; }
private:
  Record *records_ = nullptr;
  size_t capacity_ = 0, used_ = 0, head_ = 0;
  uint64_t next_ = 1, epochOffset_ = 0;
  bool clockSynced_ = false;
};

// Strict decimal query parsing, including overflow protection before narrowing.
inline bool wspdLogNumber(const char *text, uint64_t &value) {
  value = 0; if (!text || !*text) return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const unsigned digit = unsigned(*text - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}
