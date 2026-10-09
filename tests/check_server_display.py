#!/usr/bin/env python3
"""Run the actual LCD renderer, alarm scan, reset, and loop with host I/O shims."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"


def function(source, name):
    match = re.search(r"(?:static\s+)?(?:void|uint8_t)\s+" + name +
                      r"\([^;{]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Cannot find firmware function: {name}")
    at, depth = match.end(), 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[match.start():at]


SHIMS = r'''
#include "display_pages.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using String = std::string;
constexpr size_t SPD_TOTAL_COUNT = TOTAL_ROWS;
constexpr bool SPD_LOCAL_ENABLED = HAS_LOCAL;
constexpr size_t SPD_COUNT = SPD_TOTAL_COUNT - (SPD_LOCAL_ENABLED ? 1 : 0);
constexpr int16_t SPD_LOCAL_ID = SPD_LOCAL_ENABLED ? 0 : -1;
constexpr int8_t SPD_LOCAL_PIN = 15;
const char *SPD_LOCAL_FRIENDLYNAME = "Local contact";
constexpr float LORA_FREQ_MHZ = 865.3f;
constexpr uint8_t LORA_SF = 10;
constexpr uint16_t BLACK = 0, WHITE = 1, RED = 2, DARKGREEN = 3;
constexpr uint8_t ALARM_NONE = 0, ALARM_LOW_BATTERY = 1, ALARM_SPD_FAIL = 2;
constexpr uint8_t CONFIRM_NONE = 0, CONFIRM_RESET = 1;
constexpr uint8_t RELAY_1 = 8, RELAY_INACTIVE_LEVEL = 1;
constexpr int16_t RESET_BTN_X = 300, RESET_BTN_Y = 194, RESET_BTN_W = 78, RESET_BTN_H = 24;
constexpr int16_t MUTE_BTN_X = 388, MUTE_BTN_Y = 194, MUTE_BTN_W = 92, MUTE_BTN_H = 24;
constexpr uint32_t DISPLAY_PERIODIC_REFRESH_MS = 60000UL;
struct SpdConfig { uint8_t id; const char *friendlyName; uint32_t secret; };
struct SpdState {
  bool seen = false, fresh = false, batteryLow = false, batteryValid = false;
  uint8_t statusCode = 1;
  float batteryVoltageV = 3.2f, rssiDbm = -70, snrDb = 9;
};
SpdConfig SPD_CONFIGS[SPD_COUNT ? SPD_COUNT : 1];
SpdState spdStates[SPD_COUNT ? SPD_COUNT : 1];
bool localFail = false;
bool screenFlashRed = false, screenDirty = true, alarmMuted = false;
uint8_t confirmMode = CONFIRM_NONE;
uint32_t nowMs = 0, lastScreenDrawMs = 0, lastFlashToggleMs = 0, nonceResets = 0;
SpdDisplayPages displayPages(SPD_TOTAL_COUNT);
uint32_t millis() { return nowMs; }
bool isLocalFreshFail() { return SPD_LOCAL_ENABLED && localFail; }
bool isLocalFreshOk() { return SPD_LOCAL_ENABLED && !localFail; }
bool isFresh(const SpdState &s) { return s.seen && s.fresh; }
bool isFreshFail(const SpdState &s) { return isFresh(s) && s.statusCode == 0; }
bool isFreshBatteryLow(const SpdState &s) { return isFresh(s) && s.batteryLow; }
const char *effectiveLocalStatusText() { return localFail ? "FAIL" : "OK"; }
const char *effectiveStatusText(const SpdState &s) {
  return !isFresh(s) ? "UNKNOWN" : (s.statusCode ? "OK" : "FAIL");
}
String ipString() { return "192.0.2.1"; }
String lossText(const SpdState &) { return "0%"; }
String ageText(const SpdState &) { return "<1m"; }
uint16_t colorLightRed() { return 4; }
uint16_t colorLightGreen() { return 5; }
uint16_t colorLightOrange() { return 6; }
struct Printed { int16_t x, y; String text; };
struct Graphics {
  int16_t x = 0, y = 0;
  uint16_t background = WHITE;
  std::vector<Printed> printed;
  int16_t width() { return 480; }
  void fillScreen(uint16_t color) { background = color; printed.clear(); }
  void setTextSize(int) {}
  void setTextColor(uint16_t) {}
  void setCursor(int16_t a, int16_t b) { x = a; y = b; }
  void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void print(const String &s) { printed.push_back({x, y, s}); }
  void printf(const char *format, ...) {
    char result[256]; va_list args; va_start(args, format);
    vsnprintf(result, sizeof(result), format, args); va_end(args); print(result);
  }
} graphics;
Graphics *gfx = &graphics;
size_t buttonCount = 0, dialogCount = 0, flushCount = 0;
void drawButton(int16_t, int16_t, int16_t, int16_t, const char *) { buttonCount++; }
void drawConfirmDialog() { dialogCount++; }
void flushDisplay() { flushCount++; }
void requestScreenRedraw() { screenDirty = true; }
void resetLocalSpdLiveState() {}
void digitalWrite(int, int) {}
struct SerialShim { void printf(const char *, ...) {} } Serial;
struct ServerShim { void handleClient() {} } server;
void handleLoRa() {}
void handleLocalSpd() {}
void handleTouch() {}
void handleAlarmRelay() {}
'''


CHECKS = r'''
std::vector<unsigned> renderedIds() {
  std::vector<unsigned> ids;
  for (const auto &item : graphics.printed) {
    if (item.x == 2 && item.y >= 45 && item.y <= 171) {
      assert(item.text.size() == 3);
      assert(item.y == 45 + 18 * static_cast<int>(ids.size()));
      ids.push_back(static_cast<unsigned>(std::stoul(item.text)));
    }
  }
  return ids;
}
bool textContains(const char *needle) {
  for (const auto &item : graphics.printed)
    if (item.text.find(needle) != String::npos) return true;
  return false;
}
void checkRenderer() {
  displayPages.restart(100);
  std::vector<unsigned> allIds;
  const size_t expectedPages = (SPD_TOTAL_COUNT + 7) / 8;
  assert(displayPages.pageCount() == expectedPages);
  for (size_t page = 0; page < expectedPages; page++) {
    buttonCount = dialogCount = flushCount = 0;
    drawStatusTable();
    const auto ids = renderedIds();
    assert(ids.size() == (page + 1 < expectedPages ? 8 : SPD_TOTAL_COUNT - 8 * page));
    assert(buttonCount == 2 && dialogCount == 0 && flushCount == 1);
    assert(textContains("LOCAL") == (SPD_LOCAL_ENABLED && page == 0));
    assert(textContains("Page ") == (expectedPages > 1));
    allIds.insert(allIds.end(), ids.begin(), ids.end());
    if (expectedPages > 1) {
      nowMs = 100 + static_cast<uint32_t>(5000 * (page + 1));
      assert(displayPages.advance(nowMs, false));
    }
  }
  assert(displayPages.pageIndex() == 0);
  assert(allIds.size() == SPD_TOTAL_COUNT);
  size_t index = 0;
  if (SPD_LOCAL_ENABLED) assert(allIds[index++] == 0);
  for (size_t wireless = 1; wireless <= SPD_COUNT; wireless++)
    assert(allIds[index++] == wireless);
}
void checkTiming() {
  SpdDisplayPages pages(17);
  pages.restart(100);
  assert(!pages.advance(5099, false) && pages.pageIndex() == 0);
  assert(pages.advance(5100, false) && pages.pageIndex() == 1);
  assert(!pages.advance(9000, true) && pages.pageIndex() == 1);
  assert(!pages.advance(14000, true) && pages.pageIndex() == 1);
  assert(!pages.advance(18999, false));
  assert(pages.advance(19000, false) && pages.pageIndex() == 2);
  assert(pages.rowCount() == 1);
  assert(pages.advance(24000, false) && pages.pageIndex() == 0);
  pages.restart(UINT32_MAX - 2000);
  assert(!pages.advance(2998, false));
  assert(pages.advance(2999, false) && pages.pageIndex() == 1);
  pages.restart(12000);
  assert(pages.pageIndex() == 0 && !pages.advance(16999, false));
  for (size_t total : {size_t(0), size_t(1), size_t(8)}) {
    SpdDisplayPages single(total);
    single.restart(0);
    assert(single.rowCount() == total);
    assert(!single.advance(5000, false) && !single.advance(UINT32_MAX, false));
    assert(single.pageIndex() == 0);
  }
}
void checkLoopAndAlarm() {
  displayPages.restart(0); nowMs = 4999; screenDirty = false;
  loop(); assert(displayPages.pageIndex() == 0);
  nowMs = 5000; loop();
  assert(displayPages.pageIndex() == (SPD_TOTAL_COUNT > 8 ? 1 : 0));
  if (SPD_TOTAL_COUNT > 8) {
    confirmMode = CONFIRM_RESET; requestScreenRedraw(); nowMs = 10000; loop();
    assert(displayPages.pageIndex() == 1 && dialogCount > 0);
    nowMs = 15000; loop(); assert(displayPages.pageIndex() == 1);
    confirmMode = CONFIRM_NONE; requestScreenRedraw(); nowMs = 19999; loop();
    assert(displayPages.pageIndex() == 1);
    nowMs = 20000; loop();
    assert(displayPages.pageIndex() == (2 % displayPages.pageCount()));
  }
  if (SPD_TOTAL_COUNT > 8 && SPD_COUNT > 0) {
    displayPages.restart(nowMs);
    auto &offPage = spdStates[SPD_COUNT - 1];
    offPage.seen = offPage.fresh = offPage.batteryLow = true;
    assert(currentAlarmKind() == ALARM_LOW_BATTERY);
    offPage.statusCode = 0;
    assert(currentAlarmKind() == ALARM_SPD_FAIL);
    screenFlashRed = true; drawStatusTable();
    assert(graphics.background == colorLightRed() && textContains("SPD FAIL ALARM"));
    const auto ids = renderedIds();
    for (auto id : ids) assert(id != SPD_COUNT);
    offPage.fresh = false;
    assert(currentAlarmKind() == ALARM_NONE);
    screenFlashRed = false;
  }
  if (SPD_LOCAL_ENABLED) {
    localFail = true; assert(currentAlarmKind() == ALARM_SPD_FAIL); localFail = false;
  }
  nowMs = 25000; confirmMode = CONFIRM_RESET;
  resetAllSpdLiveState();
  assert(displayPages.pageIndex() == 0 && confirmMode == CONFIRM_NONE && nonceResets == 1);
  assert(!displayPages.advance(29999, false));
  assert(displayPages.advance(30000, false) == (SPD_TOTAL_COUNT > 8));
  for (size_t i = 0; i < SPD_COUNT; i++) assert(!spdStates[i].seen);
}
int main() {
  for (size_t i = 0; i < SPD_COUNT; i++) {
    SPD_CONFIGS[i] = {static_cast<uint8_t>(i + 1), "Wireless", 1};
    spdStates[i].seen = spdStates[i].fresh = true;
  }
  checkRenderer(); checkTiming(); checkLoopAndAlarm();
}
'''


def main():
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    source = (SERVER / "tconnectpro_868.ino").read_text()
    # Reuse production capacity assertions instead of duplicating their limits.
    assertions = re.findall(r"static_assert\(SPD_[^;]+;", source)
    body = SHIMS + "\n" + "\n".join(assertions) + "\n"
    for name in ["currentAlarmKind", "resetAllSpdLiveState", "drawStatusTable", "loop"]:
        body += function(source, name) + "\n"
    body += CHECKS
    variants = [(rows, local) for rows in (1, 8, 9, 16, 17, 127) for local in (0, 1)]
    variants.append((128, 1))
    with tempfile.TemporaryDirectory(prefix="wspd-display-") as temporary:
        work = Path(temporary)
        driver = work / "display.cpp"
        driver.write_text(body)
        for rows, local in variants:
            binary = work / f"display-{rows}-{local}"
            subprocess.run([compiler, "-std=c++11", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(SERVER), f"-DTOTAL_ROWS={rows}", f"-DHAS_LOCAL={local}",
                            str(driver), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        for rows, local in [(0, 0), (128, 0), (129, 1)]:
            result = subprocess.run([compiler, "-std=c++11", "-fsyntax-only", "-I", str(SERVER),
                                     f"-DTOTAL_ROWS={rows}", f"-DHAS_LOCAL={local}", str(driver)],
                                    capture_output=True, text=True)
            if result.returncode == 0 or "static assertion" not in result.stderr:
                raise SystemExit(f"Expected production capacity assertion for {rows} rows, local={local}")
    print(f"PASS: {len(variants)} LCD configurations; actual renderer, loop, alarm scan, and reset.")
    print("Verified complete row coverage, local mapping, off-page alarms, 5-second timing, rollover, and confirmation pause.")
    print("Host graphics and telemetry shims do not qualify physical LCD, touch, relay, RF, or maximum-device performance.")


if __name__ == "__main__":
    main()
