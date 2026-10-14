#pragma once
// desky v2 config store — header-only, Arduino Preferences (NVS) wrapper.
//
// Day-1 schema discipline: every persisted setting lives under namespace
// "desky" alongside a `cfg_version` key. Bump kConfigVersion only when a
// key's SEMANTICS change (not for additive keys). On missing/mismatched
// version the namespace is cleared and defaults reapplied — a stale motor
// trim or sensor threshold silently bricking behavior is worse than losing
// a user tweak. Per-key migration (preserving values) is a later upgrade
// once settings are user-valuable; full nvs_flash_erase is never used
// (it would destroy other namespaces, e.g. provisioning).
//
// First real keys: pairing/auth v1 bond (PSK + counter + bonded flag).
// Later keys (motor trim, display brightness, ...) slot in below.

#include <Arduino.h>
#include <Preferences.h>

#include "common/logger.h"

class ConfigStore {
 public:
  static constexpr const char* kNamespace = "desky";
  static constexpr const char* kVersionKey = "cfg_version";
  // v1 -> v2: pairing bond added. Per the discipline above a version bump
  // clears the namespace (a stale PSK silently bricking control is worse
  // than one re-pairing after the firmware upgrade).
  static constexpr uint8_t kConfigVersion = 2;
  static constexpr const char* kPskKey = "psk";
  static constexpr const char* kCounterKey = "bctr";
  static constexpr const char* kBondedKey = "bonded";
  static constexpr size_t kPskLen = 16;

  // Opens NVS, enforces schema version, seeds defaults. Call once in setup().
  static void begin() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed");
      return;
    }
    const uint8_t stored = prefs.isKey(kVersionKey) ? prefs.getUChar(kVersionKey, 0) : 0;
    if (stored != kConfigVersion) {
      if (stored == 0) {
        LOG_I("CONFIG", "fresh namespace, seeding v%d", kConfigVersion);
      } else {
        LOG_W("CONFIG", "migrating v%d -> v%d (clear + defaults)", stored, kConfigVersion);
        prefs.clear();
      }
      applyDefaults(prefs);
      prefs.putUChar(kVersionKey, kConfigVersion);
    } else {
      LOG_D("CONFIG", "schema v%d ok", kConfigVersion);
    }
    prefs.end();
  }

  // Persists a fresh bond: 16-byte PSK + starting counter. Overwrites any
  // previous owner (single-owner rule). Returns false if NVS is unavailable
  // or psk is null (nothing is partially written on invalid input).
  static bool saveBond(const uint8_t* psk, uint32_t counter) {
    if (psk == nullptr) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveBond)");
      return false;
    }
    const size_t written = prefs.putBytes(kPskKey, psk, kPskLen);
    prefs.putUInt(kCounterKey, counter);
    prefs.putUChar(kBondedKey, 1);
    prefs.end();
    if (written != kPskLen) {
      LOG_E("CONFIG", "psk write short (%u)", (unsigned)written);
      return false;
    }
    return true;
  }

  // Persists the drive counter alone (called periodically from the drive
  // verify path so a reboot resyncs without replaying old packets). Cheap
  // enough for every N packets; not per-packet (flash wear).
  static bool saveCounter(uint32_t counter) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveCounter)");
      return false;
    }
    prefs.putUInt(kCounterKey, counter);
    prefs.end();
    return true;
  }

  // Loads the bond. Returns true with psk/counter filled when a bond exists
  // under the current schema version; false (bonded=false, buffers
  // untouched) on fresh/mismatched schema or missing keys.
  static bool loadBond(uint8_t* psk, uint32_t& counter, bool& bonded) {
    bonded = false;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed (loadBond)");
      return false;
    }
    const uint8_t stored = prefs.isKey(kVersionKey) ? prefs.getUChar(kVersionKey, 0) : 0;
    if (stored != kConfigVersion) {
      prefs.end();
      return false;
    }
    if (psk == nullptr || !prefs.isKey(kPskKey) || !prefs.isKey(kBondedKey)) {
      prefs.end();
      return false;
    }
    if (prefs.getBytes(kPskKey, psk, kPskLen) != kPskLen) {
      prefs.end();
      return false;
    }
    counter = prefs.isKey(kCounterKey) ? prefs.getUInt(kCounterKey, 0) : 0;
    bonded = prefs.getUChar(kBondedKey, 0) != 0;
    prefs.end();
    return bonded;
  }

  // Explicit unpair / 10s-hold factory reset. Removes bond keys only (never
  // nvs_flash_erase); schema version is preserved so no re-migration runs.
  // The in-memory pairing::Core::clearBond() must be called alongside this.
  static bool clearBond() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (clearBond)");
      return false;
    }
    prefs.remove(kPskKey);
    prefs.remove(kCounterKey);
    prefs.remove(kBondedKey);
    prefs.end();
    LOG_I("CONFIG", "bond cleared");
    return true;
  }

 private:
  // Default values for all persisted keys. Extend as keys land.
  // Note: no bond defaults — absence of the keys IS the unbonded state.
  static void applyDefaults(Preferences& prefs) { (void)prefs; }
};
