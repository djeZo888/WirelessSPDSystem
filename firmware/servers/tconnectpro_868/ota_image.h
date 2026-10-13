#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Selection guards; authentication is required separately. These are not signatures.
constexpr char WSPD_OTA_IMAGE_MARKER[] = "WirelessSPDSystem:tconnpro:ota-v1";
constexpr size_t WSPD_OTA_PREFIX_SIZE = 288;

inline bool wspdOtaSize(const char *text, size_t limit, size_t &size) {
  if (!text || !*text) return false;
  size = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const unsigned digit = *text - '0';
    if (size > limit / 10 || (size == limit / 10 && digit > limit % 10)) return false;
    size = size * 10 + digit;
  }
  return size >= WSPD_OTA_PREFIX_SIZE && size <= limit;
}

inline bool wspdOtaDigest(const char *text, uint8_t digest[32]) {
  if (!text || strlen(text) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    const char c = text[i];
    const int n = c >= '0' && c <= '9' ? c - '0' :
                  c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                  c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (n < 0) return false;
    if ((i & 1) == 0) digest[i / 2] = n << 4; else digest[i / 2] |= n;
  }
  return true;
}

inline bool wspdOtaPrefix(const uint8_t *p, size_t n) {
  return n >= WSPD_OTA_PREFIX_SIZE && p[0] == 0xE9 && p[1] >= 1 && p[1] <= 16 &&
         p[12] == 9 && p[13] == 0 &&
         p[32] == 0x32 && p[33] == 0x54 && p[34] == 0xCD && p[35] == 0xAB;
}

class WspdOtaMarker {
public:
  void clear() { used_ = 0; found_ = false; }
  void append(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n && !found_; ++i) {
      if (used_ == sizeof(window_)) { memmove(window_, window_ + 1, sizeof(window_) - 1); --used_; }
      window_[used_++] = p[i];
      found_ = used_ == sizeof(window_) && memcmp(window_, WSPD_OTA_IMAGE_MARKER, sizeof(window_)) == 0;
    }
  }
  bool found() const { return found_; }
private:
  uint8_t window_[sizeof(WSPD_OTA_IMAGE_MARKER) - 1];
  size_t used_ = 0;
  bool found_ = false;
};
