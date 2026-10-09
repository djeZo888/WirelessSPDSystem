#!/usr/bin/env python3
"""Exercise the production JSON response and browser renderer with host I/O shims."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"


def function(source, name):
    match = re.search(r"(?:static\s+)?(?:const char \*|String|bool|uint32_t|uint8_t|void)\s*" +
                      name + r"\([^;{]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Cannot find firmware function: {name}")
    at, depth = match.end(), 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[match.start():at]


SHIMS = r'''
#include "display_pages.h"
#include "version.h"
#include "config_types.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
// Arduino String's numeric constructors print numbers rather than characters.
struct String : std::string {
  String() = default;
  String(const char *s) : std::string(s) {}
  String(const std::string &s) : std::string(s) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T v) : std::string(std::to_string(v)) {}
  String(double value, int decimals = 2) {
    char out[80]; snprintf(out, sizeof(out), "%.*f", decimals, value); assign(out);
  }
};
String operator+(const String &a, const String &b) { return String(std::string(a) + std::string(b)); }
String operator+(const String &a, const char *b) { return String(std::string(a) + b); }
String operator+(const char *a, const String &b) { return String(a + std::string(b)); }
constexpr size_t SPD_COUNT = WIRELESS_COUNT;
constexpr bool SPD_LOCAL_ENABLED = HAS_LOCAL;
constexpr size_t SPD_TOTAL_COUNT = SPD_COUNT + (SPD_LOCAL_ENABLED ? 1 : 0);
constexpr int16_t SPD_LOCAL_ID = SPD_LOCAL_ENABLED ? 0 : -1;
constexpr int8_t SPD_LOCAL_PIN = 15;
const char *SPD_LOCAL_FRIENDLYNAME = "Local <contact>";
SpdConfig SPD_CONFIGS[SPD_COUNT ? SPD_COUNT : 1];
constexpr size_t SPD_PAYLOAD_LEN = 20;
constexpr uint16_t PACKET_LOSS_WINDOW_SIZE = 1000;
constexpr uint32_t STALE_AFTER_SECONDS = 700;
constexpr bool LOW_BATTERY_WARNING_ENABLED = true;
constexpr float LOW_BATTERY_WARNING_V = 2.95f;
constexpr float LORA_FREQ_MHZ = 865.3f, LORA_BW_KHZ = 125;
constexpr uint8_t LORA_SF = 10;
constexpr uint8_t ALARM_NONE = 0, ALARM_LOW_BATTERY = 1, ALARM_SPD_FAIL = 2;
uint32_t nowMs = 1000000, bootMs = 0;
uint32_t totalValidPackets = 0, totalInvalidPackets = 0, totalOldNoncePackets = 0;
uint32_t totalAuthRejects = 0, totalUnconfiguredRejects = 0, nonceResets = 0;
bool alarmMuted = false;
SpdDisplayPages displayPages(SPD_TOTAL_COUNT, 5000UL);
uint32_t millis() { return nowMs; }
String ipString() { return "192.0.2.1"; }
String u64Hex(uint64_t value) {
  char out[19]; snprintf(out, sizeof(out), "0x%016llX", static_cast<unsigned long long>(value));
  return out;
}
struct ServerShim {
  unsigned sends = 0, noStoreHeaders = 0;
  int status = 0;
  String contentType, body;
  void sendHeader(const char *name, const char *value) {
    assert(String(name) == "Cache-Control" && String(value) == "no-store"); noStoreHeaders++;
  }
  void send(int code, const char *type, const String &data) {
    sends++; status = code; contentType = type; body = data;
  }
} server;
'''


CHECKS = r'''
int main() {
  for (size_t i = 0; i < SPD_COUNT; i++) {
    SPD_CONFIGS[i] = {static_cast<uint8_t>(i + 1), "Remote \"rack\" <&>\n\t", 1};
    auto &s = spdStates[i];
    s.seen = (i % 4 != 0);
    s.statusCode = (i % 4 == 2 ? 0 : 1);
    s.lastSeenMs = nowMs - (i % 4 == 3 ? 701000 : 2000);
    s.hardwareVersion = 2; s.firmwareVersion = 0;
    s.batteryValid = true; s.batteryVoltageV = (i % 4 == 1 ? 2.8f : 3.2f);
    s.temperatureC = -5; s.rssiDbm = -70.0f; s.snrDb = 9.0f;
    s.nonceKnown = s.seen; s.nonce = i + 1;
    s.packetCount = s.seen ? 5 : 0; s.totalSkipped = s.seen ? 2 : 0;
    if (s.seen) { s.loss.addMany(true, 5); s.loss.addMany(false, 2); }
    strcpy(s.rawHex, "0000000000000000000000000000000000000000");
  }
  localSpd.seen = SPD_LOCAL_ENABLED; localSpd.statusCode = 1;
  // A GET is one complete HTTP response even when another LCD page is active.
  displayPages.restart(0);
  String firstResponse;
  for (size_t page = 0; page < displayPages.pageCount(); page++) {
    const unsigned before = server.sends;
    handleApiGet();
    assert(server.sends == before + 1 && server.noStoreHeaders == server.sends);
    assert(server.status == 200 && server.contentType == "application/json; charset=utf-8");
    if (page == 0) firstResponse = server.body;
    else assert(server.body == firstResponse);
    std::cout << server.body << '\n';
    if (displayPages.pageCount() > 1)
      assert(displayPages.advance(static_cast<uint32_t>((page + 1) * 5000), false));
  }
  assert(displayPages.pageIndex() == 0);
}
'''


WEB_CHECKS = r'''
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const payloads = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const script = fs.readFileSync(process.argv[3], 'utf8');
(async () => {
  for (const payload of payloads) {
    const elements = new Map();
    const context = {
      document: { getElementById(id) {
        if (!elements.has(id)) elements.set(id, { textContent: '', innerHTML: '',
          classList: { toggle() {} } });
        return elements.get(id);
      } },
      fetch: async (url, options) => {
        assert.strictEqual(url, '/api/v1/get');
        assert.strictEqual(options.cache, 'no-store');
        return { json: async () => payload };
      },
      setInterval: (fn, ms) => { assert.strictEqual(ms, 5000); },
    };
    vm.createContext(context);
    vm.runInContext(script, context);
    await vm.runInContext('load()', context);
    const html = elements.get('body').innerHTML;
    assert(elements.get('meta').textContent.includes('v0.1-tconnpro'));
    assert.strictEqual((html.match(/<tr\b/g) || []).length, payload.spds.length);
    const ids = [...html.matchAll(/<td>(\d{3})<\/td>/g)].map(match => Number(match[1]));
    assert.deepStrictEqual(ids, payload.spds.map(spd => spd.spd_id));
    assert(!html.includes('<contact>') && !html.includes('<&>'));
    assert(html.includes('&lt;contact&gt;') === payload.gateway.local_spd_enabled);
    if (payload.spds.some(spd => !spd.local))
      assert(html.includes('&quot;rack&quot; &lt;&amp;&gt;'));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
'''


def verify_response(payload, wireless, local):
    expected_ids = ([0] if local else []) + list(range(1, wireless + 1))
    rows = payload["spds"]
    assert payload["gateway"]["server_version"] == "v0.1"
    assert payload["gateway"]["firmware_id"] == "v0.1-tconnpro"
    assert payload["summary"]["configured"] == wireless + local
    assert [row["spd_id"] for row in rows] == expected_ids
    assert len(rows) == len(expected_ids)
    for row in rows:
        if row["local"]:
            assert row["source"] == "local" and row["friendly_name"] == "Local <contact>"
            assert row["status"] == "OK" and row["hardware_version"] is None
            continue
        index = row["spd_id"] - 1
        mode = index % 4
        assert row["friendly_name"] == 'Remote "rack" <&>\n\t'
        assert row["status"] == ["UNKNOWN", "OK", "FAIL", "STALE"][mode]
        assert row["status_code"] == [None, 1, 0, None][mode]
        assert row["battery_low_alarm"] == (mode == 1)
        assert row["stale"] == (mode == 3)
        if mode != 0:
            assert row["hardware_version"] == 2 and row["firmware_version"] == 0
            assert row["temperature_c"] == -5 and row["packet_count"] == 5
            assert row["packet_loss_window_total"] == 7
            assert row["packet_loss_window_missed"] == 2
    statuses = [row["status"] for row in rows]
    summary = payload["summary"]
    assert summary["seen"] == sum(status != "UNKNOWN" for status in statuses)
    assert summary["ok"] == statuses.count("OK")
    assert summary["fail"] == statuses.count("FAIL")
    assert summary["unknown"] == statuses.count("UNKNOWN") + statuses.count("STALE")
    assert summary["stale"] == statuses.count("STALE")
    assert summary["low_battery"] == sum(row["battery_low_alarm"] for row in rows)


def main():
    compiler, node = shutil.which("c++"), shutil.which("node")
    if not compiler or not node:
        raise SystemExit("A host C++ compiler and Node.js are required.")
    source = (SERVER / "tconnectpro_868.ino").read_text()
    states = source[source.index("struct PacketLossWindow {"):source.index("volatile bool loraPacketFlag")]
    body = SHIMS + states + "\n"
    for name in ["ageSecondsFor", "isStale", "isBatteryLow", "isFresh", "isFreshFail",
                 "isFreshBatteryLow", "effectiveStatusText", "effectiveLocalStatusText",
                 "isLocalFreshFail", "currentAlarmKind", "alarmKindText", "jsonEscape",
                 "appendLocalSpdJson", "appendWirelessSpdJson", "buildApiJson", "sendJson", "handleApiGet"]:
        body += function(source, name) + "\n"
    body += CHECKS
    web_script = re.search(r'<script>(.*?)</script>', source, re.S).group(1)
    variants = [(count, local) for count in (1, 8, 9, 17, 24, 127) for local in (0, 1)]
    variants.append((0, 1))
    payloads = []
    with tempfile.TemporaryDirectory(prefix="wspd-api-") as temporary:
        work = Path(temporary)
        driver = work / "api.cpp"
        driver.write_text(body)
        for wireless, local in variants:
            binary = work / f"api-{wireless}-{local}"
            subprocess.run([compiler, "-std=c++11", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(SERVER), f"-DWIRELESS_COUNT={wireless}", f"-DHAS_LOCAL={local}",
                            str(driver), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            responses = [json.loads(line) for line in result.stdout.splitlines()]
            assert len(responses) == (wireless + local + 7) // 8
            for response in responses:
                verify_response(response, wireless, local)
            payloads.append(responses[0])
        (work / "payloads.json").write_text(json.dumps(payloads))
        (work / "dashboard.js").write_text(web_script)
        (work / "check-web.js").write_text(WEB_CHECKS)
        subprocess.run([node, str(work / "check-web.js"), str(work / "payloads.json"),
                        str(work / "dashboard.js")], check=True)
    print(f"PASS: {len(variants)} API/web configurations, up to 127 wireless SPDs plus local ID 0.")
    print("Verified one complete GET response on every LCD page, JSON telemetry/summary, and full browser row coverage.")
    print("Host String, HTTP, and DOM shims do not qualify physical Wi-Fi or maximum-device performance.")


if __name__ == "__main__":
    main()
