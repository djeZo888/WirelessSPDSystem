#pragma once

#include <stdint.h>

struct SpdConfig {
  uint8_t id;
  const char *friendlyName;
  uint32_t secret;
};
