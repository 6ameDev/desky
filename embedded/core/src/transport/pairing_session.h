#pragma once
// Transport-owned UDP<->pairing seam — header-only, firmware-only.
//
// Declares the two sides of the RX-path contract as abstract interfaces so
// the dependency points ability->transport (downward, dumb pipes stay dumb):
// UdpServer implements IUdpTransport, pairing::PairingTask implements
// IPairingSession. Neither header names the other.
//
// Sending passes plain bytes (pointer + length): the wire sizes stay owned
// by the pairing codec, where the frames are composed. Tokens keep their
// move-only funnel (Verified in, Pinned out).

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>

#include "pairing_receipt.h"
#include "transport/connection_manager.h"

namespace transport {

// Socket back-calls implemented by UdpServer: byte composition lives in the
// ability, the gate-check + unicast stays in transport.
class IUdpTransport {
 public:
  virtual ~IUdpTransport() = default;

  // Alive funnel for one verified paired packet: pins the telemetry peer +
  // refreshes the stamp. Consumes the Verified token; invalid token keeps
  // every byte of state.
  virtual connection::Pinned markAlive(connection::Verified&& verified, const IPAddress& rip, uint16_t rport,
                                       uint32_t nowMs, uint16_t era) = 0;
  // PAIR_OK unicast: consumes the PairingReceipt (proves the pair hit NVS
  // before anything left the radio). Invalid receipt stays silent.
  virtual void sendPairOk(connection::PairingReceipt receipt, const IPAddress& ip, uint16_t port, const uint8_t* ok,
                          size_t len) = 0;
  // PAIR_OK redelivery unicast (lost-PAIR_OK race).
  virtual void sendPairRedeliver(const IPAddress& ip, uint16_t port, const uint8_t* ok, size_t len) = 0;
  // PAIR_ERR unicast (composed by the ability; failure replies ride no gate).
  virtual void sendPairErr(const IPAddress& ip, uint16_t port, const uint8_t* err, size_t len) = 0;
  // Core-originated DISCONNECT unicast (physical pair-button eviction):
  // composed ability-side, fire-and-forget here with no gate and no retry.
  virtual void sendDisconnect(const IPAddress& ip, uint16_t port, const uint8_t* frame, size_t len) = 0;
  // EXPR_LIST_RESP unicast (expression picker list): composed ability-side
  // from the shared catalog, single send to the requester with no retry.
  virtual void sendExprList(const IPAddress& ip, uint16_t port, const uint8_t* frame, size_t len) = 0;
  // PAIR_REFRESH_ACK unicast, composed by the ability. The token-gated send is
  // the only path the verified switch takes; the Impl path re-ACKs the
  // idempotent duplicate (the ACK carries no secret, so it bypasses the gate).
  virtual void sendPairRefreshAck(connection::Pinned pinned, const IPAddress& ip, uint16_t port, const uint8_t* ack,
                                  size_t len) = 0;
  virtual void sendPairRefreshAckImpl(const IPAddress& ip, uint16_t port, const uint8_t* ack, size_t len) = 0;
  // Wrap-aware counter freshness + PAIR_OK redelivery predicates (single
  // owner stays in the transport top half; bridged here for the handlers).
  virtual bool isFreshCounter(uint32_t lastSeen, uint32_t got) = 0;
  virtual bool shouldRedeliver(bool valid, const uint8_t* storedCode, const uint8_t* storedNonce, const uint8_t* inCode,
                               const uint8_t* inNonce, uint32_t pairMs, uint32_t nowMs) = 0;
};

// Pairing/auth session implemented by pairing::PairingTask: pair state,
// counter/era reads, the atomic verify service, and the pair-family RX
// entry. Every read on the RX path is null-safe until attach lands.
class IPairingSession {
 public:
  virtual ~IPairingSession() = default;

  // Pins the transport-owned shared state (peer slot + alive window).
  virtual void attach(connection::ConnectionManager* conn, IPAddress* peerIp, uint16_t* peerPort) = 0;
  // Pair restore: paired -> verifies against the stored key; unpaired ->
  // opens a pairing window.
  virtual void restorePair() = 0;
  virtual bool isPaired() const = 0;
  virtual uint16_t era() const = 0;
  virtual uint32_t fromAppCounter() const = 0;
  // Atomic session verify: folds paired + eraOk + freshness + HMAC into one
  // step. On success consumes the counter and mints a valid token; on any
  // failure returns invalid with zero state change.
  virtual connection::Verified tryConsume(uint32_t ctr, bool eraOk, const uint8_t* tag, const uint8_t* msg,
                                          size_t msgLen) = 0;
  // Keepalive/goodbye verifies: compose the era-bound tag message
  // ability-side ("ALIVE"/"DISCONNECT" domain separators are pairing-protocol
  // vocabulary), then fold through the same atomic tryConsume step. Zero
  // state change on any failure.
  virtual connection::Verified tryConsumeAlive(uint16_t wireEra, uint32_t ctr, const uint8_t* tag) = 0;
  virtual connection::Verified tryConsumeDisconnect(uint16_t wireEra, uint32_t ctr, const uint8_t* tag) = 0;
  // Alive-fresh poll for the MANUAL exit rule: true while the transport-owned
  // peer slot reads alive at nowMs (same window the telemetry gate uses).
  // False until attach lands.
  virtual bool isAliveFresh(uint32_t nowMs) const = 0;
  // Pair-family RX entry (transport -> ability): the header -> handler map
  // lives in the ability.
  virtual void handlePairFrame(uint8_t header, const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                               IUdpTransport& t) = 0;
};

}  // namespace transport

#endif  // ARDUINO
