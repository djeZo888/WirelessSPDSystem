#pragma once

// Release identity is maintained with the firmware, outside deployment config.
#define WSPD_SERVER_VERSION "v0.1"
#define WSPD_SERVER_VARIANT "tconnpro"

constexpr char SERVER_VERSION[] = WSPD_SERVER_VERSION;
constexpr char SERVER_FIRMWARE_ID[] = WSPD_SERVER_VERSION "-" WSPD_SERVER_VARIANT;
