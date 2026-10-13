#pragma once
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include "ota_image.h"

class WspdWebServer : public WebServer {
public:
  using WebServer::WebServer;
  // Core 2 returns a copy from client(); stop() on that copy does not close
  // the original shared connection. Stop the parser's client in both cores.
  void closeCurrentUpload() { _currentClient.stop(); }
  bool uploadClientConnected() { return _currentClient.connected(); }
};

class WspdWebOta {
public:
  using Maintenance = void (*)(bool);
  WspdWebOta(WspdWebServer &server, bool enabled, const char *user, const char *password, Maintenance maintenance)
      : server_(server), enabled_(enabled), user_(user), password_(password), maintenance_(maintenance) {}
  bool enabled() const { return enabled_; }
  bool busy() const { return active_ || restartPending_; }
  size_t slotSize() const {
    const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->size : 0;
  }
  void setup() {
    // Upload callbacks run before the route's final handler in Arduino WebServer.
    server_.on("/api/v1/firmware", HTTP_POST, [this]() { finishRequest(); }, [this]() { upload(); });
  }
  void loop(uint32_t now) {
    if (active_ && uint32_t(now - lastChunk_) > 20000UL) { fail(400, "timeout", "Upload timed out."); clear(); }
    if (restartPending_ && uint32_t(now - restartStarted_) >= 1500UL) ESP.restart();
  }
private:
  WspdWebServer &server_;
  bool enabled_;
  const char *user_, *password_;
  Maintenance maintenance_;
  bool active_ = false, hadFile_ = false, complete_ = false, restartPending_ = false, hashOpen_ = false;
  bool rejectedConnection_ = false;
  size_t expected_ = 0, received_ = 0, prefixUsed_ = 0;
  uint32_t lastChunk_ = 0, restartStarted_ = 0;
  uint8_t prefix_[WSPD_OTA_PREFIX_SIZE], wanted_[32];
  mbedtls_sha256_context hash_;
  WspdOtaMarker marker_;
  int errorCode_ = 0;
  const char *error_ = "", *message_ = "";
  bool authorized() { return server_.authenticate(user_, password_); }
  bool requestAllowed() {
    if (!enabled_) { fail(403, "disabled", "Network firmware updates are disabled."); return false; }
    if (!authorized()) { fail(401, "unauthorized", "Firmware update credentials are required."); return false; }
    if (server_.header("X-WSPD-Update") != "1") { fail(400, "request_header", "X-WSPD-Update: 1 is required."); return false; }
    return true;
  }
  void closeHash() { if (hashOpen_) { mbedtls_sha256_free(&hash_); hashOpen_ = false; } }
  void fail(int code, const char *error, const char *message) {
    if (!errorCode_) { errorCode_ = code; error_ = error; message_ = message; }
    if (active_) { Update.abort(); active_ = false; maintenance_(false); }
    closeHash(); complete_ = false;
  }
  void clear() {
    closeHash(); hadFile_ = complete_ = false;
    errorCode_ = 0; expected_ = received_ = prefixUsed_ = 0; marker_.clear();
  }
  void reply(int code, const String &body) {
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(code, "application/json; charset=utf-8", body);
  }
  void errorReply() {
    if (errorCode_ == 401) server_.sendHeader("WWW-Authenticate", "Basic realm=\"WirelessSPDSystem firmware\"");
    reply(errorCode_, String("{\"error\":\"") + error_ + "\",\"message\":\"" + message_ + "\"}");
  }
  void rejectUpload() {
    if (!rejectedConnection_) {
      server_.sendHeader("Connection", "close");
      errorReply();
      rejectedConnection_ = true;
    }
    // Returning from an upload callback alone still drains the full body in
    // handleClient(), freezing the sensing/alarm loop. End rejected requests.
    server_.closeCurrentUpload();
    clear();
  }
  void upload() {
    HTTPUpload &u = server_.upload();
    if (rejectedConnection_) {
      // Both parsers issue ABORTED after the close and skip the final handler.
      // Ignore any buffered callbacks, then reset only for a new live request.
      if (u.status != UPLOAD_FILE_START || !server_.uploadClientConnected()) return;
      rejectedConnection_ = false;
      clear();
    }
    if (restartPending_) {
      if (u.status != UPLOAD_FILE_ABORTED) {
        fail(409, "busy", "Receiver is restarting."); rejectUpload();
      }
      return;
    }
    if (u.status == UPLOAD_FILE_START) {
      if (hadFile_) { fail(400, "multiple_files", "Select exactly one application image."); rejectUpload(); return; }
      hadFile_ = true;
      if (!requestAllowed()) { rejectUpload(); return; }
      if (u.name != "firmware") { fail(400, "file_field", "The firmware file field is required."); rejectUpload(); return; }
      if (!server_.hasArg("size") || !wspdOtaSize(server_.arg("size").c_str(), slotSize(), expected_)) {
        fail(413, "size", "Declare a valid application size within the OTA slot before the file."); rejectUpload(); return;
      }
      if (!server_.hasArg("sha256") || !wspdOtaDigest(server_.arg("sha256").c_str(), wanted_)) {
        fail(400, "sha256", "Provide the 64-character SHA-256 before the file."); rejectUpload(); return;
      }
      if (!Update.begin(expected_, U_FLASH)) { fail(500, "begin", "Could not open the inactive OTA slot."); rejectUpload(); return; }
      active_ = true; maintenance_(true); lastChunk_ = millis();
      mbedtls_sha256_init(&hash_); hashOpen_ = true;
#if MBEDTLS_VERSION_MAJOR >= 3
      if (mbedtls_sha256_starts(&hash_, 0) != 0) fail(500, "hash", "SHA-256 initialization failed.");
#else
      if (mbedtls_sha256_starts_ret(&hash_, 0) != 0) fail(500, "hash", "SHA-256 initialization failed.");
#endif
      if (errorCode_) rejectUpload();
      return;
    }
    if (u.status == UPLOAD_FILE_ABORTED) { fail(400, "aborted", "Upload cancelled."); clear(); return; }
    if (errorCode_ || !active_) return;
    if (!requestAllowed()) { rejectUpload(); return; }
    lastChunk_ = millis();
    if (u.status == UPLOAD_FILE_WRITE) {
      if (u.currentSize > expected_ - received_) { fail(400, "length", "Image exceeds its declared size."); rejectUpload(); return; }
#if MBEDTLS_VERSION_MAJOR >= 3
      const int hashResult = mbedtls_sha256_update(&hash_, u.buf, u.currentSize);
#else
      const int hashResult = mbedtls_sha256_update_ret(&hash_, u.buf, u.currentSize);
#endif
      if (hashResult) { fail(500, "hash", "SHA-256 processing failed."); rejectUpload(); return; }
      marker_.append(u.buf, u.currentSize); received_ += u.currentSize;
      size_t offset = 0;
      if (prefixUsed_ < sizeof(prefix_)) {
        const size_t n = min(u.currentSize, sizeof(prefix_) - prefixUsed_);
        memcpy(prefix_ + prefixUsed_, u.buf, n); prefixUsed_ += n; offset = n;
        if (prefixUsed_ < sizeof(prefix_)) return;
        if (!wspdOtaPrefix(prefix_, prefixUsed_)) { fail(400, "image", "Select an ESP32-S3 application .bin, not a merged or bootloader image."); rejectUpload(); return; }
        if (Update.write(prefix_, sizeof(prefix_)) != sizeof(prefix_)) { fail(500, "write", "Flash write failed."); rejectUpload(); return; }
      }
      if (offset < u.currentSize && Update.write(u.buf + offset, u.currentSize - offset) != u.currentSize - offset)
        fail(500, "write", "Flash write failed.");
      if (errorCode_) rejectUpload();
    } else if (u.status == UPLOAD_FILE_END) {
      // The file body is finished. Core 2's parser does not check connectivity
      // before reading the closing multipart line, so closing here can leave
      // its empty-field loop stuck. Abort flashing now; reply in final handler.
      if (received_ != expected_ || u.totalSize != expected_ || prefixUsed_ < sizeof(prefix_) || !marker_.found()) {
        fail(400, "image", "Incomplete image or wrong receiver firmware type."); return;
      }
      uint8_t actual[32] = {0};
#if MBEDTLS_VERSION_MAJOR >= 3
      const int result = mbedtls_sha256_finish(&hash_, actual);
#else
      const int result = mbedtls_sha256_finish_ret(&hash_, actual);
#endif
      closeHash(); uint8_t difference = 0;
      for (size_t i = 0; i < sizeof(actual); ++i) difference |= actual[i] ^ wanted_[i];
      if (result || difference) { fail(400, "checksum", "SHA-256 does not match the selected image."); return; }
      complete_ = true; // Activation waits for the complete multipart request.
    }
  }
  void finishRequest() {
    if (rejectedConnection_) {
      if (!server_.uploadClientConnected()) return; // The early error was already sent.
      rejectedConnection_ = false; clear(); // A later request without a file.
    }
    if (restartPending_) { reply(409, "{\"error\":\"busy\",\"message\":\"Receiver is restarting.\"}"); return; }
    if (!requestAllowed()) {} // Includes fresh authentication after all body data.
    else if (!hadFile_ || !complete_ || !active_) fail(400, "incomplete", "A complete application image is required.");
    if (!errorCode_ && !Update.end(false)) fail(500, "finalize", "Image validation or activation failed; current firmware remains selected.");
    if (errorCode_) {
      errorReply(); clear(); return;
    }
    active_ = false; restartPending_ = true; restartStarted_ = millis();
    reply(202, "{\"status\":\"ok\",\"message\":\"Firmware verified; restarting.\"}");
  }
};
