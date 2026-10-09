#pragma once
#include "config_types.h"

// Copy to config.h (gitignored). Configure, then set CONFIGURED to true.
// These examples intentionally contain no working deployment credentials.
// CONFIGURED: false while preparing settings; true only after completing them.
constexpr auto CONFIGURED = false;

// Wi-Fi strings: SSID 0..32 bytes (empty = fallback access point only).
// Station password: empty for an open network, 8..63 characters, or 64 hex digits.
static const char *WIFI_SSID = "";
static const char *WIFI_PASSWORD = "";
// Fallback SSID: 1..32 bytes; password: 8..63 characters.
static const char *FALLBACK_AP_SSID = "WirelessSPDSystem";
static const char *FALLBACK_AP_PASSWORD = "CHANGE_ME_AP_PASSWORD";
// Station connection timeout: 0..2147483647 ms (0 = immediate fallback).
constexpr auto WIFI_CONNECT_TIMEOUT_MS = 15000;

// LCD: eight rows per page. One page stays fixed; multiple pages cycle.
// Set the time each page is visible before compiling (1..3600 seconds).
constexpr auto DISPLAY_PAGE_SECONDS = 5;
// Periodic redraw interval: 1..2147483647 ms.
constexpr auto DISPLAY_PERIODIC_REFRESH_MS = 60000UL;

// Touch poll interval: 1..2147483647 ms.
constexpr auto TOUCH_POLL_MS = 40UL;
// Press/release debounce: 0..2147483647 ms (0 disables that debounce).
constexpr auto TOUCH_DEBOUNCE_MS = 350UL;
constexpr auto TOUCH_RELEASE_STABLE_MS = 160UL;

// Apartment/home radio settings: every beacon and its server must use the
// same frequency and SF. Set a frequency directly; there is no channel ID.
// Carrier range: 863.0625..869.9375 MHz, keeping the fixed 125 kHz bandwidth
// inside the EU 863..870 MHz band. Local sub-band/power/duty-cycle rules apply.
constexpr float LORA_FREQ_MHZ = 865.3f;
// Spreading factor: 5..12. It must match the beacons; the SX1262 listens on one SF.
constexpr auto LORA_SF = 10;
// Radio output setting: -9..22 dBm. This receiver currently does not transmit.
// It need not match beacon TX power; permitted radiated power may be lower.
constexpr auto LORA_RX_TX_DBM = 10;
// Bandwidth, coding rate, sync word, preamble and CRC are fixed in the .ino.

// Register up to 127 beacons; ID and secret must match each beacon's config.h.
// ID: 1..127 (0 reserved); use 10..127 for production, 1..9 for pre-production.
// Secret: exactly 4 bytes (uint32_t), 0x00000001..0xFFFFFFFF; write eight hex
// digits, including leading zeros. Generate a random nonzero key for the home
// and use that same key for all its beacons and corresponding entries below.
// Friendly name: nonempty NUL-terminated text; LCD displays the first 18 bytes.
// The legacy 32-bit key and truncated SHA256 tag have limited security; see docs/protocol.md.
static const SpdConfig SPD_CONFIGS[] = {
  { 10, "Example beacon", 0x00000000UL }
};

// Stale timeout: 1..2147483 seconds; choose >600 seconds for 5..10 minute beacons.
constexpr auto STALE_AFTER_SECONDS = 700;
// Loss window: 1..65535 packets; uses one RAM byte per packet per wireless SPD.
constexpr auto PACKET_LOSS_WINDOW_SIZE = 1000;
// Low-battery warning: true/false; threshold 2.20..4.74 V (packet voltage range).
constexpr auto LOW_BATTERY_WARNING_ENABLED = true;
constexpr float LOW_BATTERY_WARNING_V = 2.95f;

// SPD FAIL takes priority over low battery; stale/unknown rows do not alarm.
// Alarm period: 1..2147483647 ms; each on-time: 0..ALARM_PERIOD_MS.
constexpr auto ALARM_PERIOD_MS = 60000UL;
constexpr auto ALARM_FAIL_ON_MS = 5000UL;
constexpr auto ALARM_LOW_BATTERY_ON_MS = 500UL;
// Relay levels: LOW or HIGH, and opposite to each other.
constexpr auto RELAY_ACTIVE_LEVEL = LOW;
constexpr auto RELAY_INACTIVE_LEVEL = HIGH;

// Optional local input: 3V3 -> isolated SPD dry contact -> IO15.
// Closed/HIGH = OK; open/LOW = FAIL. Never connect a live/mains signal.
// Keep ID=-1 to disable. The local input occupies the first row on page 1.
// Pin: board-compatible input GPIO 0..21 or 26..48; IO15 is the tested input.
constexpr auto SPD_LOCAL_PIN = 15;
// Local ID: -1 disabled, otherwise 0..127; must not duplicate a wireless ID.
constexpr auto SPD_LOCAL_ID = -1;
// Friendly name: nonempty NUL-terminated text while enabled; LCD shows 18 bytes.
static const char *SPD_LOCAL_FRIENDLYNAME = "Local contact";
// Poll interval: 1..2147483647 ms; debounce: 0..2147483647 ms.
constexpr auto SPD_LOCAL_POLL_MS = 50UL;
constexpr auto SPD_LOCAL_DEBOUNCE_MS = 250UL;
