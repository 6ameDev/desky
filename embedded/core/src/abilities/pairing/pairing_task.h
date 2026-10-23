#pragma once
// Pairing task binding — abilities/pairing (firmware-only crypto).
//
// mbedTLS glue (deriveKek/wrapPsk/ctEqual/derivePairRefreshPsk) + the PairingTask
// session class below (enrolment + pair destruction + refresh + LastPair/QR
// state + pair-family RX dispatch). Never included by host tests.

#include "pairing_codec.h"
#include "pairing_manager.h"

#ifdef ARDUINO
// ── Firmware: pairing crypto glue (mbedTLS) ───────────────────────
// Arduino-ESP32 (S3) bundles mbedTLS; headers live under the framework
// package as <mbedtls/...>. No heap, no String, no logging of key material
// (never LOG key, nonce, or code bytes) — failures surface as silent bools
// for the PAIR_ERR path.
//
// CALL-ORDER CONTRACT (store-before-send): savePair BEFORE PAIR_OK leaves
// the radio; a failed save answers PAIR_ERR, never PAIR_OK.

#include <Arduino.h>
#include <esp_system.h>
#include <mbedtls/cipher.h>
// constant_time.h ships WITHOUT the extern "C" guard the other mbedtls
// headers have, so include it wrapped or ctEqual fails at link.
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include <utility>

#include "common/fault_manager.h"  // DESKY_ASSERT (firmware side only)
#include "common/logger.h"
#include "services/config_store.h"
#include "transport/connection_manager.h"
#include "transport/link_server.h"
#include "transport/pairing_session.h"

namespace pairing {

// Derives the pairing KEK: HKDF-SHA256(salt=client||server nonce,
// IKM=6 ASCII code digits, info="desky-pair-v1", L=16). False on null input
// or mbedTLS error (kekOut zeroized on failure).
inline bool deriveKek(const uint8_t code6[kCodeLen], const uint8_t clientNonce[kPairNonceLen],
                      const uint8_t serverNonce[kPairNonceLen], uint8_t kekOut[kKekLen]) {
  DESKY_ASSERT(code6 != nullptr);
  DESKY_ASSERT(clientNonce != nullptr);
  DESKY_ASSERT(serverNonce != nullptr);
  DESKY_ASSERT(kekOut != nullptr);
  if (code6 == nullptr || clientNonce == nullptr || serverNonce == nullptr || kekOut == nullptr) {
    return false;
  }
  uint8_t salt[kPairNonceLen * 2];
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    salt[i] = clientNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    salt[kPairNonceLen + i] = serverNonce[i];
  }
  static const char kInfo[] = "desky-pair-v1";
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = -1;
  if (md != nullptr) {
    rc = mbedtls_hkdf(md, salt, sizeof(salt), code6, kCodeLen, reinterpret_cast<const unsigned char*>(kInfo),
                      sizeof(kInfo) - 1, kekOut, kKekLen);
  }
  mbedtls_platform_zeroize(salt, sizeof(salt));
  if (rc != 0) {
    mbedtls_platform_zeroize(kekOut, kKekLen);
    return false;
  }
  return true;
}

// Wraps the paired PSK + persist-interval byte for PAIR_OK:
// ct||tag = AES-128-GCM(KEK, nonce12=server||client nonce||00s,
// AAD=0x51||client||server nonce, psk16||persist_u8). False on null input or
// mbedTLS error (ct/tag zeroized on failure; caller zeroizes psk/kek).
inline bool wrapPsk(const uint8_t psk[kPskLen], const uint8_t kek[kKekLen], const uint8_t clientNonce[kPairNonceLen],
                    const uint8_t serverNonce[kPairNonceLen], uint8_t persistU8, uint8_t ctOut[kPairCtLen],
                    uint8_t tagOut[kPairTagLen]) {
  DESKY_ASSERT(psk != nullptr);
  DESKY_ASSERT(kek != nullptr);
  DESKY_ASSERT(clientNonce != nullptr);
  DESKY_ASSERT(serverNonce != nullptr);
  DESKY_ASSERT(ctOut != nullptr);
  DESKY_ASSERT(tagOut != nullptr);
  if (psk == nullptr || kek == nullptr || clientNonce == nullptr || serverNonce == nullptr || ctOut == nullptr ||
      tagOut == nullptr) {
    return false;
  }
  uint8_t iv[kPairIvLen];
  uint8_t aad[kPairAadLen];
  uint8_t plain[kPairWrapPlainLen];
  for (size_t i = 0; i < kPskLen; ++i) {
    plain[i] = psk[i];
  }
  plain[kPskLen] = sanitizePersistEvery(persistU8);
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    iv[i] = serverNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    iv[kPairNonceLen + i] = clientNonce[i];
  }
  for (size_t i = 2 * kPairNonceLen; i < kPairIvLen; ++i) {
    iv[i] = 0x00;
  }
  aad[0] = kPairOkHeader;
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    aad[1 + i] = clientNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    aad[1 + kPairNonceLen + i] = serverNonce[i];
  }
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, kek, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, kPairWrapPlainLen, iv, sizeof(iv), aad, sizeof(aad),
                                   plain, ctOut, kPairTagLen, tagOut);
  }
  mbedtls_gcm_free(&ctx);
  mbedtls_platform_zeroize(iv, sizeof(iv));
  mbedtls_platform_zeroize(aad, sizeof(aad));
  mbedtls_platform_zeroize(plain, sizeof(plain));
  if (rc != 0) {
    mbedtls_platform_zeroize(ctOut, kPairCtLen);
    mbedtls_platform_zeroize(tagOut, kPairTagLen);
    return false;
  }
  return true;
}

// Constant-time buffer compare (HMAC tag check); false on null input.
inline bool ctEqual(const void* a, const void* b, size_t n) {
  DESKY_ASSERT(a != nullptr);
  DESKY_ASSERT(b != nullptr);
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return mbedtls_ct_memcmp(a, b, n) == 0;
}

// Tier-1 pair-refresh key schedule: PSK_next = HKDF-SHA256(salt="desky-pair-refresh-v1",
// IKM=oldPsk16, info=eraBE2(new), L=16). False on null input or mbedTLS
// error (out zeroized on failure).
inline bool derivePairRefreshPsk(const uint8_t oldPsk[kPskLen], uint16_t nextEra, uint8_t nextPskOut[kPskLen]) {
  DESKY_ASSERT(oldPsk != nullptr);
  DESKY_ASSERT(nextPskOut != nullptr);
  if (oldPsk == nullptr || nextPskOut == nullptr) {
    return false;
  }
  static const char kSalt[] = "desky-pair-refresh-v1";
  uint8_t info[2];
  info[0] = static_cast<uint8_t>((nextEra >> 8) & 0xFFu);
  info[1] = static_cast<uint8_t>(nextEra & 0xFFu);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = -1;
  if (md != nullptr) {
    rc = mbedtls_hkdf(md, reinterpret_cast<const unsigned char*>(kSalt), sizeof(kSalt) - 1, oldPsk, kPskLen, info,
                      sizeof(info), nextPskOut, kPskLen);
  }
  mbedtls_platform_zeroize(info, sizeof(info));
  if (rc != 0) {
    mbedtls_platform_zeroize(nextPskOut, kPskLen);
    return false;
  }
  return true;
}

// PairingTask: pairing/auth session policy + PIN PairShow link stage +
// atomic auth service. Owns ALL session state: paired_, psk_, lastCounter_,
// era_, validCount_, pairCore_, the LastPair redelivery cache, pendingCode_/pendingValid_,
// and the dropFirst flags for the retained handlers. The open window's 6-digit
// code + valid-seconds go out once per window via publishPairShow (transport only
// unicasts); completion goes out via notifyPairingDone (empty PairDone frame,
// fire-and-forget). pairing::Core stays window/tries/lockout policy only;
// this class is the firmware session layer around it.
//
// Transport back-call contract (transport::IUdpTransport, implemented by
// UdpServer): the ability never touches sockets — byte composition lives
// here, the gate-check + unicast stays in transport:
//   markAlive(Verified&&, IPAddress, uint16_t, uint32_t, uint16_t era)
//     -> connection::Pinned (pins the telemetry peer + refreshes the stamp)
//   sendPairOk(PairingReceipt, IPAddress, uint16_t, bytes, len)
//   sendPairRedeliver(IPAddress, uint16_t, bytes, len) (lost-PAIR_OK race)
//   sendPairErr(IPAddress, uint16_t, bytes, len)
//   sendPairRefreshAck(Pinned, IPAddress, uint16_t, bytes, len)
//   sendPairRefreshAckImpl(IPAddress, uint16_t, bytes, len) (idempotent duplicate path)
//   isFreshCounter(lastSeen, got) -> bool (udpstatus bridge)
//   shouldRedeliver(...) -> bool (udpstatus bridge)
// Shared socket-adjacent state (peer slot + alive stamp) stays in transport:
// attach() pins the ConnectionManager + IPAddress peer pointers (set once
// from UdpServer::attachPairing before the task reads the radio).
// Link back-call (pinned by attachHeadLink from setup; null-checked at every
// notify site): the ability never touches sockets — notifyPairingDone
// fire-and-forgets the empty PairDone frame via LinkServer::sendUnicast
// (peerless = silent no-op).
class PairingTask : public transport::IPairingSession {
 public:
  PairingTask()
      : conn_(nullptr),
        peerIp_(nullptr),
        peerPort_(nullptr),
        link_(nullptr),
        pairCore_(),
        psk_{},
        lastCounter_(0),
        paired_(false),
        era_(0),
        validCount_(0),
        maxVerifyUs_(0) {
    lastPair_.valid = false;
  }

  // Pins transport-owned shared state. Call once from UdpServer::attachPairing
  // before restorePair (handlers dereference these on every packet).
  void attach(connection::ConnectionManager* conn, IPAddress* peerIp, uint16_t* peerPort) override {
    DESKY_ASSERT(conn != nullptr);
    DESKY_ASSERT(peerIp != nullptr);
    DESKY_ASSERT(peerPort != nullptr);
    conn_ = conn;
    peerIp_ = peerIp;
    peerPort_ = peerPort;
  }

  // Pins the core->head link endpoint. Call once from setup() after
  // UdpServer::begin + LinkServer::begin (mirrors Coordinator::attachPairing;
  // null-checked at every notify site so bench builds without the link still
  // boot clean).
  void attachHeadLink(LinkServer* link) {
    DESKY_ASSERT(link != nullptr);
    link_ = link;
  }

  // Pair-restore: paired -> DRIVE verifies against the stored PSK;
  // unpaired -> stays idle with no window. Only the physical pair button
  // opens a window. ConfigStore::begin() already ran in setup(). The PAIR_OK
  // redelivery cache stays invalid across reboot (RAM-only).
  void restorePair() override {
    lastPair_.valid = false;
    uint8_t loadedPsk[16] = {};
    uint32_t storedCtr = 0;
    bool storedPair = false;
    if (ConfigStore::loadPair(loadedPsk, storedCtr, storedPair) && storedPair) {
      paired_ = true;
      lastCounter_ = storedCtr;
      era_ = ConfigStore::loadEra();
      for (size_t i = 0; i < sizeof(psk_); ++i) {
        psk_[i] = loadedPsk[i];
      }
      LOG_I("UDP", "pair loaded counter=%lu era=%u", static_cast<unsigned long>(storedCtr),
            static_cast<unsigned>(era_));
    } else {
      paired_ = false;
      lastCounter_ = 0;
      era_ = 0;
      pendingValid_ = false;
      pairShowSent_ = false;
      LOG_I("UDP", "unpaired idle, press pair button to pair");
    }
    mbedtls_platform_zeroize(loadedPsk, sizeof(loadedPsk));
  }

  // Opens a pairing window showing a fresh esp_random 6-digit code (the LOG_I
  // code line is the display). Refused while paired (clearPair() first).
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE.
  bool openPairingWindow() {
    if (paired_) {
      return false;
    }
    uint8_t code[pairing::kCodeLen];
    for (size_t i = 0; i < pairing::kCodeLen; ++i) {
      code[i] = static_cast<uint8_t>('0' + (esp_random() % 10u));
    }
    if (!pairCore_.openWindow(millis(), code)) {
      return false;
    }
    LOG_I("UDP", "pair window open code=%c%c%c%c%c%c", code[0], code[1], code[2], code[3], code[4], code[5]);
    // PIN stage for the head-link pump below: the window's 6 digits are
    // staged in ALL builds (release included); valid-seconds derive from
    // kWindowMs/1000 at pump time (single source). A fresh window always
    // re-arms the once-per-window send latch (covers a pair/unpair reopen
    // the main-loop pump never observed as closed).
    for (size_t i = 0; i < pairing::kCodeLen; ++i) {
      pendingCode_[i] = code[i];
    }
    pendingValid_ = true;
    pairShowSent_ = false;  // Fresh window re-arms the pump's once-per-window latch.
    return true;
  }

  // Physical pair-button entry: evicts the current owner with one
  // authenticated fire-and-forget DISCONNECT, then clears and re-opens the
  // window. Counter 0 never goes on the wire, so a wrapped next-counter
  // uses 1. No peer pinned -> the send is skipped, the clear still runs.
  bool enterPairModeFromButton(transport::IUdpTransport& t, uint32_t nowMs) {
    (void)nowMs;
    const bool wasPaired = paired_;
    const uint16_t wasEra = era_;
    const uint32_t wasCounter = lastCounter_;
    uint8_t wasPsk[16];
    for (size_t i = 0; i < sizeof(wasPsk); ++i) {
      wasPsk[i] = psk_[i];
    }
    IPAddress wasIp;
    uint16_t wasPort = 0;
    bool havePeer = false;
    if (peerIp_ != nullptr && peerPort_ != nullptr) {
      wasIp = *peerIp_;
      wasPort = *peerPort_;
      const uint32_t peerU32 = connection::ipToU32(wasIp[0], wasIp[1], wasIp[2], wasIp[3]);
      havePeer = (conn_ != nullptr && conn_->havePeer()) || peerU32 != 0 || wasPort != 0;
    }
    if (wasPaired && havePeer) {
      uint32_t ctr = wasCounter + 1;
      if (ctr == 0) {
        ctr = 1;
      }
      uint8_t msg[pairing::kDisconnectMsgLen];
      pairing::disconnectTagMsg(wasEra, ctr, msg);
      uint8_t mac[32];
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      int rc = -1;
      if (md != nullptr) {
        rc = mbedtls_md_hmac(md, wasPsk, sizeof(wasPsk), msg, sizeof(msg), mac);
      }
      mbedtls_platform_zeroize(msg, sizeof(msg));
      if (rc == 0) {
        uint8_t out[pairing::kDisconnectSize];
        out[0] = pairing::kDisconnectHeader;
        out[1] = static_cast<uint8_t>((wasEra >> 8) & 0xFFu);
        out[2] = static_cast<uint8_t>(wasEra & 0xFFu);
        out[3] = static_cast<uint8_t>((ctr >> 24) & 0xFFu);
        out[4] = static_cast<uint8_t>((ctr >> 16) & 0xFFu);
        out[5] = static_cast<uint8_t>((ctr >> 8) & 0xFFu);
        out[6] = static_cast<uint8_t>(ctr & 0xFFu);
        for (size_t i = 0; i < pairing::kDisconnectTagLen; ++i) {
          out[7 + i] = mac[i];
        }
        uint8_t xorSum = 0;
        for (size_t i = 0; i + 1 < sizeof(out); ++i) {
          xorSum ^= out[i];
        }
        out[sizeof(out) - 1] = xorSum;
        t.sendDisconnect(wasIp, wasPort, out, sizeof(out));
      }
      mbedtls_platform_zeroize(mac, sizeof(mac));
    }
    mbedtls_platform_zeroize(wasPsk, sizeof(wasPsk));
    clearPair();
    return openPairingWindow();
  }

  // PIN link pump: ability-side policy half of the head display path. Called
  // from the main loop: while a pairing window is open, composes the 8B
  // PairShow payload and hands it to the transport's pinned-peer sendUnicast
  // exactly once per window. No head linked -> waits silently for a late
  // joiner; window closed -> unlatch for the next window. Never touches
  // sockets; never logs code bytes (seq/len only).
  bool publishPairShow(LinkServer& link) {
    if (!pendingValid_) {
      pairShowSent_ = false;  // Window closed: unlatch for the next window.
      return false;
    }
    if (pairShowSent_) {
      return false;  // Already sent this window.
    }
    const uint16_t validSecs = static_cast<uint16_t>(pairing::kWindowMs / 1000u);
    uint8_t payload[linkcode::kPairShowLen] = {};
    if (!linkcode::packPairShow(pendingCode_, validSecs, payload)) {
      return false;
    }
    uint16_t seq = 0;
    if (!link.sendUnicast(linkcode::kTypePairShow, payload, sizeof(payload), seq)) {
      LOG_EVERY_N(20, "LINK", "pairshow pending, no head peer");
      return false;
    }
    pairShowSent_ = true;
    LOG_I("LINK", "pairshow sent seq=%u len=%u", static_cast<unsigned>(seq), static_cast<unsigned>(sizeof(payload)));
    return true;
  }

  // Pairing-complete signal: fire-and-forget empty PairDone frame to the
  // HB-pinned head. Peerless = silent no-op. The link overload below takes
  // the link explicitly for the main-loop/test seam.
  bool notifyPairingDone(LinkServer& link) {
    uint16_t seq = 0;
    return link.sendUnicast(linkcode::kTypePairDone, nullptr, 0, seq);
  }

  // Paired-state read for the transport-retained paths: the session flag
  // lives here.
  bool isPaired() const override { return paired_; }

  // Session-era read for the ALIVE/DISCONNECT handlers and the drive
  // publish pin (DRIVE carries no era).
  uint16_t era() const override { return era_; }

  // Last-consumed-counter read for the logging-only freshness pre-checks in
  // the opcode handlers. The authoritative freshness gate is inside tryConsume
  // (same thread, no interleaving between pre-check and commit).
  uint32_t lastCounter() const override { return lastCounter_; }

  // Atomic session-auth service: separates proving a frame belongs to the
  // session from acting on it. Folds paired_ + eraOk + counter freshness +
  // constant-time HMAC-SHA256(psk_, msg) == tag (first 6 bytes) into one
  // atomic step. On success consumes the counter, bumps validCount_, runs the
  // periodic saveCounter cadence, and returns a valid Verified; on ANY failure
  // returns invalid with zero state change.
  //
  // Callers pass eraOk per the connection::Checks mapping: drive -> true (no
  // era on the wire), alive/disconnect -> (wireEra == era()), unpair -> true
  // (no era on the wire). Refresh does NOT use this (bespoke: derivation +
  // savePairRefresh + switch), and unpair does NOT use this (it destroys the
  // session, so its verify stays inline in handleUnpair).
  //
  // Freshness is the wrap-aware strictly-greater idiom. All session tags are
  // first6, so the compare length is the literal 6. HMAC timing feeds the
  // shared maxVerifyUs_ funnel (throttled line, drive at 50Hz dominates).
  connection::Verified tryConsume(uint32_t ctr, bool eraOk, const uint8_t* tag, const uint8_t* msg,
                                  size_t msgLen) override {
    bool fresh = false;
    bool hmacOk = false;
    if (paired_ && eraOk && tag != nullptr && msg != nullptr && msgLen != 0) {
      fresh = (ctr != lastCounter_) && ((ctr - lastCounter_) < 0x80000000u);
      if (fresh) {
        uint8_t mac[32];
        const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        const uint32_t t0 = micros();
        int rc = -1;
        if (md != nullptr) {
          rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, msgLen, mac);
        }
        const uint32_t dt = micros() - t0;  // Unsigned subtraction: wrap-safe.
        if (dt > maxVerifyUs_) {
          maxVerifyUs_ = dt;
        }
        LOG_EVERY_N(128, "UDP", "drive verify dt=%luus max=%luus", static_cast<unsigned long>(dt),
                    static_cast<unsigned long>(maxVerifyUs_));
        hmacOk = (rc == 0);
        if (hmacOk) {
          hmacOk = pairing::ctEqual(mac, tag, 6);
        }
        mbedtls_platform_zeroize(mac, sizeof(mac));
      }
    }
    const connection::Checks checks{paired_, eraOk, fresh, hmacOk};
    connection::Verified verified = connection::AuthVerifier::verify(checks, ctr, era_);
    if (!verified.valid()) {
      return verified;  // Zero state change on any failure (fail-closed).
    }
    lastCounter_ = ctr;
    ++validCount_;
    // Robot-side persist cadence matches the interval advertised in PAIR_OK
    // (pairing::kPairPersistDefault); the app mirrors it per negotiated byte.
    if ((validCount_ % pairing::kPairPersistDefault) == 0) {
      ConfigStore::saveCounter(lastCounter_);  // Periodic persist, never per-packet (flash wear).
    }
    return verified;
  }

  // ALIVE verify: tag = first6(HMAC-SHA256(psk, "ALIVE"||eraBE2||counterBE4)).
  // Folds the same paired/era/fresh/HMAC checks through tryConsume.
  connection::Verified tryConsumeAlive(uint16_t wireEra, uint32_t ctr, const uint8_t* tag) override {
    uint8_t msg[pairing::kAliveMsgLen];
    pairing::aliveTagMsg(wireEra, ctr, msg);
    connection::Verified verified = tryConsume(ctr, wireEra == era_, tag, msg, sizeof(msg));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    return verified;
  }

  // DISCONNECT verify: tag = first6(HMAC-SHA256(psk, "DISCONNECT"||eraBE2||
  // counterBE4)). Folds the same paired/era/fresh/HMAC checks through
  // tryConsume.
  connection::Verified tryConsumeDisconnect(uint16_t wireEra, uint32_t ctr, const uint8_t* tag) override {
    uint8_t msg[pairing::kDisconnectMsgLen];
    pairing::disconnectTagMsg(wireEra, ctr, msg);
    connection::Verified verified = tryConsume(ctr, wireEra == era_, tag, msg, sizeof(msg));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    return verified;
  }

  // Explicit unpair / 10s-hold factory reset hook: clears the NVS pair +
  // in-memory Core + key material. Also invalidates the PAIR_OK redelivery
  // cache (+zeroizes its secrets).
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE.
  bool clearPair() {
    pairCore_.clearPair();
    paired_ = false;
    lastCounter_ = 0;
    conn_->clear();
    era_ = 0;
    validCount_ = 0;
    mbedtls_platform_zeroize(psk_, sizeof(psk_));
    lastPair_.valid = false;
    mbedtls_platform_zeroize(lastPair_.code6, sizeof(lastPair_.code6));
    mbedtls_platform_zeroize(lastPair_.cliNonce4, sizeof(lastPair_.cliNonce4));
    mbedtls_platform_zeroize(lastPair_.srvNonce4, sizeof(lastPair_.srvNonce4));
    mbedtls_platform_zeroize(lastPair_.ok39, sizeof(lastPair_.ok39));
    lastPair_.pairMs = 0;
    pendingValid_ = false;  // No open window after an unpair/reset.
    pairShowSent_ = false;  // Next window sends exactly once.
    if (link_ != nullptr) {
      notifyPairingDone(*link_);  // Fire-and-forget; peerless = silent no-op.
    }
    return ConfigStore::clearPair();
  }

  // Pair-family RX entry (transport -> ability): the UdpServer RX path calls
  // this (and ONLY this) for the pair family after its framing demux. The
  // header -> handler map lives here. Default is unreachable and fail-closed
  // silent.
  void handlePairFrame(uint8_t header, const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                       transport::IUdpTransport& t) override {
    switch (header) {
      case pairing::kPairReqHeader:
        handlePairReq(buf, rip, rport, nowMs, t);
        break;
      case pairing::kUnpairHeader:
        handleUnpair(buf, rip, rport, nowMs, t);
        break;
      case pairing::kPairRefreshReqHeader:
        handlePairRefresh(buf, rip, rport, nowMs, t);
        break;
      default:
        break;
    }
  }

  // PAIR_REQ layout [0x50|code6|clientNonce4|XOR] (len + header + XOR checked
  // by the caller). Paired -> PAIR_ERR already_paired. Else tryCode; kOk
  // mints a fresh PSK with store-before-send ordering (savePair FIRST, then
  // KEK->wrap->PAIR_OK unicast). Crypto/store failures after kOk answer
  // PAIR_ERR internal_retry_fresh (reason 4): the code is already burned, so
  // must pair again with a fresh display code. Genuine not_in_window still
  // sends reason 2. Every reply is unicast to the requester.
  void handlePairReq(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                     transport::IUdpTransport& t) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    // Lost-PAIR_OK race: while paired, an identical retry (same code +
    // client nonce) inside the redelivery window re-unicasts the cached
    // PAIR_OK. Checked BEFORE the already-paired reply below.
    if (paired_ && t.shouldRedeliver(lastPair_.valid, lastPair_.code6, lastPair_.cliNonce4, buf + 1, buf + 7,
                                     lastPair_.pairMs, nowMs)) {
      t.sendPairRedeliver(rip, rport, lastPair_.ok39, sizeof(lastPair_.ok39));
      return;
    }
    if (paired_) {
      uint8_t err[4];
      err[0] = 0x52;
      err[1] = pairing::toWireReason(pairing::Result::kAlreadyPaired);
      err[2] = 0;
      err[3] = static_cast<uint8_t>(err[0] ^ err[1] ^ err[2]);
      t.sendPairErr(rip, rport, err, sizeof(err));
      dropFirst(firstPairDrop_, "pair req while paired (first of boot)");
      LOG_EVERY_N(10, "UDP", "pair req while paired");
      return;
    }
    const pairing::Result r = pairCore_.tryCode(buf + 1, pairing::kCodeLen, nowMs);
    if (r != pairing::Result::kOk) {
      const uint8_t retryS = (r == pairing::Result::kLocked) ? static_cast<uint8_t>(pairing::kLockoutMs / 1000u) : 0;
      uint8_t err[4];
      err[0] = 0x52;
      err[1] = pairing::toWireReason(r);
      err[2] = retryS;
      err[3] = static_cast<uint8_t>(err[0] ^ err[1] ^ err[2]);
      t.sendPairErr(rip, rport, err, sizeof(err));
      dropFirst(firstPairDrop_, "pair err reason=%u (first of boot)", static_cast<unsigned>(pairing::toWireReason(r)));
      LOG_EVERY_N(10, "UDP", "pair err reason=%u", static_cast<unsigned>(pairing::toWireReason(r)));
      return;
    }
    // Code verified + burned inside Core. Mint the pair: 16 PSK bytes + 4
    // server-nonce bytes from esp_random (CSPRNG) into stack buffers.
    uint8_t newPsk[pairing::kPskLen];
    for (size_t i = 0; i < pairing::kPskLen / 4; ++i) {
      const uint32_t rnd = esp_random();
      newPsk[4 * i] = static_cast<uint8_t>((rnd >> 24) & 0xFFu);
      newPsk[4 * i + 1] = static_cast<uint8_t>((rnd >> 16) & 0xFFu);
      newPsk[4 * i + 2] = static_cast<uint8_t>((rnd >> 8) & 0xFFu);
      newPsk[4 * i + 3] = static_cast<uint8_t>(rnd & 0xFFu);
    }
    uint8_t serverNonce[pairing::kPairNonceLen];
    {
      const uint32_t rnd = esp_random();
      serverNonce[0] = static_cast<uint8_t>((rnd >> 24) & 0xFFu);
      serverNonce[1] = static_cast<uint8_t>((rnd >> 16) & 0xFFu);
      serverNonce[2] = static_cast<uint8_t>((rnd >> 8) & 0xFFu);
      serverNonce[3] = static_cast<uint8_t>(rnd & 0xFFu);
    }
    const uint8_t* code6 = buf + 1;  // Verified match — the HKDF IKM.
    const uint8_t* clientNonce = buf + 7;
    bool sent = false;
    uint8_t out[pairing::kPairOkSize] = {};  // Function scope: cached into lastPair_ on send.
    // Pair baseline is counter 0 (never valid on the wire): the first
    // DRIVE uses counter 1. The receipt overload mints the
    // store-before-send proof consumed by sendPairOk (invalid until the NVS
    // save lands).
    connection::PairingReceipt receipt;
    if (ConfigStore::savePair(newPsk, 0, receipt)) {
      uint8_t kek[pairing::kKekLen];
      uint8_t ct[pairing::kPairCtLen];
      uint8_t tag[pairing::kPairTagLen];
      if (pairing::deriveKek(code6, clientNonce, serverNonce, kek) &&
          pairing::wrapPsk(newPsk, kek, clientNonce, serverNonce, pairing::kPairPersistDefault, ct, tag)) {
        // PAIR_OK 39B [0x51|server_nonce4|ct17|tag16|XOR]: ct covers
        // psk16||persist_u8 (v1.1 addendum; old 38B frames have no byte).
        out[0] = pairing::kPairOkHeader;
        for (size_t i = 0; i < pairing::kPairNonceLen; ++i) {
          out[1 + i] = serverNonce[i];
        }
        for (size_t i = 0; i < pairing::kPairCtLen; ++i) {
          out[5 + i] = ct[i];
        }
        for (size_t i = 0; i < pairing::kPairTagLen; ++i) {
          out[5 + pairing::kPairCtLen + i] = tag[i];
        }
        uint8_t outXor = 0;
        for (size_t i = 0; i < sizeof(out) - 1; ++i) {
          outXor ^= out[i];
        }
        out[sizeof(out) - 1] = outXor;
        t.sendPairOk(std::move(receipt), rip, rport, out, sizeof(out));
        sent = true;
      }
      mbedtls_platform_zeroize(kek, sizeof(kek));
      mbedtls_platform_zeroize(ct, sizeof(ct));
      mbedtls_platform_zeroize(tag, sizeof(tag));
    }
    if (sent) {
      for (size_t i = 0; i < sizeof(psk_); ++i) {
        psk_[i] = newPsk[i];
      }
      lastCounter_ = 0;
      era_ = 0;  // Fresh pair starts era 0 (savePair persists it).
      paired_ = true;
      pendingValid_ = false;      // Code burned: the window is consumed.
      pairShowSent_ = false;      // Closed window unlatches the pump.
      pairCore_.onPaired(nowMs);  // PairingExchange forward (same point, same order; idempotent).
      // Cache the reply BEFORE the stack zeroizes below: an identical retry
      // inside kRedeliveryMs re-unicasts these bytes (RAM-only redelivery).
      for (size_t i = 0; i < sizeof(lastPair_.code6); ++i) {
        lastPair_.code6[i] = code6[i];
      }
      for (size_t i = 0; i < sizeof(lastPair_.cliNonce4); ++i) {
        lastPair_.cliNonce4[i] = clientNonce[i];
      }
      for (size_t i = 0; i < sizeof(lastPair_.srvNonce4); ++i) {
        lastPair_.srvNonce4[i] = serverNonce[i];
      }
      for (size_t i = 0; i < sizeof(lastPair_.ok39); ++i) {
        lastPair_.ok39[i] = out[i];
      }
      lastPair_.pairMs = nowMs;
      lastPair_.valid = true;
      *peerIp_ = rip;  // The new owner pins the telemetry unicast path.
      *peerPort_ = rport;
      conn_->pinPeer(connection::ipToU32(rip[0], rip[1], rip[2], rip[3]));
      if (link_ != nullptr) {
        notifyPairingDone(*link_);  // Fire-and-forget; peerless = silent no-op.
      }
      LOG_I("UDP", "pair ok");
    } else {
      // internal_retry_fresh (reason 4): the code is already burned
      // (single-use), so an NVS or crypto failure after kOk sends 4, not 2.
      // The client pairs again with a fresh display code. Reasons 0-3 are
      // frozen (genuine not_in_window still sends 2).
      uint8_t err[4];
      err[0] = 0x52;
      err[1] = pairing::toWireReason(pairing::Result::kInternal);
      err[2] = 0;
      err[3] = static_cast<uint8_t>(err[0] ^ err[1] ^ err[2]);
      t.sendPairErr(rip, rport, err, sizeof(err));
      dropFirst(firstPairDrop_, "pair err internal (first of boot)");
      LOG_EVERY_N(10, "UDP", "pair err internal");
    }
    mbedtls_platform_zeroize(newPsk, sizeof(newPsk));
    mbedtls_platform_zeroize(serverNonce, sizeof(serverNonce));
  }

  // UNPAIR layout [0xAD|ctrBE4|tag6|XOR] (len + header + XOR checked by the
  // caller). Symmetric forget: a signed fire-and-forget from the paired
  // owner. Fail = silent drop. Pass clears the NVS pair + Core + key material
  // + redelivery cache and stays idle; only the physical pair button opens
  // the next window. No reply packet.
  void handleUnpair(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                    transport::IUdpTransport& t) {
    (void)rip;
    (void)rport;
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!paired_) {
      dropFirst(firstUnpairDrop_, "drop unpair unpaired (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop unpair unpaired");
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[1]) << 24) | (static_cast<uint32_t>(buf[2]) << 16) |
                         (static_cast<uint32_t>(buf[3]) << 8) | static_cast<uint32_t>(buf[4]);
    if (!t.isFreshCounter(lastCounter_, ctr)) {
      dropFirst(firstUnpairDrop_, "drop unpair replay ctr=%lu (first of boot)", static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop unpair replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(psk, "UNPAIR"||counterBE4)).
    uint8_t msg[pairing::kUnpairMsgLen];
    pairing::unpairTagMsg(ctr, msg);
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 5, pairing::kUnpairTagLen);
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstUnpairDrop_, "drop unpair auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop unpair auth");
      return;
    }
    // Fold the passed prechecks into Verified (per-cause drops+logs above
    // stay verbatim), consumed by the refresh-then-clear below.
    const connection::Checks unpairChecks{paired_, true, t.isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified unpairVerified = connection::AuthVerifier::verify(unpairChecks, ctr, era_);
    ConfigStore::clearPair();
    pairCore_.onUnpaired();  // PairingExchange forward (superset of clearPair: +era/pairMs reset).
    mbedtls_platform_zeroize(psk_, sizeof(psk_));
    paired_ = false;
    lastCounter_ = 0;
    era_ = 0;
    // Refresh-then-clear: the signed goodbye still proves the owner was alive
    // at nowMs; the net peer is gone.
    conn_->refreshThenClear(std::move(unpairVerified), nowMs);
    lastPair_.valid = false;
    mbedtls_platform_zeroize(lastPair_.code6, sizeof(lastPair_.code6));
    mbedtls_platform_zeroize(lastPair_.cliNonce4, sizeof(lastPair_.cliNonce4));
    mbedtls_platform_zeroize(lastPair_.srvNonce4, sizeof(lastPair_.srvNonce4));
    mbedtls_platform_zeroize(lastPair_.ok39, sizeof(lastPair_.ok39));
    lastPair_.pairMs = 0;
    pendingValid_ = false;  // Window consumed; reopen stages the fresh code.
    pairShowSent_ = false;
    if (link_ != nullptr) {
      notifyPairingDone(*link_);  // Fire-and-forget; peerless = silent no-op.
    }
    LOG_I("UDP", "unpair ok, idle");
  }

  // PAIR_REFRESH_REQ layout [0x53|eraBE2|ctrBE4|tag6|XOR] (len + header + XOR
  // checked by the caller). Silent Tier-1 rotation: the app derives PSK_next
  // under the old psk and asks to move to era+1; the robot verifies (paired
  // -> era is exactly next -> counter fresh -> HMAC under the old psk),
  // persists BEFORE the ACK, then switches atomically and replies PAIR_REFRESH_ACK.
  // Next drive uses counter 1 under the new PSK. A failed NVS save answers
  // nothing (the app retries). Old PSK is dropped immediately on switch (no
  // reorder window). Duplicate REQ with era == current era re-ACKs
  // idempotently without re-deriving or re-verifying (the ACK carries no
  // secret). Any other era is a silent drop. No reply on failure.
  void handlePairRefresh(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                         transport::IUdpTransport& t) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!paired_) {
      dropFirst(firstPairRefreshDrop_, "drop pair-refresh unpaired (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop pair-refresh unpaired");
      return;
    }
    const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 8) | static_cast<uint16_t>(buf[2]));
    if (era == era_) {
      // Idempotent duplicate: re-ACK WITHOUT re-deriving or re-verifying
      // (the lost-ACK retry reuses the same era; the ACK carries no secret).
      // Explicitly bypasses the Pinned gate below — documented, by design.
      uint8_t ack[pairing::kPairRefreshAckSize];
      if (composePairRefreshAck(era_, ack)) {
        t.sendPairRefreshAckImpl(rip, rport, ack, sizeof(ack));
      }
      LOG_EVERY_N(20, "UDP", "pair-refresh redeliver era=%u", static_cast<unsigned>(era_));
      return;
    }
    if (era != static_cast<uint16_t>(era_ + 1)) {
      dropFirst(firstPairRefreshDrop_, "drop pair-refresh era=%u have=%u (first of boot)", static_cast<unsigned>(era),
                static_cast<unsigned>(era_));
      LOG_EVERY_N(20, "UDP", "drop pair-refresh era=%u have=%u", static_cast<unsigned>(era),
                  static_cast<unsigned>(era_));
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[3]) << 24) | (static_cast<uint32_t>(buf[4]) << 16) |
                         (static_cast<uint32_t>(buf[5]) << 8) | static_cast<uint32_t>(buf[6]);
    if (!t.isFreshCounter(lastCounter_, ctr)) {
      dropFirst(firstPairRefreshDrop_, "drop pair-refresh replay ctr=%lu (first of boot)",
                static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop pair-refresh replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(oldPsk, "PAIRREFRESH"||eraBE2||counterBE4)).
    uint8_t msg[pairing::kPairRefreshMsgLen];
    pairing::pairRefreshTagMsg(era, ctr, msg);
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 7, pairing::kPairRefreshTagLen);
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstPairRefreshDrop_, "drop pair-refresh auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop pair-refresh auth");
      return;
    }
    // Fold the passed prechecks into Verified (per-cause drops+logs above
    // stay verbatim).
    const connection::Checks pairRefreshChecks{paired_, era == static_cast<uint16_t>(era_ + 1),
                                               t.isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified pairRefreshVerified = connection::AuthVerifier::verify(pairRefreshChecks, ctr, era);
    uint8_t nextPsk[pairing::kPskLen];
    if (!pairing::derivePairRefreshPsk(psk_, era, nextPsk)) {
      dropFirst(firstPairRefreshDrop_, "drop pair-refresh derive (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop pair-refresh derive");
      return;
    }
    if (!ConfigStore::savePairRefresh(nextPsk, era, 0)) {
      mbedtls_platform_zeroize(nextPsk, sizeof(nextPsk));
      dropFirst(firstPairRefreshDrop_, "drop pair-refresh persist (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop pair-refresh persist");
      return;
    }
    for (size_t i = 0; i < sizeof(psk_); ++i) {
      psk_[i] = nextPsk[i];
    }
    mbedtls_platform_zeroize(nextPsk, sizeof(nextPsk));
    era_ = era;
    pairCore_.onPairRefreshApplied(era);  // PairingExchange forward (same point, same order).
    lastCounter_ = 0;
    validCount_ = 0;
    // Alive funnel BEFORE the ACK below.
    connection::Pinned pairRefreshPinned = t.markAlive(std::move(pairRefreshVerified), rip, rport, nowMs, era_);
    uint8_t ack[pairing::kPairRefreshAckSize];
    if (composePairRefreshAck(era_, ack)) {
      t.sendPairRefreshAck(std::move(pairRefreshPinned), rip, rport, ack, sizeof(ack));
    }
    LOG_I("UDP", "pair-refresh ok era=%u", static_cast<unsigned>(era_));
  }

 private:
  // PAIR_REFRESH_ACK 10B composer: [0x54|eraBE2|tag6|XOR] with tag =
  // first6(HMAC-SHA256(newPsk, "PAIRREFRESHACK"||eraBE2)). `psk_` IS the new key at
  // both call sites. False on HMAC failure (no send). Key material never
  // logged.
  bool composePairRefreshAck(uint16_t era, uint8_t out[pairing::kPairRefreshAckSize]) {
    uint8_t msg[pairing::kPairRefreshAckMsgLen];
    pairing::pairRefreshAckTagMsg(era, msg);
    uint8_t mac[32];
    out[0] = pairing::kPairRefreshAckHeader;
    out[1] = static_cast<uint8_t>((era >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>(era & 0xFFu);
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    if (rc != 0) {
      mbedtls_platform_zeroize(mac, sizeof(mac));
      return false;
    }
    for (size_t i = 0; i < pairing::kPairRefreshTagLen; ++i) {
      out[3 + i] = mac[i];
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    uint8_t xorSum = 0;
    for (size_t i = 0; i + 1 < pairing::kPairRefreshAckSize; ++i) {
      xorSum ^= out[i];
    }
    out[pairing::kPairRefreshAckSize - 1] = xorSum;
    return true;
  }

  // First occurrence per boot per class always logs at INFO; afterwards the
  // call-site LOG_EVERY_N throttle applies. No key/code/nonce material here.
  template <typename... Args>
  void dropFirst(bool& first, const char* fmt, Args... args) {
    if (first) {
      first = false;
      LOG_I("UDP", fmt, args...);
    }
  }

  // Transport-owned shared state (pinned by attach(); never owned here).
  connection::ConnectionManager* conn_;
  IPAddress* peerIp_;
  uint16_t* peerPort_;
  // Core->head link endpoint (pinned by attachHeadLink(); never owned here).
  // Null until setup() pins it — every notify site null-checks.
  LinkServer* link_;
  pairing::Core pairCore_;
  uint8_t psk_[16];
  uint32_t lastCounter_;
  bool paired_;
  // Tier-1 pair-refresh era (u16, NVS; fresh pair resets to 0). DRIVE carries no
  // era — post-switch old-key frames drop via HMAC.
  uint16_t era_;
  uint32_t validCount_;
  uint32_t maxVerifyUs_;
  // Staged 6-digit code of the current open window (6 ASCII digits +
  // valid-seconds u16BE; see packPairShow in shared/link/link_codec.h).
  // Copied on every window open so the main-loop pump reads it without
  // re-generating. Invalid whenever no window is open.
  uint8_t pendingCode_[pairing::kCodeLen] = {};
  bool pendingValid_ = false;
  // Once-per-window PairShow link send latch: set by publishPairShow when a
  // frame is handed to the radio; cleared when the window closes or the pair
  // clears. Main-loop-written.
  bool pairShowSent_ = false;
  // Last PAIR_OK redelivery cache (RAM-only, never persisted): an identical
  // PAIR_REQ inside kRedeliveryMs re-unicasts the cached bytes. Invalidated
  // on reboot, clearPair, and signed UNPAIR; secrets zeroized alongside psk_.
  struct LastPair {
    bool valid = false;
    uint8_t code6[6] = {};
    uint8_t cliNonce4[4] = {};
    uint8_t srvNonce4[4] = {};
    uint8_t ok39[pairing::kPairOkSize] = {};
    uint32_t pairMs = 0;
  };
  // The redelivery cache holds exactly one PAIR_OK frame — a size drift here
  // breaks the unicast on the lost-PAIR_OK race.
  static_assert(sizeof(LastPair::ok39) == pairing::kPairOkSize, "LastPair cache must hold exactly one PAIR_OK frame");
  LastPair lastPair_;
  // First-drop flags: first occurrence per boot per class always logs.
  // Reset only by reboot.
  bool firstPairDrop_ = true;
  bool firstUnpairDrop_ = true;
  bool firstPairRefreshDrop_ = true;
};

}  // namespace pairing

#endif  // ARDUINO
