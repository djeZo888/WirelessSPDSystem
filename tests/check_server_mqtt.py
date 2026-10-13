#!/usr/bin/env python3
"""Run the production MQTT worker against host transport/task shims."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"

ARDUINO = r'''
#pragma once
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <cstdint>
#include <iostream>
#include <vector>
struct String : std::string {
  String() = default;
  String(const char *s) : std::string(s) {}
  String(const std::string &s) : std::string(s) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value,int>::type = 0>
  String(T v) : std::string(std::to_string(v)) {}
  String(double value, int decimals = 2) {
    char out[80]; snprintf(out,sizeof(out),"%.*f",decimals,value); assign(out);
  }
};
String operator+(const String &a,const String &b) { return String(std::string(a)+std::string(b)); }
String operator+(const String &a,const char *b) { return String(std::string(a)+b); }
String operator+(const char *a,const String &b) { return String(a+std::string(b)); }
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
'''

WIFI = r'''
#pragma once
struct IPAddress {};
class WiFiClient {
 public:
  bool alive=false;
  virtual int connect(IPAddress,uint16_t) { return 1; }
  virtual int connect(const char *,uint16_t) { return 1; }
  int connect(IPAddress,uint16_t,int timeout) { assert(timeout==1000); alive=true; return 1; }
  int connect(const char *,uint16_t,int timeout) { assert(timeout==1000); alive=true; return 1; }
  void stop() { alive=false; }
};
'''

TASKS = r'''
#pragma once
using TaskHandle_t = void *;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do { (void)(x); } while (0)
#define portEXIT_CRITICAL(x) do { (void)(x); } while (0)
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
unsigned taskCount = 0;
int xTaskCreate(void(*)(void *),const char *,int bytes,void *,int priority,TaskHandle_t *out) {
  assert(bytes==8192 && priority==1); *out=reinterpret_cast<void *>(1); ++taskCount; return pdPASS;
}
void vTaskDelay(uint32_t) {}
'''

MQTT = r'''
#pragma once
#include <functional>
struct Publication { std::string topic,payload; bool retain; };
class PubSubClient {
 public:
  inline static PubSubClient *last = nullptr;
  std::vector<Publication> sent;
  std::function<void(char *,uint8_t *,unsigned int)> callback;
  bool online = false, brokerAvailable = true, failPublish = false;
  unsigned attempts = 0, loopCalls = 0;
  std::string clientId, willTopic, subscription;
  WiFiClient &transport;
  explicit PubSubClient(WiFiClient &client) : transport(client) { last=this; }
  bool setBufferSize(unsigned bytes) { assert(bytes==6144); return true; }
  void setServer(const char *,uint16_t) {}
  void setSocketTimeout(unsigned seconds) { assert(seconds==1); }
  void setKeepAlive(unsigned seconds) { assert(seconds==15); }
  template<class F> void setCallback(F fn) { callback=fn; }
  bool connected() { return online && transport.alive; }
  bool connect(const char *id,const char *,const char *,const char *topic,unsigned qos,bool retain,const char *will) {
    ++attempts; clientId=id; willTopic=topic; assert(qos==0 && retain && std::string(will)=="offline");
    transport.connect("broker.local",1883); online=brokerAvailable; return online;
  }
  bool subscribe(const char *topic) { subscription=topic; return true; }
  bool loop() { ++loopCalls; return connected(); }
  void disconnect() { online=false; transport.stop(); }
  bool publish(const char *topic,const char *payload,bool retain) {
    if (failPublish) return false;
    sent.push_back({topic,payload,retain}); return true;
  }
  void birth() {
    char topic[]="homeassistant/status"; uint8_t payload[]="online";
    callback(topic,payload,6);
  }
};
'''

CHECKS = r'''
#define WSPD_MQTT_HOST_TEST
#include "mqtt_bridge.h"
#include <algorithm>
#include <cstdlib>
unsigned callbacks=0;
size_t count=0;
bool hasLocal=false;
bool staleRows=false;
std::string name="Remote \"cabinet\" <&>\n\t";
void row(size_t index,WspdMqttSpdSnapshot &s) {
  ++callbacks;
  s.id=static_cast<uint8_t>(index+(hasLocal?0:1)); s.name=name.c_str();
  s.local=hasLocal && index==0;
  s.seen=index%4!=0 || s.local;
  s.fresh=s.seen && index%4!=3 && !staleRows;
  s.status=!s.seen?"UNKNOWN":!s.fresh?"STALE":index%4==2?"FAIL":"OK";
  // Even an inconsistent producer flag cannot turn UNKNOWN/STALE into safe.
  if (!staleRows && (index%4==0 || index%4==3)) s.fresh=true;
  s.batteryValid=index%4!=2;
  s.temperatureC=index%4==2?NAN:-5; s.batteryV=3.25; s.rssiDbm=-77; s.snrDb=9.5;
  s.ageSeconds=s.fresh?2:701; s.lossPercent=index%4==1?NAN:12.5;
}
void gateway(WspdMqttGatewaySnapshot &s) {
  s.configured=count; s.failed=2; s.stale=1; s.unknown=1;
  s.uptimeSeconds=1000; s.alarmMuted=true;
}
char baseTopic[65]="wirelessspd";
char receiverId[33]="aabbccddeeff";
static WspdMqttBridge bridge("broker.local",1883,"receiver","private",baseTopic,5);
static PubSubClient *bridgeBroker=PubSubClient::last;
void steps(unsigned calls) {
  for (unsigned i=0;i<calls;++i) {
    const auto before=PubSubClient::last->sent.size();
    const auto copied=callbacks;
    bridge.loop(clockMs,true);
    assert(callbacks-copied <=1);
    bridge.testNetworkStep(clockMs);
    assert(PubSubClient::last->sent.size()-before <=1);
    clockMs+=25;
  }
}
int main(int argc,char **argv) {
  assert(argc==3 || argc==4); count=std::strtoul(argv[1],nullptr,10); hasLocal=std::atoi(argv[2]);
  if (argc==4) {
    name=std::string(120,'\x01'); // Maximum JSON expansion for a long name.
    memset(baseTopic,'a',64); baseTopic[64]=0;
    memset(receiverId,'b',32); receiverId[32]=0;
  }
  WspdMqttBridge disabled("",1883,"","","wirelessspd",5);
  assert(!disabled.enabled());
  assert(disabled.begin(nullptr,nullptr,nullptr,0,nullptr,nullptr) && taskCount==0);
  PubSubClient::last=bridgeBroker;
  // Main-loop calls never attempt a network connection, even to a failed broker.
  assert(bridge.begin(receiverId,"v0.2-tconnpro","192.0.2.10",count,row,gateway));
  auto &mqtt=*bridgeBroker;
  assert(taskCount==1 && bridge.enabled());
  mqtt.brokerAvailable=false;
  bridge.loop(clockMs,true);
  assert(mqtt.attempts==0); // No network calls on the receiver loop.
  steps(1200);
  assert(mqtt.attempts==1 && !bridge.connected());
  mqtt.brokerAvailable=true;
  steps(1);
  assert(mqtt.attempts==2 && bridge.connected());
  assert(mqtt.clientId==std::string("wspd-")+receiverId);
  assert(mqtt.willTopic==std::string(baseTopic)+"/"+receiverId+"/availability");
  assert(mqtt.subscription=="homeassistant/status");
  steps(static_cast<unsigned>(2*count+6));
  assert(mqtt.sent.front().payload=="offline");
  assert(std::any_of(mqtt.sent.begin(),mqtt.sent.end(),[](const Publication &p) {return p.payload=="online";}));
  std::cout << "INITIAL\n";
  for (const auto &p:mqtt.sent) {
    assert(p.retain && p.payload.size()+p.topic.size()+8<6144);
    std::cout << p.topic << '\t' << p.payload << '\n';
  }
  // Home Assistant birth resends discovery and current state incrementally.
  mqtt.sent.clear(); mqtt.birth();
  steps(static_cast<unsigned>(2*count+6));
  assert(std::any_of(mqtt.sent.begin(),mqtt.sent.end(),[](const Publication &p) {return p.topic.find("/config")!=std::string::npos;}));
  // A producer update overwrites the latest sample; there is no unbounded queue.
  staleRows=true;
  for (size_t i=0;i<count;++i) bridge.loop(clockMs,true);
  bridge.requestPublish(); mqtt.sent.clear();
  steps(static_cast<unsigned>(count+3));
  std::cout << "STALE\n";
  for (const auto &p:mqtt.sent) std::cout << p.topic << '\t' << p.payload << '\n';
  // Pause during OTA removes availability and prevents connect/publication.
  bridge.setPaused(true); bridge.testNetworkStep(clockMs);
  assert(!bridge.connected() && !mqtt.connected() && mqtt.sent.back().payload=="offline");
  const auto before=mqtt.sent.size(); const auto attempts=mqtt.attempts;
  steps(1400);
  assert(mqtt.sent.size()==before && mqtt.attempts==attempts);
  bridge.setPaused(false); steps(1); assert(bridge.connected());
  // Failed publication backs off, rather than hammering the unavailable broker.
  mqtt.failPublish=true; steps(1); assert(!bridge.connected());
  const auto failedAttempts=mqtt.attempts;
  steps(1199); assert(mqtt.attempts==failedAttempts);
  mqtt.failPublish=false; steps(2); assert(mqtt.attempts==failedAttempts+1);
  // Disconnected station cleanly withdraws availability; no reconnect while down.
  bridge.loop(clockMs,false); bridge.testNetworkStep(clockMs);
  assert(!bridge.connected());
  const auto offlineAttempts=mqtt.attempts;
  clockMs+=60000; bridge.testNetworkStep(clockMs);
  assert(mqtt.attempts==offlineAttempts);
}
'''


def main():
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("A C++ compiler is required")
    with tempfile.TemporaryDirectory(prefix="wspd-mqtt-") as directory:
        temp = Path(directory)
        for filename, text in {"Arduino.h": ARDUINO, "WiFiClient.h": WIFI,
                               "PubSubClient.h": MQTT, "freertos/FreeRTOS.h": TASKS,
                               "freertos/task.h": "#pragma once\n"}.items():
            path = temp / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (temp / "check.cpp").write_text(CHECKS)
        subprocess.run([compiler, "-std=c++17", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", "-g", "-I", str(temp), "-I", str(SERVER),
                        str(temp / "check.cpp"), "-o", str(temp / "check")], check=True)
        for count, local, long_name in [(1, False, False), (1, True, False), (8, False, False), (9, True, False), (24, False, False), (128, True, False), (9, False, True)]:
            command = [str(temp / "check"), str(count), str(int(local))]
            if long_name:
                command.append("long-name")
            output = subprocess.check_output(command, text=True)
            sections = {"INITIAL": [], "STALE": []}
            current = None
            for line in output.splitlines():
                if line in sections:
                    current = line
                    continue
                topic, raw = line.split("\t", 1)
                sections[current].append((topic, raw if raw in ("online", "offline") else json.loads(raw)))
            initial = sections["INITIAL"]
            receiver_id = "b" * 32 if long_name else "aabbccddeeff"
            discoveries = [data for topic, data in initial if topic.endswith("/config")]
            assert len(discoveries) == count + 1
            identifiers, unique_ids = set(), set()
            for data in discoveries:
                device_id = data["device"]["identifiers"][0]
                assert device_id not in identifiers
                identifiers.add(device_id)
                assert data["origin"]["sw_version"] == "v0.2-tconnpro"
                for entity in data["components"].values():
                    assert entity["unique_id"] not in unique_ids
                    unique_ids.add(entity["unique_id"])
                    assert entity["platform"] in ("sensor", "binary_sensor")
                    assert entity["availability_mode"] == "all" and entity["expire_after"] == 45
                if device_id != receiver_id:
                    problem = data["components"]["problem"]
                    assert "value_json.fresh" in problem["availability"][1]["value_template"]
                    assert problem["device_class"] == "problem"
                    assert data["device"]["via_device"] == receiver_id
                    if device_id.endswith("_spd_0"):
                        assert set(data["components"]) == {"status", "problem"}
            states = [data for topic, data in initial if "/spd/" in topic and topic.endswith("/state")]
            assert len(states) == count
            assert len({data["spd_id"] for data in states}) == count
            for data in states:
                assert data["problem"] in ("ON", "OFF") if data["fresh"] else data["problem"] is None
                if data["status"] in ("UNKNOWN", "STALE"):
                    assert data["fresh"] is False and data["problem"] is None
                if data["local"] or not data["seen"]:
                    assert all(data[key] is None for key in ("temperature_c", "battery_voltage_v", "rssi_dbm", "snr_db", "age_seconds", "packet_loss_percent"))
                elif (data["spd_id"] - int(not local)) % 4 == 2:
                    assert data["temperature_c"] is None
                if not data["local"] and (data["spd_id"] - int(not local)) % 4 == 1:
                    assert data["packet_loss_percent"] is None
            stale = [data for topic, data in sections["STALE"] if "/spd/" in topic]
            assert len(stale) == count
            assert all(data["fresh"] is False and data["problem"] is None for data in stale)
            print(f"MQTT: {count} rows, local={local}, long_name={long_name}: discovery, availability, complete telemetry and reconnect checks passed")


if __name__ == "__main__":
    main()
