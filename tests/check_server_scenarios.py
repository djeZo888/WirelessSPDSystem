#!/usr/bin/env python3
"""Interleave real receiver actions/packets/HTTP while USB diagnostics cannot drain.

Production extraction deliberately includes sessionLogLine, setAlarmMuted and
loop: replacing those with harmless stubs would hide the USB-stall regression.
Hardware transports, SHA library entrypoint and LCD painting are test seams;
their separate focused suites qualify encoding, MQTT, layouts and OTA lifecycle.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"
spec = importlib.util.spec_from_file_location("server_api_harness", ROOT / "tests/check_server_api.py")
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def function(source, name):
    """Brace-safe extraction, including the logger's uint64_t return type."""
    match = re.search(r"(?:static\s+)?(?:const SpdConfig \*|const char \*|String|bool|size_t|uint64_t|uint32_t|uint8_t|void)\s*" +
                      name + r"\([^;{]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Cannot find production function: {name}")
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', source[match.end():], re.S):
        if token.group() in ("{", "}"):
            depth += 1 if token.group() == "{" else -1
            if not depth:
                return source[match.start():match.end() + token.end()]
    raise RuntimeError(f"Unclosed production function: {name}")


def replace(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError(f"Harness seam changed; review its production coverage: {old[:90]}")
    return source.replace(old, new)


SHIMS = r'''
#include <array>
#include <deque>
#include <functional>
#include <limits>
#include <cstdarg>
#include <cmath>
constexpr size_t SPD_AUTH_PAYLOAD_LEN = 12;
constexpr uint8_t BATTERY_VOLTAGE_ERROR_CODE = 255;
constexpr float BATTERY_VOLTAGE_OFFSET_V = 2.20f, BATTERY_VOLTAGE_STEP_V = 0.01f;
constexpr uint8_t RELAY_1 = 8, RELAY_ACTIVE_LEVEL = 0, RELAY_INACTIVE_LEVEL = 1;
constexpr uint32_t ALARM_PERIOD_MS = 60000, ALARM_FAIL_ON_MS = 5000, ALARM_LOW_BATTERY_ON_MS = 500;
constexpr uint32_t SPD_LOCAL_POLL_MS = 50, SPD_LOCAL_DEBOUNCE_MS = 250;
constexpr int HIGH = 1;
constexpr int16_t MUTE_BTN_X = 388, MUTE_BTN_Y = 194, MUTE_BTN_W = 92, MUTE_BTN_H = 24;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 350, TOUCH_RELEASE_STABLE_MS = 160;
constexpr uint32_t DISPLAY_PERIODIC_REFRESH_MS = 60000;
bool screenFlashRed = false, touchWasDown = false;
uint32_t lastTouchActionMs = 0, lastTouchDownSampleMs = 0;
uint32_t lastScreenDrawMs = 0, lastFlashToggleMs = 0;
bool localContactOk = true, relayOn = false;
uint32_t longestIteration = 0, iterations = 0, paints = 0, redPaints = 0, whitePaints = 0;
uint32_t wifiServices = 0;
uint64_t esp_timer_get_time() { return uint64_t(nowMs) * 1000; }
int digitalRead(int) { return localContactOk ? HIGH : 0; }
void digitalWrite(int pin, int value) { assert(pin == RELAY_1); relayOn = value == RELAY_ACTIVE_LEVEL; }
void serviceWiFi(uint32_t) { ++wifiServices; }
void serviceSessionClock() {}
bool stationOnline() { return true; }
void drawStatusTable() {
  ++paints; if (screenFlashRed) ++redPaints; else ++whitePaints;
  screenDirty = false; lastScreenDrawMs = millis();
}
// USB stays physically attached in several scenarios but its host does not read.
// Match HWCDC's default 2-second full-ring retry per write. A println performs
// two writes. Setting a zero TX timeout makes races/partial writes best effort.
struct SerialShim {
  bool attached = true, draining = false, raceFull = false, zeroWrite = false, partialWrite = false;
  size_t room = 0, written = 0, writes = 0, maxWrite = 0;
  std::string output;
  uint32_t timeoutMs = 2000, blockedMs = 0;
  operator bool() const { return attached; }
  int availableForWrite() const { return attached ? int(draining ? 4096 : room) : 0; }
  void setTxTimeoutMs(uint32_t value) { timeoutMs = value; }
  size_t write(const uint8_t *data, size_t length) {
    ++writes; maxWrite = std::max(maxWrite, length);
    if (!attached) return 0;
    const size_t capacity = raceFull || zeroWrite ? 0 : (draining ? length : room);
    const size_t accepted = partialWrite ? std::min(capacity, (length + 1) / 2) : std::min(capacity, length);
    written += accepted;
    output.append(reinterpret_cast<const char *>(data), accepted);
    if (!draining) room -= std::min(room, accepted);
    if (accepted != length) { nowMs += timeoutMs; blockedMs += timeoutMs; }
    return accepted;
  }
  size_t write(const char *p, size_t n) { return write(reinterpret_cast<const uint8_t *>(p), n); }
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t print(const char *text) { return write(text, strlen(text)); }
  size_t println(const char *text) { const size_t n = print(text); return n + write("\r\n", 2); }
  void printf(const char *format, ...) {
    char text[1024]; va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args); print(text);
  }
} Serial;
static void setRelay(bool);
static void sessionLogf(const char *, ...);
void mqttAlarmMute(bool);
struct TouchSample { bool available = false, down = false; int16_t x = 400, y = 200; } touch;
bool pollTouchLandscape(bool &down, int16_t &x, int16_t &y) {
  down = touch.down; x = touch.x; y = touch.y; return touch.available;
}
struct RadioShim {
  std::vector<uint8_t> packet;
  int16_t readCode = 0, restartCode = 0;
  unsigned reads = 0, restarts = 0;
  size_t getPacketLength() { return packet.size(); }
  int16_t readData(uint8_t *out, size_t count) {
    assert(count <= packet.size()); ++reads;
    if (readCode == 0) memcpy(out, packet.data(), count); return readCode;
  }
  float getRSSI() { return -86.5f; }
  float getSNR() { return -4.25f; }
  int16_t startReceive() { ++restarts; return restartCode; }
} radio;
constexpr int16_t RADIOLIB_ERR_NONE = 0;
bool loraInitialized = true;
volatile bool loraPacketFlag = false;
struct ShaFixture { std::array<uint8_t, 16> input; std::array<uint8_t, 32> digest; };
std::vector<ShaFixture> shaFixtures;
// The platform SHA entrypoint is replaced with independent Python hashlib
// vectors. The real verifier/decoder still runs; cryptographic implementation
// and every encoded byte are separately covered by check_packet_compatibility.
void mbedtls_sha256(const uint8_t *input, size_t size, uint8_t *out, int mode) {
  assert(size == 16 && mode == 0);
  for (const auto &fixture : shaFixtures) if (memcmp(fixture.input.data(), input, size) == 0) {
    memcpy(out, fixture.digest.data(), 32); return;
  }
  assert(false && "Missing independent SHA fixture");
}
'''

CHECKS = r'''
unsigned cases = 0;
void checkQueue() {
  WspdSerialLogMirror mirror;
  SerialShim port; port.draining = true; mirror.begin(port);
  assert(port.timeoutMs == 0 && mirror.pending() == 0);
  mirror.enqueue(nullptr); assert(mirror.pending() == 0);
  mirror.enqueue(""); mirror.enqueue("Alpha"); mirror.enqueue("Beta");
  const size_t pending = mirror.pending();
  port.attached = false; mirror.pump(port); assert(mirror.pending() == pending && port.writes == 0);
  port.attached = true; port.draining = false; port.room = 8;
  mirror.pump(port); assert(mirror.pending() == pending && port.writes == 0);
  port.room = 9; mirror.pump(port); assert(mirror.pending() == pending - 1 && port.writes == 1);
  port.draining = true; port.partialWrite = true;
  while (mirror.pending()) mirror.pump(port);
  assert(port.output == "\r\nAlpha\r\nBeta\r\n" && port.blockedMs == 0);
  // Capacity/connection may change after it was sampled. Never wait or discard
  // unsent bytes; the next pump resumes from exactly the accepted position.
  mirror.enqueue("Retry me"); const auto retained = mirror.pending();
  port.zeroWrite = true; mirror.pump(port); assert(mirror.pending() == retained);
  port.zeroWrite = false; port.raceFull = true; mirror.pump(port); assert(mirror.pending() == retained);
  port.raceFull = false; port.partialWrite = false;
  while (mirror.pending()) mirror.pump(port);
  assert(port.output == "\r\nAlpha\r\nBeta\r\nRetry me\r\n");
  const auto before = port.output;
  const std::string full(WspdSerialLogMirror::CAPACITY - 2, 'F');
  mirror.enqueue(full.c_str()); assert(mirror.pending() == WspdSerialLogMirror::CAPACITY);
  mirror.enqueue("Dropped whole line");
  const std::string oversized(WspdSerialLogMirror::CAPACITY, 'X'); mirror.enqueue(oversized.c_str());
  assert(mirror.dropped() == 2 && mirror.pending() == WspdSerialLogMirror::CAPACITY);
  while (mirror.pending()) mirror.pump(port);
  assert(port.output == before + full + "\r\n");
  std::string expected = port.output;
  // More than a ring's worth of varying messages exercises both head/tail
  // wrapping, partial writes and overflow without allocating inside the mirror.
  for (unsigned i = 0; i < 350; ++i) {
    const std::string line = std::to_string(i) + std::string(30 + i % 61, 'a' + i % 20);
    const bool fits = line.size() + 2 <= WspdSerialLogMirror::CAPACITY - mirror.pending();
    mirror.enqueue(line.c_str()); if (fits) expected += line + "\r\n";
    if (i % 3 == 0) { port.partialWrite = i % 2; mirror.pump(port); }
  }
  port.partialWrite = false; while (mirror.pending()) mirror.pump(port);
  assert(port.output == expected && port.maxWrite <= 64 && port.blockedMs == 0);
}
void step(uint32_t elapsed = 20) {
  nowMs += elapsed;
  const uint32_t start = millis(); const size_t writesBefore = Serial.writes; loop();
  const uint32_t duration = millis() - start;
  longestIteration = std::max(longestIteration, duration); ++iterations;
  if (duration > 100) {
    fprintf(stderr, "Loop stalled %u ms; USB blocked %u ms at case %u\n", duration, Serial.blockedMs, cases);
    assert(false && "Receiver loop blocked; web/RX/blink cannot progress");
  }
  assert(Serial.writes - writesBefore <= 1 && Serial.maxWrite <= 64);
}
void runFor(uint32_t duration) {
  const unsigned notificationsBefore = mqttBridge.notifications;
  const uint32_t start = millis(); while (uint32_t(millis() - start) < duration) step();
  assert(mqttBridge.notifications == notificationsBefore);
}
void request(const char *path, int method = HTTP_GET, const char *value = nullptr) {
  const unsigned before = server.sends;
  server.pending.push_back([=]() { server.dispatch(path, method, value); }); step();
  assert(server.sends == before + 1); ++cases;
}
void packet(const std::vector<uint8_t> &bytes, int16_t readCode = 0, int16_t restartCode = 0) {
  const unsigned before = radio.restarts;
  radio.packet = bytes; radio.readCode = readCode; radio.restartCode = restartCode; loraPacketFlag = true;
  step(); assert(!loraPacketFlag && radio.restarts == before + 1); ++cases;
  radio.readCode = radio.restartCode = 0;
}
bool logContains(const char *needle) {
  for (uint64_t seq = sessionLog.oldest(); seq < sessionLog.next(); ++seq)
    if (strstr(sessionLog.get(seq)->message, needle)) return true;
  return false;
}
void snapshot(const char *label) {
  request("/api/v1/get"); assert(server.status == 200);
  std::cout << "JSON " << label << " " << server.body << '\n';
}
void muteWeb(const char *value, bool expected) {
  const unsigned notificationsBefore = mqttBridge.notifications;
  request("/api/v1/mute", HTTP_POST, value);
  assert(server.status == 200 && alarmMuted == expected);
  assert(mqttBridge.notifications == notificationsBefore + 1);
  assert(server.body == String("{\"status\":\"ok\",\"alarm_muted\":") + (expected ? "true}" : "false}"));
  if (expected) assert(!relayOn);
}
void touchSample(bool down, uint32_t elapsed = 20, int16_t x = 400, int16_t y = 200, bool action = false) {
  const unsigned notificationsBefore = mqttBridge.notifications;
  touch.available = true; touch.down = down; touch.x = x; touch.y = y; step(elapsed); ++cases;
  assert(mqttBridge.notifications == notificationsBefore + (action ? 1 : 0));
}
void muteMqtt(bool muted) {
  const unsigned notificationsBefore = mqttBridge.notifications;
  mqttBridge.pendingMute = muted ? 1 : 0; step();
  assert(alarmMuted == muted && mqttBridge.notifications == notificationsBefore + 1);
  if (muted) assert(!relayOn);
}
int main(int argc, char **argv) {
  assert(argc == 2); const int profile = atoi(argv[1]);
  Serial.attached = profile != 0;
  Serial.draining = profile == 3;
  Serial.room = profile == 2 ? 8 : (profile >= 4 ? 4096 : 0);
  Serial.raceFull = profile == 4;
  Serial.zeroWrite = profile == 5;
  Serial.partialWrite = profile == 6;
  checkQueue();
  serialLogMirror.begin(Serial);
  sessionLog.begin(logRecords, 8); // Small ring makes overflow part of every run.
  for (size_t i = 0; i < SPD_COUNT; ++i)
    SPD_CONFIGS[i] = {static_cast<uint8_t>(i + 1), "Remote rack", 0x7C9E4A21UL};
  initializeFixtures();
  assert(setupWebServer()); displayPages.restart(millis());
  packet(fixtures.at("healthy")); packet(fixtures.at("failed"));
  assert(currentAlarmKind() == ALARM_SPD_FAIL && !alarmMuted);
  snapshot("mixed");
  const auto beforeRed = redPaints, beforeWhite = whitePaints;
  runFor(1100); assert(redPaints > beforeRed && whitePaints > beforeWhite);
  // The fault must continue blinking and remain in every API while muted.
  muteWeb("1", true); muteWeb("1", true); snapshot("muted");
  const auto mutedRed = redPaints, mutedWhite = whitePaints;
  runFor(1100); assert(redPaints > mutedRed && whitePaints > mutedWhite && !relayOn);
  muteWeb("0", false); muteWeb("0", false); muteWeb(nullptr, true); muteWeb(nullptr, false);
  // A long press, a missing capacitive sample and a brief empty sample never
  // toggle repeatedly; only a released, debounced fresh press toggles again.
  touchSample(true, 20, 400, 200, true); assert(alarmMuted);
  runFor(500); assert(alarmMuted);
  touch.available = false; runFor(400); assert(alarmMuted);
  touchSample(true); assert(alarmMuted);
  touchSample(false); touchSample(true, 20); assert(alarmMuted);
  touchSample(false, 200); touchSample(true, 200, 400, 200, true); assert(!alarmMuted);
  touchSample(false, 200); touchSample(true, 20); assert(!alarmMuted); // Debounced bounce.
  touchSample(false, 200); touchSample(true, 400, 10, 10); assert(!alarmMuted); // Outside button.
  touchSample(false, 200); touch.available = false;
  // MQTT transport validates payloads in its focused suite. Here its main-loop
  // callback shares real alarm/relay/log paths with touch and HTTP.
  muteMqtt(true); muteMqtt(true); muteMqtt(false);
  request("/healthz"); assert(server.status == 200);
  request("/"); assert(server.status == 200 && server.contentType == "text/html; charset=utf-8");
  request("/api/v1/mute", HTTP_GET); assert(server.status == 404 && !alarmMuted);
  request("/api/v1/unknown"); assert(server.status == 404 && !alarmMuted);
  server.pending.push_back([](){ server.query={{"limit","0"}}; handleLogs(); }); step(); assert(server.status==400);
  // Even rejected traffic is logged, cannot change accepted state and must
  // return the radio to receive mode without blocking the next HTTP request.
  const auto count = spdStates[1].packetCount, seenAt = spdStates[1].lastSeenMs;
  const auto invalid = totalInvalidPackets, old = totalOldNoncePackets;
  packet(std::vector<uint8_t>(19, 0)); packet(std::vector<uint8_t>(65, 0));
  auto corrupt = fixtures.at("failed"); corrupt.back() ^= 1; packet(corrupt);
  packet(fixtures.at("unknown_id")); packet(fixtures.at("failed"));
  assert(totalInvalidPackets == invalid + 4 && totalOldNoncePackets == old + 1);
  assert(totalAuthRejects == 1 && totalUnconfiguredRejects == 1);
  assert(spdStates[1].packetCount == count && spdStates[1].lastSeenMs == seenAt);
  packet(fixtures.at("failed"), -7); packet(fixtures.at("healthy_next"), 0, -8);
  assert(logContains("restart RX failed")); request("/healthz"); assert(server.status == 200);
  // Genuine transitions, skipped nonces and raw telemetry remain visible.
  packet(fixtures.at("battery_low")); packet(fixtures.at("recovered"));
  assert(currentAlarmKind() == ALARM_LOW_BATTERY);
  assert(spdStates[1].skippedLast == 2 && spdStates[1].packetCount == count + 1);
  snapshot("low_battery");
  // A fault on another LCD page must alarm globally and survive page rotation.
  displayPages.restart(millis());
  packet(fixtures.at("offpage_failed")); assert(currentAlarmKind() == ALARM_SPD_FAIL);
  assert(displayPages.pageIndex() == 0); runFor(5200); assert(displayPages.pageIndex() == 1);
  snapshot("offpage_fault"); muteWeb("1", true); muteWeb("0", false);
  packet(fixtures.at("offpage_recovered"));
  packet(fixtures.at("battery_error")); assert(!spdStates[2].batteryValid && currentAlarmKind() == ALARM_NONE);
  // Inclusive freshness threshold and no alarm from stale or unknown devices.
  nowMs = spdStates[2].lastSeenMs + STALE_AFTER_SECONDS * 1000UL;
  assert(!isStale(spdStates[2])); step(1000); assert(isStale(spdStates[2]));
  assert(currentAlarmKind() == ALARM_NONE); snapshot("stale");
  packet(fixtures.at("battery_resumed")); assert(!isStale(spdStates[2]) && currentAlarmKind() == ALARM_LOW_BATTERY);
  // First nonce zero is valid, UINT64_MAX is valid once, neither a repeated
  // maximum nor a wrapped zero may roll accepted state backwards.
  packet(fixtures.at("nonce_zero")); assert(spdStates[3].nonceKnown && spdStates[3].nonce == 0);
  packet(fixtures.at("nonce_max")); assert(spdStates[3].nonce == UINT64_MAX);
  const auto maxPackets = spdStates[3].packetCount;
  packet(fixtures.at("nonce_max")); packet(fixtures.at("nonce_zero"));
  assert(spdStates[3].packetCount == maxPackets && spdStates[3].nonce == UINT64_MAX);
  if (SPD_LOCAL_ENABLED) {
    localContactOk = false; runFor(400); assert(isLocalFreshFail() && currentAlarmKind() == ALARM_SPD_FAIL);
    muteWeb("1",true); localContactOk = true; runFor(400); assert(isLocalFreshOk()); muteWeb("0",false);
  }
  // A web action and a received packet may both arrive in one iteration.
  // Neither may starve the next complete web snapshot or the radio restart.
  const auto packetsBeforeConcurrent = totalValidPackets, restartsBeforeConcurrent = radio.restarts;
  radio.packet = fixtures.at("concurrent_healthy"); loraPacketFlag = true;
  muteWeb("1", true);
  assert(totalValidPackets == packetsBeforeConcurrent + 1 && radio.restarts == restartsBeforeConcurrent + 1);
  request("/api/v1/get"); assert(server.status == 200 && alarmMuted); muteWeb("0",false);
  // A blocked debug consumer may overflow its bounded mirror, but every
  // application event still enters the authoritative ring, including the last.
  const auto logBefore = sessionLog.next();
  for (unsigned i = 0; i < 200; ++i)
    sessionLogf("Stress event %03u: attached USB diagnostic consumer cannot drain this event", i);
  assert(sessionLog.next() == logBefore + 200 && logContains("Stress event 199"));
  assert(serialLogMirror.dropped() > 0);
  // Reader reconnect, changed capacity and a fresh fault still permit control;
  // diagnostic text may be lost, but the authoritative ring remains complete.
  Serial.attached = true; Serial.draining = true;
  Serial.raceFull = Serial.zeroWrite = Serial.partialWrite = false;
  packet(fixtures.at("failed_again")); muteWeb("1",true); muteWeb("0",false);
  assert(logContains("Unmuted via web"));
  request("/api/v1/logs"); assert(server.status == 200);
  std::cout << "JSON logs " << server.body << '\n';
  const auto recoveryRed = redPaints, recoveryWhite = whitePaints; runFor(1100);
  assert(redPaints > recoveryRed && whitePaints > recoveryWhite);
  request("/api/v1/get"); assert(server.status == 200);
  assert(Serial.blockedMs == 0 && longestIteration <= 100 && wifiServices == iterations);
  std::cout << "PASS " << profile << " " << cases << " scenarios; " << iterations
            << " loop iterations, longest simulated blocking=" << longestIteration << " ms, "
            << radio.reads << " RX reads, " << server.sends << " HTTP responses, " << paints << " paints.\n";
}
'''


def fixture_source():
    vectors = [
        ("healthy", 1, 1, 1, 1, 255), ("failed", 2, 1, 0, 1, 255),
        ("healthy_next", 1, 1, 1, 2, 255), ("unknown_id", 127, 1, 0, 1, 255),
        ("battery_low", 3, 2, 1, 1, 60), ("recovered", 2, 1, 1, 4, 255),
        ("offpage_failed", 17, 1, 0, 1, 255), ("offpage_recovered", 17, 1, 1, 2, 255),
        ("battery_error", 3, 2, 1, 2, 255), ("battery_resumed", 3, 2, 1, 3, 60),
        ("nonce_zero", 4, 1, 1, 0, 0), ("nonce_max", 4, 1, 1, 2**64 - 1, 254),
        ("failed_again", 2, 1, 0, 5, 255),
        ("concurrent_healthy", 1, 1, 1, 3, 255),
    ]
    lines = ["std::map<std::string, std::vector<uint8_t>> fixtures;", "void initializeFixtures() {"]
    as_array = lambda data: "{" + ",".join(str(v) for v in data) + "}"
    for name, beacon_id, hardware, status, nonce, battery in vectors:
        prefix = bytes([(hardware << 4) | 1, (beacon_id << 1) | status, 251]) + nonce.to_bytes(8, "little") + bytes([battery])
        signed = prefix + (0x7C9E4A21).to_bytes(4, "little")
        digest = hashlib.sha256(signed).digest()
        lines.append(f'shaFixtures.push_back({{{as_array(signed)},{as_array(digest)}}});')
        lines.append(f'fixtures["{name}"] = {as_array(prefix + digest[:8])};')
    return "\n".join(lines + ["}"])


def driver(source):
    shims = api.SHIMS
    shims = replace(shims, "void sessionLogf(const char *, ...) {}", "")
    shims = replace(shims, "void setRelay(bool) {}", "")
    shims = replace(shims, "struct SerialShim { void printf(const char *, ...) {} } Serial;", "")
    shims = replace(shims, "  void notifyMuteStateChanged() {}", """  unsigned notifications = 0;
  int pendingMute = -1;
  void notifyMuteStateChanged() { ++notifications; }
  void loop(uint32_t, bool) { if (pendingMute >= 0) { const bool value = pendingMute; pendingMute = -1; mqttAlarmMute(value); } }""")
    shims = replace(shims, "struct MqttShim {", "void mqttAlarmMute(bool);\nstruct MqttShim {")
    shims = replace(shims, "  void setup() { ++setupCalls; }", "  void setup() { ++setupCalls; }\n  void loop(uint32_t) {}")
    shims = replace(shims, "  struct Route { String path; int method; void (*handler)(); };", """  std::deque<std::function<void()>> pending;
  void handleClient() { if (!pending.empty()) { auto request = pending.front(); pending.pop_front(); request(); } }
  struct Route { String path; int method; void (*handler)(); };""")
    # Standard-library headers required by the shared HTTP shim precede it.
    shims = "#include <deque>\n#include <functional>\nvoid mqttAlarmMute(bool);\n" + shims
    # Production sketches declare this function static; match that linkage.
    shims = shims.replace("void mqttAlarmMute(bool);", "static void mqttAlarmMute(bool);")
    body = shims + SHIMS.replace("void mqttAlarmMute(bool);", "static void mqttAlarmMute(bool);")
    body += '\n#include "serial_log.h"\nWspdSerialLogMirror serialLogMirror;\n'
    states = source[source.index("struct PacketLossWindow {"):source.index("volatile bool loraPacketFlag")]
    body += states + '\n#include "web_ui.h"\n#include "ha_guide.h"\n'
    names = ["sessionNowMs", "sessionLogLine", "serviceSerialLog", "sessionLogf", "findConfig", "readU64LE", "bytesToHex",
             "constantTimeEqual8", "verifyTag", "decodeBattery", "ageSecondsFor", "isStale", "isBatteryLow",
             "isFresh", "isFreshFail", "isFreshBatteryLow", "effectiveStatusText", "effectiveLocalStatusText",
             "isLocalOkRaw", "isLocalFreshFail", "isLocalFreshOk", "applyLocalSpdState", "handleLocalSpd",
             "currentAlarmKind", "alarmKindText", "isAlarmConditionActive", "jsonEscape", "processSpdPayload",
             "handleLoRa", "pointInRect", "setRelay", "setAlarmMuted", "mqttAlarmMute", "handleTouch", "handleAlarmRelay",
             "serviceSessionEvents", "appendLocalSpdJson", "appendWirelessSpdJson", "buildApiJson", "sendJson",
             "handleIndex", "handleHaGuide", "handleHaRest", "handleApiGet", "handleMute", "logDecimal", "handleLogs",
             "handleHealthz", "setupWebServer", "loop"]
    body += "\n".join(function(source, name) for name in names)
    return body + "\n" + fixture_source() + "\n" + CHECKS


def verify_output(output):
    responses = {}
    for line in output.splitlines():
        if line.startswith("JSON "):
            _, label, payload = line.split(" ", 2)
            responses[label] = json.loads(payload)
        elif line.startswith("PASS "):
            print(line)
    assert set(responses) == {"mixed", "muted", "low_battery", "offpage_fault", "stale", "logs"}
    for name, snapshot in responses.items():
        if name == "logs":
            assert snapshot["dropped"] > 0 and len(snapshot["entries"]) == 8
            assert any("Unmuted via web" in row["message"] for row in snapshot["entries"])
            continue
        rows = {row["spd_id"]: row for row in snapshot["spds"]}
        assert len(rows) == snapshot["summary"]["configured"]
        if name in {"mixed", "muted"}:
            assert rows[1]["status"] == "OK" and rows[2]["status"] == "FAIL"
            assert rows[1]["battery_voltage_v"] is None and rows[1]["temperature_c"] == -5
            assert snapshot["gateway"]["alarm_spd_fail"]
            assert snapshot["gateway"]["alarm_muted"] == (name == "muted")
        if name == "low_battery":
            assert rows[3]["hardware_version"] == 2 and rows[3]["battery_low_alarm"]
            assert rows[3]["rssi_dbm"] == -86.5 and rows[3]["snr_db"] == -4.2
            assert rows[2]["skipped"] == 2 and rows[2]["packet_count"] == 2
            assert snapshot["gateway"]["alarm_kind"] == "low_battery"
        if name == "offpage_fault":
            assert rows[17]["status"] == "FAIL" and snapshot["gateway"]["alarm_spd_fail"]
        if name == "stale":
            assert rows[1]["status"] == rows[2]["status"] == rows[3]["status"] == "STALE"
            assert rows[3]["battery_voltage_v"] is None and not rows[3]["battery_low_alarm"]
            assert not snapshot["gateway"]["alarm_active"]


def main():
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    source = (SERVER / "tconnectpro_868.ino").read_text()
    setup = function(source, "setup")
    assert setup.index("Serial.begin(") < setup.index("serialLogMirror.begin(Serial);") < setup.index("setupSessionLog();")
    fatal = function(source, "fatalBootError")
    assert "serviceSerialLog();" in fatal[fatal.index("while (true)"):], "Fatal setup errors must still drain diagnostics without waiting."
    assert not re.search(r"Serial\.(?:print|println|printf|write|flush)\s*\(", source), "Application diagnostics must use the bounded mirror."
    with tempfile.TemporaryDirectory(prefix="wspd-scenarios-") as temporary:
        work = Path(temporary)
        cpp = work / "scenarios.cpp"
        body = driver(source)
        cpp.write_text(body)
        for local in (0, 1):
            binary = work / f"scenarios-{local}"
            subprocess.run([compiler, "-std=c++11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-I", str(SERVER), "-DWIRELESS_COUNT=17",
                            f"-DHAS_LOCAL={local}", str(cpp), "-o", str(binary)], check=True)
            for profile in range(7):
                result = subprocess.run([str(binary), str(profile)], text=True, capture_output=True, timeout=20)
                if result.returncode:
                    raise SystemExit(result.stderr + "\n" + "\n".join(line for line in result.stdout.splitlines() if not line.startswith("JSON ")))
                verify_output(result.stdout)
        # A test that used to stub sessionLogLine/setAlarmMuted would stay green
        # with the old bug. Restore its synchronous println + default timeout
        # here and require this exact interleaved workload to catch the stall.
        legacy = replace(body, function(source, "sessionLogLine"), """static void sessionLogLine(const char *text) {
  sessionLog.append(sessionNowMs(), text);
  Serial.println(text);
}""")
        legacy = replace(legacy, "  serialLogMirror.begin(Serial);", "  // Legacy setup left the USB TX timeout at its default.")
        cpp.write_text(legacy)
        legacy_binary = work / "legacy-negative-control"
        subprocess.run([compiler, "-std=c++11", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", "-I", str(SERVER), "-DWIRELESS_COUNT=17",
                        "-DHAS_LOCAL=0", str(cpp), "-o", str(legacy_binary)], check=True)
        result = subprocess.run([str(legacy_binary), "1"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=20)
        assert result.returncode != 0 and "Loop stalled" in result.stderr and "Receiver loop blocked" in result.stderr, result.stderr
        stall = re.search(r"Loop stalled (\d+) ms", result.stderr)
        print(f"PASS: legacy synchronous-USB negative control detected {int(stall[1])} ms of blocked receiver time.")
    print("PASS: integrated action/packet/HTTP scenarios across 14 USB/local-input configurations.")
    print("Real production mute, touch, packet decoder, alarm, ring logging, HTTP routes and receiver loop were executed.")
    print("Virtual time detects blocking I/O; hardware render/HTTP latency, RF and broker transport need their separate live checks.")


if __name__ == "__main__":
    main()
