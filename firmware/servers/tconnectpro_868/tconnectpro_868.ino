/*
  WirelessSPDSystem — LILYGO T-Connect Pro V1.0, 868 MHz hardware.
  ESP32-S3-R8 / SX1262 / ST7796 222x480 LCD / CST226SE touch / relay.

  Receiver features (release identity is maintained in version.h):
  - Authenticated 20-byte LoRa packets, nonce replay rejection, loss/stale tracking.
  - LCD pages of 8 wireless/local SPD rows, with a configured page interval.
  - Touch mute; all configured SPDs remain monitored.
  - Fresh SPD failure: flashing LCD + relay 5 seconds per minute.
  - Fresh low battery: highlighted row + relay 500 ms per minute.
  - Stale/unknown devices do not trigger the relay. State is held in RAM only.
  - HTTP dashboard and JSON API over station Wi-Fi or a fallback access point.

  Build baseline: ESP32 core 2.0.14, RadioLib 7.8.1,
  GFX Library for Arduino 1.4.6. Touch uses Wire directly.
  Copy config.example.h to config.h and complete deployment settings first.
*/

#include <Arduino.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <Arduino_GFX_Library.h>
#include <RadioLib.h>
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdarg.h>
#include <type_traits>
#include "display_pages.h"
#include "config_types.h"
#include "board_pins.h"
#include "version.h"
#include "web_ui.h"
#include "ha_guide.h"
#include "ota_web.h"
#include "mqtt_bridge.h"
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <sys/time.h>
#include "session_log.h"
#include "json_format.h"
#include "wifi_recovery.h"

struct SpdState; // Arduino auto-prototypes may precede the state definition.

#ifndef BLACK
#define BLACK 0x0000
#endif
#ifndef WHITE
#define WHITE 0xFFFF
#endif
#ifndef RED
#define RED 0xF800
#endif
#ifndef DARKGREEN
#define DARKGREEN 0x03E0
#endif


// Edit only the private config.h for deployment settings.
#if __has_include("config.h")
#include "config.h"
#else
#error "Copy config.example.h to config.h, configure your gateway, then set CONFIGURED=true."
#include "config.example.h"
#endif
// BEGIN CONFIG VALIDATION
static_assert(std::is_same<decltype(CONFIGURED), const bool>::value, "CONFIGURED must be true or false.");
static_assert(CONFIGURED, "Configure config.h before flashing; example credentials are not usable.");
static_assert(std::is_integral<decltype(DISPLAY_PAGE_SECONDS)>::value, "DISPLAY_PAGE_SECONDS must be an integer.");
static_assert(DISPLAY_PAGE_SECONDS >= 1 && DISPLAY_PAGE_SECONDS <= 3600,
              "DISPLAY_PAGE_SECONDS must be 1..3600 seconds.");

// Fixed modem parameters shared by both beacon hardware versions.
constexpr float LORA_BW_KHZ = 125.0f;
constexpr uint8_t LORA_CR = 5; // coding rate 4/5
constexpr uint8_t LORA_SYNCWORD = 0x12;
constexpr uint16_t LORA_PREAMBLE = 8;
constexpr bool LORA_CRC_ENABLED = true;
static_assert(LORA_FREQ_MHZ >= 863.0625f && LORA_FREQ_MHZ <= 869.9375f,
              "LORA_FREQ_MHZ must be 863.0625..869.9375 MHz for 125 kHz bandwidth.");
static_assert(std::is_integral<decltype(LORA_SF)>::value, "LORA_SF must be an integer.");
static_assert(LORA_SF >= 5 && LORA_SF <= 12, "LORA_SF must be 5..12 and match every beacon.");
static_assert(std::is_integral<decltype(LORA_RX_TX_DBM)>::value, "LORA_RX_TX_DBM must be an integer.");
static_assert(LORA_RX_TX_DBM >= -9 && LORA_RX_TX_DBM <= 22, "LORA_RX_TX_DBM must be -9..22 dBm.");
static_assert(sizeof(SpdConfig::secret) == 4, "Every beacon secret must occupy exactly four bytes.");

// Millisecond intervals are checked before runtime timer arithmetic.
static_assert(validConfigInterval(WIFI_CONNECT_TIMEOUT_MS, true), "WIFI_CONNECT_TIMEOUT_MS must be 0..2147483647 ms.");
static_assert(validConfigInterval(WIFI_RETRY_INTERVAL_MS) && WIFI_RETRY_INTERVAL_MS >= 60000 && WIFI_RETRY_INTERVAL_MS <= 120000,
              "WIFI_RETRY_INTERVAL_MS must be an integer in 60000..120000 ms.");
static_assert(validConfigInterval(DISPLAY_PERIODIC_REFRESH_MS), "DISPLAY_PERIODIC_REFRESH_MS must be 1..2147483647 ms.");
static_assert(validConfigInterval(TOUCH_POLL_MS), "TOUCH_POLL_MS must be 1..2147483647 ms.");
static_assert(validConfigInterval(TOUCH_DEBOUNCE_MS, true), "TOUCH_DEBOUNCE_MS must be 0..2147483647 ms.");
static_assert(validConfigInterval(TOUCH_RELEASE_STABLE_MS, true), "TOUCH_RELEASE_STABLE_MS must be 0..2147483647 ms.");
static_assert(validConfigInterval(SPD_LOCAL_POLL_MS), "SPD_LOCAL_POLL_MS must be 1..2147483647 ms.");
static_assert(validConfigInterval(SPD_LOCAL_DEBOUNCE_MS, true), "SPD_LOCAL_DEBOUNCE_MS must be 0..2147483647 ms.");
static_assert(std::is_integral<decltype(STALE_AFTER_SECONDS)>::value, "STALE_AFTER_SECONDS must be an integer.");
static_assert(STALE_AFTER_SECONDS >= 1 && STALE_AFTER_SECONDS <= 2147483UL, "STALE_AFTER_SECONDS must be 1..2147483 seconds.");
static_assert(std::is_integral<decltype(PACKET_LOSS_WINDOW_SIZE)>::value, "PACKET_LOSS_WINDOW_SIZE must be an integer.");
static_assert(PACKET_LOSS_WINDOW_SIZE >= 1 && PACKET_LOSS_WINDOW_SIZE <= 65535, "PACKET_LOSS_WINDOW_SIZE must be 1..65535 packets.");
static_assert(std::is_same<decltype(LOW_BATTERY_WARNING_ENABLED), const bool>::value, "LOW_BATTERY_WARNING_ENABLED must be true or false.");
static_assert(LOW_BATTERY_WARNING_V >= 2.20f && LOW_BATTERY_WARNING_V <= 4.74f, "LOW_BATTERY_WARNING_V must be 2.20..4.74 V.");
static_assert(validConfigInterval(ALARM_PERIOD_MS), "ALARM_PERIOD_MS must be 1..2147483647 ms.");
static_assert(validConfigInterval(ALARM_FAIL_ON_MS, true) && ALARM_FAIL_ON_MS <= ALARM_PERIOD_MS, "ALARM_FAIL_ON_MS must be an integer in 0..ALARM_PERIOD_MS.");
static_assert(validConfigInterval(ALARM_LOW_BATTERY_ON_MS, true) && ALARM_LOW_BATTERY_ON_MS <= ALARM_PERIOD_MS, "ALARM_LOW_BATTERY_ON_MS must be an integer in 0..ALARM_PERIOD_MS.");
static_assert(std::is_integral<decltype(RELAY_ACTIVE_LEVEL)>::value, "RELAY_ACTIVE_LEVEL must be an integer LOW/HIGH value.");
static_assert(std::is_integral<decltype(RELAY_INACTIVE_LEVEL)>::value, "RELAY_INACTIVE_LEVEL must be an integer LOW/HIGH value.");
static_assert((RELAY_ACTIVE_LEVEL == LOW || RELAY_ACTIVE_LEVEL == HIGH) &&
              (RELAY_INACTIVE_LEVEL == LOW || RELAY_INACTIVE_LEVEL == HIGH) &&
              RELAY_ACTIVE_LEVEL != RELAY_INACTIVE_LEVEL, "Relay levels must be opposite LOW/HIGH values.");

// Fixed interoperable SPD packet format.
constexpr size_t SPD_PAYLOAD_LEN = 20;
constexpr size_t SPD_AUTH_PAYLOAD_LEN = 12;
constexpr size_t SPD_TAG_LEN = 8;
constexpr uint8_t BATTERY_VOLTAGE_ERROR_CODE = 0xFF;
constexpr float BATTERY_VOLTAGE_OFFSET_V = 2.20f;
constexpr float BATTERY_VOLTAGE_STEP_V = 0.01f;

constexpr size_t SPD_COUNT = sizeof(SPD_CONFIGS) / sizeof(SPD_CONFIGS[0]);
constexpr bool SPD_LOCAL_ENABLED = (SPD_LOCAL_ID >= 0);
constexpr size_t SPD_TOTAL_COUNT = SPD_COUNT + (SPD_LOCAL_ENABLED ? 1 : 0);
static_assert(std::is_integral<decltype(SPD_LOCAL_ID)>::value, "SPD_LOCAL_ID must be an integer.");
static_assert(SPD_LOCAL_ID <= 127 && (!std::is_signed<decltype(SPD_LOCAL_ID)>::value || SPD_LOCAL_ID >= -1), "SPD_LOCAL_ID must be -1 or 0..127");
static_assert(std::is_integral<decltype(SPD_LOCAL_PIN)>::value, "SPD_LOCAL_PIN must be an integer GPIO number.");
static_assert((SPD_LOCAL_PIN >= 0 && SPD_LOCAL_PIN <= 21) || (SPD_LOCAL_PIN >= 26 && SPD_LOCAL_PIN <= 48), "SPD_LOCAL_PIN must be a valid ESP32-S3 GPIO number");
static_assert(SPD_TOTAL_COUNT > 0, "Configure at least one wireless SPD or enable SPD_LOCAL_ID");
static_assert(SPD_COUNT <= 127, "Configure at most 127 wireless SPDs with unique IDs 1..127.");
static_assert(std::is_same<decltype(OTA_ENABLED), const bool>::value, "OTA_ENABLED must be true or false.");
static_assert(std::is_integral<decltype(MQTT_PORT)>::value && MQTT_PORT >= 1 && MQTT_PORT <= 65535, "MQTT_PORT must be 1..65535.");
static_assert(std::is_integral<decltype(MQTT_PUBLISH_SECONDS)>::value && MQTT_PUBLISH_SECONDS >= 1 && MQTT_PUBLISH_SECONDS <= 3600, "MQTT_PUBLISH_SECONDS must be 1..3600.");
static_assert(std::is_integral<decltype(SESSION_LOG_BYTES)>::value && SESSION_LOG_BYTES >= 65536UL && SESSION_LOG_BYTES <= 4UL * 1024UL * 1024UL,
              "SESSION_LOG_BYTES must be an integer in 65536..4194304 bytes.");
// END CONFIG VALIDATION

// =============================================================================
// DISPLAY, TOUCH, RADIO, WEB SERVER OBJECTS
// =============================================================================

void onTouchInterrupt(void);


Arduino_DataBus *bus = new Arduino_HWSPI(
  SCREEN_DC,
  SCREEN_CS,
  SCREEN_SCLK,
  SCREEN_MOSI,
  SCREEN_MISO
);

Arduino_GFX *lcd = new Arduino_ST7796(
  bus,
  SCREEN_RST,
  DISPLAY_ROTATION, // fixed landscape orientation
  true,             // IPS
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  49,               // col offset 1, copied from LILYGO example
  0,                // row offset 1
  0,                // col offset 2
  0                 // row offset 2
);

// Full-screen framebuffer. 480 x 222 x 2 bytes = about 213 kB.
// If allocation fails, setupDisplay() falls back to direct drawing on lcd.
Arduino_Canvas *screenCanvas = new Arduino_Canvas(
  DISPLAY_LANDSCAPE_WIDTH,
  DISPLAY_LANDSCAPE_HEIGHT,
  lcd
);

Arduino_GFX *gfx = screenCanvas;
bool displayUsesCanvas = false;

SX1262 radio = new Module(SX1262_CS, SX1262_DIO1, SX1262_RST, SX1262_BUSY, SPI);
WspdWebServer server(80);
static void firmwareMaintenance(bool active);
static void sessionLogLine(const char *text);
static void sessionLogf(const char *fmt, ...);
WspdWebOta firmwareOta(server, OTA_ENABLED, OTA_TOKEN, firmwareMaintenance, sessionLogLine);
WspdSessionLog sessionLog;
static WspdSessionLog::Record emergencyLog[16];
static bool sessionLogPsram = false;
static char sessionBootId[17] = {0};
static uint64_t sessionNowMs() { return uint64_t(esp_timer_get_time()) / 1000ULL; }

static void sessionLogLine(const char *text) {
  sessionLog.append(sessionNowMs(), text);
  Serial.println(text);
}

static void sessionLogf(const char *fmt, ...) {
  char message[496]; va_list args; va_start(args, fmt);
  const int size = vsnprintf(message, sizeof(message), fmt, args); va_end(args);
  if (size >= int(sizeof(message))) strcpy(message + sizeof(message) - 15, "...[truncated]");
  sessionLogLine(message);
}

static void setupSessionLog() {
  void *storage = heap_caps_malloc(SESSION_LOG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t bytes = SESSION_LOG_BYTES; sessionLogPsram = storage != nullptr;
  if (!storage) {
    bytes = 32768;
    storage = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!storage) { storage = emergencyLog; bytes = sizeof(emergencyLog); }
  sessionLog.begin(static_cast<WspdSessionLog::Record *>(storage), bytes / sizeof(WspdSessionLog::Record));
  snprintf(sessionBootId, sizeof(sessionBootId), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  sessionLogf("System start: %s, reset reason=%d", SERVER_FIRMWARE_ID, int(esp_reset_reason()));
  sessionLogf("Session log: %u bytes, %u records, storage=%s; free internal=%u PSRAM=%u bytes",
              unsigned(bytes), unsigned(sessionLog.capacity()), sessionLogPsram ? "PSRAM" : "emergency internal RAM",
              unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

static void serviceSessionClock() {
  static uint32_t lastCheck = 0;
  const uint32_t now = millis();
  if (uint32_t(now - lastCheck) < 1000UL) return; lastCheck = now;
  if (!sessionLog.clockSynced()) {
    struct timeval time; gettimeofday(&time, nullptr);
    if (time.tv_sec >= 1704067200) {
      sessionLog.syncClock(sessionNowMs(), uint64_t(time.tv_sec) * 1000ULL + unsigned(time.tv_usec / 1000));
      sessionLogLine("Clock: network time synchronized; startup timestamps reconstructed from time since boot");
    }
  }
}
static void mqttAlarmMute(bool muted);
WspdMqttBridge mqttBridge(MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD, MQTT_BASE_TOPIC, MQTT_PUBLISH_SECONDS, mqttAlarmMute);
char gatewayId[24] = {0};

// BEGIN WIFI RUNTIME STATE
static WspdWiFiRecovery wifiRecovery;
static WspdWiFiEventQueue wifiEvents;
static portMUX_TYPE wifiEventLock = portMUX_INITIALIZER_UNLOCKED;
static bool wifiTimeRequested = false;
static bool wifiFallbackStarted = false;
// END WIFI RUNTIME STATE

// =============================================================================
// STATE
// =============================================================================

struct PacketLossWindow {
  uint8_t buffer[PACKET_LOSS_WINDOW_SIZE] = {0};
  uint16_t head = 0;
  uint16_t used = 0;
  uint16_t successes = 0;

  void appendOne(bool success) {
    const uint8_t v = success ? 1 : 0;
    if (used < PACKET_LOSS_WINDOW_SIZE) {
      buffer[head] = v;
      used++;
      successes += v;
    } else {
      const uint8_t old = buffer[head];
      if (old) successes--;
      buffer[head] = v;
      successes += v;
    }
    head = (head + 1) % PACKET_LOSS_WINDOW_SIZE;
  }

  void addMany(bool success, uint32_t count) {
    if (count == 0) return;
    if (count >= PACKET_LOSS_WINDOW_SIZE) {
      memset(buffer, success ? 1 : 0, sizeof(buffer));
      head = 0;
      used = PACKET_LOSS_WINDOW_SIZE;
      successes = success ? PACKET_LOSS_WINDOW_SIZE : 0;
      return;
    }
    for (uint32_t i = 0; i < count; i++) appendOne(success);
  }

  uint16_t total() const { return used; }
  uint16_t missed() const { return used - successes; }
  float lossPercent() const {
    if (used == 0) return 0.0f;
    return (100.0f * float(missed())) / float(used);
  }
};

struct SpdState {
  bool seen = false;
  uint8_t statusCode = 0;       // 1=OK, 0=FAIL, valid only if seen and not stale
  uint8_t hardwareVersion = 0;
  uint8_t firmwareVersion = 0;
  bool batteryValid = false;
  float batteryVoltageV = 0.0f;
  int8_t temperatureC = 0;
  float rssiDbm = 0.0f;
  float snrDb = 0.0f;
  uint64_t nonce = 0;
  bool nonceKnown = false;
  uint32_t lastSeenMs = 0;
  uint32_t lastStatusChangeMs = 0;
  uint32_t packetCount = 0;
  uint32_t skippedLast = 0;
  uint32_t totalSkipped = 0;
  PacketLossWindow loss;
  char rawHex[SPD_PAYLOAD_LEN * 2 + 1] = {0};
};

static SpdState spdStates[(SPD_COUNT > 0) ? SPD_COUNT : 1];

struct LocalSpdState {
  bool seen = false;
  uint8_t statusCode = 0;      // 1=OK, 0=FAIL
  bool rawOkLast = false;
  bool stableOk = false;
  uint32_t rawChangedMs = 0;
  uint32_t lastPollMs = 0;
  uint32_t lastSeenMs = 0;
  uint32_t lastStatusChangeMs = 0;
};

static LocalSpdState localSpd;

volatile bool loraPacketFlag = false;
bool loraInitialized = false;
bool touchOk = false;
volatile bool touchInterruptFlag = false;
uint32_t lastTouchPollMs = 0;
bool touchWasDown = false;
uint32_t lastTouchActionMs = 0;
uint32_t lastTouchDownSampleMs = 0;

bool alarmMuted = false;
bool screenFlashRed = false;
bool screenDirty = true;
uint32_t lastScreenDrawMs = 0;
uint32_t lastFlashToggleMs = 0;
uint32_t bootMs = 0;
SpdDisplayPages displayPages(SPD_TOTAL_COUNT, uint32_t(DISPLAY_PAGE_SECONDS) * 1000UL);

// Boot progress screen state.
uint8_t bootLineIndex = 0;
char lastSetupFailureText[112] = {0};
int16_t lastSetupFailureCode = 0;

uint32_t totalValidPackets = 0;
uint32_t totalInvalidPackets = 0;
uint32_t totalOldNoncePackets = 0;
uint32_t totalAuthRejects = 0;
uint32_t totalUnconfiguredRejects = 0;

// Forward declarations used to avoid Arduino .ino auto-prototype ordering issues.
static void flushDisplay();
static void setRelay(bool on);
static void bootLogLine(uint16_t color, const char *text);
static void bootLogf(uint16_t color, const char *fmt, ...);
static bool setupFailure(const char *module, const char *step, int16_t code);
static void fatalBootError(int32_t code, const char *module, const char *text);

void onTouchInterrupt(void) {
  touchInterruptFlag = true;
}

static void requestScreenRedraw() {
  screenDirty = true;
}

// =============================================================================
// HELPERS
// =============================================================================

static void onDio1Action() {
  loraPacketFlag = true;
}

static bool setupFailure(const char *module, const char *step, int16_t code) {
  lastSetupFailureCode = code;
  snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "%s %s failed", module, step);
  sessionLogf("[%s] %s failed: %d\n", module, step, code);
  bootLogf(RED, "%s: %s failed (%d)", module, step, code);
  return false;
}

static const SpdConfig *findConfig(uint8_t id, size_t *indexOut = nullptr) {
  for (size_t i = 0; i < SPD_COUNT; i++) {
    if (SPD_CONFIGS[i].id == id) {
      if (indexOut) *indexOut = i;
      return &SPD_CONFIGS[i];
    }
  }
  return nullptr;
}

static uint64_t readU64LE(const uint8_t *p) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < 8; i++) {
    v |= (uint64_t)p[i] << (8 * i);
  }
  return v;
}

static void bytesToHex(const uint8_t *in, size_t len, char *out, size_t outLen) {
  static const char hex[] = "0123456789ABCDEF";
  if (outLen < (len * 2 + 1)) return;
  for (size_t i = 0; i < len; i++) {
    out[i * 2 + 0] = hex[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[in[i] & 0x0F];
  }
  out[len * 2] = 0;
}

static String u64Hex(uint64_t value) {
  char buf[19];
  snprintf(buf, sizeof(buf), "0x%08lX%08lX", (uint32_t)(value >> 32), (uint32_t)value);
  return String(buf);
}

static bool constantTimeEqual8(const uint8_t *a, const uint8_t *b) {
  uint8_t diff = 0;
  for (uint8_t i = 0; i < 8; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static bool verifyTag(const uint8_t *payload, uint32_t secret) {
  uint8_t input[SPD_AUTH_PAYLOAD_LEN + 4];
  memcpy(input, payload, SPD_AUTH_PAYLOAD_LEN);
  input[12] = uint8_t(secret >> 0);
  input[13] = uint8_t(secret >> 8);
  input[14] = uint8_t(secret >> 16);
  input[15] = uint8_t(secret >> 24);

  uint8_t hash[32];
  mbedtls_sha256(input, sizeof(input), hash, 0); // 0 = SHA-256, not SHA-224
  return constantTimeEqual8(payload + SPD_AUTH_PAYLOAD_LEN, hash);
}

static bool decodeBattery(uint8_t raw, float &voltageOut) {
  if (raw == BATTERY_VOLTAGE_ERROR_CODE) return false;
  voltageOut = BATTERY_VOLTAGE_OFFSET_V + float(raw) * BATTERY_VOLTAGE_STEP_V;
  return true;
}

static uint32_t ageSecondsFor(const SpdState &s) {
  if (!s.seen) return 0;
  return (uint32_t)((uint32_t)(millis() - s.lastSeenMs) / 1000UL);
}

static bool isStale(const SpdState &s) {
  return s.seen && ageSecondsFor(s) > STALE_AFTER_SECONDS;
}

static bool isBatteryLow(const SpdState &s) {
  return LOW_BATTERY_WARNING_ENABLED && s.seen && s.batteryValid && (s.batteryVoltageV < LOW_BATTERY_WARNING_V);
}

static bool isFresh(const SpdState &s) {
  return s.seen && !isStale(s);
}

static bool isFreshFail(const SpdState &s) {
  return isFresh(s) && s.statusCode == 0;
}

static bool isFreshBatteryLow(const SpdState &s) {
  return isFresh(s) && isBatteryLow(s);
}

static const char *effectiveStatusText(const SpdState &s) {
  if (!s.seen) return "UNKNOWN";
  if (isStale(s)) return "STALE";
  return s.statusCode == 1 ? "OK" : "FAIL";
}

static const char *effectiveLocalStatusText() {
  if (!SPD_LOCAL_ENABLED) return "UNKNOWN";
  if (!localSpd.seen) return "UNKNOWN";
  return localSpd.statusCode == 1 ? "OK" : "FAIL";
}

static bool isLocalOkRaw() {
  if (!SPD_LOCAL_ENABLED) return false;
  return digitalRead(SPD_LOCAL_PIN) == HIGH;
}

static bool isLocalFreshFail() {
  return SPD_LOCAL_ENABLED && localSpd.seen && localSpd.statusCode == 0;
}

static bool isLocalFreshOk() {
  return SPD_LOCAL_ENABLED && localSpd.seen && localSpd.statusCode == 1;
}

static void applyLocalSpdState(bool ok, bool force) {
  if (!SPD_LOCAL_ENABLED) return;
  const uint32_t now = millis();
  const uint8_t newStatus = ok ? 1 : 0;
  const bool statusChanged = !localSpd.seen || localSpd.statusCode != newStatus;

  if (force || statusChanged) {
    localSpd.statusCode = newStatus;
    localSpd.stableOk = ok;
    localSpd.seen = true;
    localSpd.lastSeenMs = now;
    localSpd.lastStatusChangeMs = now;
    sessionLogf("[Local SPD] ID %03d %s pin=IO%d\n", SPD_LOCAL_ID, ok ? "OK" : "FAIL", SPD_LOCAL_PIN);
    requestScreenRedraw();
  } else {
    localSpd.seen = true;
    localSpd.lastSeenMs = now;
  }
}

static bool validateSpdConfig() {
  if (!WIFI_SSID || !WIFI_PASSWORD || !FALLBACK_AP_SSID || !FALLBACK_AP_PASSWORD ||
      strlen(WIFI_SSID) > 32 || strlen(FALLBACK_AP_SSID) < 1 || strlen(FALLBACK_AP_SSID) > 32) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "WiFi SSID: station 0..32, AP 1..32 bytes");
    lastSetupFailureCode = -1;
    return false;
  }
  const size_t stationPasswordLength = strlen(WIFI_PASSWORD);
  bool stationHexKey = (stationPasswordLength == 64);
  if (stationHexKey) {
    for (size_t i = 0; i < stationPasswordLength; i++) {
      const char c = WIFI_PASSWORD[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
        stationHexKey = false;
        break;
      }
    }
  }
  if (stationPasswordLength != 0 && !(stationPasswordLength >= 8 && stationPasswordLength <= 63) && !stationHexKey) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "WiFi password: empty, 8..63 chars or 64 hex");
    lastSetupFailureCode = -1;
    return false;
  }
  const size_t apPasswordLength = strlen(FALLBACK_AP_PASSWORD);
  if (apPasswordLength < 8 || apPasswordLength > 63 ||
      strcmp(FALLBACK_AP_PASSWORD, "CHANGE_ME_AP_PASSWORD") == 0) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Configure AP password");
    lastSetupFailureCode = -1;
    return false;
  }

  WspdWiFiNetworkConfig networkConfig;
  const char *networkError = wspdParseNetworkConfig(WIFI_STATIC_IP, WIFI_SUBNET_MASK, WIFI_GATEWAY, WIFI_DNS1, WIFI_DNS2, networkConfig);
  if (networkError) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "%s", networkError);
    lastSetupFailureCode = -1;
    return false;
  }

  const size_t tokenLength = OTA_TOKEN ? strlen(OTA_TOKEN) : 0;
  bool tokenValid = OTA_TOKEN && (tokenLength == 0 ? !OTA_ENABLED : tokenLength >= 32 && tokenLength <= 128);
  for (size_t i = 0; tokenValid && i < tokenLength; ++i) {
    const char c = OTA_TOKEN[i]; tokenValid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }
  if (!tokenValid) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "OTA token: 32..128 ASCII letters; empty only if disabled");
    lastSetupFailureCode = -1; return false;
  }
  if (!NTP_SERVER || strlen(NTP_SERVER) > 253) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "NTP server: empty or up to 253 bytes");
    lastSetupFailureCode = -1; return false;
  }
  if (!MQTT_HOST || !MQTT_USERNAME || !MQTT_PASSWORD || !MQTT_BASE_TOPIC ||
      strlen(MQTT_HOST) > 253 || strlen(MQTT_USERNAME) > 128 || strlen(MQTT_PASSWORD) > 128 ||
      strlen(MQTT_BASE_TOPIC) < 1 || strlen(MQTT_BASE_TOPIC) > 64 ||
      strchr(MQTT_BASE_TOPIC, '+') || strchr(MQTT_BASE_TOPIC, '#')) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Configure valid MQTT host, credentials and topic");
    lastSetupFailureCode = -1; return false;
  }
  for (size_t i = 0; i < SPD_COUNT; i++) {
    if (SPD_CONFIGS[i].id == 0 || SPD_CONFIGS[i].id > 127 || SPD_CONFIGS[i].secret == 0) {
      snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Configure IDs 1..127 and nonzero beacon keys");
      lastSetupFailureCode = -1;
      return false;
    }
    if (!SPD_CONFIGS[i].friendlyName || SPD_CONFIGS[i].friendlyName[0] == '\0') {
      snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Configure nonempty beacon names");
      lastSetupFailureCode = SPD_CONFIGS[i].id;
      return false;
    }
    for (size_t j = 0; j < i; j++) {
      if (SPD_CONFIGS[i].id == SPD_CONFIGS[j].id) {
        snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Duplicate LoRa SPD ID %u", SPD_CONFIGS[i].id);
        lastSetupFailureCode = SPD_CONFIGS[i].id;
        return false;
      }
    }
  }

  if (!SPD_LOCAL_ENABLED) return true;

  if (!SPD_LOCAL_FRIENDLYNAME || SPD_LOCAL_FRIENDLYNAME[0] == '\0') {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Configure nonempty local SPD name");
    lastSetupFailureCode = SPD_LOCAL_ID;
    return false;
  }

  if (SPD_LOCAL_ID < 0 || SPD_LOCAL_ID > 127) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "SPD_LOCAL_ID must be -1 or 0..127");
    lastSetupFailureCode = SPD_LOCAL_ID;
    return false;
  }

  for (size_t i = 0; i < SPD_COUNT; i++) {
    if (SPD_CONFIGS[i].id == (uint8_t)SPD_LOCAL_ID) {
      snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Local SPD ID duplicates LoRa SPD ID %d", SPD_LOCAL_ID);
      lastSetupFailureCode = SPD_LOCAL_ID;
      return false;
    }
  }

  return true;
}

static void setupLocalSpd() {
  if (!SPD_LOCAL_ENABLED) {
    bootLogLine(BLACK, "Local SPD: disabled");
    return;
  }

  pinMode(SPD_LOCAL_PIN, INPUT_PULLDOWN);
  delay(2);

  const bool rawOk = isLocalOkRaw();
  const uint32_t now = millis();
  localSpd.rawOkLast = rawOk;
  localSpd.stableOk = rawOk;
  localSpd.rawChangedMs = now;
  localSpd.lastPollMs = now;
  localSpd.seen = false;
  applyLocalSpdState(rawOk, true);

  bootLogf(BLACK, "Local SPD: ID %03d IO%d active HIGH", SPD_LOCAL_ID, SPD_LOCAL_PIN);
}

static void handleLocalSpd() {
  if (!SPD_LOCAL_ENABLED) return;

  const uint32_t now = millis();
  if ((uint32_t)(now - localSpd.lastPollMs) < SPD_LOCAL_POLL_MS) return;
  localSpd.lastPollMs = now;

  const bool rawOk = isLocalOkRaw();

  if (rawOk != localSpd.rawOkLast) {
    localSpd.rawOkLast = rawOk;
    localSpd.rawChangedMs = now;
  }

  if (!localSpd.seen) {
    applyLocalSpdState(rawOk, true);
    return;
  }

  if ((uint32_t)(now - localSpd.rawChangedMs) >= SPD_LOCAL_DEBOUNCE_MS && rawOk != localSpd.stableOk) {
    applyLocalSpdState(rawOk, true);
    return;
  }

  localSpd.lastSeenMs = now;
}

// Use plain uint8_t constants instead of a custom enum type in function signatures.
// This avoids Arduino .ino auto-prototype issues where generated prototypes can
// be inserted before custom types are declared.
constexpr uint8_t ALARM_NONE = 0;
constexpr uint8_t ALARM_LOW_BATTERY = 1;
constexpr uint8_t ALARM_SPD_FAIL = 2;

static uint8_t currentAlarmKind() {
  if (isLocalFreshFail()) {
    return ALARM_SPD_FAIL;
  }

  bool hasFreshLowBattery = false;

  for (size_t i = 0; i < SPD_COUNT; i++) {
    const SpdState &s = spdStates[i];

    // Highest priority: any fresh SPD reporting FAIL.
    if (isFreshFail(s)) {
      return ALARM_SPD_FAIL;
    }

    // Lower priority: any fresh SPD reporting a low battery voltage.
    // Stale/unknown devices are informational only and do not alarm.
    if (isFreshBatteryLow(s)) {
      hasFreshLowBattery = true;
    }
  }

  return hasFreshLowBattery ? ALARM_LOW_BATTERY : ALARM_NONE;
}

static const char *alarmKindText(uint8_t kind) {
  switch (kind) {
    case ALARM_SPD_FAIL:    return "spd_fail";
    case ALARM_LOW_BATTERY: return "low_battery";
    default:                return "none";
  }
}

static bool isAlarmConditionActive() {
  return currentAlarmKind() != ALARM_NONE;
}

static String jsonEscape(const char *s) {
  String out;
  out.reserve(strlen(s) + 8);
  while (*s) {
    char c = *s++;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) out += ' ';
        else out += c;
    }
  }
  return out;
}

static bool configuredStationWiFi() {
  return WIFI_SSID && WIFI_SSID[0] && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

static bool stationOnline() {
  return (int(WiFi.getMode()) & int(WIFI_STA)) && WiFi.status() == WL_CONNECTED &&
         uint32_t(WiFi.localIP()) != 0;
}

static bool fallbackApOnline() {
  return wifiFallbackStarted && (int(WiFi.getMode()) & int(WIFI_AP)) && uint32_t(WiFi.softAPIP()) != 0;
}

static String ipString() {
  if (stationOnline()) return WiFi.localIP().toString();
  if (fallbackApOnline()) return WiFi.softAPIP().toString();
  return configuredStationWiFi() ? String("connecting") : String("offline");
}

// =============================================================================
// LORA RECEIVE AND SPD PROCESSING
// =============================================================================

static void processSpdPayload(const uint8_t *payload, size_t len, float rssi, float snr) {
  if (len != SPD_PAYLOAD_LEN) {
    totalInvalidPackets++;
    sessionLogf("[LoRa] Reject wrong payload length: %u\n", (unsigned)len);
    return;
  }

  char packetRaw[SPD_PAYLOAD_LEN * 2 + 1]; bytesToHex(payload, len, packetRaw, sizeof(packetRaw));
  sessionLogf("[LoRa] Received %u bytes RSSI=%.1f dBm SNR=%.1f dB raw=%s", unsigned(len), rssi, snr, packetRaw);
  const uint8_t claimedSpdId = (payload[1] >> 1) & 0x7F;
  size_t idx = 0;
  const SpdConfig *cfg = findConfig(claimedSpdId, &idx);
  if (!cfg) {
    totalInvalidPackets++;
    totalUnconfiguredRejects++;
    sessionLogf("[LoRa] Reject unconfigured SPD ID %u\n", claimedSpdId);
    return;
  }

  if (!verifyTag(payload, cfg->secret)) {
    totalInvalidPackets++;
    totalAuthRejects++;
    sessionLogf("[LoRa] Reject auth/tag failure for SPD ID %u\n", claimedSpdId);
    return;
  }

  const uint8_t hwFw = payload[0];
  const uint8_t hardwareVersion = (hwFw >> 4) & 0x0F;
  const uint8_t firmwareVersion = hwFw & 0x0F;
  const uint8_t statusCode = payload[1] & 0x01;
  const int8_t tempC = (int8_t)payload[2];
  const uint64_t nonce = readU64LE(payload + 3);
  const uint8_t batteryRaw = payload[11];

  SpdState &s = spdStates[idx];

  if (s.nonceKnown && nonce <= s.nonce) {
    totalOldNoncePackets++;
    sessionLogf("[LoRa] Reject old nonce SPD %u got=%s last=%s\n",
                  claimedSpdId, u64Hex(nonce).c_str(), u64Hex(s.nonce).c_str());
    return;
  }

  uint32_t skipped = 0;
  if (s.nonceKnown) {
    skipped = (uint32_t)(nonce - s.nonce - 1ULL);
  }

  const bool firstSeen = !s.seen;
  const uint8_t prevStatus = s.statusCode;

  s.seen = true;
  s.statusCode = statusCode;
  s.hardwareVersion = hardwareVersion;
  s.firmwareVersion = firmwareVersion;
  s.temperatureC = tempC;
  s.rssiDbm = rssi;
  s.snrDb = snr;
  s.nonce = nonce;
  s.nonceKnown = true;
  s.lastSeenMs = millis();
  s.packetCount++;
  s.skippedLast = skipped;
  s.totalSkipped += skipped;
  s.loss.addMany(false, skipped);
  s.loss.addMany(true, 1);
  s.batteryValid = decodeBattery(batteryRaw, s.batteryVoltageV);
  bytesToHex(payload, len, s.rawHex, sizeof(s.rawHex));

  if (firstSeen || prevStatus != statusCode) {
    s.lastStatusChangeMs = millis();
  }

  totalValidPackets++;

  sessionLogf("[LoRa] SPD %03u %s name='%.80s' hw/fw=%u/%u temp=%+dC bat=%s RSSI=%.1f SNR=%.1f nonce=%s skipped=%lu loss=%.1f%% (%u/%u) raw=%s\n",
                claimedSpdId,
                statusCode ? "OK" : "FAIL",
                cfg->friendlyName,
                hardwareVersion,
                firmwareVersion,
                tempC,
                s.batteryValid ? String(s.batteryVoltageV, 2).c_str() : "N/A",
                rssi,
                snr,
                u64Hex(nonce).c_str(),
                (unsigned long)skipped,
                s.loss.lossPercent(),
                s.loss.missed(),
                s.loss.total(),
                s.rawHex);

  requestScreenRedraw();
}

static bool loraApplyStep(const char *name, int16_t code) {
  if (code != RADIOLIB_ERR_NONE) {
    return setupFailure("LoRa", name, code);
  }
  bootLogf(BLACK, "LoRa: %s OK", name);
  return true;
}

static bool setupLoRa() {
  lastSetupFailureCode = 0;
  lastSetupFailureText[0] = '\0';

  bootLogLine(BLACK, "LoRa: starting SX1262 SPI");
  SPI.begin(SX1262_SCLK, SX1262_MISO, SX1262_MOSI, SX1262_CS);
  radio.setDio1Action(onDio1Action);

  if (!loraApplyStep("radio.begin", radio.begin())) return false;
  if (!loraApplyStep("setFrequency", radio.setFrequency(LORA_FREQ_MHZ))) return false;
  if (!loraApplyStep("setBandwidth", radio.setBandwidth(LORA_BW_KHZ))) return false;
  if (!loraApplyStep("setSpreadingFactor", radio.setSpreadingFactor(LORA_SF))) return false;
  if (!loraApplyStep("setCodingRate", radio.setCodingRate(LORA_CR))) return false;
  if (!loraApplyStep("setSyncWord", radio.setSyncWord(LORA_SYNCWORD))) return false;
  if (!loraApplyStep("setPreambleLength", radio.setPreambleLength(LORA_PREAMBLE))) return false;
  if (!loraApplyStep("setOutputPower", radio.setOutputPower(LORA_RX_TX_DBM))) return false;
  if (!loraApplyStep("explicitHeader", radio.explicitHeader())) return false;

  int16_t state;
  if (LORA_CRC_ENABLED) {
    state = radio.setCRC(2);
  } else {
    state = radio.setCRC(false);
  }
  if (!loraApplyStep("setCRC", state)) return false;
  if (!loraApplyStep("startReceive", radio.startReceive())) return false;

  sessionLogf("[LoRa] RX ready: %.4f MHz SF%u BW%.1fkHz CR4/%u sync=0x%02X preamble=%u CRC=%s\n",
                LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ, LORA_CR, LORA_SYNCWORD, LORA_PREAMBLE,
                LORA_CRC_ENABLED ? "on" : "off");
  bootLogf(DARKGREEN, "LoRa ready: %.4fMHz SF%u BW%.0fk", LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ);
  loraInitialized = true;
  return true;
}

static void handleLoRa() {
  if (!loraInitialized) return;
  if (!loraPacketFlag) return;

  loraPacketFlag = false;

  uint8_t payload[SPD_PAYLOAD_LEN];
  const size_t packetLen = radio.getPacketLength();
  int16_t state;

  if (packetLen == SPD_PAYLOAD_LEN) {
    state = radio.readData(payload, sizeof(payload));
    if (state == RADIOLIB_ERR_NONE) {
      const float rssi = radio.getRSSI();
      const float snr  = radio.getSNR();
      processSpdPayload(payload, packetLen, rssi, snr);
    } else {
      totalInvalidPackets++;
      sessionLogf("[LoRa] readData failed: %d RSSI=%.1f dBm SNR=%.1f dB", state, radio.getRSSI(), radio.getSNR());
    }
  } else {
    // Drain unexpected packet length.
    uint8_t drain[64];
    const size_t drainLen = (packetLen < sizeof(drain)) ? packetLen : sizeof(drain);
    state = radio.readData(drain, drainLen);
    totalInvalidPackets++;
    char raw[129]; bytesToHex(drain, state == RADIOLIB_ERR_NONE ? drainLen : 0, raw, sizeof(raw));
    sessionLogf("[LoRa] Reject packet length %u, read state=%d RSSI=%.1f dBm SNR=%.1f dB first64=%s", unsigned(packetLen), state, radio.getRSSI(), radio.getSNR(), raw);
  }

  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    sessionLogf("[LoRa] restart RX failed: %d\n", state);
  }
}

// =============================================================================
// DISPLAY AND TOUCH
// =============================================================================

static uint16_t colorLightRed() { return gfx->color565(255, 220, 220); }
static uint16_t colorLightGreen() { return gfx->color565(220, 255, 230); }
static uint16_t colorLightOrange() { return gfx->color565(255, 238, 210); }
static uint16_t colorLightGray() { return gfx->color565(238, 238, 238); }

static void drawBootHeader() {
  if (!gfx) return;
  bootLineIndex = 0;
  gfx->fillScreen(WHITE);
  gfx->setTextSize(1);
  gfx->setTextColor(BLACK);
  gfx->setCursor(4, 4);
  gfx->print("SPD Monitor booting...");
  gfx->setCursor(gfx->width() - 4 - 6 * (sizeof(SERVER_FIRMWARE_ID) - 1), 4);
  gfx->print(SERVER_FIRMWARE_ID);
  gfx->drawLine(0, 18, gfx->width(), 18, BLACK);
  flushDisplay();
}

static void bootLogLine(uint16_t color, const char *text) {
  sessionLogLine(text);
  if (!gfx) return;
  const int16_t y = 22 + (int16_t)bootLineIndex * 9;
  if (y <= gfx->height() - 8) {
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    gfx->setCursor(4, y);
    gfx->print(text);
    flushDisplay();
  }
  if (bootLineIndex < 26) bootLineIndex++;
}

static void bootLogf(uint16_t color, const char *fmt, ...) {
  char buf[112];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  bootLogLine(color, buf);
}

static void fatalBootError(int32_t code, const char *module, const char *text) {
  setRelay(false);
  sessionLogf("[BOOT ERROR] %s code=%ld text=%s\n", module, (long)code, text);

  if (gfx) {
    gfx->fillScreen(WHITE);
    gfx->setTextSize(1);
    gfx->setTextColor(RED);
    gfx->setCursor(4, 4);
    gfx->print("BOOT ERROR - STOPPED");
    gfx->setCursor(gfx->width() - 4 - 6 * (sizeof(SERVER_FIRMWARE_ID) - 1), 4);
    gfx->print(SERVER_FIRMWARE_ID);
    gfx->drawLine(0, 18, gfx->width(), 18, RED);

    gfx->setCursor(4, 32);
    gfx->print("Module:");
    gfx->setCursor(88, 32);
    gfx->print(module ? module : "unknown");

    gfx->setCursor(4, 48);
    gfx->print("Code:");
    gfx->setCursor(88, 48);
    gfx->print(code);

    gfx->setCursor(4, 64);
    gfx->print("Error:");
    gfx->setCursor(88, 64);
    gfx->print(text ? text : "unknown");

    gfx->setTextColor(BLACK);
    gfx->setCursor(4, 96);
    gfx->print("Fix issue and press RESET.");
    flushDisplay();
  }

  while (true) {
    setRelay(false);
    delay(250);
  }
}

constexpr int16_t MUTE_BTN_X  = 388;
constexpr int16_t MUTE_BTN_Y  = 194;
constexpr int16_t MUTE_BTN_W  = 92;
constexpr int16_t MUTE_BTN_H  = 24;

static bool pointInRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh);
}

static void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char *label) {
  gfx->fillRect(x, y, w, h, colorLightGray());
  gfx->drawRect(x, y, w, h, BLACK);
  gfx->setTextColor(BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(x + 8, y + 7);
  gfx->print(label);
}

static void flushDisplay() {
  if (displayUsesCanvas && screenCanvas) {
    screenCanvas->flush();
  }
}

static String ageText(const SpdState &s) {
  if (!s.seen) return "never";
  const uint32_t age = ageSecondsFor(s);
  if (age < 60) return "<1m";
  if (age < 3600) return String(age / 60) + "m";
  return String(age / 3600) + "h";
}

static String lossText(const SpdState &s) {
  if (s.loss.total() == 0) return "-";
  return String(s.loss.lossPercent(), 0) + "%";
}

static bool hasBatteryBeacon() {
  for (size_t i = 0; i < SPD_COUNT; ++i) {
    if (spdStates[i].seen && spdStates[i].hardwareVersion == 2) return true;
  }
  return false;
}

static void drawStatusTable() {
  // Keep column positions consistent across pages, including stale battery beacons.
  const bool showBattery = hasBatteryBeacon();
  const int16_t rssiX = showBattery ? 244 : 202;
  const int16_t snrX = showBattery ? 310 : 280;
  const int16_t lossX = 378;
  const int16_t ageX = 434;
  const uint8_t alarmKind = currentAlarmKind();
  const bool failAlarm = (alarmKind == ALARM_SPD_FAIL);
  const bool redFlashFrame = failAlarm && screenFlashRed;
  const uint16_t bg = redFlashFrame ? colorLightRed() : WHITE;
  gfx->fillScreen(bg);

  gfx->setTextSize(1);
  gfx->setTextColor(BLACK);
  gfx->setCursor(2, 2);
  gfx->printf("SPD Monitor  LoRa %.4f SF%u  IP %s", LORA_FREQ_MHZ, LORA_SF, ipString().c_str());

  gfx->setCursor(2, 12);
  if (alarmKind == ALARM_SPD_FAIL) {
    gfx->setTextColor(RED);
    gfx->printf("SPD FAIL ALARM%s", alarmMuted ? " MUTED" : "");
  } else if (alarmKind == ALARM_LOW_BATTERY) {
    gfx->setTextColor(BLACK);
    gfx->printf("LOW BATTERY ALARM%s", alarmMuted ? " MUTED" : "");
  } else {
    gfx->setTextColor(DARKGREEN);
    gfx->printf("No active alarm%s", alarmMuted ? " (muted)" : "");
  }

  const int16_t y0 = 30;
  const int16_t rowH = 18;

  gfx->setTextColor(BLACK);
  gfx->drawLine(0, y0 - 4, gfx->width(), y0 - 4, BLACK);
  gfx->setCursor(2, y0);
  gfx->print("ID");
  gfx->setCursor(28, y0);
  gfx->print("ST");
  gfx->setCursor(78, y0);
  gfx->print("NAME");
  if (showBattery) {
    gfx->setCursor(202, y0);
    gfx->print("BAT");
  }
  gfx->setCursor(rssiX, y0);
  gfx->print("RSSI dBm");
  gfx->setCursor(snrX, y0);
  gfx->print("SNR dB");
  gfx->setCursor(lossX, y0);
  gfx->print("LOSS");
  gfx->setCursor(ageX, y0);
  gfx->print("AGE");
  gfx->drawLine(0, y0 + 12, gfx->width(), y0 + 12, BLACK);

  for (size_t row = 0; row < displayPages.rowCount(); row++) {
    const size_t absoluteRow = displayPages.firstRow() + row;
    const int16_t y = y0 + 15 + (int16_t)row * rowH;
    const bool rowIsLocal = SPD_LOCAL_ENABLED && absoluteRow == 0;

    if (rowIsLocal) {
      if (redFlashFrame) {
        gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightRed());
      } else if (isLocalFreshFail()) {
        gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightRed());
      } else if (isLocalFreshOk()) {
        gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightGreen());
      }

      gfx->drawLine(0, y + rowH - 3, gfx->width(), y + rowH - 3, BLACK);

      gfx->setTextColor(BLACK);
      gfx->setCursor(2, y);
      gfx->printf("%03d", SPD_LOCAL_ID);

      const char *st = effectiveLocalStatusText();
      gfx->setCursor(28, y);
      if (strcmp(st, "FAIL") == 0) {
        gfx->setTextColor(RED);
      } else if (strcmp(st, "OK") == 0) {
        gfx->setTextColor(DARKGREEN);
      } else {
        gfx->setTextColor(BLACK);
      }
      gfx->print(st);

      gfx->setTextColor(BLACK);
      gfx->setCursor(78, y);
      char nameBuf[19];
      snprintf(nameBuf, sizeof(nameBuf), "%-18.18s", SPD_LOCAL_FRIENDLYNAME);
      gfx->print(nameBuf);

      // The local SPD has no LoRa telemetry, battery, nonce, packet loss, RSSI, or SNR.
      // Visually treat the remaining telemetry columns as one merged cell.
      gfx->setCursor(270, y);
      gfx->print("LOCAL");
      continue;
    }

    const size_t i = absoluteRow - (SPD_LOCAL_ENABLED ? 1 : 0);
    const SpdConfig &cfg = SPD_CONFIGS[i];
    const SpdState &s = spdStates[i];

    if (redFlashFrame) {
      // During SPD failure flash, every row is light-red.
      gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightRed());
    } else if (isFreshFail(s)) {
      // Fresh failed SPD rows stay light-red when the base background is white.
      gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightRed());
    } else if (isFreshBatteryLow(s)) {
      // Low-battery rows are light-orange, but do not make the full screen flash.
      gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightOrange());
    } else if (isFresh(s) && s.statusCode == 1) {
      // Fresh OK rows are light-green.
      gfx->fillRect(0, y - 2, gfx->width(), rowH, colorLightGreen());
    }
    // Unknown/stale rows are intentionally left white unless the full-screen
    // SPD-failure flash frame is active.

    gfx->drawLine(0, y + rowH - 3, gfx->width(), y + rowH - 3, BLACK);

    gfx->setTextColor(BLACK);
    gfx->setCursor(2, y);
    gfx->printf("%03u", cfg.id);

    const char *st = effectiveStatusText(s);
    gfx->setCursor(28, y);
    if (strcmp(st, "FAIL") == 0) {
      gfx->setTextColor(RED);
    } else if (strcmp(st, "OK") == 0) {
      gfx->setTextColor(DARKGREEN);
    } else {
      gfx->setTextColor(BLACK);
    }
    gfx->print(st);

    gfx->setTextColor(BLACK);
    gfx->setCursor(78, y);
    char nameBuf[19];
    snprintf(nameBuf, sizeof(nameBuf), "%-18.18s", cfg.friendlyName);
    gfx->print(nameBuf);

    if (showBattery) {
      gfx->setCursor(202, y);
      if (!s.seen) {
        gfx->print("-");
      } else if (!s.batteryValid) {
        gfx->print("N/A");
      } else {
        if (isFreshBatteryLow(s)) gfx->setTextColor(RED);
        gfx->printf("%.2f", s.batteryVoltageV);
        gfx->setTextColor(BLACK);
      }
    }

    gfx->setCursor(rssiX, y);
    if (s.seen) gfx->printf("%.0f", s.rssiDbm); else gfx->print("-");

    gfx->setCursor(snrX, y);
    if (s.seen) gfx->printf("%.1f", s.snrDb); else gfx->print("-");

    gfx->setCursor(lossX, y);
    gfx->print(lossText(s));

    gfx->setCursor(ageX, y);
    gfx->print(ageText(s));
  }

  if (displayPages.pageCount() > 1) {
    gfx->setTextColor(BLACK);
    gfx->setCursor(2, MUTE_BTN_Y + 7);
    gfx->printf("Page %u/%u  SPDs %u-%u/%u",
                (unsigned)(displayPages.pageIndex() + 1),
                (unsigned)displayPages.pageCount(),
                (unsigned)(displayPages.firstRow() + 1),
                (unsigned)(displayPages.firstRow() + displayPages.rowCount()),
                (unsigned)SPD_TOTAL_COUNT);
  }

  gfx->setTextColor(BLACK);
  gfx->setCursor(2, 211);
  gfx->print(SERVER_FIRMWARE_ID);

  drawButton(MUTE_BTN_X, MUTE_BTN_Y, MUTE_BTN_W, MUTE_BTN_H, alarmMuted ? "UNMUTE" : "MUTE");

  flushDisplay();
  screenDirty = false;
  lastScreenDrawMs = millis();
}

static void setupDisplay() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // ESP32 Arduino core 3.x: LEDC channel is assigned automatically by pin.
  ledcAttach(SCREEN_BL, 2000, 8);
  ledcWrite(SCREEN_BL, 255);
#else
  // ESP32 Arduino core 2.x: separate channel setup/attach API.
  ledcSetup(1, 2000, 8);
  ledcAttachPin(SCREEN_BL, 1);
  ledcWrite(1, 255);
#endif

  // Prefer a framebuffer canvas so fillScreen/table redraw happens off-screen.
  // screenCanvas->begin() also begins the physical lcd output.
  displayUsesCanvas = screenCanvas && screenCanvas->begin();
  if (displayUsesCanvas) {
    gfx = screenCanvas;
    lcd->setRotation(DISPLAY_ROTATION);
    sessionLogf("[Display] Canvas enabled: %d x %d, rotation=%u\n", gfx->width(), gfx->height(), DISPLAY_ROTATION);
  } else {
    sessionLogLine("[Display] Canvas allocation failed; falling back to direct LCD drawing");
    lcd->begin();
    lcd->setRotation(DISPLAY_ROTATION);
    gfx = lcd;
  }

  drawBootHeader();
  bootLogf(BLACK, "Display: %s %dx%d rot=%u", displayUsesCanvas ? "canvas" : "direct", gfx->width(), gfx->height(), DISPLAY_ROTATION);
}

// ---- CST226SE direct I2C touch driver ----
// This avoids pulling in Arduino_DriveBus, whose unused I2S source currently
// does not compile with ESP32 Arduino core 3.x.
constexpr uint8_t CST226SE_ADDR = 0x5A;
constexpr uint8_t CST_REG_X1_H  = 0x01;
constexpr uint8_t CST_REG_Y1_H  = 0x02;
constexpr uint8_t CST_REG_XY1_L = 0x03;
constexpr uint8_t CST_REG_FINGERS = 0x05;
constexpr uint8_t CST_REG_ID = 0x06;

static bool cstReadReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(CST226SE_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom((uint8_t)CST226SE_ADDR, (uint8_t)1) != 1) {
    return false;
  }

  value = Wire.read();
  return true;
}

static int16_t cstReadDeviceId() {
  uint8_t id = 0;
  return cstReadReg(CST_REG_ID, id) ? (int16_t)id : -1;
}

static bool cstReadFirstTouch(uint8_t &fingersOut, int16_t &rawXOut, int16_t &rawYOut) {
  uint8_t xh = 0, yh = 0, xyl = 0, fingers = 0;
  if (!cstReadReg(CST_REG_FINGERS, fingers)) return false;
  fingers &= 0x0F;
  fingersOut = fingers;
  if (fingers == 0) return true;

  if (!cstReadReg(CST_REG_X1_H, xh)) return false;
  if (!cstReadReg(CST_REG_Y1_H, yh)) return false;
  if (!cstReadReg(CST_REG_XY1_L, xyl)) return false;

  rawXOut = (int16_t)(((uint16_t)xh << 4) | ((xyl & 0xF0) >> 4));
  rawYOut = (int16_t)(((uint16_t)yh << 4) |  (xyl & 0x0F));
  return true;
}

static bool setupTouch() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  if (TOUCH_RST >= 0) {
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, HIGH);
    delay(100);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(200);
  }

  // Equivalent to the Arduino_DriveBus init buffer: begin/end transmission + short delay.
  Wire.beginTransmission(CST226SE_ADDR);
  const bool present = (Wire.endTransmission() == 0);
  delay(20);

  if (!present) {
    sessionLogLine("[Touch] CST226SE not found at I2C address 0x5A");
    touchOk = false;
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "CST226SE not found at I2C 0x5A");
    lastSetupFailureCode = -1;
    return false;
  }

  pinMode(TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TOUCH_INT), onTouchInterrupt, FALLING);

  const int16_t id = cstReadDeviceId();
  sessionLogf("[Touch] CST226SE init OK, ID=0x%02X\n", id >= 0 ? id : 0);
  bootLogf(BLACK, "Touch: CST226SE OK ID=0x%02X", id >= 0 ? id : 0);
  touchInterruptFlag = false;
  lastTouchPollMs = millis();
  touchOk = true;
  return true;
}

static bool pollTouchLandscape(bool &downOut, int16_t &xOut, int16_t &yOut) {
  downOut = false;
  if (!touchOk) return false;

  // Poll often enough to see finger release. Interrupt is still used as a fast hint,
  // but debounce/edge detection below is what prevents repeated button toggles.
  const uint32_t now = millis();
  if (!touchInterruptFlag && (now - lastTouchPollMs) < TOUCH_POLL_MS) {
    return false;
  }
  touchInterruptFlag = false;
  lastTouchPollMs = now;

  uint8_t fingers = 0;
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!cstReadFirstTouch(fingers, rawX, rawY)) {
    return false;
  }

  if (fingers == 0) {
    downOut = false;
    return true;
  }

  // Touch controller reports portrait coordinates: X 0..221, Y 0..479.
  // Map native portrait touch coordinates to landscape rotation 3.
  int16_t x = (int16_t)(SCREEN_HEIGHT - 1 - rawY);
  int16_t y = (int16_t)rawX;

  x = constrain(x, 0, (int16_t)gfx->width() - 1);
  y = constrain(y, 0, (int16_t)gfx->height() - 1);

  downOut = true;
  xOut = x;
  yOut = y;
  return true;
}

static void setAlarmMuted(bool muted, const char *source) {
  alarmMuted = muted;
  sessionLogf("[Alarm] %s via %s", muted ? "Muted" : "Unmuted", source);
  if (muted) setRelay(false);
  requestScreenRedraw();
  mqttBridge.notifyMuteStateChanged();
}

static void mqttAlarmMute(bool muted) { setAlarmMuted(muted, "Home Assistant MQTT"); }

static void handleTouch() {
  bool down = false;
  int16_t x = 0, y = 0;
  if (!pollTouchLandscape(down, x, y)) return;

  const uint32_t now = millis();

  if (!down) {
    // Capacitive controllers can occasionally report one empty sample during a
    // long press. Require a short stable no-touch period before arming again.
    if (touchWasDown && (uint32_t)(now - lastTouchDownSampleMs) >= TOUCH_RELEASE_STABLE_MS) {
      touchWasDown = false;
    }
    return;
  }

  lastTouchDownSampleMs = now;

  // Trigger only on a fresh press edge, not continuously while the finger remains down.
  if (touchWasDown) return;
  if (now - lastTouchActionMs < TOUCH_DEBOUNCE_MS) {
    touchWasDown = true;
    return;
  }

  touchWasDown = true;
  lastTouchActionMs = now;

  if (pointInRect(x, y, MUTE_BTN_X, MUTE_BTN_Y, MUTE_BTN_W, MUTE_BTN_H)) {
    setAlarmMuted(!alarmMuted, "touch");
  }
}

// =============================================================================
// RELAY ALARM
// =============================================================================

static void setRelay(bool on) {
  static bool known = false, current = false;
  if (!known || current != on) { sessionLogf("[Relay] Output %s", on ? "ON" : "OFF"); current = on; known = true; }
  digitalWrite(RELAY_1, on ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);
}

static void setupRelay() {
  pinMode(RELAY_1, OUTPUT);
  setRelay(false);
}

static void handleAlarmRelay() {
  const uint8_t alarmKind = currentAlarmKind();
  if (alarmKind == ALARM_NONE || alarmMuted) {
    setRelay(false);
    return;
  }

  const uint32_t onMs = (alarmKind == ALARM_SPD_FAIL)
                          ? ALARM_FAIL_ON_MS
                          : ALARM_LOW_BATTERY_ON_MS;
  const uint32_t phase = (millis() - bootMs) % ALARM_PERIOD_MS;
  setRelay(phase < onMs);
}

static void serviceSessionEvents() {
  static int previousMqtt = -1, previousAlarm = -1;
  static uint32_t lastCheck = 0; static bool wasStale[SPD_COUNT] = {false};
  const uint32_t now = millis();
  if (uint32_t(now - lastCheck) < 1000UL) return; lastCheck = now;
  const int mqtt = mqttBridge.connected() ? 1 : 0;
  if (mqtt != previousMqtt) { sessionLogf("[MQTT] %s", mqttBridge.enabled() ? (mqtt ? "Connected" : "Disconnected") : "Disabled"); previousMqtt = mqtt; }
  const int alarm = currentAlarmKind();
  if (alarm != previousAlarm) { sessionLogf("[Alarm] State=%s muted=%s", alarmKindText(alarm), alarmMuted ? "yes" : "no"); previousAlarm = alarm; }
  for (size_t i = 0; i < SPD_COUNT; ++i) {
    const bool stale = isStale(spdStates[i]);
    if (stale != wasStale[i]) { sessionLogf("[SPD] ID %u %s", SPD_CONFIGS[i].id, stale ? "STALE: report timeout" : "Fresh reports resumed"); wasStale[i] = stale; }
  }
}

// =============================================================================
// WI-FI AND WEB SERVER
// =============================================================================

static void firmwareMaintenance(bool active) {
  sessionLogf("[Monitoring] %s for firmware update", active ? "Paused; relay OFF" : "Resumed");
  mqttBridge.setPaused(active);
  setRelay(false);
  if (loraInitialized) {
    if (active) radio.standby(); else radio.startReceive();
    loraPacketFlag = false;
  }
  if (active) {
    gfx->fillScreen(WHITE); gfx->setTextColor(BLACK); gfx->setTextSize(1);
    gfx->setCursor(16, 50); gfx->print("Firmware update in progress");
    gfx->setCursor(16, 72); gfx->print("Monitoring paused; relay OFF");
    gfx->setCursor(16, 94); gfx->print("Keep power connected");
    flushDisplay();
  } else requestScreenRedraw();
}

static void mqttSpdSnapshot(size_t row, WspdMqttSpdSnapshot &out) {
  if (SPD_LOCAL_ENABLED && row == 0) {
    out.id = SPD_LOCAL_ID; out.name = SPD_LOCAL_FRIENDLYNAME; out.local = true;
    out.seen = out.fresh = localSpd.seen; out.status = effectiveLocalStatusText(); return;
  }
  const size_t i = row - (SPD_LOCAL_ENABLED ? 1 : 0);
  const SpdConfig &cfg = SPD_CONFIGS[i]; const SpdState &s = spdStates[i];
  out.id = cfg.id; out.name = cfg.friendlyName; out.status = effectiveStatusText(s);
  out.seen = s.seen; out.fresh = isFresh(s); out.batteryValid = s.batteryValid;
  out.temperatureC = s.temperatureC <= -126 ? NAN : float(s.temperatureC);
  out.batteryV = s.batteryVoltageV; out.rssiDbm = s.rssiDbm; out.snrDb = s.snrDb;
  out.lossPercent = s.loss.total() ? s.loss.lossPercent() : NAN;
  out.ageSeconds = s.seen ? ageSecondsFor(s) : 0;
}
static void mqttGatewaySnapshot(WspdMqttGatewaySnapshot &out) {
  out.configured = SPD_TOTAL_COUNT; out.uptimeSeconds = (millis() - bootMs) / 1000UL; out.alarmMuted = alarmMuted;
  if (SPD_LOCAL_ENABLED) { if (!localSpd.seen) ++out.unknown; else if (!localSpd.statusCode) ++out.failed; }
  for (size_t i = 0; i < SPD_COUNT; ++i) {
    if (!spdStates[i].seen) ++out.unknown;
    else if (isStale(spdStates[i])) { ++out.stale; ++out.unknown; }
    else if (!spdStates[i].statusCode) ++out.failed;
  }
}

static void appendLocalSpdJson(String &out) {
  const char *status = effectiveLocalStatusText();
  out += "{";
  out += "\"spd_id\":" + String(SPD_LOCAL_ID) + ",";
  out += "\"friendly_name\":\"" + jsonEscape(SPD_LOCAL_FRIENDLYNAME) + "\",";
  out += "\"source\":\"local\",";
  out += "\"local\":true,";
  out += "\"hardware_version\":null,";
  out += "\"firmware_version\":null,";
  out += "\"battery_voltage_v\":null,";
  out += "\"battery_low\":false,";
  out += "\"battery_low_alarm\":false,";
  out += "\"status\":\"" + String(status) + "\",";
  out += "\"status_code\":";
  if (localSpd.seen) out += String(localSpd.statusCode); else out += "null";
  out += ",\"last_reported_status\":";
  if (localSpd.seen) out += String("\"") + (localSpd.statusCode ? "OK" : "FAIL") + "\""; else out += "null";
  out += ",\"temperature_c\":null,";
  out += "\"rssi_dbm\":null,";
  out += "\"snr_db\":null,";
  out += "\"nonce_hex\":null,";
  out += "\"raw_hex\":null,";
  out += "\"packet_count\":0,";
  out += "\"skipped\":0,";
  out += "\"total_skipped\":0,";
  out += "\"packet_expected_total\":0,";
  out += "\"packet_loss_percent\":null,";
  out += "\"packet_loss_window_total\":0,";
  out += "\"packet_loss_window_missed\":0,";
  out += "\"packet_loss_window_received\":0,";
  out += "\"age_seconds\":null,";
  out += "\"stale\":false";
  out += "}";
}

static void appendWirelessSpdJson(String &out, size_t i) {
  const SpdConfig &cfg = SPD_CONFIGS[i];
  const SpdState &s = spdStates[i];
  const bool staleFlag = isStale(s);
  const bool low = isBatteryLow(s);
  const bool freshLow = isFreshBatteryLow(s);
  const char *status = effectiveStatusText(s);

  out += "{";
  out += "\"spd_id\":" + String(cfg.id) + ",";
  out += "\"friendly_name\":\"" + jsonEscape(cfg.friendlyName) + "\",";
  out += "\"source\":\"lora\",";
  out += "\"local\":false,";
  out += "\"hardware_version\":" + String(s.seen ? String(s.hardwareVersion) : "null") + ",";
  out += "\"firmware_version\":" + String(s.seen ? String(s.firmwareVersion) : "null") + ",";
  out += "\"battery_voltage_v\":";
  if (s.seen && s.batteryValid) out += String(s.batteryVoltageV, 2); else out += "null";
  out += ",\"battery_low\":" + String(low ? "true" : "false") + ",";
  out += "\"battery_low_alarm\":" + String(freshLow ? "true" : "false") + ",";
  out += "\"status\":\"" + String(status) + "\",";
  out += "\"status_code\":";
  if (s.seen && !staleFlag) out += String(s.statusCode); else out += "null";
  out += ",\"last_reported_status\":";
  if (s.seen) out += String("\"") + (s.statusCode ? "OK" : "FAIL") + "\""; else out += "null";
  out += ",\"temperature_c\":" + String(s.seen ? String(s.temperatureC) : "null") + ",";
  out += "\"rssi_dbm\":" + String(s.seen ? String(s.rssiDbm, 1) : "null") + ",";
  out += "\"snr_db\":" + String(s.seen ? String(s.snrDb, 1) : "null") + ",";
  out += "\"nonce_hex\":";
  if (s.nonceKnown) out += String("\"") + u64Hex(s.nonce) + "\""; else out += "null";
  out += ",\"raw_hex\":";
  if (s.seen) out += String("\"") + s.rawHex + "\""; else out += "null";
  out += ",\"packet_count\":" + String(s.packetCount) + ",";
  out += "\"skipped\":" + String(s.skippedLast) + ",";
  out += "\"total_skipped\":" + String(s.totalSkipped) + ",";
  out += "\"packet_expected_total\":" + String(s.packetCount + s.totalSkipped) + ",";
  out += "\"packet_loss_percent\":";
  if (s.loss.total() > 0) out += String(s.loss.lossPercent(), 3); else out += "null";
  out += ",\"packet_loss_window_total\":" + String(s.loss.total()) + ",";
  out += "\"packet_loss_window_missed\":" + String(s.loss.missed()) + ",";
  out += "\"packet_loss_window_received\":" + String(s.loss.total() - s.loss.missed()) + ",";
  out += "\"age_seconds\":";
  if (s.seen) out += String(ageSecondsFor(s)); else out += "null";
  out += ",\"stale\":" + String(staleFlag ? "true" : "false");
  out += "}";
}

static String buildApiJson() {
  uint16_t seen = 0, ok = 0, fail = 0, unknown = 0, stale = 0, lowBatt = 0;

  if (SPD_LOCAL_ENABLED) {
    if (localSpd.seen) {
      seen++;
      if (localSpd.statusCode == 1) ok++; else fail++;
    } else {
      unknown++;
    }
  }

  for (size_t i = 0; i < SPD_COUNT; i++) {
    const SpdState &s = spdStates[i];
    if (s.seen) seen++;
    if (!s.seen) {
      unknown++;
    } else if (isStale(s)) {
      stale++;
      unknown++;
    } else if (s.statusCode == 1) {
      ok++;
    } else {
      fail++;
    }
    if (isFreshBatteryLow(s)) lowBatt++;
  }

  String out;
  out.reserve(768 + SPD_TOTAL_COUNT * 800);
  out += "{";
  out += "\"api_version\":1,";
  out += "\"generated_at_ms\":" + String(millis()) + ",";
  out += "\"gateway\":{";
  out += "\"gateway_id\":\"" + String(gatewayId) + "\",";
  out += "\"server_version\":\"" + String(SERVER_VERSION) + "\",";
  out += "\"firmware_id\":\"" + String(SERVER_FIRMWARE_ID) + "\",";
  out += "\"ip\":\"" + ipString() + "\",";
  out += "\"uptime_seconds\":" + String((millis() - bootMs) / 1000UL) + ",";
  out += "\"total_valid_packets\":" + String(totalValidPackets) + ",";
  out += "\"total_invalid_packets\":" + String(totalInvalidPackets) + ",";
  out += "\"total_old_nonce_packets\":" + String(totalOldNoncePackets) + ",";
  out += "\"total_auth_rejects\":" + String(totalAuthRejects) + ",";
  out += "\"total_unconfigured_rejects\":" + String(totalUnconfiguredRejects) + ",";
  out += "\"lora_freq_mhz\":" + String(LORA_FREQ_MHZ, 4) + ",";
  out += "\"lora_sf\":" + String(LORA_SF) + ",";
  out += "\"lora_bw_khz\":" + String(LORA_BW_KHZ, 1) + ",";
  out += "\"local_spd_enabled\":" + String(SPD_LOCAL_ENABLED ? "true" : "false") + ",";
  out += "\"local_spd_pin\":";
  if (SPD_LOCAL_ENABLED) out += String(SPD_LOCAL_PIN); else out += "null";
  out += ",\"local_spd_id\":";
  if (SPD_LOCAL_ENABLED) out += String(SPD_LOCAL_ID); else out += "null";
  out += ",\"battery_low_threshold_v\":";
  if (LOW_BATTERY_WARNING_ENABLED) out += String(LOW_BATTERY_WARNING_V, 2); else out += "null";
  out += ",\"stale_after_seconds\":" + String(STALE_AFTER_SECONDS) + ",";
  const uint8_t alarmKind = currentAlarmKind();
  out += "\"packet_loss_window_size\":" + String(PACKET_LOSS_WINDOW_SIZE) + ",";
  out += "\"alarm_active\":" + String(alarmKind != ALARM_NONE ? "true" : "false") + ",";
  out += "\"alarm_kind\":\"" + String(alarmKindText(alarmKind)) + "\",";
  out += "\"alarm_spd_fail\":" + String(alarmKind == ALARM_SPD_FAIL ? "true" : "false") + ",";
  out += "\"alarm_low_battery\":" + String(alarmKind == ALARM_LOW_BATTERY ? "true" : "false") + ",";
  out += "\"alarm_muted\":" + String(alarmMuted ? "true" : "false") + ",";
  out += "\"ota\":{\"enabled\":" + String(firmwareOta.enabled() ? "true" : "false") + ",\"busy\":" + String(firmwareOta.busy() ? "true" : "false");
  out += ",\"slot_size\":" + String(firmwareOta.slotSize()) + ",\"image_marker\":\"" + String(WSPD_OTA_IMAGE_MARKER) + "\"},";
  out += "\"mqtt\":{\"enabled\":" + String(mqttBridge.enabled() ? "true" : "false") + ",\"connected\":" + String(mqttBridge.connected() ? "true" : "false") + "}";
  out += "},";

  out += "\"summary\":{";
  out += "\"configured\":" + String(SPD_TOTAL_COUNT) + ",";
  out += "\"seen\":" + String(seen) + ",";
  out += "\"ok\":" + String(ok) + ",";
  out += "\"fail\":" + String(fail) + ",";
  out += "\"unknown\":" + String(unknown) + ",";
  out += "\"stale\":" + String(stale) + ",";
  out += "\"low_battery\":" + String(lowBatt);
  out += "},";

  out += "\"spds\":[";
  bool first = true;
  if (SPD_LOCAL_ENABLED) {
    appendLocalSpdJson(out);
    first = false;
  }
  for (size_t i = 0; i < SPD_COUNT; i++) {
    if (!first) out += ",";
    appendWirelessSpdJson(out, i);
    first = false;
  }
  out += "]}";
  return out;
}

static void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

static void handleIndex() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleHaGuide() {
  server.sendHeader("Cache-Control", "no-store");
  if (server.hasArg("download") && server.arg("download") == "1")
    server.sendHeader("Content-Disposition", "attachment; filename=\"wireless-spd-home-assistant.html\"");
  server.send_P(200, "text/html; charset=utf-8", HA_GUIDE_HTML);
}

static void handleHaRest() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "attachment; filename=\"wireless-spd-rest.yaml\"");
  server.send_P(200, "text/yaml; charset=utf-8", HA_REST_YAML);
}

static void handleApiGet() {
  const String body = buildApiJson();
  if (!server.hasArg("pretty") || server.arg("pretty") != "1") { sendJson(200, body); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  char chunk[1025]; size_t used = 0;
  wspdPrettyJson(body.c_str(), body.length(), [&](char c) {
    chunk[used++] = c;
    if (used == sizeof(chunk) - 1) { chunk[used] = 0; server.sendContent(chunk); used = 0; }
  });
  if (used) { chunk[used] = 0; server.sendContent(chunk); }
  server.sendContent("");
}

static void handleMute() {
  const bool muted = server.hasArg("value") ? server.arg("value") != "0" : !alarmMuted;
  setAlarmMuted(muted, "web");
  sendJson(200, String("{\"status\":\"ok\",\"alarm_muted\":") + (alarmMuted ? "true" : "false") + "}");
}

static String logDecimal(uint64_t value) {
  char text[21]; snprintf(text, sizeof(text), "%llu", (unsigned long long)value); return String(text);
}

static void handleLogs() {
  uint64_t after = 0, before = 0, limit = 100;
  if ((server.hasArg("after") && !wspdLogNumber(server.arg("after").c_str(), after)) ||
      (server.hasArg("before") && !wspdLogNumber(server.arg("before").c_str(), before)) ||
      (server.hasArg("limit") && !wspdLogNumber(server.arg("limit").c_str(), limit)) ||
      limit < 1 || limit > 100 || (server.hasArg("after") && server.hasArg("before")) ||
      (server.hasArg("tail") && server.arg("tail") != "1")) {
    sendJson(400, "{\"error\":\"query\",\"message\":\"Use after or before, limit 1..100, or tail=1.\"}"); return;
  }
  uint64_t end = sessionLog.next(), begin = sessionLog.oldest();
  if (server.hasArg("before")) {
    if (before < end) end = before;
    if (end < begin) end = begin;
    begin = end - begin > limit ? end - limit : begin;
  } else if (server.hasArg("tail")) begin = end - begin > limit ? end - limit : begin;
  else if (server.hasArg("after") && after >= begin) begin = after >= end ? end : after + 1;
  const uint64_t finish = end - begin > limit ? begin + limit : end;
  String meta = "{\"boot_id\":\"" + String(sessionBootId) + "\",\"capacity\":" + String(sessionLog.capacity());
  meta += ",\"bytes\":" + String(sessionLog.capacity() * sizeof(WspdSessionLog::Record));
  meta += ",\"psram\":" + String(sessionLogPsram ? "true" : "false") + ",\"dropped\":" + logDecimal(sessionLog.dropped());
  meta += ",\"clock_synced\":" + String(sessionLog.clockSynced() ? "true" : "false");
  meta += ",\"oldest_seq\":" + logDecimal(sessionLog.oldest()) + ",\"next_seq\":" + logDecimal(sessionLog.next());
  meta += ",\"has_more\":" + String((server.hasArg("before") ? begin > sessionLog.oldest() : finish < end) ? "true" : "false");
  meta += ",\"entries\":[";
  server.sendHeader("Cache-Control", "no-store"); server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", ""); server.sendContent(meta);
  for (uint64_t seq = begin; seq < finish; ++seq) {
    const WspdSessionLog::Record *record = sessionLog.get(seq); if (!record) continue;
    String row = seq == begin ? "{" : ",{"; // Assemble only one entry at a time.
    row += "\"seq\":" + logDecimal(seq) + ",\"boot_ms\":" + logDecimal(record->bootMs) + ",\"unix_ms\":";
    const uint64_t epoch = sessionLog.unixMs(record->bootMs);
    row += epoch ? logDecimal(epoch) : String("null");
    row += ",\"message\":\"" + jsonEscape(record->message) + "\"}"; server.sendContent(row);
  }
  server.sendContent("]}"); server.sendContent("");
}

static void handleHealthz() {
  sendJson(200, "{\"status\":\"ok\"}");
}

static bool setupWebServer() {
  const char *headers[] = {"Authorization", "X-WSPD-Update"};
  server.collectHeaders(headers, 2);
  firmwareOta.setup();
  server.on("/", HTTP_GET, handleIndex);
  server.on("/index.html", HTTP_GET, handleIndex);
  server.on("/ha-guide", HTTP_GET, handleHaGuide);
  server.on("/ha-rest.yaml", HTTP_GET, handleHaRest);
  server.on("/api/v1/get", HTTP_GET, handleApiGet);
  server.on("/api/v1/logs", HTTP_GET, handleLogs);
  server.on("/api/v1/mute", HTTP_POST, handleMute);
  server.on("/healthz", HTTP_GET, handleHealthz);
  server.onNotFound([]() {
    sendJson(404, "{\"error\":\"not_found\"}");
  });
  server.begin();
  sessionLogf("[Web] http://%s/\n", ipString().c_str());
  bootLogf(BLACK, "Web: http://%s/", ipString().c_str());
  return true;
}

// Wi-Fi events run on the core's event task. Only copy fixed-size diagnostics;
// the receiver loop owns log writes, display updates and all recovery actions.
static void queueWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  WspdWiFiEvent entry;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    entry.type = WspdWiFiEvent::Disconnected;
    entry.reason = info.wifi_sta_disconnected.reason;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    entry.type = WspdWiFiEvent::GotIp;
    entry.ip = info.got_ip.ip_info.ip.addr;
  } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
    entry.type = WspdWiFiEvent::LostIp;
  } else return;
  portENTER_CRITICAL(&wifiEventLock);
  wifiEvents.push(entry);
  portEXIT_CRITICAL(&wifiEventLock);
}

static const char *wifiDisconnectReasonText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "unspecified";
    case WIFI_REASON_AUTH_EXPIRE: return "authentication expired";
    case WIFI_REASON_AUTH_LEAVE: return "authentication leave";
    case WIFI_REASON_ASSOC_EXPIRE: return "association expired";
    case WIFI_REASON_ASSOC_LEAVE: return "association leave / local retry";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "WPA handshake timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "group key timeout";
    case WIFI_REASON_BEACON_TIMEOUT: return "access point beacon timeout";
    case WIFI_REASON_NO_AP_FOUND: return "access point not found";
    case WIFI_REASON_AUTH_FAIL: return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    default: return "other; see ESP32 Wi-Fi reason code";
  }
}

static void drainWiFiEvents() {
  uint32_t dropped;
  portENTER_CRITICAL(&wifiEventLock);
  dropped = wifiEvents.takeDropped();
  portEXIT_CRITICAL(&wifiEventLock);
  if (dropped) sessionLogf("[WiFi] Diagnostic queue full; %lu older events omitted", (unsigned long)dropped);
  for (size_t i = 0; i < WspdWiFiEventQueue::CAPACITY; ++i) {
    WspdWiFiEvent event;
    portENTER_CRITICAL(&wifiEventLock);
    const bool available = wifiEvents.pop(event);
    portEXIT_CRITICAL(&wifiEventLock);
    if (!available) break;
    if (event.type == WspdWiFiEvent::Disconnected) {
      sessionLogf("[WiFi] Station disconnected: reason=%u (%s)", unsigned(event.reason), wifiDisconnectReasonText(event.reason));
    } else if (event.type == WspdWiFiEvent::GotIp) {
      sessionLogf("[WiFi] Station received IPv4 %s", IPAddress(event.ip).toString().c_str());
    } else sessionLogLine("[WiFi] Station lost IPv4; waiting for DHCP / reconnect");
  }
}

static bool startFallbackAp() {
  if (fallbackApOnline()) return true;
  // A configured station stays enabled so it can recover while the local
  // dashboard remains available through the fallback AP.
  const bool modeOk = WiFi.mode(configuredStationWiFi() ? WIFI_AP_STA : WIFI_AP);
  const bool apOk = modeOk && WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD);
  wifiFallbackStarted = apOk;
  if (apOk) {
    sessionLogf("[WiFi] Fallback AP started: SSID=%s IP=%s", FALLBACK_AP_SSID, WiFi.softAPIP().toString().c_str());
    requestScreenRedraw();
  } else sessionLogLine("[WiFi] Fallback AP start failed");
  return apOk;
}

static IPAddress wifiIPv4Address(uint32_t address) {
  return IPAddress(uint8_t(address >> 24), uint8_t(address >> 16), uint8_t(address >> 8), uint8_t(address));
}

static bool applyWiFiNetworkConfig() {
  WspdWiFiNetworkConfig config;
  const char *error = wspdParseNetworkConfig(WIFI_STATIC_IP, WIFI_SUBNET_MASK, WIFI_GATEWAY, WIFI_DNS1, WIFI_DNS2, config);
  if (error) { sessionLogf("[WiFi] Network configuration rejected: %s", error); return false; }
  // All zeros explicitly select DHCP again after a driver restart. Optional
  // static gateway/DNS remain zero; no router or DNS addresses are guessed.
  const IPAddress address = wifiIPv4Address(config.address), gateway = wifiIPv4Address(config.gateway);
  const IPAddress mask = wifiIPv4Address(config.mask), dns1 = wifiIPv4Address(config.dns1), dns2 = wifiIPv4Address(config.dns2);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // The compatibility WiFi.config wrapper reorders parameters for masks whose
  // first octet is not 255. The explicit STA API preserves every valid mask.
  const bool ok = WiFi.STA.begin(false) && WiFi.STA.config(address, gateway, mask, dns1, dns2);
#else
  const bool ok = WiFi.config(address, gateway, mask, dns1, dns2);
#endif
  if (!ok) { sessionLogLine("[WiFi] Applying station IPv4 configuration failed; next retry remains scheduled"); return false; }
  if (config.dhcp) sessionLogLine("[WiFi] Station IPv4: DHCP");
  else sessionLogf("[WiFi] Station IPv4: static %s mask=%s gateway=%s DNS=%s,%s",
                   address.toString().c_str(), mask.toString().c_str(), gateway.toString().c_str(),
                   dns1.toString().c_str(), dns2.toString().c_str());
  return true;
}

static bool restartStationWiFi(bool keepAp) {
  sessionLogf("[WiFi] Restarting Wi-Fi%s; retry interval %lus", keepAp ? " (fallback AP briefly cycles)" : "",
              (unsigned long)(WIFI_RETRY_INTERVAL_MS / 1000UL));
  // Reinitialize the full driver rather than depending on a reason-specific
  // reconnect. Framework stop/start calls are bounded; there is no association
  // or DHCP wait here, and the main-loop OTA guard defers the whole restart.
  const bool stopped = WiFi.mode(WIFI_OFF);
  wifiFallbackStarted = false;
  const bool started = WiFi.mode(keepAp ? WIFI_AP_STA : WIFI_STA);
  if (!started) {
    sessionLogLine("[WiFi] Driver restart failed; next retry remains scheduled");
    if (keepAp) startFallbackAp();
    return false;
  }
  if (!stopped) sessionLogLine("[WiFi] Driver stop was not confirmed; reapplying station settings");
  const bool configured = applyWiFiNetworkConfig();
  if (configured) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const bool apOk = !keepAp || startFallbackAp();
  return configured && apOk;
}

static void serviceWiFi(uint32_t now) {
  drainWiFiEvents();
  const bool paused = firmwareOta.busy();
  const WspdWiFiRecovery::Actions actions = wifiRecovery.poll(now, stationOnline(), fallbackApOnline(), paused);
  if (actions.retry) {
    restartStationWiFi(actions.startAp || fallbackApOnline());
  } else if (actions.startAp) startFallbackAp();

  static int previousStatus = -100, previousMode = -100;
  static uint32_t previousStationIp = 0, previousApIp = 0;
  static bool previousOnline = false;
  const int status = int(WiFi.status()), mode = int(WiFi.getMode());
  const uint32_t stationIp = uint32_t(WiFi.localIP()), apIp = uint32_t(WiFi.softAPIP());
  const bool online = stationOnline();
  if (online && (!previousOnline || stationIp != previousStationIp)) {
    mqttBridge.setNetworkAddress(WiFi.localIP().toString().c_str());
  }
  if (status != previousStatus || mode != previousMode || stationIp != previousStationIp || apIp != previousApIp || online != previousOnline) {
    sessionLogf("[WiFi] Station=%s status=%d mode=%d address=%s", online ? "online" : "offline", status, mode, ipString().c_str());
    requestScreenRedraw();
    previousStatus = status; previousMode = mode;
    previousStationIp = stationIp; previousApIp = apIp; previousOnline = online;
  }
  if (online && !paused && !wifiTimeRequested && NTP_SERVER[0]) {
    configTime(0, 0, NTP_SERVER);
    wifiTimeRequested = true;
    sessionLogLine("[Clock] Requesting network time after station connection");
  }
}

static bool setupWiFi() {
  WiFi.persistent(false);
  // Application retries cover every failure reason and DHCP loss at a bounded
  // cadence; avoid competing with the core's reason-dependent retry policy.
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(queueWiFiEvent);
  WiFi.mode(WIFI_STA);
  bool connected = false;

  if (configuredStationWiFi()) {
    bootLogf(BLACK, "WiFi: connecting to %s", WIFI_SSID);
    sessionLogf("[WiFi] Connecting to %s\n", WIFI_SSID);
    if (applyWiFiNetworkConfig()) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const uint32_t start = millis();
    while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      if (stationOnline()) {
        connected = true;
        break;
      }
      delay(250);
    }
  } else {
    bootLogLine(BLACK, "WiFi: station SSID not configured");
  }

  wifiRecovery.begin(millis(), configuredStationWiFi(), stationOnline(), WIFI_RETRY_INTERVAL_MS);
  if (connected) {
    sessionLogf("[WiFi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
    bootLogf(BLACK, "WiFi: STA IP %s", WiFi.localIP().toString().c_str());
    return true;
  }

  if (configuredStationWiFi()) {
    bootLogLine(BLACK, "WiFi: STA timeout, starting AP");
  }

  if (!startFallbackAp()) {
    snprintf(lastSetupFailureText, sizeof(lastSetupFailureText), "Fallback AP start failed");
    lastSetupFailureCode = -1;
    return false;
  }

  bootLogf(BLACK, "WiFi: AP %s IP %s", FALLBACK_AP_SSID, WiFi.softAPIP().toString().c_str());
  return true;
}

// =============================================================================
// SETUP / LOOP
// =============================================================================

void setup() {
  bootMs = millis();
  snprintf(gatewayId, sizeof(gatewayId), "wspd-%012llx", (unsigned long long)ESP.getEfuseMac());
  Serial.begin(115200);
  delay(200);
  setupSessionLog();
  sessionLogf("T-Connect Pro SPD Monitor %s starting\n", SERVER_FIRMWARE_ID);

  setupRelay();
  setupDisplay();
  bootLogLine(BLACK, "Relay: output initialized OFF");

  if (!validateSpdConfig()) {
    fatalBootError(lastSetupFailureCode, "Config", lastSetupFailureText[0] ? lastSetupFailureText : "Invalid local SPD configuration");
  }

  bootLogf(BLACK, "Config: %u SPDs (%u LoRa + %u local), stale %lus",
           (unsigned)SPD_TOTAL_COUNT,
           (unsigned)SPD_COUNT,
           (unsigned)(SPD_LOCAL_ENABLED ? 1 : 0),
           (unsigned long)STALE_AFTER_SECONDS);
  bootLogf(BLACK, "Radio cfg: %.4fMHz SF%u BW%.0fk CR4/%u", LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ, LORA_CR);
  setupLocalSpd();

  bootLogLine(BLACK, "Touch: initializing CST226SE");
  if (!setupTouch()) {
    fatalBootError(lastSetupFailureCode, "Touch", lastSetupFailureText[0] ? lastSetupFailureText : "Touch initialization failed");
  }

  if (!setupWiFi()) {
    fatalBootError(lastSetupFailureCode, "WiFi", lastSetupFailureText[0] ? lastSetupFailureText : "WiFi initialization failed");
  }

  if (stationOnline() && NTP_SERVER[0]) {
    configTime(0, 0, NTP_SERVER);
    wifiTimeRequested = true;
    bootLogLine(BLACK, "Clock: requesting network time (UTC)");
  } else bootLogLine(BLACK, "Clock: using time since boot until network time is available");

  if (!setupWebServer()) {
    fatalBootError(-1, "Web", "Web server initialization failed");
  }

  loraInitialized = setupLoRa();
  if (!loraInitialized) {
    fatalBootError(lastSetupFailureCode, "LoRa SX1262", lastSetupFailureText[0] ? lastSetupFailureText : "LoRa initialization failed");
  }

  if (!mqttBridge.begin(gatewayId, SERVER_FIRMWARE_ID, ipString().c_str(), SPD_TOTAL_COUNT,
                        mqttSpdSnapshot, mqttGatewaySnapshot)) {
    bootLogLine(RED, "MQTT: initialization failed; monitoring continues");
  } else if (mqttBridge.enabled()) bootLogLine(BLACK, "MQTT: discovery prepared for station LAN");
  bootLogLine(DARKGREEN, "Boot: complete");
  delay(1200);

  requestScreenRedraw();
  drawStatusTable();
  // Start timing after boot messages and the first complete table draw.
  displayPages.restart(millis());
}

void loop() {
  serviceWiFi(millis());
  serviceSessionClock();
  serviceSessionEvents();
  server.handleClient();
  firmwareOta.loop(millis());
  if (firmwareOta.busy()) { setRelay(false); return; }
  mqttBridge.loop(millis(), stationOnline());
  handleLoRa();
  handleLocalSpd();
  handleTouch();
  handleAlarmRelay();

  const uint32_t now = millis();
  if (displayPages.advance(now)) {
    requestScreenRedraw();
  }
  const uint8_t alarmKind = currentAlarmKind();
  const bool failAlarm = (alarmKind == ALARM_SPD_FAIL);

  if (failAlarm && (uint32_t)(now - lastFlashToggleMs) >= 500UL) {
    lastFlashToggleMs = now;
    screenFlashRed = !screenFlashRed;
    requestScreenRedraw();
  }

  if (!failAlarm && screenFlashRed) {
    screenFlashRed = false;
    requestScreenRedraw();
  }

  if ((uint32_t)(now - lastScreenDrawMs) >= DISPLAY_PERIODIC_REFRESH_MS) {
    requestScreenRedraw();
  }

  if (screenDirty) {
    drawStatusTable();
  }
}
