#pragma once

#include <stdint.h>

struct SpdConfig {
  uint8_t id;
  const char *friendlyName;
  uint32_t secret;
};

// Keep configured millisecond intervals within the wrap-safe half range.
constexpr bool validConfigInterval(uint32_t interval, bool allowZero = false) {
  return (allowZero || interval > 0) && interval <= 2147483647UL;
}
