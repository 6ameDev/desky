#pragma once
// desky v2 connection manager — header-only, platform split task 1.1.
//
// Arduino-free peer + liveness core (stdint/stddef only) so host Unity
// tests include this header directly. No heap, no exceptions, no Strings,
// no sockets: the single-peer slot + the alive window + the auth token
// funnel live here; WiFiUDP/IPAddress stay in UdpServer (which adapts
// IPAddress <-> u32 with ipToU32/u32ToIp below).
//
// Layer rule: peer identity + freshness math is transport (this header);
// bond/era lifecycle is pairing::Core (abilities/pairing/pairing_manager.h);
// is services/ConfigStore; telemetry cadence is services/TelemetryManager.
// Subscription state is explicitly OUT (decided later — Phase-2 SET_RATE
// lands in TelemetryManager, never here).
//
// Single-peer behavior today. Table-ready, no over-engineering: the slot
// is one (peer_, have_, lastAliveMs_); a future head/server peer table is
// an array/map keyed by the same u32 id with per-entry lastAliveMs_ — the
// markAlive/clear/isAlive shapes already take the id, so the call sites
// do not change.
//
// Alive window ownership: udpstatus::kAliveMs (transport/udp_server.h) is
// the single owner — this header NEVER redefines the 5s literal. The window
// arrives as a ctor parameter and UdpServer passes udpstatus::kAliveMs; the
// host tests pin the equality (see test_udp_server.cpp).
//
// Token funnel (token #1 — AuthVerifier threading):
//   handlers obtain Verified via AuthVerifier::verify() (which folds the
//   EXISTING call-site verdicts — bonded, eraOk, fresh from
//   udpstatus::isFreshCounter, hmacOk from the untouched mbedTLS +
//   pairing::ctEqual block — into one token; NO crypto is reimplemented
//   here, the HMAC bytes stay verbatim at the call site), then Pinned via
//   ConnectionManager::markAlive(Verified&&), then call the privileged
//   effects publishDrive(Pinned&&) / sendRekeyAck(Pinned&&) /
//   sendPairOk(PersistReceipt). Swapping the order (effect before verify)
//   fails to compile: the effect signatures REQUIRE the moved token, and
//   every token is move-only with deleted copies (no duplication, no
//   replay of a proof). Verified::valid()/Pinned::valid() stay fail-closed
//   at runtime (invalid token -> no state change, silent drop).
// PersistReceipt default-constructs INVALID (fail-closed); only ConfigStore
// (friend) can mark one valid via saveBond/saveRekey — forging a receipt
// buys nothing.

#include <stddef.h>
#include <stdint.h>

// Firmware minter of PersistReceipt (services/config_store.h). Forward
// declaration only — this header stays dependency-free (required for the
// friend declaration below; the definition lives firmware-side).
class ConfigStore;

namespace connection {

// ── IPAddress <-> u32 adapter (UdpServer side) ────────────────────────────
// Network order: a is the most significant octet (192.168.4.1 ->
// 0xC0A80401). Pure byte math, shared by firmware + host tests.

inline uint32_t ipToU32(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) |
         static_cast<uint32_t>(d);
}

inline void u32ToIp(uint32_t ip, uint8_t out4[4]) {
  if (out4 == nullptr) {
    return;
  }
  out4[0] = static_cast<uint8_t>((ip >> 24) & 0xFFu);
  out4[1] = static_cast<uint8_t>((ip >> 16) & 0xFFu);
  out4[2] = static_cast<uint8_t>((ip >> 8) & 0xFFu);
  out4[3] = static_cast<uint8_t>(ip & 0xFFu);
}

// ── Verified: proof that one RX frame passed the full gate ───────────────
// Move-only, private ctor (friend AuthVerifier), deleted copies. Holds the
// consumed counter + era so downstream minting cannot mix frames.

class AuthVerifier;  // Factory only (defined below).

class Verified {
 public:
  Verified(Verified&& other) : valid_(other.valid_), ctr_(other.ctr_), era_(other.era_) { other.valid_ = false; }
  Verified& operator=(Verified&& other) {
    if (this != &other) {
      valid_ = other.valid_;
      ctr_ = other.ctr_;
      era_ = other.era_;
      other.valid_ = false;
    }
    return *this;
  }
  Verified(const Verified&) = delete;
  Verified& operator=(const Verified&) = delete;

  bool valid() const { return valid_; }
  uint32_t ctr() const { return ctr_; }
  uint16_t era() const { return era_; }

 private:
  friend class AuthVerifier;
  Verified(bool valid, uint32_t ctr, uint16_t era) : valid_(valid), ctr_(ctr), era_(era) {}

  bool valid_;
  uint32_t ctr_;
  uint16_t era_;
};

// Precheck verdicts, computed by the caller with the EXISTING helpers
// (udpstatus::isFreshCounter for fresh, the untouched mbedTLS HMAC +
// pairing::ctEqual block for hmacOk, plain member reads for bonded/eraOk).
// Per-opcode eraOk mapping (documented once, here):
//   drive     -> true (DRIVE_V2 carries no era; session era rides along)
//   alive/disconnect -> (wireEra == sessionEra)
//   rekey     -> (wireEra == sessionEra + 1); the idempotent duplicate
//                (wireEra == sessionEra) re-ACKs WITHOUT verifying.
//   unbond    -> true (UNBOND carries no era).
struct Checks {
  bool bonded;
  bool eraOk;
  bool fresh;
  bool hmacOk;
};

class AuthVerifier {
 public:
  // Single funnel: every verdict true -> valid token carrying ctr+era;
  // any verdict false (or null checks) -> invalid token, no side effects.
  [[nodiscard]] static Verified verify(const Checks& checks, uint32_t ctr, uint16_t era) {
    const bool ok = checks.bonded && checks.eraOk && checks.fresh && checks.hmacOk;
    return Verified(ok, ctr, era);
  }

 private:
  AuthVerifier();  // Never instantiated: static factory only.
};

// ── Pinned: proof of liveness for one verified frame ─────────────────────
// Move-only, private ctor (friend ConnectionManager), deleted copies.
// Minted ONLY by ConnectionManager::markAlive(Verified&&): no Verified,
// no Pinned; no Pinned, no drive publish / rekey ACK.

class ConnectionManager;  // Minter only (defined below).

class Pinned {
 public:
  Pinned(Pinned&& other) : valid_(other.valid_), peer_(other.peer_) { other.valid_ = false; }
  Pinned& operator=(Pinned&& other) {
    if (this != &other) {
      valid_ = other.valid_;
      peer_ = other.peer_;
      other.valid_ = false;
    }
    return *this;
  }
  Pinned(const Pinned&) = delete;
  Pinned& operator=(const Pinned&) = delete;

  bool valid() const { return valid_; }
  uint32_t peer() const { return peer_; }

 private:
  friend class ConnectionManager;
  Pinned(bool valid, uint32_t peer) : valid_(valid), peer_(peer) {}

  bool valid_;
  uint32_t peer_;
};

// ── ConnectionManager: single-peer slot + alive window ───────────────────

class ConnectionManager {
 public:
  // aliveWindowMs is OWNED by udpstatus::kAliveMs — always pass that
  // (UdpServer does); never a literal here.
  explicit ConnectionManager(uint32_t aliveWindowMs) : windowMs_(aliveWindowMs), peer_(0), have_(false), lastMs_(0) {}

  // Alive funnel (mirrors UdpServer::markAlive order): consumes the
  // Verified token, pins the peer, refreshes the stamp, mints Pinned.
  // Invalid token -> no state change, invalid Pinned (fail-closed).
  Pinned markAlive(Verified&& token, uint32_t peerU32, uint32_t nowMs) {
    // Token arrives moved-in (single-use by construction); an invalid
    // token changes nothing (fail-closed silent drop downstream).
    if (!token.valid()) {
      return Pinned(false, 0);
    }
    peer_ = peerU32;
    have_ = true;
    lastMs_ = nowMs;
    return Pinned(true, peerU32);
  }

  // Pre-bond / post-pair pin-only (legacy quirk, preserved exactly):
  // remembers the peer for unicast replies WITHOUT refreshing the alive
  // stamp — pinning alone never makes the link alive.
  void pinPeer(uint32_t peerU32) {
    peer_ = peerU32;
    have_ = true;
  }

  // Refresh-then-clear (UNBOND/DISCONNECT goodbye): consumes the Verified
  // token (the signed frame was verified), stamps the goodbye instant, then
  // drops the net peer (bond kept or cleared by the caller — never here).
  // Invalid token -> no state change (fail-closed).
  void refreshThenClear(Verified&& token, uint32_t nowMs) {
    if (!token.valid()) {
      return;
    }
    lastMs_ = nowMs;
    have_ = false;
  }

  void clear() {
    peer_ = 0;
    have_ = false;
    lastMs_ = 0;
  }

  bool havePeer() const { return have_; }
  uint32_t peer() const { return peer_; }
  uint32_t lastAliveMs() const { return lastMs_; }

  // Wrap-safe freshness (same unsigned-subtraction idiom as
  // udpstatus::isAliveFresh; the boundary counts as fresh). Single owner
  // of the WINDOW stays udpstatus::kAliveMs (arrived via the ctor).
  bool isAlive(uint32_t nowMs) const { return have_ && (nowMs - lastMs_) <= windowMs_; }

 private:
  uint32_t windowMs_;
  uint32_t peer_;
  bool have_;
  uint32_t lastMs_;
};

// ── PersistReceipt: proof that the bond hit NVS before the ACK ──────────
// Move-only, deleted copies. Default-constructs INVALID (fail-closed —
// forging one buys nothing); only ConfigStore (friend, via saveBond/
// saveRekey) can mark one valid. Consumed by value (moved) by
// sendPairOk/sendRekeyAck.

class PersistReceipt {
 public:
  PersistReceipt() : valid_(false) {}
  PersistReceipt(PersistReceipt&& other) : valid_(other.valid_) { other.valid_ = false; }
  PersistReceipt& operator=(PersistReceipt&& other) {
    if (this != &other) {
      valid_ = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }
  PersistReceipt(const PersistReceipt&) = delete;
  PersistReceipt& operator=(const PersistReceipt&) = delete;

  bool valid() const { return valid_; }

 private:
  // NOTE: the global qualifier matters — an unqualified `friend class
  // ConfigStore` would befriend a never-defined connection::ConfigStore.
  friend class ::ConfigStore;
  void mark(bool valid) { valid_ = valid; }

  bool valid_;
};

}  // namespace connection
