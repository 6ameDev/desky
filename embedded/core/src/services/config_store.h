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
// First real keys (motor trim, display brightness, ...) slot in here.

#include <Arduino.h>
#include <Preferences.h>

#include "services/logger.h"

class ConfigStore {
 public:
  static constexpr const char* kNamespace = "desky";
  static constexpr const char* kVersionKey = "cfg_version";
  static constexpr uint8_t kConfigVersion = 1;

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

 private:
  // Default values for all persisted keys. Extend as keys land.
  static void applyDefaults(Preferences& prefs) { (void)prefs; }
};
