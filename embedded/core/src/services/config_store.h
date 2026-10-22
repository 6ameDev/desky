#pragma once
// Config store — header-only, Arduino Preferences (NVS) wrapper.
//
// Every persisted setting lives under namespace "desky" alongside a
// `cfg_version` key. Bump kConfigVersion only when a key's SEMANTICS change
// (not for additive keys). On missing/mismatched version the namespace is
// cleared and defaults reapplied — a stale setting silently bricking behavior
// is worse than losing a user tweak. Full nvs_flash_erase is never used (it
// would destroy other namespaces).

#include <Arduino.h>
#include <Preferences.h>

#include "common/logger.h"
#include "pairing_receipt.h"

class ConfigStore {
 public:
  static constexpr const char* kNamespace = "desky";
  static constexpr const char* kVersionKey = "cfg_version";
  // v1 -> v2: pair added. Per the discipline above a version bump
  // clears the namespace.
  static constexpr uint8_t kConfigVersion = 2;
  static constexpr const char* kPskKey = "psk";
  static constexpr const char* kCounterKey = "bctr";
  static constexpr const char* kPairedKey = "bonded";
  // Tier-1 refresh era (u16, additive key — no version bump: pairs written
  // before the refresh feature read back era 0).
  static constexpr const char* kEraKey = "bera";
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

  // Persists a fresh pair: 16-byte PSK + starting counter. Overwrites any
  // previous owner (single-owner rule). False if NVS is unavailable or psk is
  // null (nothing partially written). The Receipt& overload is the minter
  // (marks the receipt valid ONLY on a full persist — the store-before-send
  // proof); the plain overload delegates.
  static bool savePair(const uint8_t* psk, uint32_t counter) {
    connection::PairingReceipt receipt;
    return savePair(psk, counter, receipt);
  }

  static bool savePair(const uint8_t* psk, uint32_t counter, connection::PairingReceipt& receipt) {
    receipt.mark(false);
    if (psk == nullptr) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveBond)");
      return false;
    }
    const size_t written = prefs.putBytes(kPskKey, psk, kPskLen);
    const size_t counterWritten = prefs.putUInt(kCounterKey, counter);
    const size_t pairedWritten = prefs.putUChar(kPairedKey, 1);
    // Fresh pair starts era 0 (single owner: a new pair evicts the old era).
    const size_t eraWritten = prefs.putUShort(kEraKey, 0);
    prefs.end();
    if (written != kPskLen) {
      LOG_E("CONFIG", "psk write short (%u)", (unsigned)written);
      return false;
    }
    // All writes must succeed — a half-persisted pair is worse than none.
    if (counterWritten == 0 || pairedWritten == 0 || eraWritten == 0) {
      LOG_E("CONFIG", "bond counter/flag write failed (%u/%u)", (unsigned)counterWritten, (unsigned)pairedWritten);
      return false;
    }
    receipt.mark(true);  // Store-before-ACK proof minted ONLY on full persist.
    return true;
  }

  // Persists the drive counter alone (a reboot resyncs without replaying old
  // packets). Cheap enough for every N packets; not per-packet (flash wear).
  // NOTE: counter-only saves never touch the era key.
  static bool saveCounter(uint32_t counter) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveCounter)");
      return false;
    }
    const size_t written = prefs.putUInt(kCounterKey, counter);
    prefs.end();
    if (written == 0) {
      LOG_E("CONFIG", "counter write failed");
      return false;
    }
    return true;
  }

  // Loads the pair. Returns true with psk/counter filled when a pair exists
  // under the current schema version; false (paired=false, buffers
  // untouched) on fresh/mismatched schema or missing keys.
  static bool loadPair(uint8_t* psk, uint32_t& counter, bool& paired) {
    paired = false;
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
    if (psk == nullptr || !prefs.isKey(kPskKey) || !prefs.isKey(kPairedKey)) {
      prefs.end();
      return false;
    }
    if (prefs.getBytes(kPskKey, psk, kPskLen) != kPskLen) {
      prefs.end();
      return false;
    }
    counter = prefs.isKey(kCounterKey) ? prefs.getUInt(kCounterKey, 0) : 0;
    paired = prefs.getUChar(kPairedKey, 0) != 0;
    prefs.end();
    return paired;
  }

  // Tier-1 refresh era accessors (additive: a pair written before the refresh
  // feature reads back 0).
  static uint16_t loadEra() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      return 0;
    }
    const uint16_t era = prefs.isKey(kEraKey) ? static_cast<uint16_t>(prefs.getUShort(kEraKey, 0)) : 0;
    prefs.end();
    return era;
  }

  // Persists a refreshed pair atomically: next PSK + next era + baseline
  // counter 0. Same minter/delegating split as savePair above.
  static bool savePairRefresh(const uint8_t* psk, uint16_t era, uint32_t counter) {
    connection::PairingReceipt receipt;
    return savePairRefresh(psk, era, counter, receipt);
  }

  static bool savePairRefresh(const uint8_t* psk, uint16_t era, uint32_t counter, connection::PairingReceipt& receipt) {
    receipt.mark(false);
    if (psk == nullptr) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveRekey)");
      return false;
    }
    const size_t written = prefs.putBytes(kPskKey, psk, kPskLen);
    const size_t eraWritten = prefs.putUShort(kEraKey, era);
    const size_t counterWritten = prefs.putUInt(kCounterKey, counter);
    const size_t pairedWritten = prefs.putUChar(kPairedKey, 1);
    prefs.end();
    if (written != kPskLen) {
      LOG_E("CONFIG", "rekey psk write short (%u)", (unsigned)written);
      return false;
    }
    if (eraWritten == 0 || counterWritten == 0 || pairedWritten == 0) {
      LOG_E("CONFIG", "rekey era/counter/flag write failed");
      return false;
    }
    receipt.mark(true);  // Store-before-ACK proof minted ONLY on full persist.
    return true;
  }

  // Explicit unpair / 10s-hold factory reset. Removes pair keys only (never
  // nvs_flash_erase); schema version is preserved so no re-migration runs.
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE. The in-memory pairing::Core::clearPair() must be called alongside this.
  static bool clearPair() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (clearBond)");
      return false;
    }
    prefs.remove(kPskKey);
    prefs.remove(kCounterKey);
    prefs.remove(kPairedKey);
    prefs.remove(kEraKey);
    prefs.end();
    LOG_I("CONFIG", "bond cleared");
    return true;
  }

 private:
  // Default values for all persisted keys. Extend as keys land.
  // Note: no pair defaults — absence of the keys IS the unpaired state.
  static void applyDefaults(Preferences& prefs) { (void)prefs; }
};
