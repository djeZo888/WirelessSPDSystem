#pragma once

#include <stddef.h>
#include <stdint.h>

// No network calls or waits live in this policy. The sketch applies at most one
// retry and one fallback-AP attempt per interval while monitoring keeps running.
class WspdWiFiRecovery {
 public:
  static constexpr uint32_t DEFAULT_RETRY_INTERVAL_MS = 60000UL;
  struct Actions { bool startAp = false, retry = false; };

  void begin(uint32_t now, bool configured, bool online,
             uint32_t intervalMs = DEFAULT_RETRY_INTERVAL_MS) {
    configured_ = configured;
    online_ = online;
    intervalMs_ = intervalMs;
    offlineSince_ = lastRetry_ = lastApAttempt_ = now;
  }

  Actions poll(uint32_t now, bool online, bool apRunning, bool paused) {
    Actions actions;
    if (online) {
      online_ = true;
      return actions;
    }
    if (online_) {
      online_ = false;
      offlineSince_ = lastRetry_ = lastApAttempt_ = now;
    }
    if (!configured_ || paused) return actions;
    if (!apRunning && uint32_t(now - offlineSince_) >= intervalMs_ &&
        uint32_t(now - lastApAttempt_) >= intervalMs_) {
      actions.startAp = true;
      lastApAttempt_ = now;
    }
    if (uint32_t(now - lastRetry_) >= intervalMs_) {
      actions.retry = true;
      lastRetry_ = now;
    }
    return actions;
  }

 private:
  bool configured_ = false, online_ = false;
  uint32_t intervalMs_ = DEFAULT_RETRY_INTERVAL_MS;
  uint32_t offlineSince_ = 0, lastRetry_ = 0, lastApAttempt_ = 0;
};

struct WspdWiFiNetworkConfig {
  bool dhcp = true;
  // Canonical host-order values, converted to IPAddress octets by the sketch.
  uint32_t address = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
};

inline bool wspdParseIPv4(const char *text, uint32_t &address, bool allowEmpty = false) {
  address = 0;
  if (!text) return false;
  if (!text[0]) return allowEmpty;
  for (unsigned octet = 0; octet < 4; ++octet) {
    unsigned value = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      if (++digits > 3) return false;
      value = value * 10 + unsigned(*text++ - '0');
    }
    if (!digits || value > 255) return false;
    address = (address << 8) | value;
    if (octet < 3) { if (*text++ != '.') return false; }
    else if (*text) return false;
  }
  return true;
}

inline bool wspdUnicastIPv4(uint32_t address) {
  const uint8_t first = uint8_t(address >> 24);
  return first != 0 && first != 127 && first < 224;
}

// A null return means valid. Empty static address explicitly selects DHCP and
// ignores all unused static fields. Gateway/DNS may be empty or 0.0.0.0.
inline const char *wspdParseNetworkConfig(const char *address, const char *mask,
                                        const char *gateway, const char *dns1,
                                        const char *dns2, WspdWiFiNetworkConfig &out) {
  out = WspdWiFiNetworkConfig();
  if (!address) return "WIFI_STATIC_IP must be a string (empty selects DHCP)";
  if (!address[0]) return nullptr;
  out.dhcp = false;
  if (!wspdParseIPv4(address, out.address) || !wspdUnicastIPv4(out.address))
    return "WIFI_STATIC_IP must be a unicast IPv4 address";
  if (!wspdParseIPv4(mask, out.mask)) return "WIFI_SUBNET_MASK must be an IPv4 mask";
  const uint32_t hostMask = ~out.mask;
  if (!out.mask || hostMask < 3 || (hostMask & (hostMask + 1)) != 0)
    return "WIFI_SUBNET_MASK must be contiguous /1../30";
  const uint32_t host = out.address & hostMask;
  if (!host || host == hostMask) return "WIFI_STATIC_IP cannot be the network or broadcast address";
  if (!wspdParseIPv4(gateway, out.gateway, true) ||
      (out.gateway && (!wspdUnicastIPv4(out.gateway) || out.gateway == out.address ||
       (out.gateway & out.mask) != (out.address & out.mask) ||
       !(out.gateway & hostMask) || (out.gateway & hostMask) == hostMask)))
    return "WIFI_GATEWAY must be empty/zero or another host in the same subnet";
  if (!wspdParseIPv4(dns1, out.dns1, true) || (out.dns1 && !wspdUnicastIPv4(out.dns1)))
    return "WIFI_DNS1 must be empty/zero or a unicast IPv4 address";
  if (!wspdParseIPv4(dns2, out.dns2, true) || (out.dns2 && !wspdUnicastIPv4(out.dns2)))
    return "WIFI_DNS2 must be empty/zero or a unicast IPv4 address";
  return nullptr;
}

struct WspdWiFiEvent {
  enum Type : uint8_t { Disconnected, GotIp, LostIp };
  Type type = LostIp;
  uint8_t reason = 0;
  uint32_t ip = 0;
};

// The caller holds its critical-section lock around every queue operation.
// Keep the newest diagnostics if a burst fills the fixed storage.
class WspdWiFiEventQueue {
 public:
  static constexpr size_t CAPACITY = 16;
  void push(const WspdWiFiEvent &event) {
    if (count_ == CAPACITY) {
      head_ = (head_ + 1) % CAPACITY;
      --count_;
      if (dropped_ != UINT32_MAX) ++dropped_;
    }
    events_[(head_ + count_) % CAPACITY] = event;
    ++count_;
  }
  bool pop(WspdWiFiEvent &event) {
    if (!count_) return false;
    event = events_[head_];
    head_ = (head_ + 1) % CAPACITY;
    --count_;
    return true;
  }
  uint32_t takeDropped() { const uint32_t dropped = dropped_; dropped_ = 0; return dropped; }

 private:
  WspdWiFiEvent events_[CAPACITY];
  size_t head_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};
