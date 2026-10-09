#pragma once

#include <stdint.h>
#include <type_traits>

struct SpdConfig {
  uint8_t id;
  const char *friendlyName;
  uint32_t secret;
};

// Keep configured millisecond intervals within the wrap-safe half range.
template <typename T>
constexpr bool validConfigInterval(T interval, bool allowZero = false) {
  return std::is_integral<T>::value && interval >= 0 &&
         (allowZero || interval > 0) && interval <= 2147483647ULL;
}
