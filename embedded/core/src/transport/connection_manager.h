#pragma once
// Connection manager — header-only. Arduino-free peer + liveness core
// (stdint/stddef only) so host tests include this header directly. No heap,
// no sockets: the single-peer slot + alive window + auth token funnel live
// here; WiFiUDP/IPAddress stay in UdpServer (adapted via ipToU32/u32ToIp).
//
// Layer rule: peer identity + freshness math is transport (this header);
// pair/era lifecycle is pairing; persistence is ConfigStore; telemetry
// cadence is TelemetryManager.
//
// Single-peer slot today. The markAlive/clear/isAlive shapes already take
// the peer id, so a future peer table does not change the call sites.
//
// Alive window ownership: udpstatus::kAliveMs is the single owner — this
// header never redefines the literal. The window arrives as a ctor parameter;
// host tests pin the equality.
//
// Token funnel: handlers obtain Verified via AuthVerifier::verify (folds the
// call-site verdicts — paired, eraOk, fresh, hmacOk — into one token; no
// crypto is reimplemented here), then Pinned via markAlive(Verified&&), then
// call the privileged effects. Effect-before-verify fails to compile: the
// effect signatures require the moved token, and every token is move-only
// with deleted copies. Invalid token -> no state change (fail-closed).
// PairingReceipt default-constructs INVALID; only ConfigStore (friend) can
// mark one valid via savePair/savePairRefresh.

#include <stddef.h>
#include <stdint.h>

#include "pairing_receipt.h"

namespace connection {

// IPAddress <-> u32 adapter (UdpServer side). Network order: a is the most
// significant octet. Pure byte math, shared by firmware + host tests.

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

// Verified: proof that one RX frame passed the full gate. Move-only, private
// ctor (friend AuthVerifier). Holds the consumed counter + era so downstream
// minting cannot mix frames.

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

// Precheck verdicts, computed by the caller. Per-opcode eraOk mapping:
//   drive/unpair -> true (no era on the wire); alive/disconnect ->
//   (wireEra == sessionEra); refresh -> (wireEra == sessionEra + 1), while the
//   idempotent duplicate (wireEra == sessionEra) re-ACKs without verifying.
struct Checks {
  bool paired;
  bool eraOk;
  bool fresh;
  bool hmacOk;
};

class AuthVerifier {
 public:
  // Single funnel: every verdict true -> valid token carrying ctr+era;
  // any verdict false (or null checks) -> invalid token, no side effects.
  [[nodiscard]] static Verified verify(const Checks& checks, uint32_t ctr, uint16_t era) {
    const bool ok = checks.paired && checks.eraOk && checks.fresh && checks.hmacOk;
    return Verified(ok, ctr, era);
  }

 private:
  AuthVerifier();  // Never instantiated: static factory only.
};

// Pinned: proof of liveness for one verified frame. Move-only, minted ONLY
// by markAlive(Verified&&): no Verified, no Pinned.

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

  // Alive funnel: consumes the Verified token, pins the peer, refreshes the
  // stamp, mints Pinned. Invalid token -> no state change.
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

  // Pin-only (legacy): remembers the peer for unicast replies WITHOUT
  // refreshing the alive stamp — pinning alone never makes the link alive.
  void pinPeer(uint32_t peerU32) {
    peer_ = peerU32;
    have_ = true;
  }

  // Refresh-then-clear (UNBOND/DISCONNECT goodbye): consumes the Verified
  // token, stamps the goodbye instant, then drops the net peer. Invalid
  // token -> no state change.
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

  // Wrap-safe freshness (unsigned subtraction; the boundary counts as
  // fresh). The WINDOW stays owned by udpstatus::kAliveMs (ctor param).
  bool isAlive(uint32_t nowMs) const { return have_ && (nowMs - lastMs_) <= windowMs_; }

 private:
  uint32_t windowMs_;
  uint32_t peer_;
  bool have_;
  uint32_t lastMs_;
};

}  // namespace connection
