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
#include "services/wifi_credentials.h"

class ConfigStore {
 public:
  static constexpr const char* kNamespace = "desky";
  static constexpr const char* kVersionKey = "cfg_version";
  // v2 -> v3: pair keys renamed (paired/pera). Per the discipline above a
  // version bump clears the namespace.
  static constexpr uint8_t kConfigVersion = 3;
  static constexpr const char* kPskKey = "psk";
  static constexpr const char* kCounterKey = "bctr";
  static constexpr const char* kPairedKey = "paired";
  // Robot->app send-domain counter (toAppCounter_, per-link peer session;
  // single owner today so one key; additive key — no version bump, absent
  // reads back 0).
  static constexpr const char* kToAppCounterKey = "tapp";
  // Tier-1 pair-refresh era (u16, additive key — no version bump: pairs
  // written before the refresh feature read back era 0).
  static constexpr const char* kEraKey = "pera";
  static constexpr size_t kPskLen = 16;
  // Home-router STA credentials (additive keys — no version bump, absent
  // reads back unusable). Names live in services/wifi_credentials.h.
  static constexpr const char* kWifiSsidKey = wificreds::kSsidKey;
  static constexpr const char* kWifiPassKey = wificreds::kPassKey;
  static constexpr const char* kWifiValidKey = wificreds::kValidKey;

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
      LOG_E("CONFIG", "nvs open failed (savePair)");
      return false;
    }
    const size_t written = prefs.putBytes(kPskKey, psk, kPskLen);
    const size_t counterWritten = prefs.putUInt(kCounterKey, counter);
    const size_t pairedWritten = prefs.putUChar(kPairedKey, 1);
    // Fresh pair starts era 0 (single owner: a new pair evicts the old era).
    const size_t eraWritten = prefs.putUShort(kEraKey, 0);
    // Fresh pair resets the robot->app send domain to its baseline.
    const size_t tappWritten = prefs.putUInt(kToAppCounterKey, 0);
    prefs.end();
    if (written != kPskLen) {
      LOG_E("CONFIG", "psk write short (%u)", (unsigned)written);
      return false;
    }
    // All writes must succeed — a half-persisted pair is worse than none.
    if (counterWritten == 0 || pairedWritten == 0 || eraWritten == 0 || tappWritten == 0) {
      LOG_E("CONFIG", "pair counter/flag write failed (%u/%u)", (unsigned)counterWritten, (unsigned)pairedWritten);
      return false;
    }
    receipt.mark(true);  // Store-before-ACK proof minted ONLY on full persist.
    return true;
  }

  // Persists the app->robot receive-domain counter alone (a reboot resyncs
  // without replaying old packets). Cheap enough for every N packets; not
  // per-packet (flash wear).
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

  // Persists the robot->app send-domain counter alone (evictions are rare;
  // direct save, no wear concern). Session-scoped with PSK+era.
  static bool saveToAppCounter(uint32_t counter) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveToAppCounter)");
      return false;
    }
    const size_t written = prefs.putUInt(kToAppCounterKey, counter);
    prefs.end();
    if (written == 0) {
      LOG_E("CONFIG", "to-app counter write failed");
      return false;
    }
    return true;
  }

  // Persists home-router STA credentials: SSID (1..32) + passphrase
  // (0..64, empty = open network) + validity marker. False when NVS is
  // unavailable or the inputs are out of bounds (nothing written).
  static bool saveWifiCredentials(const char* ssid, const char* pass) {
    if (ssid == nullptr || pass == nullptr) {
      return false;
    }
    size_t ssidLen = 0;
    while (ssid[ssidLen] != '\0') {
      ++ssidLen;
    }
    size_t passLen = 0;
    while (pass[passLen] != '\0') {
      ++passLen;
    }
    if (!wificreds::credentialsUsable(true, ssidLen) || passLen > wificreds::kPassMax) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (saveWifi)");
      return false;
    }
    const size_t ssidWritten = prefs.putString(kWifiSsidKey, ssid);
    const size_t passWritten = prefs.putString(kWifiPassKey, pass);
    const size_t validWritten = prefs.putUChar(kWifiValidKey, 1);
    prefs.end();
    if (ssidWritten == 0 || passWritten == 0 || validWritten == 0) {
      LOG_E("CONFIG", "wifi credential write failed");
      return false;
    }
    return true;
  }

  // Loads home-router STA credentials. Returns true with ssid/pass filled
  // (NUL-terminated, truncated to the caller's buffers) only when the
  // validity marker is present and the SSID is non-empty; false (buffers
  // untouched) otherwise. A missing passphrase reads back empty (open net).
  static bool loadWifiCredentials(char* ssid, size_t ssidCap, char* pass, size_t passCap) {
    if (ssid == nullptr || pass == nullptr || ssidCap == 0 || passCap == 0) {
      return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed (loadWifi)");
      return false;
    }
    const uint8_t stored = prefs.isKey(kVersionKey) ? prefs.getUChar(kVersionKey, 0) : 0;
    if (stored != kConfigVersion) {
      prefs.end();
      return false;
    }
    if (!prefs.isKey(kWifiValidKey) || prefs.getUChar(kWifiValidKey, 0) == 0 || !prefs.isKey(kWifiSsidKey)) {
      prefs.end();
      return false;
    }
    // putString stores the NUL terminator; content length is one less.
    const size_t ssidStored = prefs.getBytesLength(kWifiSsidKey);
    const size_t passStored = prefs.isKey(kWifiPassKey) ? prefs.getBytesLength(kWifiPassKey) : 1;
    if (ssidStored < 2 || !wificreds::credentialsUsable(true, ssidStored - 1)) {
      prefs.end();
      return false;
    }
    char ssidTmp[wificreds::kSsidMax + 1];
    char passTmp[wificreds::kPassMax + 1];
    const size_t ssidGot = prefs.getBytes(kWifiSsidKey, ssidTmp, sizeof(ssidTmp));
    const size_t passGot = prefs.isKey(kWifiPassKey) ? prefs.getBytes(kWifiPassKey, passTmp, sizeof(passTmp)) : 1;
    prefs.end();
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

  // Forgets home-router STA credentials (removes the three keys only; schema
  // version is preserved so no re-migration runs).
  static bool clearWifiCredentials() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
      LOG_E("CONFIG", "nvs open failed (clearWifi)");
      return false;
    }
    prefs.remove(kWifiSsidKey);
    prefs.remove(kWifiPassKey);
    prefs.remove(kWifiValidKey);
    prefs.end();
    LOG_I("CONFIG", "wifi credentials cleared");
    return true;
  }

  // Loads the pair. Returns true with psk/counter filled when a pair exists
  // under the current schema version; false (paired=false, buffers
  // untouched) on fresh/mismatched schema or missing keys.
  static bool loadPair(uint8_t* psk, uint32_t& counter, bool& paired) {
    paired = false;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      LOG_E("CONFIG", "nvs open failed (loadPair)");
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

  // Robot->app send-domain read (additive: pairs written before the
  // per-link split read back 0, the pair baseline).
  static uint32_t loadToAppCounter() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
      return 0;
    }
    const uint32_t ctr = prefs.isKey(kToAppCounterKey) ? prefs.getUInt(kToAppCounterKey, 0) : 0;
    prefs.end();
    return ctr;
  }

  // Tier-1 pair-refresh era accessors (additive: a pair written before the
  // refresh feature reads back 0).
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
      LOG_E("CONFIG", "nvs open failed (savePairRefresh)");
      return false;
    }
    const size_t written = prefs.putBytes(kPskKey, psk, kPskLen);
    const size_t eraWritten = prefs.putUShort(kEraKey, era);
    const size_t counterWritten = prefs.putUInt(kCounterKey, counter);
    const size_t pairedWritten = prefs.putUChar(kPairedKey, 1);
    // Rotation resets the robot->app send domain to its baseline.
    const size_t tappWritten = prefs.putUInt(kToAppCounterKey, 0);
    prefs.end();
    if (written != kPskLen) {
      LOG_E("CONFIG", "pair-refresh psk write short (%u)", (unsigned)written);
      return false;
    }
    if (eraWritten == 0 || counterWritten == 0 || pairedWritten == 0 || tappWritten == 0) {
      LOG_E("CONFIG", "pair-refresh era/counter/flag write failed");
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
      LOG_E("CONFIG", "nvs open failed (clearPair)");
      return false;
    }
    prefs.remove(kPskKey);
    prefs.remove(kCounterKey);
    prefs.remove(kPairedKey);
    prefs.remove(kEraKey);
    prefs.remove(kToAppCounterKey);
    prefs.end();
    LOG_I("CONFIG", "pair cleared");
    return true;
  }

 private:
  // Default values for all persisted keys. Extend as keys land.
  // Note: no pair defaults — absence of the keys IS the unpaired state.
  static void applyDefaults(Preferences& prefs) { (void)prefs; }
};
