#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <new>

// Producer callbacks run on the receiver loop, never on the network task.
// Names/status strings may point at configuration or static text: they are copied.
struct WspdMqttSpdSnapshot {
  uint8_t id = 0;
  const char *name = "SPD";
  bool local = false, seen = false, fresh = false, batteryValid = false;
  const char *status = "UNKNOWN";
  float temperatureC = 0, batteryV = 0, rssiDbm = 0, snrDb = 0, lossPercent = 0;
  uint32_t ageSeconds = 0;
};

struct WspdMqttGatewaySnapshot {
  uint16_t configured = 0, failed = 0, stale = 0, unknown = 0;
  uint32_t uptimeSeconds = 0;
  bool alarmMuted = false;
};

// MQTT owns a separate low-priority task. A failed DNS lookup, TCP connection,
// broker handshake or write cannot stall radio reception or the alarm loop.
// The receiver supplies one latest snapshot per loop; no telemetry backlog grows.
class WspdMqttBridge {
 public:
  using RowCallback = void (*)(size_t, WspdMqttSpdSnapshot &);
  using GatewayCallback = void (*)(WspdMqttGatewaySnapshot &);
  using MuteCallback = void (*)(bool);

  WspdMqttBridge(const char *host, uint16_t port, const char *username,
                 const char *password, const char *baseTopic, uint32_t publishSeconds, MuteCallback muteCallback = nullptr)
      : host_(host), port_(port), username_(username), password_(password),
        baseTopic_(baseTopic), muteCallback_(muteCallback), publishMs_(publishSeconds * 1000UL), mqtt_(transport_) {}

  bool begin(const char *gatewayId, const char *firmwareId, const char *ip,
             size_t rowCount, RowCallback rowCallback, GatewayCallback gatewayCallback) {
    if (!enabled()) return true;
    if (!gatewayId || !firmwareId || !ip || !rowCallback || !gatewayCallback ||
        rowCount > 128 || strlen(gatewayId) > 32 || !baseTopic_ || !baseTopic_[0] ||
        strlen(baseTopic_) > 64 || task_) return false;
    gatewayId_ = gatewayId;
    firmwareId_ = firmwareId;
    dashboardUrl_ = String("http://") + ip + "/";
    boundedText(pendingAddress_, sizeof(pendingAddress_), ip);
    topicRoot_ = String(baseTopic_) + "/" + gatewayId_;
    availabilityTopic_ = topicRoot_ + "/availability";
    commandTopic_ = topicRoot_ + "/command/alarm_muted";
    rowCount_ = rowCount;
    rowCallback_ = rowCallback;
    gatewayCallback_ = gatewayCallback;
    rows_ = new (std::nothrow) CachedRow[rowCount_ ? rowCount_ : 1];
    if (!rows_ || !mqtt_.setBufferSize(MQTT_BUFFER_BYTES)) {
      delete[] rows_;
      rows_ = nullptr;
      return false;
    }
    mqtt_.setServer(host_, port_);
    mqtt_.setSocketTimeout(1); // CONNACK/read wait, seconds; reconnects back off.
    mqtt_.setKeepAlive(15);
    mqtt_.setCallback([this](char *topic, uint8_t *payload, unsigned int length) {
      if (!strcmp(topic, "homeassistant/status") && length == 6 &&
          !memcmp(payload, "online", 6)) discoveryRequested_ = true;
      if (muteCallback_ && !strcmp(topic, commandTopic_.c_str())) {
        const bool on = length == 2 && !memcmp(payload, "ON", 2);
        const bool off = length == 3 && !memcmp(payload, "OFF", 3);
        if (!on && !off) return;
        // Network-task callbacks only queue an explicit state. The sketch loop
        // owns alarm state, GPIO, display and log writes.
        portENTER_CRITICAL(&lock_);
        if (connected_ && networkReady_ && !paused_) { pendingMute_ = on; mutePending_ = true; }
        portEXIT_CRITICAL(&lock_);
      }
    });
    for (size_t i = 0; i < rowCount_; ++i) copyRow(i);
    copyGateway();
    if (xTaskCreate(taskEntry, "wspd-mqtt", 8192, this, 1, &task_) != pdPASS) {
      delete[] rows_;
      rows_ = nullptr;
      return false;
    }
    return true;
  }

  bool enabled() const { return host_ && host_[0]; }
  bool connected() {
    portENTER_CRITICAL(&lock_);
    const bool value = connected_;
    portEXIT_CRITICAL(&lock_);
    return value;
  }
  void loop(uint32_t, bool stationConnected) {
    if (!task_) return;
    bool apply = false, muted = false;
    portENTER_CRITICAL(&lock_);
    if (!stationConnected || paused_ || !connected_) mutePending_ = false;
    else if (mutePending_) { muted = pendingMute_; apply = true; mutePending_ = false; }
    portEXIT_CRITICAL(&lock_);
    if (apply && muteCallback_) muteCallback_(muted);
    // Snapshot exactly one row per call to keep the receiver's work bounded.
    if (rowCount_) {
      copyRow(nextSnapshot_);
      nextSnapshot_ = (nextSnapshot_ + 1) % rowCount_;
    }
    copyGateway();
    portENTER_CRITICAL(&lock_);
    networkReady_ = stationConnected;
    portEXIT_CRITICAL(&lock_);
  }
  void requestPublish() {
    portENTER_CRITICAL(&lock_);
    publishRequested_ = true;
    portEXIT_CRITICAL(&lock_);
  }
  // Main-loop producer; the worker owns String allocation and discovery.
  void setNetworkAddress(const char *ip) {
    if (!task_ || !ip || !ip[0] || strlen(ip) >= sizeof(pendingAddress_)) return;
    portENTER_CRITICAL(&lock_);
    if (strcmp(pendingAddress_, ip)) {
      strcpy(pendingAddress_, ip);
      addressPending_ = true;
    }
    portEXIT_CRITICAL(&lock_);
  }
  // Called only by the sketch after web/touch/MQTT changes. Copy the latest
  // mute state before waking publication, so acknowledgement is authoritative.
  void notifyMuteStateChanged() {
    if (!task_) return;
    copyGateway();
    portENTER_CRITICAL(&lock_); muteStatePending_ = true; portEXIT_CRITICAL(&lock_);
  }
  void setPaused(bool paused) {
    portENTER_CRITICAL(&lock_);
    paused_ = paused;
    if (paused) mutePending_ = false;
    portEXIT_CRITICAL(&lock_);
  }

#ifdef WSPD_MQTT_HOST_TEST
  // Drive the actual network state machine with a fake broker in host tests.
  void testNetworkStep(uint32_t now) { workerStep(now); }
#endif

 private:
  // PubSubClient invokes the two-argument connect(). Bound TCP establishment;
  // hostname resolution, if needed, also stays entirely on the MQTT worker.
  class Transport : public WiFiClient {
   public:
    int connect(IPAddress ip, uint16_t port) override {
      return WiFiClient::connect(ip, port, 1000);
    }
    int connect(const char *host, uint16_t port) override {
      return WiFiClient::connect(host, port, 1000);
    }
  };
  struct CachedRow {
    WspdMqttSpdSnapshot value;
    char name[97] = {0};
    char status[8] = {0};
  };
  enum Phase { Idle, Commands, Offline, Discovery, States, Online };
  static constexpr uint16_t MQTT_BUFFER_BYTES = 6144;
  static constexpr uint32_t RECONNECT_MS = 30000UL;
  const char *host_, *username_, *password_, *baseTopic_;
  MuteCallback muteCallback_;
  uint16_t port_;
  uint32_t publishMs_;
  Transport transport_;
  PubSubClient mqtt_;
  TaskHandle_t task_ = nullptr;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  bool connected_ = false, networkReady_ = false, paused_ = false;
  bool publishRequested_ = false, muteStatePending_ = false;
  bool mutePending_ = false, pendingMute_ = false;
  bool addressPending_ = false;
  char pendingAddress_[16] = {0};
  bool discoveryRequested_ = false; // Used only by the MQTT task and its callback.
  bool attempted_ = false;
  uint32_t lastAttemptMs_ = 0, lastCycleMs_ = 0;
  Phase phase_ = Idle;
  size_t cursor_ = 0, rowCount_ = 0, nextSnapshot_ = 0;
  bool initialCycle_ = false;
  CachedRow *rows_ = nullptr;
  WspdMqttGatewaySnapshot gateway_;
  RowCallback rowCallback_ = nullptr;
  GatewayCallback gatewayCallback_ = nullptr;
  String gatewayId_, firmwareId_, dashboardUrl_, topicRoot_, availabilityTopic_, commandTopic_;

  static void boundedText(char *dst, size_t capacity, const char *src) {
    if (!src) src = "";
    size_t length = strlen(src);
    if (length >= capacity) {
      length = capacity - 1;
      // Keep a complete UTF-8 prefix when clipping a long friendly name.
      while (length && (uint8_t(src[length]) & 0xC0) == 0x80) --length;
    }
    memcpy(dst, src, length);
    dst[length] = 0;
  }
  void copyRow(size_t index) {
    CachedRow row;
    rowCallback_(index, row.value);
    boundedText(row.name, sizeof(row.name), row.value.name);
    boundedText(row.status, sizeof(row.status), row.value.status);
    row.value.name = nullptr;
    row.value.status = nullptr;
    portENTER_CRITICAL(&lock_);
    rows_[index] = row;
    portEXIT_CRITICAL(&lock_);
  }
  void copyGateway() {
    WspdMqttGatewaySnapshot value;
    gatewayCallback_(value);
    portENTER_CRITICAL(&lock_);
    gateway_ = value;
    portEXIT_CRITICAL(&lock_);
  }
  CachedRow rowAt(size_t index) {
    portENTER_CRITICAL(&lock_);
    const CachedRow row = rows_[index];
    portEXIT_CRITICAL(&lock_);
    return row;
  }
  WspdMqttGatewaySnapshot gatewaySnapshot() {
    portENTER_CRITICAL(&lock_);
    const WspdMqttGatewaySnapshot value = gateway_;
    portEXIT_CRITICAL(&lock_);
    return value;
  }
  void markConnected(bool value) {
    portENTER_CRITICAL(&lock_);
    connected_ = value;
    if (!value) mutePending_ = false;
    portEXIT_CRITICAL(&lock_);
  }
  static void taskEntry(void *arg) {
    auto *self = static_cast<WspdMqttBridge *>(arg);
    for (;;) {
      self->workerStep(millis());
      vTaskDelay(pdMS_TO_TICKS(25));
    }
  }
  bool publish(const String &topic, const String &payload) {
    // PubSubClient's fixed buffer includes MQTT header and the topic itself.
    if (payload.length() + topic.length() + 8 >= MQTT_BUFFER_BYTES) return false;
    return mqtt_.publish(topic.c_str(), payload.c_str(), true);
  }
  void workerStep(uint32_t now) {
    bool ready, paused;
    char address[16] = {0};
    portENTER_CRITICAL(&lock_);
    ready = networkReady_;
    paused = paused_;
    if (addressPending_) {
      memcpy(address, pendingAddress_, sizeof(address));
      addressPending_ = false;
    }
    portEXIT_CRITICAL(&lock_);
    if (address[0]) {
      dashboardUrl_ = String("http://") + address + "/";
      discoveryRequested_ = true;
    }
    if (!ready || paused) {
      if (mqtt_.connected()) {
        // Best effort graceful availability update; LWT covers an unclean stop.
        if (publish(availabilityTopic_, "offline")) mqtt_.disconnect();
        else transport_.stop(); // Preserve the broker's LWT if publishing failed.
      }
      markConnected(false);
      phase_ = Idle;
      return;
    }
    if (!mqtt_.connected()) {
      markConnected(false);
      if (attempted_ && uint32_t(now - lastAttemptMs_) < RECONNECT_MS) return;
      attempted_ = true;
      lastAttemptMs_ = now;
      const String clientId = "wspd-" + gatewayId_;
      if (!mqtt_.connect(clientId.c_str(), username_, password_,
                         availabilityTopic_.c_str(), 0, true, "offline")) return;
      markConnected(true);
      phase_ = Commands;
      initialCycle_ = true;
      cursor_ = 0;
    }
    if (!mqtt_.loop()) { markConnected(false); return; }
    if (phase_ == Idle && discoveryRequested_) {
      discoveryRequested_ = false;
      phase_ = Discovery;
      cursor_ = 0;
    }
    bool requested = false, muteRequested = false;
    portENTER_CRITICAL(&lock_);
    if (phase_ == Idle) { requested = publishRequested_; publishRequested_ = false; }
    if (phase_ == Idle || phase_ == States || phase_ == Online) {
      muteRequested = muteStatePending_; muteStatePending_ = false;
    }
    portEXIT_CRITICAL(&lock_);
    if (phase_ == Idle && (requested || uint32_t(now - lastCycleMs_) >= publishMs_)) {
      phase_ = States; cursor_ = 0;
    }
    // At most one discovery/state/availability publication per task iteration.
    bool success = true;
    if (muteRequested) {
      // Acknowledge controls promptly even halfway through a large row cycle.
      // Keep that row cursor and publish at most one message this iteration.
      success = publishGatewayState();
    } else if (phase_ == Commands) {
      // Remove obsolete retained commands BEFORE subscribing. HA commands use
      // retain:false; stale mute messages must not replay after reconnect.
      success = publish(commandTopic_, "");
      if (success) success = mqtt_.subscribe("homeassistant/status");
      if (success && muteCallback_) success = mqtt_.subscribe(commandTopic_.c_str());
      if (success) phase_ = Offline;
    } else if (phase_ == Offline) {
      success = publish(availabilityTopic_, "offline");
      if (success) phase_ = Discovery;
    } else if (phase_ == Discovery) {
      success = cursor_ == 0 ? publishGatewayDiscovery() : publishRowDiscovery(cursor_ - 1);
      if (success && ++cursor_ > rowCount_) { phase_ = States; cursor_ = 0; }
    } else if (phase_ == States) {
      success = cursor_ == 0 ? publishGatewayState() : publishRowState(cursor_ - 1);
      if (success && ++cursor_ > rowCount_) {
        lastCycleMs_ = now;
        phase_ = initialCycle_ ? Online : Idle;
      }
    } else if (phase_ == Online) {
      success = publish(availabilityTopic_, "online");
      if (success) { initialCycle_ = false; phase_ = Idle; }
    }
    if (!success) {
      // A failed/oversized publish must not create a fast retry loop.
      // Close without MQTT DISCONNECT so the broker publishes our offline LWT.
      // A graceful disconnect would suppress the will and leave retained online.
      transport_.stop();
      markConnected(false);
      lastAttemptMs_ = now;
    }
  }

  static String quoted(const char *text) {
    String result = "\"";
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(text); *p; ++p) {
      if (*p == '\\' || *p == '"') { result += '\\'; result += char(*p); }
      else if (*p < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", *p); result += escaped; }
      else result += char(*p);
    }
    result += '"';
    return result;
  }
  String rowId(uint8_t id) const { return gatewayId_ + "_spd_" + String(id); }
  String rowTopic(uint8_t id) const { return topicRoot_ + "/spd/" + String(id) + "/state"; }
  String availability(const String &stateTopic, const char *condition = nullptr) const {
    String value = "[ {\"topic\":" + quoted(availabilityTopic_.c_str()) + "}";
    if (condition) value += ",{\"topic\":" + quoted(stateTopic.c_str()) +
        ",\"value_template\":\"{{ 'online' if " + condition + " else 'offline' }}\"}";
    return value + "]";
  }
  uint32_t expiresAfter() const { return publishMs_ / 1000UL * 3UL + 30UL; }
  String sensor(const String &id, const char *name, const char *field,
                const String &topic, const char *condition = nullptr,
                const char *unit = nullptr, const char *deviceClass = nullptr,
                bool diagnostic = false, bool binary = false) const {
    String out = "{\"platform\":" + quoted(binary ? "binary_sensor" : "sensor") +
        ",\"unique_id\":" + quoted(id.c_str()) + ",\"name\":" + quoted(name) +
        ",\"value_template\":\"{{ value_json." + field + " }}\"," +
        "\"availability\":" + availability(topic, condition) + ",\"availability_mode\":\"all\"," +
        "\"expire_after\":" + String(expiresAfter());
    if (binary) out += ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"";
    if (unit) out += ",\"unit_of_measurement\":" + quoted(unit) + ",\"state_class\":\"measurement\"";
    if (deviceClass) out += ",\"device_class\":" + quoted(deviceClass);
    if (diagnostic) out += ",\"entity_category\":\"diagnostic\"";
    return out + "}";
  }
  String discoveryPrefix(const String &deviceId, const char *name, const char *model,
                         const String &stateTopic, bool gateway) const {
    String out = "{\"device\":{\"identifiers\":[" + quoted(deviceId.c_str()) +
        "],\"name\":" + quoted(name) + ",\"manufacturer\":\"WirelessSPDSystem\",\"model\":" + quoted(model);
    if (gateway) out += ",\"sw_version\":" + quoted(firmwareId_.c_str()) +
        ",\"configuration_url\":" + quoted(dashboardUrl_.c_str());
    else out += ",\"via_device\":" + quoted(gatewayId_.c_str());
    out += "},\"origin\":{\"name\":\"WirelessSPDSystem\",\"sw_version\":" + quoted(firmwareId_.c_str()) +
        ",\"support_url\":\"https://github.com/djeZo888/WirelessSPDSystem\"},\"state_topic\":" +
        quoted(stateTopic.c_str()) + ",\"components\":{";
    return out;
  }
  bool publishRowDiscovery(size_t index) {
    const CachedRow row = rowAt(index);
    const String id = rowId(row.value.id), topic = rowTopic(row.value.id);
    String out = discoveryPrefix(id, row.name, row.value.local ? "Local SPD dry contact" : "LoRa SPD beacon", topic, false);
    out += "\"status\":" + sensor(id + "_status", "Status", "status", topic);
    out += ",\"problem\":" + sensor(id + "_problem", "Fault", "problem", topic,
                                      "value_json.fresh", nullptr, "problem", false, true);
    if (!row.value.local) {
      out += ",\"temperature\":" + sensor(id + "_temperature", "Temperature", "temperature_c", topic,
                                          "value_json.fresh and value_json.temperature_c is not none", "°C", "temperature");
      out += ",\"battery\":" + sensor(id + "_battery", "Battery voltage", "battery_voltage_v", topic,
                                      "value_json.fresh and value_json.battery_voltage_v is not none", "V", "voltage", true);
      out += ",\"rssi\":" + sensor(id + "_rssi", "RSSI", "rssi_dbm", topic,
                                   "value_json.fresh and value_json.rssi_dbm is not none", "dBm", "signal_strength", true);
      out += ",\"snr\":" + sensor(id + "_snr", "SNR", "snr_db", topic,
                                  "value_json.fresh and value_json.snr_db is not none", "dB", nullptr, true);
      out += ",\"age\":" + sensor(id + "_age", "Report age", "age_seconds", topic,
                                  "value_json.seen", "s", "duration", true);
      out += ",\"loss\":" + sensor(id + "_loss", "Packet loss", "packet_loss_percent", topic,
                                   "value_json.seen and value_json.packet_loss_percent is not none", "%", nullptr, true);
    }
    return publish("homeassistant/device/" + id + "/config", out + "}}");
  }
  bool publishGatewayDiscovery() {
    const String topic = topicRoot_ + "/state";
    String out = discoveryPrefix(gatewayId_, "Wireless SPD receiver", "LILYGO T Connect Pro 868 MHz", topic, true);
    out += "\"configured\":" + sensor(gatewayId_ + "_configured", "Configured SPDs", "configured", topic, nullptr, nullptr, nullptr, true);
    out += ",\"failed\":" + sensor(gatewayId_ + "_failed", "Faulty SPDs", "failed", topic);
    out += ",\"stale\":" + sensor(gatewayId_ + "_stale", "Stale SPDs", "stale", topic, nullptr, nullptr, nullptr, true);
    out += ",\"unknown\":" + sensor(gatewayId_ + "_unknown", "Unknown SPDs", "unknown", topic, nullptr, nullptr, nullptr, true);
    out += ",\"uptime\":" + sensor(gatewayId_ + "_uptime", "Uptime", "uptime_seconds", topic, nullptr, "s", "duration", true);
    out += ",\"muted\":" + sensor(gatewayId_ + "_muted", "Alarm muted", "alarm_muted", topic, nullptr, nullptr, nullptr, true, true);
    if (muteCallback_) {
      out += ",\"alarm_mute\":{\"platform\":\"switch\",\"name\":\"Alarm muted\",\"unique_id\":" + quoted((gatewayId_ + "_alarm_mute").c_str());
      out += ",\"command_topic\":" + quoted(commandTopic_.c_str()) + ",\"state_topic\":" + quoted(topic.c_str());
      out += ",\"value_template\":\"{{ value_json.alarm_muted }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"";
      out += ",\"retain\":false,\"optimistic\":false,\"qos\":0,\"icon\":\"mdi:volume-off\",\"availability_mode\":\"all\",\"availability\":" + availability(topic) + "}";
    }
    return publish("homeassistant/device/" + gatewayId_ + "/config", out + "}}");
  }
  bool publishRowState(size_t index) {
    const CachedRow row = rowAt(index);
    const WspdMqttSpdSnapshot &s = row.value;
    const bool fresh = s.seen && s.fresh &&
        (!strcmp(row.status, "OK") || !strcmp(row.status, "FAIL"));
    String out = "{\"spd_id\":" + String(s.id) + ",\"status\":" + quoted(row.status) +
        ",\"local\":" + (s.local ? "true" : "false") + ",\"seen\":" + (s.seen ? "true" : "false") +
        ",\"fresh\":" + (fresh ? "true" : "false") + ",\"problem\":";
    out += fresh ? quoted(!strcmp(row.status, "FAIL") ? "ON" : "OFF") : String("null");
    out += ",\"temperature_c\":" + (s.seen && !s.local ? numberOrNull(s.temperatureC, 1) : String("null"));
    out += ",\"battery_voltage_v\":" + (s.seen && !s.local && s.batteryValid ? numberOrNull(s.batteryV, 2) : String("null"));
    out += ",\"rssi_dbm\":" + (s.seen && !s.local ? numberOrNull(s.rssiDbm, 1) : String("null"));
    out += ",\"snr_db\":" + (s.seen && !s.local ? numberOrNull(s.snrDb, 1) : String("null"));
    out += ",\"age_seconds\":" + (s.seen && !s.local ? String(s.ageSeconds) : String("null"));
    out += ",\"packet_loss_percent\":" + (s.seen && !s.local ? numberOrNull(s.lossPercent, 3) : String("null"));
    return publish(rowTopic(s.id), out + "}");
  }
  static String numberOrNull(float value, uint8_t decimals) {
    return isfinite(value) ? String(value, static_cast<unsigned int>(decimals)) : String("null");
  }
  bool publishGatewayState() {
    const WspdMqttGatewaySnapshot s = gatewaySnapshot();
    String out = "{\"configured\":" + String(s.configured) + ",\"failed\":" + String(s.failed) +
        ",\"stale\":" + String(s.stale) + ",\"unknown\":" + String(s.unknown) +
        ",\"uptime_seconds\":" + String(s.uptimeSeconds) + ",\"alarm_muted\":\"" +
        (s.alarmMuted ? "ON" : "OFF") + "\",\"firmware_id\":" + quoted(firmwareId_.c_str());
    return publish(topicRoot_ + "/state", out + "}");
  }
};
