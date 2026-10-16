#!/usr/bin/env python3
"""Exercise the receiver's actual Wi-Fi recovery and event handoff on a host."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"


def function(source, name):
    match = re.search(r"(?:static\s+)?(?:const char \*|String|IPAddress|bool|void)\s*" +
                      name + r"\([^;{]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Cannot find firmware function: {name}")
    depth = 1
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]',
                         source[match.end():], re.S)
    for token in tokens:
        if token.group() not in ("{", "}"):
            continue
        depth += 1 if token.group() == "{" else -1
        if not depth:
            return source[match.start():match.end() + token.end()]
    raise RuntimeError(f"Unclosed function: {name}")


SHIMS = r'''
#include "wifi_recovery.h"
#include "config_types.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <array>
#include <type_traits>
#include <vector>
#include <iostream>
using String = std::string;
constexpr int WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3;
constexpr int WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6;
constexpr int BLACK = 0, ESP_OK = 0;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = RETRY_MS;
constexpr uint8_t WIFI_REASON_UNSPECIFIED = 1, WIFI_REASON_AUTH_EXPIRE = 2,
  WIFI_REASON_AUTH_LEAVE = 3, WIFI_REASON_ASSOC_EXPIRE = 4,
  WIFI_REASON_ASSOC_LEAVE = 8, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
  WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT = 16, WIFI_REASON_BEACON_TIMEOUT = 200,
  WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_AUTH_FAIL = 202,
  WIFI_REASON_ASSOC_FAIL = 203, WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
  WIFI_REASON_CONNECTION_FAIL = 205;
using esp_err_t = int;
const char *esp_err_to_name(esp_err_t value) { return value ? "ESP_FAIL" : "ESP_OK"; }
uint32_t nowMs = 0, delayCalls = 0, redrawCalls = 0, ntpCalls = 0;
uint32_t connectCalls = 0, disconnectCalls = 0;
bool inWiFiCallback = false;
int criticalDepth = 0;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) (++criticalDepth)
#define portEXIT_CRITICAL(mux) (--criticalDepth)
uint32_t millis() { return nowMs; }
void delay(uint32_t ms) { assert(!inWiFiCallback); nowMs += ms; ++delayCalls; }
struct IPAddress {
  uint32_t value;
  IPAddress(uint32_t v = 0) : value(v) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
    : value((uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | d) {}
  uint8_t operator[](int index) const { return uint8_t(value >> ((3 - index) * 8)); }
  operator uint32_t() const { return value; }
  bool operator==(const IPAddress &other) const { return value == other.value; }
  String toString() const {
    char text[24]; snprintf(text, sizeof(text), "%u.%u.%u.%u",
      unsigned((value >> 24) & 255), unsigned((value >> 16) & 255),
      unsigned((value >> 8) & 255), unsigned(value & 255)); return text;
  }
};
enum WiFiEvent_t {
  ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 5,
  ARDUINO_EVENT_WIFI_STA_GOT_IP = 7,
  ARDUINO_EVENT_WIFI_STA_LOST_IP = 8,
  ARDUINO_EVENT_WIFI_STA_CONNECTED = 4,
  ARDUINO_EVENT_WIFI_AP_START = 10
};
struct WiFiEventInfo_t {
  struct { uint8_t reason = 0; } wifi_sta_disconnected;
  struct { struct { struct { uint32_t addr = 0; } ip; } ip_info; } got_ip;
};
bool firmwareBusy();
struct WiFiShim {
  int modeValue = WIFI_OFF, statusValue = WL_DISCONNECTED;
  uint32_t ipValue = 0;
  unsigned beginCalls = 0, apCalls = 0, autoReconnectCalls = 0, configsAtLastBegin = 0;
  bool apSuccess = true, apActive = false, autoReconnect = true, succeedConnect = false;
  bool configSuccess = true, modeSuccess = true;
  std::vector<int> modes;
  std::vector<std::array<uint32_t, 5>> configs;
  bool connectDuringBoot = false;
  void (*callback)(WiFiEvent_t, WiFiEventInfo_t) = nullptr;
  bool mode(int mode) {
    assert(!inWiFiCallback); modes.push_back(mode);
    if (!modeSuccess) return false;
    modeValue = mode;
    if (mode == WIFI_OFF) { statusValue = WL_DISCONNECTED; ipValue = 0; apActive = false; }
    return true;
  }
  int getMode() { return modeValue; }
  int status() {
    if (connectDuringBoot && nowMs >= 500) {
      statusValue = WL_CONNECTED; ipValue = 0xC000020A;
    }
    return statusValue;
  }
  IPAddress localIP() { return IPAddress(ipValue); }
  IPAddress softAPIP() { return IPAddress(apActive ? 0xC0A80401 : 0); }
  bool softAP(const char *ssid, const char *password) {
    assert(!inWiFiCallback && !firmwareBusy()); ++apCalls;
    assert(String(ssid) == "test-ap" && String(password) == "test-password");
    apActive = apSuccess; return apSuccess;
  }
  int begin(const char *ssid, const char *password) {
    assert(!inWiFiCallback && !firmwareBusy()); ++beginCalls;
    assert(String(ssid) == "test-network" && String(password) == "test-password");
    assert(configs.size() > configsAtLastBegin); // Config reapplied before every begin.
    configsAtLastBegin = configs.size();
    if (succeedConnect) {
      statusValue = WL_CONNECTED; ipValue = configs.back()[0] ? configs.back()[0] : 0xC000020B;
    }
    return statusValue;
  }
  bool config(IPAddress ip, IPAddress gateway, IPAddress mask, IPAddress dns1 = IPAddress(), IPAddress dns2 = IPAddress()) {
    // Reproduce the core-3 compatibility wrapper's parameter heuristic. A /7
    // mask must reach the explicit STA API, otherwise this changes its route.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (uint32_t(ip) != 0 && mask[0] != 255) {
      const auto originalDns = dns1;
      dns1 = gateway; gateway = mask;
      mask = uint32_t(originalDns) ? originalDns : IPAddress(255, 255, 255, 0);
    }
#endif
    return recordConfig(ip, gateway, mask, dns1, dns2);
  }
  bool recordConfig(IPAddress ip, IPAddress gateway, IPAddress mask, IPAddress dns1, IPAddress dns2) {
    assert(!inWiFiCallback); configs.push_back({uint32_t(ip), uint32_t(gateway), uint32_t(mask), uint32_t(dns1), uint32_t(dns2)});
    return configSuccess;
  }
  void setAutoReconnect(bool enabled) { ++autoReconnectCalls; autoReconnect = enabled; }
  void persistent(bool enabled) { assert(!enabled); }
  void onEvent(void (*fn)(WiFiEvent_t, WiFiEventInfo_t)) { callback = fn; }
  struct StaShim {
    WiFiShim *parent;
    bool begin(bool connect) { assert(!connect); return true; }
    bool config(IPAddress ip, IPAddress gateway, IPAddress mask, IPAddress dns1 = IPAddress(), IPAddress dns2 = IPAddress()) {
      return parent->recordConfig(ip, gateway, mask, dns1, dns2);
    }
  } STA{this};
} WiFi;
const char *WIFI_SSID = "test-network", *WIFI_PASSWORD = "test-password";
const char *FALLBACK_AP_SSID = "test-ap", *FALLBACK_AP_PASSWORD = "test-password";
const char *NTP_SERVER = "pool.ntp.org";
const char *WIFI_STATIC_IP = "", *WIFI_SUBNET_MASK = "255.255.255.0";
const char *WIFI_GATEWAY = "", *WIFI_DNS1 = "", *WIFI_DNS2 = "";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 750;
char lastSetupFailureText[160] = {};
int lastSetupFailureCode = 0;
struct OtaShim {
  bool active = false;
  bool busy() const { return active; }
  void loop(uint32_t) {}
} firmwareOta;
bool firmwareBusy() { return firmwareOta.busy(); }
esp_err_t esp_wifi_disconnect() {
  assert(!inWiFiCallback && !firmwareOta.busy()); ++disconnectCalls;
  WiFi.statusValue = WL_DISCONNECTED; WiFi.ipValue = 0; return ESP_OK;
}
esp_err_t esp_wifi_connect() {
  assert(!inWiFiCallback && !firmwareOta.busy()); ++connectCalls;
  if (WiFi.succeedConnect) { WiFi.statusValue = WL_CONNECTED; WiFi.ipValue = 0xC000020B; }
  return WiFi.succeedConnect ? ESP_OK : -1;
}
void configTime(long, int, const char *) { assert(!inWiFiCallback); ++ntpCalls; }
void requestScreenRedraw() { assert(!inWiFiCallback); ++redrawCalls; }
std::vector<String> logs;
void sessionLogf(const char *format, ...) {
  assert(!inWiFiCallback && criticalDepth == 0);
  char line[512]; va_list args; va_start(args, format);
  vsnprintf(line, sizeof(line), format, args); va_end(args); logs.emplace_back(line);
}
void sessionLogLine(const char *text) { sessionLogf("%s", text); }
void bootLogLine(int, const char *) { assert(!inWiFiCallback); }
void bootLogf(int, const char *, ...) { assert(!inWiFiCallback); }
struct MqttShim {
  bool lastOnline = false;
  unsigned loopCalls = 0;
  String address;
  void loop(uint32_t, bool online) { lastOnline = online; ++loopCalls; }
  void setNetworkAddress(const char *ip) { assert(!inWiFiCallback); address = ip; }
} mqttBridge;
struct ServerShim { void handleClient() {} } server;
void serviceSerialLog() {}
void serviceSessionClock() {}
void serviceSessionEvents() {}
unsigned relayOffCalls = 0, loRaCalls = 0;
void setRelay(bool on) { if (!on) ++relayOffCalls; }
void handleLoRa() { ++loRaCalls; }
void handleLocalSpd() {}
void handleTouch() {}
void handleAlarmRelay() {}
struct PagesShim { bool advance(uint32_t) { return false; } } displayPages;
constexpr uint8_t ALARM_NONE = 0, ALARM_SPD_FAIL = 2;
uint8_t currentAlarmKind() { return ALARM_NONE; }
bool screenFlashRed = false, screenDirty = false;
uint32_t lastFlashToggleMs = 0, lastScreenDrawMs = 0;
constexpr uint32_t DISPLAY_PERIODIC_REFRESH_MS = 60000;
void drawStatusTable() { screenDirty = false; lastScreenDrawMs = nowMs; }
bool containsLog(const char *text) {
  for (const auto &line : logs) if (line.find(text) != String::npos) return true;
  return false;
}
void emit(WiFiEvent_t event, uint8_t reason = 0, uint32_t ip = 0) {
  assert(WiFi.callback); WiFiEventInfo_t info;
  info.wifi_sta_disconnected.reason = reason; info.got_ip.ip_info.ip.addr = ip;
  const auto count = logs.size(); const auto calls = connectCalls;
  inWiFiCallback = true; WiFi.callback(event, info); inWiFiCallback = false;
  assert(logs.size() == count && connectCalls == calls && criticalDepth == 0);
}
'''

CHECKS = r'''
void bootOnline() {
  WiFi.connectDuringBoot = true;
  assert(setupWiFi()); assert(nowMs == 500);
  assert(stationOnline() && WiFi.beginCalls == 1 && WiFi.apCalls == 0);
  assert(!WiFi.autoReconnect && WiFi.autoReconnectCalls == 1);
  WiFi.connectDuringBoot = false;
  loop(); assert(mqttBridge.lastOnline && ntpCalls == 1);
  assert(mqttBridge.address == "192.0.2.10" && ipString() == "192.0.2.10");
}
void assertRuntimeDoesNotWait(uint32_t expectedDelays) {
  assert(delayCalls == expectedDelays && loRaCalls > 0);
}
unsigned retries() { return WiFi.beginCalls - 1; }
void checkFullRestart(unsigned previousModeCalls) {
  assert(WiFi.modes.size() >= previousModeCalls + 2);
  assert(WiFi.modes[previousModeCalls] == WIFI_OFF);
  assert(WiFi.modes[previousModeCalls + 1] & WIFI_STA);
  assert(connectCalls == 0 && disconnectCalls == 0); // Full driver restart is required.
}
void checkRuntimeLoss(bool lostIp) {
  bootOnline();
  const auto bootDelays = delayCalls;
  nowMs = 100000;
  WiFi.statusValue = lostIp ? WL_CONNECTED : WL_DISCONNECTED;
  WiFi.ipValue = 0;
  emit(lostIp ? ARDUINO_EVENT_WIFI_STA_LOST_IP : ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
       WIFI_REASON_AUTH_FAIL);
  loop(); assert(!stationOnline() && !mqttBridge.lastOnline);
  assert(ipString() == "connecting");
  assert(containsLog(lostIp ? "lost IPv4" : "reason=202 (authentication failed)"));
  // Continuous monitoring must survive a reason the core does not retry and
  // several immediate SDK failures, with no reconnect storm between attempts.
  for (unsigned attempt = 1; attempt <= 4; ++attempt) {
    const auto previous = retries();
    for (uint32_t delta : {uint32_t(1), uint32_t(1000), WIFI_RETRY_INTERVAL_MS - 1}) {
      nowMs = 100000 + (attempt - 1) * WIFI_RETRY_INTERVAL_MS + delta; loop();
      assert(retries() == previous);
    }
    if (attempt == 4) WiFi.succeedConnect = true;
    const auto modeCalls = WiFi.modes.size();
    nowMs = 100000 + attempt * WIFI_RETRY_INTERVAL_MS; loop();
    assert(retries() == attempt);
    checkFullRestart(modeCalls);
    assert(WiFi.getMode() == WIFI_AP_STA && fallbackApOnline());
  }
  assert(stationOnline() && mqttBridge.lastOnline);
  assert(ipString() == "192.0.2.11" && mqttBridge.address == "192.0.2.11");
  emit(ARDUINO_EVENT_WIFI_STA_GOT_IP, 0, WiFi.ipValue); loop();
  assert(containsLog("received IPv4 192.0.2.11"));
  const auto attempts = retries();
  nowMs += 1000000; loop(); assert(retries() == attempts);
  assertRuntimeDoesNotWait(bootDelays);
  assert(ntpCalls == 1); // One NTP setup keeps its background resynchronization.
}
void checkApOnly(const char *ssid) {
  WIFI_SSID = ssid;
  assert(setupWiFi());
  assert(WiFi.beginCalls == 0 && WiFi.apCalls == 1 && WiFi.getMode() == WIFI_AP);
  assert(ipString() == "192.168.4.1");
  for (uint32_t now : {uint32_t(0), uint32_t(120000), UINT32_MAX}) {
    nowMs = now; loop();
    assert(!stationOnline() && !mqttBridge.lastOnline && connectCalls == 0 && ntpCalls == 0);
  }
  assert(delayCalls == 0);
}
void checkBootFallback(bool ntpEnabled) {
  if (!ntpEnabled) NTP_SERVER = "";
  assert(setupWiFi());
  assert(nowMs == WIFI_CONNECT_TIMEOUT_MS && WiFi.beginCalls == 1);
  assert(WiFi.getMode() == WIFI_AP_STA && fallbackApOnline() && !stationOnline());
  const auto bootDelays = delayCalls;
  loop(); assert(!mqttBridge.lastOnline && ntpCalls == 0);
  nowMs = WIFI_CONNECT_TIMEOUT_MS + WIFI_RETRY_INTERVAL_MS - 1; loop(); assert(retries() == 0);
  WiFi.succeedConnect = true;
  ++nowMs; const auto modeCalls = WiFi.modes.size(); loop();
  assert(retries() == 1 && stationOnline() && mqttBridge.lastOnline);
  checkFullRestart(modeCalls);
  assert(WiFi.getMode() == WIFI_AP_STA && fallbackApOnline());
  assert(ntpCalls == unsigned(ntpEnabled));
  nowMs += WIFI_RETRY_INTERVAL_MS; loop(); assert(retries() == 1);
  assertRuntimeDoesNotWait(bootDelays);
}
void checkOta() {
  bootOnline(); const auto bootDelays = delayCalls;
  nowMs = 100000; WiFi.statusValue = WL_DISCONNECTED; WiFi.ipValue = 0;
  loop();
  firmwareOta.active = true;
  const auto priorLoRa = loRaCalls;
  for (unsigned i = 1; i <= 20; ++i) {
    nowMs = 100000 + i * 10000; loop();
    assert(retries() == 0 && disconnectCalls == 0 && WiFi.apCalls == 0);
  }
  assert(loRaCalls == priorLoRa && relayOffCalls >= 20);
  WiFi.succeedConnect = true; firmwareOta.active = false;
  loop(); assert(retries() == 1 && stationOnline() && mqttBridge.lastOnline);
  assert(loRaCalls == priorLoRa + 1);
  assertRuntimeDoesNotWait(bootDelays);
}
void checkWrap() {
  bootOnline(); const auto bootDelays = delayCalls;
  const uint32_t lostAt = UINT32_MAX - WIFI_RETRY_INTERVAL_MS / 2;
  nowMs = lostAt; WiFi.statusValue = WL_DISCONNECTED; WiFi.ipValue = 0;
  loop();
  nowMs = lostAt + WIFI_RETRY_INTERVAL_MS - 1; loop(); assert(retries() == 0 && WiFi.apCalls == 0);
  nowMs = lostAt + WIFI_RETRY_INTERVAL_MS; loop(); assert(retries() == 1 && fallbackApOnline());
  nowMs += WIFI_RETRY_INTERVAL_MS - 1; loop(); assert(retries() == 1);
  WiFi.succeedConnect = true;
  ++nowMs; loop(); assert(retries() == 2 && stationOnline());
  assertRuntimeDoesNotWait(bootDelays);
}
void checkQueue() {
  bootOnline(); logs.clear();
  // An unknown event must not enter the diagnostics stream.
  emit(ARDUINO_EVENT_WIFI_AP_START); drainWiFiEvents(); assert(logs.empty());
  // Keep the newest bounded set and report exactly how much was discarded.
  for (unsigned i = 0; i < 100; ++i) emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, uint8_t(i));
  assert(logs.empty()); drainWiFiEvents();
  assert(logs.size() == WspdWiFiEventQueue::CAPACITY + 1);
  assert(containsLog("84 older events omitted"));
  for (unsigned i = 84; i < 100; ++i) {
    char expected[30]; snprintf(expected, sizeof(expected), "reason=%u (", i);
    assert(logs[i - 84 + 1].find(expected) != String::npos);
  }
  const auto count = logs.size(); drainWiFiEvents(); assert(logs.size() == count);
  for (unsigned burst = 0; burst < 100; ++burst) {
    emit(ARDUINO_EVENT_WIFI_STA_LOST_IP);
    emit(ARDUINO_EVENT_WIFI_STA_GOT_IP, 0, 0xC000020C);
    drainWiFiEvents();
  }
  assert(containsLog("received IPv4 192.0.2.12") && criticalDepth == 0);
}
void checkApFailure() {
  bootOnline(); const auto bootDelays = delayCalls;
  nowMs = 100000; WiFi.statusValue = WL_DISCONNECTED; WiFi.ipValue = 0;
  WiFi.apSuccess = false; loop();
  nowMs += WIFI_RETRY_INTERVAL_MS; loop(); assert(WiFi.apCalls >= 1 && retries() == 1);
  assert(containsLog("Fallback AP start failed"));
  for (unsigned i = 0; i < 100; ++i) { nowMs += 100; loop(); }
  assert(retries() == 1);
  WiFi.apSuccess = true; nowMs = 100000 + 2 * WIFI_RETRY_INTERVAL_MS; loop();
  assert(retries() == 2 && fallbackApOnline());
  assertRuntimeDoesNotWait(bootDelays);
}
void checkNetworkAddress() {
  bootOnline();
  WiFi.modeValue = WIFI_AP_STA; WiFi.apActive = true;
  assert(!fallbackApOnline()); // Default AP IP does not mean private AP setup succeeded.
  loop(); assert(mqttBridge.lastOnline);
  WiFi.ipValue = 0xC0000214; loop();
  assert(mqttBridge.address == "192.0.2.20" && ipString() == "192.0.2.20");
  WiFi.ipValue = 0; loop(); assert(!stationOnline() && !mqttBridge.lastOnline);
  assert(ipString() == "connecting");
  WiFi.statusValue = WL_CONNECTED; WiFi.ipValue = 0xC0000214; WiFi.modeValue = WIFI_AP;
  loop(); assert(!stationOnline() && !mqttBridge.lastOnline);
}
void checkStaticConfig() {
  WIFI_STATIC_IP = "192.0.2.25"; WIFI_SUBNET_MASK = "255.255.255.0";
  WIFI_GATEWAY = "192.0.2.1"; WIFI_DNS1 = "192.0.2.53"; WIFI_DNS2 = "1.1.1.1";
  assert(setupWiFi()); assert(WiFi.configs.size() == 1);
  const std::array<uint32_t, 5> expected = {0xC0000219, 0xC0000201, 0xFFFFFF00, 0xC0000235, 0x01010101};
  assert(WiFi.configs.back() == expected);
  // Reinitializing the driver must restore static address, mask, route and DNS.
  nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(WiFi.configs.size() == 2 && WiFi.configs.back() == expected);
  WiFi.succeedConnect = true; nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(stationOnline() && ipString() == "192.0.2.25" && mqttBridge.lastOnline);
  assert(WiFi.configs.size() == 3 && WiFi.configs.back() == expected);
}
void checkDhcpReapply() {
  WIFI_STATIC_IP = ""; WIFI_SUBNET_MASK = "not-a-mask";
  WIFI_GATEWAY = nullptr; WIFI_DNS1 = "bad"; WIFI_DNS2 = "bad";
  assert(setupWiFi()); const std::array<uint32_t, 5> zeros = {0, 0, 0, 0, 0};
  assert(WiFi.configs.back() == zeros);
  nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(WiFi.configs.size() == 2 && WiFi.configs.back() == zeros);
  WiFi.succeedConnect = true; nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(WiFi.configs.size() == 3 && WiFi.configs.back() == zeros && stationOnline());
}
void checkWideMask() {
  WIFI_STATIC_IP = "10.1.2.3"; WIFI_SUBNET_MASK = "254.0.0.0";
  WIFI_GATEWAY = "10.0.0.1";
  assert(setupWiFi());
  assert(WiFi.configs.back()[0] == 0x0A010203 && WiFi.configs.back()[1] == 0x0A000001);
  assert(WiFi.configs.back()[2] == 0xFE000000);
}
void checkNetworkValidation() {
  WspdWiFiNetworkConfig out;
  auto parse = [&](const char *ip, const char *mask, const char *gateway = "", const char *dns1 = "", const char *dns2 = "") {
    return wspdParseNetworkConfig(ip, mask, gateway, dns1, dns2, out);
  };
  assert(!parse("", nullptr, nullptr, nullptr, nullptr) && out.dhcp);
  assert(parse(nullptr, "255.255.255.0"));
  for (const char *ip : {"192.0.2", "192.0.2.1.1", "256.1.2.3", "1.2.3.999", "1.2.3.-1", " 192.0.2.1", "192.0.2.1 ",
                        "+1.2.3.4", "1.2.3.4x", "1..3.4", "1.2.3.", "1.2.3.0000", "1.2.3.1/24", "hostname", "0.0.0.0",
                        "0.1.2.3", "127.0.0.1", "224.0.0.1", "239.255.255.255", "255.255.255.255", "192.0.2.0", "192.0.2.255"})
    assert(parse(ip, "255.255.255.0"));
  for (const char *mask : {"", "bad", "255.255.255", "0.0.0.0", "255.0.255.0", "255.255.0.128", "255.255.255.254", "255.255.255.255"})
    assert(parse("192.0.2.10", mask));
  for (const char *gateway : {"192.0.3.1", "192.0.2.10", "192.0.2.0", "192.0.2.255", "224.1.2.3", "bad"})
    assert(parse("192.0.2.10", "255.255.255.0", gateway));
  for (const char *dns : {"bad", "0.1.2.3", "127.0.0.1", "224.0.0.1", "255.255.255.255"}) {
    assert(parse("192.0.2.10", "255.255.255.0", "", dns));
    assert(parse("192.0.2.10", "255.255.255.0", "", "", dns));
  }
  // Verify every accepted prefix with boundary hosts and network/broadcast rejection.
  for (unsigned prefix = 1; prefix <= 30; ++prefix) {
    const uint32_t maskValue = UINT32_MAX << (32 - prefix);
    const uint32_t network = 0x8A010200 & maskValue;
    const auto mask = IPAddress(maskValue).toString();
    const auto address = IPAddress(network + 1).toString();
    assert(!parse(address.c_str(), mask.c_str()));
    assert(out.address == network + 1 && out.mask == maskValue && !out.dhcp);
    const auto net = IPAddress(network).toString(), broadcast = IPAddress(network | ~maskValue).toString();
    assert(parse(net.c_str(), mask.c_str()) && parse(broadcast.c_str(), mask.c_str()));
  }
  assert(!parse("192.0.2.10", "255.255.255.0", "0.0.0.0", "0.0.0.0", "0.0.0.0"));
  assert(!out.dhcp && out.gateway == 0 && out.dns1 == 0 && out.dns2 == 0);
  assert(!parse("192.0.2.10", "255.255.255.0", "192.0.2.1", "1.1.1.1", "8.8.8.8"));
}
void checkRestartErrors() {
  bootOnline(); const auto bootDelays = delayCalls;
  nowMs = 100000; WiFi.statusValue = WL_DISCONNECTED; WiFi.ipValue = 0; loop();
  WiFi.configSuccess = false; nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(WiFi.beginCalls == 1 && fallbackApOnline());
  assert(containsLog("Applying station IPv4 configuration failed"));
  WiFi.configSuccess = true; WiFi.modeSuccess = false;
  nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(WiFi.beginCalls == 1 && containsLog("Driver restart failed"));
  WiFi.modeSuccess = true; WiFi.succeedConnect = true;
  nowMs += WIFI_RETRY_INTERVAL_MS; loop();
  assert(WiFi.beginCalls == 2 && stationOnline() && fallbackApOnline() && mqttBridge.lastOnline);
  assertRuntimeDoesNotWait(bootDelays);
}
int main(int argc, char **argv) {
  assert(argc == 2); const String test = argv[1];
  if (test == "runtime-association") checkRuntimeLoss(false);
  else if (test == "runtime-dhcp") checkRuntimeLoss(true);
  else if (test == "empty-ssid") checkApOnly("");
  else if (test == "null-ssid") checkApOnly(nullptr);
  else if (test == "placeholder-ssid") checkApOnly("YOUR_WIFI_SSID");
  else if (test == "boot-fallback") checkBootFallback(true);
  else if (test == "ntp-disabled") checkBootFallback(false);
  else if (test == "ota-deferral") checkOta();
  else if (test == "millis-wrap") checkWrap();
  else if (test == "bounded-events") checkQueue();
  else if (test == "ap-failure") checkApFailure();
  else if (test == "network-address") checkNetworkAddress();
  else if (test == "static-config") checkStaticConfig();
  else if (test == "dhcp-reapply") checkDhcpReapply();
  else if (test == "wide-mask") checkWideMask();
  else if (test == "network-validation") checkNetworkValidation();
  else if (test == "restart-errors") checkRestartErrors();
  else assert(false);
  std::cout << "PASS: " << test << "\n";
}
'''


def main():
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    source = (SERVER / "tconnectpro_868.ino").read_text()
    state = source.split("// BEGIN WIFI RUNTIME STATE", 1)[1].split("// END WIFI RUNTIME STATE", 1)[0]
    body = SHIMS + state + "\n"
    names = ("configuredStationWiFi", "stationOnline", "fallbackApOnline", "ipString",
             "queueWiFiEvent", "wifiDisconnectReasonText", "drainWiFiEvents", "startFallbackAp",
             "wifiIPv4Address", "applyWiFiNetworkConfig", "restartStationWiFi", "serviceWiFi", "setupWiFi", "loop")
    for name in names:
        body += function(source, name) + "\n"
    body += CHECKS
    cases = ("runtime-association", "runtime-dhcp", "empty-ssid", "null-ssid", "placeholder-ssid",
             "boot-fallback", "ntp-disabled", "ota-deferral", "millis-wrap", "bounded-events",
             "ap-failure", "network-address", "static-config", "dhcp-reapply", "wide-mask", "network-validation", "restart-errors")
    with tempfile.TemporaryDirectory(prefix="wspd-wifi-") as temporary:
        work = Path(temporary)
        driver, binary = work / "wifi.cpp", work / "wifi"
        driver.write_text(body)
        for core in (2, 3):
            for interval in (60000, 120000):
                subprocess.run([compiler, "-std=c++11", "-O1", "-g", "-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer", f"-DESP_ARDUINO_VERSION_MAJOR={core}", f"-DRETRY_MS={interval}",
                                "-I", str(SERVER), str(driver), "-o", str(binary)], check=True)
                for case in cases:
                    subprocess.run([str(binary), case], check=True)
        retry_assertions = re.findall(r"static_assert\([^;]*WIFI_RETRY_INTERVAL_MS[^;]*;", source)
        assert retry_assertions, "Production retry interval must be validated before conversion to a timer value."
        interval_driver = work / "interval.cpp"
        interval_driver.write_text('#include "config_types.h"\nconstexpr auto WIFI_RETRY_INTERVAL_MS = RETRY_VALUE;\n' +
                                   "\n".join(retry_assertions) + "\nint main() {}\n")
        for value, valid in (("60000", True), ("90000UL", True), ("120000ULL", True),
                             ("0", False), ("-60000", False), ("59999", False), ("120001", False),
                             ("60000.0", False), ("60000.5", False), ("true", False), ("4295027296ULL", False)):
            result = subprocess.run([compiler, "-std=c++11", "-fsyntax-only", f"-DRETRY_VALUE={value}",
                                     "-I", str(SERVER), str(interval_driver)], capture_output=True, text=True)
            assert (result.returncode == 0) == valid, f"Retry interval validation failed for {value}: {result.stderr}"
    print("PASS: production recovery, callback handoff, full restart, IP configuration and receiver loop across 68 scenarios.")
    print("PASS: 11 literal retry interval boundaries, including fractions, overflow, booleans and invalid ranges.")
    print("Host checks do not qualify an access point outage, RF coexistence or DHCP recovery on hardware.")


if __name__ == "__main__":
    main()
