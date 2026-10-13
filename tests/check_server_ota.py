#!/usr/bin/env python3
"""Exercise production OTA handlers with host I/O and real SHA-256 adapters.

The WebServer/Update/partition shims record calls, not actual flash operations.
Both MbedTLS API variants use OpenSSL EVP for real streaming SHA-256. ESP32
builds and a physical update still verify hardware/library integration.
"""
import hashlib
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"
MARKER = b"WirelessSPDSystem:tconnpro:ota-v1"
IMAGE = bytearray((i * 17 + 3) % 256 for i in range(1024))
IMAGE[0:2] = bytes([0xE9, 4])
IMAGE[12:14] = bytes([9, 0])
IMAGE[32:36] = bytes([0x32, 0x54, 0xCD, 0xAB])
IMAGE[300:300 + len(MARKER)] = MARKER

SHIMS = r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <openssl/evp.h>
using std::min;
struct String : std::string {
  String() = default;
  String(const char *s) : std::string(s) {}
  String(const std::string &s) : std::string(s) {}
};
String operator+(const String &a, const char *b) { return String(std::string(a) + b); }
String operator+(const String &a, const String &b) { return String(std::string(a) + std::string(b)); }
uint32_t nowMs = 100;
uint32_t millis() { return nowMs; }
std::vector<std::string> events;
struct EspShim { unsigned restarts = 0; void restart() { restarts++; events.push_back("restart"); } } ESP;
constexpr int HTTP_POST = 1, U_FLASH = 0;
enum UploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  UploadStatus status = UPLOAD_FILE_START;
  String name = "firmware";
  size_t currentSize = 0, totalSize = 0;
  uint8_t buf[4096] = {};
};
struct ClientShim {
  bool live = true;
  unsigned stops = 0;
  void stop() { live = false; stops++; events.push_back("client-close"); }
  bool connected() { return live; }
};
struct WebServer {
 protected:
  ClientShim _currentClient;
 public:
  HTTPUpload incoming;
  std::function<void()> requestHandler, uploadHandler;
  bool authenticated = true;
  unsigned authentications = 0, responses = 0;
  std::map<std::string, String> fields, requestHeaders, responseHeaders;
  int status = 0;
  String body;
  // Mirror the core 2 copy-returning API: closing its temporary would leave the
  // parser's original client live. The production subclass must close original.
#if MBEDTLS_VERSION_MAJOR >= 3
  ClientShim &client() { return _currentClient; }
#else
  ClientShim client() { return _currentClient; }
#endif
  unsigned clientStops() { return _currentClient.stops; }
  void newConnection() { _currentClient.live = true; responseHeaders.clear(); }
  void on(const char *path, int method, std::function<void()> request, std::function<void()> upload) {
    assert(String(path) == "/api/v1/firmware" && method == HTTP_POST);
    requestHandler = request; uploadHandler = upload;
  }
  HTTPUpload &upload() { return incoming; }
  bool authenticate(const char *user, const char *password) {
    assert(String(user) == "updater" && String(password) == "synthetic-test-password");
    authentications++; return authenticated;
  }
  String header(const char *name) { return requestHeaders[name]; }
  bool hasArg(const char *name) { return fields.count(name); }
  String arg(const char *name) { return fields[name]; }
  void sendHeader(const char *name, const char *value) { responseHeaders[name] = value; }
  void send(int code, const char *type, const String &data) {
    assert(String(type) == "application/json; charset=utf-8");
    assert(responseHeaders["Cache-Control"] == "no-store");
    responses++; status = code; body = data; events.push_back("response");
  }
};
struct esp_partition_t { size_t size; };
esp_partition_t partition = {0x300000};
bool partitionPresent = true;
const esp_partition_t *esp_ota_get_next_update_partition(void *) { return partitionPresent ? &partition : nullptr; }
struct UpdateShim {
  bool beginOk = true, endOk = true, shortWrite = false, running = false;
  unsigned begins = 0, writes = 0, aborts = 0, ends = 0, activations = 0;
  size_t declared = 0;
  std::vector<uint8_t> bytes;
  bool begin(size_t size, int mode) {
    assert(mode == U_FLASH); begins++; events.push_back("begin"); declared = size;
    if (!beginOk) return false;
    assert(!running); bytes.clear(); running = true; return true;
  }
  size_t write(uint8_t *data, size_t count) {
    assert(running); writes++; events.push_back("write");
    const size_t actual = shortWrite && count ? count - 1 : count;
    bytes.insert(bytes.end(), data, data + actual); return actual;
  }
  void abort() { running = false; aborts++; events.push_back("abort"); }
  bool end(bool allowRemaining) {
    assert(running && !allowRemaining); ends++; events.push_back("end");
    if (!endOk) { running = false; return false; }
    assert(bytes.size() == declared); running = false; activations++; return true;
  }
} Update;
bool maintenanceActive = false;
void maintenance(bool value) { maintenanceActive = value; events.push_back(value ? "maintenance-on" : "maintenance-off"); }
bool hashStartFail = false, hashUpdateFail = false, hashFinishFail = false;
unsigned hashAllocations = 0, hashFrees = 0;
struct mbedtls_sha256_context { EVP_MD_CTX *context; };
void mbedtls_sha256_init(mbedtls_sha256_context *hash) { hash->context = nullptr; }
void mbedtls_sha256_free(mbedtls_sha256_context *hash) {
  if (hash->context) { EVP_MD_CTX_free(hash->context); hash->context = nullptr; hashFrees++; }
}
int shaStarts(mbedtls_sha256_context *hash, int is224) {
  assert(is224 == 0 && !hash->context);
  if (hashStartFail) return -1;
  hash->context = EVP_MD_CTX_new(); assert(hash->context); hashAllocations++;
  return EVP_DigestInit_ex(hash->context, EVP_sha256(), nullptr) == 1 ? 0 : -1;
}
int shaUpdate(mbedtls_sha256_context *hash, const uint8_t *data, size_t count) {
  assert(hash->context); if (hashUpdateFail) return -1;
  return EVP_DigestUpdate(hash->context, data, count) == 1 ? 0 : -1;
}
int shaFinish(mbedtls_sha256_context *hash, uint8_t *out) {
  assert(hash->context); if (hashFinishFail) return -1;
  unsigned count = 0; return EVP_DigestFinal_ex(hash->context, out, &count) == 1 && count == 32 ? 0 : -1;
}
#if MBEDTLS_VERSION_MAJOR >= 3
int mbedtls_sha256_starts(mbedtls_sha256_context *h, int type) { return shaStarts(h, type); }
int mbedtls_sha256_update(mbedtls_sha256_context *h, const uint8_t *data, size_t n) { return shaUpdate(h, data, n); }
int mbedtls_sha256_finish(mbedtls_sha256_context *h, uint8_t *out) { return shaFinish(h, out); }
#else
int mbedtls_sha256_starts_ret(mbedtls_sha256_context *h, int type) { return shaStarts(h, type); }
int mbedtls_sha256_update_ret(mbedtls_sha256_context *h, const uint8_t *data, size_t n) { return shaUpdate(h, data, n); }
int mbedtls_sha256_finish_ret(mbedtls_sha256_context *h, uint8_t *out) { return shaFinish(h, out); }
#endif
'''

CHECKS = r'''
#include "ota_web.h"
#include <iostream>
unsigned cases = 0;
std::vector<uint8_t> image() {
  std::vector<uint8_t> b(1024);
  for (size_t i = 0; i < b.size(); ++i) b[i] = (i * 17 + 3) % 256;
  b[0] = 0xE9; b[1] = 4; b[12] = 9; b[13] = 0;
  b[32] = 0x32; b[33] = 0x54; b[34] = 0xCD; b[35] = 0xAB;
  memcpy(b.data() + 300, WSPD_OTA_IMAGE_MARKER, sizeof(WSPD_OTA_IMAGE_MARKER) - 1);
  return b;
}
String digest(const std::vector<uint8_t> &bytes) {
  uint8_t out[32]; unsigned n = 0;
  assert(EVP_Digest(bytes.data(), bytes.size(), out, &n, EVP_sha256(), nullptr) == 1 && n == 32);
  char text[65]; for (size_t i = 0; i < 32; ++i) snprintf(text + i * 2, 3, "%02x", out[i]);
  return text;
}
struct Fixture {
  WspdWebServer web;
  WspdWebOta ota;
  Fixture(bool enabled = true) : ota(web, enabled, "updater", "synthetic-test-password", maintenance) {
    assert(!Update.running && !maintenanceActive && hashAllocations == hashFrees);
    Update = UpdateShim(); ESP.restarts = 0; events.clear(); nowMs = 100;
    hashStartFail = hashUpdateFail = hashFinishFail = false;
    partitionPresent = true; partition.size = 0x300000;
    web.requestHeaders["X-WSPD-Update"] = "1";
    ota.setup(); assert(web.requestHandler && web.uploadHandler);
  }
  void start(const std::vector<uint8_t> &bytes, const String &size = "", const String &sha = "") {
    web.fields["size"] = size.empty() ? String(std::to_string(bytes.size())) : size;
    web.fields["sha256"] = sha.empty() ? digest(bytes) : sha;
    web.incoming.status = UPLOAD_FILE_START; web.incoming.name = "firmware";
    web.incoming.currentSize = web.incoming.totalSize = 0; web.uploadHandler();
  }
  void chunk(const uint8_t *bytes, size_t count) {
    assert(count <= sizeof(web.incoming.buf));
    web.incoming.status = UPLOAD_FILE_WRITE; web.incoming.currentSize = count;
    if (count) memcpy(web.incoming.buf, bytes, count);
    web.uploadHandler(); web.incoming.totalSize += count;
  }
  void write(const std::vector<uint8_t> &bytes, size_t chunkSize = 137) {
    for (size_t i = 0; i < bytes.size() && web.uploadClientConnected(); i += chunkSize)
      chunk(bytes.data() + i, min(chunkSize, bytes.size() - i));
  }
  void end() { web.incoming.status = UPLOAD_FILE_END; web.uploadHandler(); }
  void abort() { web.incoming.status = UPLOAD_FILE_ABORTED; web.uploadHandler(); }
  void finish(int code, const char *error = nullptr) {
    const bool earlyResponse = !web.uploadClientConnected();
    const unsigned before = web.responses;
    const String previousBody = web.body;
    web.requestHandler();
    assert(web.responses == before + (earlyResponse ? 0 : 1) && web.status == code);
    if (earlyResponse) {
      assert(web.body == previousBody && web.clientStops() > 0);
      assert(web.responseHeaders["Connection"] == "close");
      assert(events[events.size() - 2] == "response" && events.back() == "client-close");
    }
    if (error) assert(web.body.find(String("\"error\":\"") + error + "\"") != String::npos);
    if (code != 202) {
      assert(!Update.running && !maintenanceActive && !ota.busy() && Update.activations == 0 && ESP.restarts == 0);
      assert(hashAllocations == hashFrees);
      nowMs += 2000; ota.loop(nowMs); assert(ESP.restarts == 0);
    }
    cases++;
  }
  void uploadSuccess(const std::vector<uint8_t> &bytes, size_t chunkSize = 137) {
    const unsigned previousBegins = Update.begins, previousEnds = Update.ends, previousActivations = Update.activations;
    start(bytes); assert(ota.busy() && maintenanceActive && Update.begins == previousBegins + 1);
    write(bytes, chunkSize); assert(Update.bytes == bytes);
    end(); assert(Update.ends == previousEnds && Update.activations == previousActivations && ota.busy());
    finish(202);
    assert(Update.ends == previousEnds + 1 && Update.activations == previousActivations + 1 && ota.busy() && maintenanceActive && ESP.restarts == 0);
    assert(hashAllocations == hashFrees);
    assert(events.back() == "response");
    nowMs += 1499; ota.loop(nowMs); assert(ESP.restarts == 0);
    nowMs++; ota.loop(nowMs); assert(ESP.restarts == 1 && events.back() == "restart");
    // Test harness teardown represents the successful reboot.
    maintenanceActive = false;
  }
};
void parsingAndMarker() {
  size_t size = 0;
  for (const char *bad : {"", "0", "1", "287", "-1", "+1024", " 1024", "1024 ", "1e3", "1024.0", "3145729", "999999999999999999999999999999999"})
    assert(!wspdOtaSize(bad, 0x300000, size));
  assert(!wspdOtaSize(nullptr, 0x300000, size));
  assert(wspdOtaSize("288", 0x300000, size) && size == 288);
  assert(wspdOtaSize("0001024", 0x300000, size) && size == 1024);
  assert(wspdOtaSize("3145728", 0x300000, size) && size == 0x300000);
  uint8_t sha[32]; assert(!wspdOtaDigest(nullptr, sha));
  assert(!wspdOtaDigest("", sha) && !wspdOtaDigest(std::string(64, 'z').c_str(), sha));
  assert(wspdOtaDigest(std::string(64, 'A').c_str(), sha)); for (auto byte : sha) assert(byte == 0xAA);
  const size_t n = sizeof(WSPD_OTA_IMAGE_MARKER) - 1;
  for (size_t split = 0; split <= n; ++split) {
    WspdOtaMarker marker; const uint8_t junk[] = {0, 1, 2}; marker.append(junk, sizeof(junk));
    marker.append(reinterpret_cast<const uint8_t *>(WSPD_OTA_IMAGE_MARKER), split);
    marker.append(reinterpret_cast<const uint8_t *>(WSPD_OTA_IMAGE_MARKER) + split, n - split);
    assert(marker.found()); marker.clear(); assert(!marker.found());
  }
  WspdOtaMarker bytewise; for (size_t i = 0; i < n; ++i) bytewise.append(reinterpret_cast<const uint8_t *>(WSPD_OTA_IMAGE_MARKER) + i, 1);
  assert(bytewise.found()); cases++;
}
int main() {
  const auto good = image(); assert(digest(good) == PYTHON_SHA256); parsingAndMarker();
  for (size_t chunkSize : {size_t(1), size_t(16), size_t(137), size_t(288), size_t(301), size_t(1024)}) {
    Fixture f; f.uploadSuccess(good, chunkSize);
  }
  { Fixture f(false); f.start(good); f.write(good); f.end(); f.finish(403, "disabled"); assert(Update.begins == 0); }
  { Fixture f; f.web.authenticated = false; f.start(good);
    assert(!f.web.uploadClientConnected() && f.web.responses == 1 && Update.begins == 0);
    const unsigned authentications = f.web.authentications;
    f.chunk(good.data(), 100); f.end(); f.abort(); // Buffered/parser-abort callbacks cannot reply or write.
    assert(f.web.authentications == authentications);
    f.finish(401, "unauthorized"); assert(Update.begins == 0 && f.web.responseHeaders.count("WWW-Authenticate"));
    f.web.authenticated = true; f.web.newConnection(); f.uploadSuccess(good);
  }
  { Fixture f; f.web.requestHeaders.clear(); f.start(good); f.finish(400, "request_header"); assert(Update.begins == 0); }
  { Fixture f; f.web.requestHeaders["X-WSPD-Update"] = "0"; f.start(good); f.finish(400, "request_header"); assert(Update.begins == 0); }
  for (const char *bad : {"0", "287", "3145729", "999999999999999999999999", "-1024", "not-a-size"}) {
    Fixture f; f.start(good, bad); f.finish(413, "size"); assert(Update.begins == 0);
  }
  { Fixture f; partitionPresent = false; f.start(good); f.finish(413, "size"); assert(Update.begins == 0); }
  { Fixture f; f.web.fields["sha256"] = digest(good); f.web.incoming.status = UPLOAD_FILE_START; f.web.uploadHandler(); f.finish(413, "size"); assert(Update.begins == 0); }
  { Fixture f; f.web.fields["size"] = "1024"; f.web.incoming.status = UPLOAD_FILE_START; f.web.uploadHandler(); f.finish(400, "sha256"); assert(Update.begins == 0); }
  for (const char *bad : {"a", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFG"}) {
    Fixture f; f.start(good, "", bad); f.finish(400, "sha256"); assert(Update.begins == 0);
  }
  { Fixture f; f.web.fields["size"] = "1024"; f.web.fields["sha256"] = digest(good); f.web.incoming.name = "other"; f.web.uploadHandler(); f.finish(400, "file_field"); assert(Update.begins == 0); }
  { Fixture f; Update.beginOk = false; f.start(good); f.finish(500, "begin"); assert(Update.writes == 0); }
  { Fixture f; f.start(good); f.finish(400, "incomplete"); assert(Update.aborts == 1 && Update.ends == 0); }
  { Fixture f; f.finish(400, "incomplete"); assert(Update.begins == 0); }
  { Fixture f; f.start(good); f.end(); f.finish(400, "image"); assert(Update.ends == 0); }
  { Fixture f; f.start(good); f.chunk(good.data(), 100); assert(Update.writes == 0); f.end(); f.finish(400, "image"); }
  { Fixture f; f.start(good); f.chunk(good.data(), good.size() - 1); f.end(); f.finish(400, "image"); }
  { Fixture f; f.start(good); f.write(good); f.web.incoming.totalSize--; f.end(); f.finish(400, "image"); }
  { Fixture f; f.start(good); f.write(good); uint8_t extra = 0; f.chunk(&extra, 1); f.end(); f.finish(400, "length"); }
  { Fixture f; f.start(good); f.write(good); f.abort(); f.finish(400, "incomplete"); assert(Update.aborts == 1); }
  // Each distinct application prefix guard must reject before flash writing.
  for (const size_t offset : {size_t(0), size_t(1), size_t(12), size_t(13), size_t(32), size_t(33), size_t(34), size_t(35)}) {
    auto bad = good; bad[offset] = 0; if (offset == 13) bad[offset] = 1;
    Fixture f; f.start(bad); f.write(bad); f.end(); f.finish(400, "image"); assert(Update.writes == 0);
  }
  { auto bad = good; bad[1] = 17; Fixture f; f.start(bad); f.write(bad); f.end(); f.finish(400, "image"); assert(Update.writes == 0); }
  { auto bad = good; bad[300] ^= 1; Fixture f; f.start(bad); f.write(bad); f.end(); f.finish(400, "image"); assert(Update.ends == 0); }
  { Fixture f; f.start(good, "", std::string(64, '0')); f.write(good); f.end();
    // Core 2 must consume the closing multipart line before ending this socket.
    assert(f.web.uploadClientConnected() && f.web.responses == 0 && !maintenanceActive && !Update.running);
    f.finish(400, "checksum"); assert(Update.ends == 0);
  }
  { Fixture f; Update.shortWrite = true; f.start(good); f.write(good); f.end();
    assert(!f.web.uploadClientConnected() && !maintenanceActive && !Update.running);
    f.abort(); f.finish(500, "write"); assert(Update.ends == 0);
    Update.shortWrite = false; f.web.newConnection(); f.uploadSuccess(good);
  }
  { Fixture f; f.start(good); f.write(good); f.end(); Update.endOk = false; f.finish(500, "finalize"); assert(Update.ends == 1 && Update.aborts == 1); }
  { Fixture f; f.start(good); f.write(good); f.end(); f.start(good); f.finish(400, "multiple_files"); assert(Update.begins == 1 && Update.ends == 0); }
  { Fixture f; f.start(good); f.chunk(good.data(), 300); f.web.authenticated = false; f.chunk(good.data() + 300, 100); f.finish(401, "unauthorized"); assert(Update.bytes.size() == 300 && Update.ends == 0); }
  { Fixture f; f.start(good); f.write(good); f.web.authenticated = false; f.end(); f.finish(401, "unauthorized"); assert(Update.ends == 0); }
  { Fixture f; f.start(good); f.write(good); f.end(); f.web.authenticated = false; f.finish(401, "unauthorized"); assert(Update.ends == 0); }
  { Fixture f; f.start(good); f.write(good); f.end(); f.web.requestHeaders.clear(); f.finish(400, "request_header"); assert(Update.ends == 0); }
  for (int stage = 0; stage < 3; ++stage) {
    Fixture f; hashStartFail = stage == 0; hashUpdateFail = stage == 1; hashFinishFail = stage == 2;
    f.start(good); f.write(good); f.end(); f.finish(stage == 2 ? 400 : 500, stage == 2 ? "checksum" : "hash"); assert(Update.ends == 0);
  }
  for (uint32_t startTime : {100U, UINT32_MAX - 10000U}) {
    Fixture f; nowMs = startTime; f.start(good); f.chunk(good.data(), 300);
    f.ota.loop(startTime + 20000U); assert(f.ota.busy() && Update.running);
    f.ota.loop(startTime + 20001U); assert(!f.ota.busy() && !Update.running && !maintenanceActive && Update.aborts == 1);
    f.finish(400, "incomplete"); assert(Update.ends == 0);
    // A clean new request is accepted after timeout cleanup.
    f.uploadSuccess(good);
  }
  { Fixture f; nowMs = UINT32_MAX - 500U; f.uploadSuccess(good); }
  { Fixture f; f.start(good); f.write(good); f.end(); f.finish(202);
    const unsigned begins = Update.begins, ends = Update.ends;
    const unsigned responses = f.web.responses;
    f.start(good); assert(f.web.responses == responses + 1 && !f.web.uploadClientConnected());
    f.abort(); f.web.requestHandler(); assert(f.web.responses == responses + 1);
    assert(f.web.status == 409 && Update.begins == begins && Update.ends == ends);
    nowMs += 1500; f.ota.loop(nowMs); assert(ESP.restarts == 1); maintenanceActive = false; cases++;
  }
  { Fixture f; f.web.authenticated = false; f.start(good); f.abort(); f.finish(401, "unauthorized");
    f.web.authenticated = true; f.web.newConnection(); f.finish(400, "incomplete");
    f.uploadSuccess(good);
  }
  std::cout << cases << " OTA guard/lifecycle scenarios passed (MbedTLS API " << MBEDTLS_VERSION_MAJOR << ").\n";
}
'''

CONFIG_CHECKS = r'''
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
constexpr int LOW = 0, HIGH = 1;
#include "config_fixture.h"
constexpr size_t SPD_COUNT = sizeof(SPD_CONFIGS) / sizeof(SPD_CONFIGS[0]);
constexpr bool SPD_LOCAL_ENABLED = SPD_LOCAL_ID >= 0;
char lastSetupFailureText[160] = {};
int lastSetupFailureCode = 0;
VALIDATE_CONFIG_FUNCTION
unsigned configCases = 0;
void defaults() {
  WIFI_SSID = ""; WIFI_PASSWORD = "";
  FALLBACK_AP_SSID = "Synthetic OTA host test"; FALLBACK_AP_PASSWORD = "synthetic-test-password";
  OTA_ENABLED = false; OTA_USERNAME = "admin"; OTA_PASSWORD = "";
  MQTT_HOST = ""; MQTT_USERNAME = ""; MQTT_PASSWORD = ""; MQTT_BASE_TOPIC = "wirelessspd";
  lastSetupFailureText[0] = 0; lastSetupFailureCode = 0;
}
void check(bool expected) {
  const bool actual = validateSpdConfig();
  if (actual != expected) std::cerr << "Unexpected configuration result: " << lastSetupFailureText << "\n";
  assert(actual == expected); configCases++;
}
void enabled() { defaults(); OTA_ENABLED = true; OTA_USERNAME = "admin"; OTA_PASSWORD = "synthetic-test-password"; }
int main() {
  defaults(); check(true); // Public defaults after configuring AP credentials/key.
  defaults(); OTA_USERNAME = ""; OTA_PASSWORD = ""; check(true); // Disabled OTA accepts empty credentials.
  enabled(); check(true);
  std::string user(32, 'u'), password(64, 'p');
  enabled(); OTA_USERNAME = user.c_str(); OTA_PASSWORD = password.c_str(); check(true);
  assert(strlen(OTA_USERNAME) + strlen(OTA_PASSWORD) + 1 <= CHAR_MAX);
  std::string minimumPassword(12, 'p'); enabled(); OTA_PASSWORD = minimumPassword.c_str(); check(true);
  for (const char *invalid : {"", "short", "YOUR_PRIVATE_UPDATE_PASSWORD", "password\nwithcontrol", "password\twithcontrol", "password\x7f" "invalid", "password\xc3\xa9" "invalid", static_cast<const char *>(nullptr)}) {
    enabled(); OTA_PASSWORD = invalid; check(false);
  }
  std::string shortPassword(11, 'p'), longPassword(65, 'p');
  enabled(); OTA_PASSWORD = shortPassword.c_str(); check(false);
  enabled(); OTA_PASSWORD = longPassword.c_str(); check(false);
  for (const char *invalid : {"", "user:name", "user name", "user\tname", "user\nname", "user\x7f", "user\xc3\xa9", static_cast<const char *>(nullptr)}) {
    enabled(); OTA_USERNAME = invalid; check(false);
  }
  std::string longUser(33, 'u'); enabled(); OTA_USERNAME = longUser.c_str(); check(false);
  defaults(); OTA_PASSWORD = nullptr; check(false);
  defaults(); OTA_USERNAME = nullptr; check(false);
  std::string maximumTopic(64, 't'), longTopic(65, 't');
  defaults(); MQTT_BASE_TOPIC = maximumTopic.c_str(); check(true);
  defaults(); MQTT_BASE_TOPIC = "wireless/spd"; check(true);
  for (const char *invalid : {"", "wireless+spd", "wireless/#", static_cast<const char *>(nullptr)}) {
    defaults(); MQTT_BASE_TOPIC = invalid; check(false);
  }
  defaults(); MQTT_BASE_TOPIC = longTopic.c_str(); check(false);
  std::string maximumHost(253, 'h'), longHost(254, 'h');
  defaults(); MQTT_HOST = maximumHost.c_str(); check(true);
  defaults(); MQTT_HOST = longHost.c_str(); check(false);
  defaults(); MQTT_HOST = nullptr; check(false);
  std::string maximumMqttCredential(128, 'm'), longMqttCredential(129, 'm');
  defaults(); MQTT_USERNAME = maximumMqttCredential.c_str(); MQTT_PASSWORD = maximumMqttCredential.c_str(); check(true);
  defaults(); MQTT_USERNAME = longMqttCredential.c_str(); check(false);
  defaults(); MQTT_PASSWORD = longMqttCredential.c_str(); check(false);
  defaults(); MQTT_USERNAME = nullptr; check(false);
  defaults(); MQTT_PASSWORD = nullptr; check(false);
  std::cout << configCases << " production credential/configuration boundary scenarios passed.\n";
}
'''


def validation_function(source):
    """Extract the production runtime validator without importing deployment config."""
    match = re.search(r"static bool validateSpdConfig\(\)\s*\{", source)
    if not match:
        raise SystemExit("Cannot find the production configuration validator.")
    at, depth = match.end(), 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[match.start():at]


def openssl_flags():
    pkg_config = shutil.which("pkg-config")
    if pkg_config:
        result = subprocess.run([pkg_config, "--cflags", "--libs", "openssl"],
                                text=True, capture_output=True)
        if result.returncode == 0:
            return shlex.split(result.stdout)
    if Path("/usr/include/openssl/evp.h").exists():
        return ["-lcrypto"]
    brew = Path("/opt/homebrew/opt/openssl@3")
    if (brew / "include/openssl/evp.h").exists():
        return ["-I" + str(brew / "include"), "-L" + str(brew / "lib"), "-lcrypto"]
    raise SystemExit("OpenSSL development headers are required for the OTA host tests.")


def main():
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    crypto_flags = openssl_flags()
    with tempfile.TemporaryDirectory(prefix="wspd-ota-check-") as temp:
        directory = Path(temp)
        (directory / "mbedtls").mkdir()
        for header in ["WebServer.h", "Update.h", "esp_ota_ops.h", "mbedtls/sha256.h", "mbedtls/version.h"]:
            (directory / header).write_text('#pragma once\n#include "host_shims.h"\n')
        (directory / "host_shims.h").write_text(SHIMS)
        source = directory / "checks.cpp"
        source.write_text('#define PYTHON_SHA256 "' + hashlib.sha256(IMAGE).hexdigest() + '"\n' + CHECKS)
        for api in [2, 3]:
            binary = directory / f"check-api-{api}"
            subprocess.run([compiler, "-std=c++11", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            f"-DMBEDTLS_VERSION_MAJOR={api}", "-I", str(directory),
                            "-I", str(SERVER), str(source), "-o", str(binary),
                            *crypto_flags], check=True)
            subprocess.run([str(binary)], check=True)
        # Keep public settings as the fixture; only the runtime OTA enabled switch
        # is mutable so the same production validator can cover both modes.
        fixture = (SERVER / "config.example.h").read_text()
        fixture = fixture.replace("constexpr auto CONFIGURED = false;", "constexpr auto CONFIGURED = true;")
        fixture = fixture.replace("constexpr auto OTA_ENABLED = false;", "bool OTA_ENABLED = false;")
        fixture = fixture.replace("0x00000000UL", "0x7C9E4A21UL")
        (directory / "config_fixture.h").write_text(fixture)
        validator = validation_function((SERVER / "tconnectpro_868.ino").read_text())
        config_source = directory / "config_checks.cpp"
        config_source.write_text(CONFIG_CHECKS.replace("VALIDATE_CONFIG_FUNCTION", validator))
        config_binary = directory / "check-config"
        subprocess.run([compiler, "-std=c++11", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I", str(directory), "-I", str(SERVER), str(config_source),
                        "-o", str(config_binary)], check=True)
        subprocess.run([str(config_binary)], check=True)


if __name__ == "__main__":
    main()
