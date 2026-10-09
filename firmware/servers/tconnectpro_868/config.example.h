#pragma once
#include "config_types.h"

// Copy to config.h (gitignored). Configure, then set CONFIGURED to true.
// These examples intentionally contain no working deployment credentials.
constexpr bool CONFIGURED = false;

// Leave station SSID empty to use the fallback access point only.
static const char *WIFI_SSID = "";
static const char *WIFI_PASSWORD = "";
static const char *FALLBACK_AP_SSID = "WirelessSPDSystem";
static const char *FALLBACK_AP_PASSWORD = "CHANGE_ME_AP_PASSWORD"; // choose 8–63 characters
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// LCD: eight rows per page. One page stays fixed; multiple pages cycle.
// Set the time each page is visible before compiling (1..3600 seconds).
constexpr uint16_t DISPLAY_PAGE_SECONDS = 5;
constexpr uint32_t DISPLAY_PERIODIC_REFRESH_MS = 60000UL;

// Touch polling and press/release debounce.
constexpr uint32_t TOUCH_POLL_MS = 40UL;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 350UL;
constexpr uint32_t TOUCH_RELEASE_STABLE_MS = 160UL;

// All beacons and receivers must match. 868 MHz hardware, 865.3 MHz carrier.
constexpr float LORA_FREQ_MHZ = 865.3f;
constexpr uint8_t LORA_SF = 10;
constexpr float LORA_BW_KHZ = 125.0f;
constexpr uint8_t LORA_CR = 5; // coding rate 4/5
constexpr uint8_t LORA_SYNCWORD = 0x12;
constexpr uint16_t LORA_PREAMBLE = 8;
constexpr int8_t LORA_RX_TX_DBM = 10;
constexpr bool LORA_CRC_ENABLED = true;

// Register up to 127 beacons; ID and secret must match each beacon's config.h.
// Use unique IDs in 1..127 (0 is reserved) and independent, randomly generated nonzero uint32 keys.
// The legacy 32-bit key and truncated SHA256 tag have limited security; see docs/protocol.md.
static const SpdConfig SPD_CONFIGS[] = {
  { 10, "Example beacon", 0x00000000UL }
};

constexpr uint32_t STALE_AFTER_SECONDS = 700;
constexpr uint16_t PACKET_LOSS_WINDOW_SIZE = 1000;
constexpr bool LOW_BATTERY_WARNING_ENABLED = true;
constexpr float LOW_BATTERY_WARNING_V = 2.95f;

// SPD FAIL takes priority over low battery; stale/unknown rows do not alarm.
constexpr uint32_t ALARM_PERIOD_MS = 60000UL;
constexpr uint32_t ALARM_FAIL_ON_MS = 5000UL;
constexpr uint32_t ALARM_LOW_BATTERY_ON_MS = 500UL;
constexpr uint8_t RELAY_ACTIVE_LEVEL = LOW;
constexpr uint8_t RELAY_INACTIVE_LEVEL = HIGH;

// Protect POST /api/v1/reset_nonces with X-API-Key-Reset.
// Empty disables protection. This HTTP server provides no TLS or web login.
static const char *RESET_API_KEY = "CHANGE_ME_RESET_API_KEY";

// Optional local input: 3V3 -> isolated SPD dry contact -> IO15.
// Closed/HIGH = OK; open/LOW = FAIL. Never connect a live/mains signal.
// Keep ID=-1 to disable. The local input occupies the first row on page 1.
constexpr int8_t SPD_LOCAL_PIN = 15;
constexpr int16_t SPD_LOCAL_ID = -1;
static const char *SPD_LOCAL_FRIENDLYNAME = "Local contact";
constexpr uint32_t SPD_LOCAL_POLL_MS = 50UL;
constexpr uint32_t SPD_LOCAL_DEBOUNCE_MS = 250UL;
