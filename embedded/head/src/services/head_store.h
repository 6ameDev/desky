#pragma once
// Head NVS key-value plumbing — SERVICES layer, one namespace shared by every
// persisted key. Callers own their keys plus one marker key each for validity
// (markValid/isMarkedValid). Null keys fail closed; every call opens and
// closes NVS itself.

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>

#include "common/logger.h"
#endif

namespace headstore {

constexpr const char* kNamespace = "deskyhead";

#ifdef ARDUINO

class HeadStore {
 public:
  // Bytes written including the NUL terminator, 0 when NVS refused the write.
  static size_t putString(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
      return 0;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed");
      return 0;
    }
    const size_t written = prefs.putString(key, value);
    prefs.end();
    return written;
  }

  // Bytes written (1), 0 when NVS refused the write.
  static size_t putUChar(const char* key, uint8_t value) {
    if (key == nullptr) {
      return 0;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed");
      return 0;
    }
    const size_t written = prefs.putUChar(key, value);
    prefs.end();
    return written;
  }

  static bool hasKey(const char* key) {
    if (key == nullptr) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed");
      return false;
    }
    const bool present = prefs.isKey(key);
    prefs.end();
    return present;
  }

  static uint8_t getUChar(const char* key, uint8_t dflt) {
    if (key == nullptr) {
      return dflt;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed");
      return dflt;
    }
    const uint8_t value = prefs.getUChar(key, dflt);
    prefs.end();
    return value;
  }

  // Stored byte count including the NUL terminator, 0 when missing.
  static size_t bytesLength(const char* key) {
    if (key == nullptr) {
      return 0;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed");
      return 0;
    }
    const size_t len = prefs.getBytesLength(key);
    prefs.end();
    return len;
  }

  // Bytes copied, 0 when NVS refused the read.
  static size_t readBytes(const char* key, char* out, size_t cap) {
    if (key == nullptr || out == nullptr || cap == 0) {
      return 0;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed");
      return 0;
    }
    const size_t got = prefs.getBytes(key, out, cap);
    prefs.end();
    return got;
  }

  // True unless NVS refused to open; a missing key still counts as removed.
  static bool remove(const char* key) {
    if (key == nullptr) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed");
      return false;
    }
    prefs.remove(key);
    prefs.end();
    return true;
  }

  // Validity marking: one marker key per caller family.
  static bool markValid(const char* validKey) { return putUChar(validKey, 1) != 0; }

  static bool isMarkedValid(const char* validKey) { return hasKey(validKey) && getUChar(validKey, 0) != 0; }
};

#endif  // ARDUINO

}  // namespace headstore
