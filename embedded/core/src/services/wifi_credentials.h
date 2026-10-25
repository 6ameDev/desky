#pragma once
// Home-router WiFi credentials + dual-mode network policy — Arduino-free.
// Host tests include this header directly; firmware NVS/WiFi calls stay in
// ConfigStore (services/config_store.h) and UdpServer (transport/udp_server.h).
// Transport owns no services types, so the firmware consumes these values by
// injection through main.cpp (credentials + timeouts passed into
// UdpServer::begin); the loop mirrors joinTimedOut/failoverDue inline.

#include <stddef.h>
#include <stdint.h>

namespace wificreds {

// NVS keys under the ConfigStore "desky" namespace (additive: no schema bump).
constexpr const char* kSsidKey = "wssid";
constexpr const char* kPassKey = "wpass";
constexpr const char* kValidKey = "wok";
// 802.11 bounds: SSID 1..32 octets, WPA2 passphrase 0..64 (empty = open net).
constexpr size_t kSsidMax = 32;
constexpr size_t kPassMax = 64;
// Boot STA join window, then AP until reboot; connected-then-lost grace before
// AP failover. Passed into the transport by value (injectable per call site).
constexpr uint32_t kStaJoinTimeoutMs = 15000;
constexpr uint32_t kStaFailoverMs = 120000;

enum class BootNet : uint8_t { kAp, kSta };

// Stored credentials count only with the validity marker set and a non-empty
// SSID within bounds. An empty passphrase is allowed (open network).
inline bool credentialsUsable(bool marker, size_t ssidLen) { return marker && ssidLen > 0 && ssidLen <= kSsidMax; }

// Boot gate: skip the STA attempt entirely without usable credentials.
inline bool shouldAttemptSta(bool storedUsable, const char* ssid) {
  return storedUsable && ssid != nullptr && ssid[0] != '\0';
}

inline BootNet decideBootNet(bool storedUsable, const char* ssid) {
  return shouldAttemptSta(storedUsable, ssid) ? BootNet::kSta : BootNet::kAp;
}

// STA join window expiry. Unsigned subtraction: wrap-safe across rollover.
inline bool joinTimedOut(uint32_t startMs, uint32_t nowMs, uint32_t timeoutMs = kStaJoinTimeoutMs) {
  return (nowMs - startMs) >= timeoutMs;
}

// Connected-then-lost grace expiry. Unsigned subtraction: wrap-safe.
inline bool failoverDue(uint32_t lastLinkMs, uint32_t nowMs, uint32_t timeoutMs = kStaFailoverMs) {
  return (nowMs - lastLinkMs) >= timeoutMs;
}

}  // namespace wificreds
