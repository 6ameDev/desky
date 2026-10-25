#pragma once
// Head home-router credential store — SERVICES plumbing, persisted through the
// generic head NVS store (head_store.h owns the "deskyhead" namespace).
// Validity mirrors the core discipline (marker set + non-empty SSID within
// bounds); an empty passphrase is an open network. Transport reaches this
// only through main.cpp injection (LinkHandlers::onWifiCreds + setHomeCreds).

#include <stddef.h>
#include <stdint.h>

#include "services/head_store.h"

#ifdef ARDUINO
#include <Arduino.h>

#include "common/logger.h"
#endif

namespace headwifi {

constexpr const char* kSsidKey = "hssid";
constexpr const char* kPassKey = "hpass";
constexpr const char* kValidKey = "hwok";
constexpr size_t kSsidMax = 32;
constexpr size_t kPassMax = 64;

// Stored credentials count only with the validity marker set and a non-empty
// SSID within bounds. An empty passphrase is allowed (open network).
inline bool credentialsUsable(bool marker, size_t ssidLen) { return marker && ssidLen > 0 && ssidLen <= kSsidMax; }

#ifdef ARDUINO

class HeadWifiStore {
 public:
  // Persists a bundle (ssidLen 1..32, passLen 0..64). False on out-of-bounds
  // input or NVS failure (nothing partially written that reads back valid).
  static bool save(const uint8_t* ssid, uint8_t ssidLen, const uint8_t* pass, uint8_t passLen) {
    if (ssid == nullptr || pass == nullptr) {
      return false;
    }
    if (!credentialsUsable(true, ssidLen) || passLen > kPassMax) {
      return false;
    }
    char ssidStr[kSsidMax + 1] = {};
    char passStr[kPassMax + 1] = {};
    for (size_t i = 0; i < ssidLen; ++i) {
      ssidStr[i] = static_cast<char>(ssid[i]);
    }
    for (size_t i = 0; i < passLen; ++i) {
      passStr[i] = static_cast<char>(pass[i]);
    }
    const size_t ssidWritten = headstore::HeadStore::putString(kSsidKey, ssidStr);
    const size_t passWritten = headstore::HeadStore::putString(kPassKey, passStr);
    const bool marked = headstore::HeadStore::markValid(kValidKey);
    if (ssidWritten == 0 || passWritten == 0 || !marked) {
      LOG_E("CONFIG", "wifi credential write failed");
      return false;
    }
    return true;
  }

  // Loads the bundle. True with ssid/pass filled (NUL-terminated) only when
  // the marker is present and the SSID is non-empty; false (buffers
  // untouched) otherwise. A missing passphrase reads back empty (open net).
  static bool load(char* ssid, size_t ssidCap, char* pass, size_t passCap) {
    if (ssid == nullptr || pass == nullptr || ssidCap == 0 || passCap == 0) {
      return false;
    }
    if (!headstore::HeadStore::isMarkedValid(kValidKey) || !headstore::HeadStore::hasKey(kSsidKey)) {
      return false;
    }
    // putString stores the NUL terminator; content length is one less.
    const size_t ssidStored = headstore::HeadStore::stringLength(kSsidKey);
    if (ssidStored < 2 || !credentialsUsable(true, ssidStored - 1)) {
      return false;
    }
    char ssidTmp[kSsidMax + 1] = {};
    char passTmp[kPassMax + 1] = {};
    const size_t ssidGot = headstore::HeadStore::readString(kSsidKey, ssidTmp, sizeof(ssidTmp));
    const size_t passGot = headstore::HeadStore::hasKey(kPassKey)
                               ? headstore::HeadStore::readString(kPassKey, passTmp, sizeof(passTmp))
                               : 1;
    if (ssidGot == 0 || passGot == 0) {
      return false;
    }
    ssidTmp[sizeof(ssidTmp) - 1] = '\0';
    passTmp[sizeof(passTmp) - 1] = '\0';
    const size_t ssidCopy = ssidGot - 1 < ssidCap - 1 ? ssidGot - 1 : ssidCap - 1;
    for (size_t i = 0; i < ssidCopy; ++i) {
      ssid[i] = ssidTmp[i];
    }
    ssid[ssidCopy] = '\0';
    const size_t passCopy = passGot - 1 < passCap - 1 ? passGot - 1 : passCap - 1;
    for (size_t i = 0; i < passCopy; ++i) {
      pass[i] = passTmp[i];
    }
    pass[passCopy] = '\0';
    return true;
  }

  // Forgets the bundle (removes the three keys only).
  static bool clear() {
    const bool ssidGone = headstore::HeadStore::remove(kSsidKey);
    const bool passGone = headstore::HeadStore::remove(kPassKey);
    const bool markGone = headstore::HeadStore::remove(kValidKey);
    if (!ssidGone || !passGone || !markGone) {
      return false;
    }
    LOG_I("CONFIG", "wifi credentials cleared");
    return true;
  }
};

#endif  // ARDUINO

}  // namespace headwifi
