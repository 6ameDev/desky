#pragma once
// Core<->head link endpoint — header-only. Owns the core-side link sockets +
// one Core 0 FreeRTOS task. Sockets only; implements the frozen B-link
// contract in shared/link (codec never touched from this file).
//
//   UDP 3334: RX link frames (FACE/OLED/CAMCTL/HB/ACK), CRC/seq validated via
//     the codec. Seq freshness via linkcode::isFreshSeq on a single shared
//     head->core seq domain (HB + staged commands share head's counter;
//     received ACKs ride outside it). Duplicate seq re-ACKs idempotently
//     without re-executing. Valid HB replies HB_ACK. Valid FACE/OLED/CAMCTL
//     stage into LastLinkCmd for the main loop — applying is the LOG_I
//     command line. Unknown types land in the unknown-type drop class.
//   TCP 3335: JPEG head->core stream, ONE client at a time. Length-framed via
//     the link header: bytes drain into a STATIC 16KB stream buffer, complete
//     frames pulled with pullFrame, per-frame CRC via the codec, fragments
//     reassembled via Reassembler into a second STATIC 16KB buffer. Validated
//     JPEG bytes are checksummed + counted, then DROPPED — no consumer yet.
//
// Big buffers are LinkServer MEMBERS (setup()-time global -> BSS, never
// stack, never heap). The UDP path uses a 1034B stack frame; everything else
// is counters. No heap/String anywhere in the RX/TX path.
//
// PairShow pump: the pairing window's 6-digit code + valid-seconds stage in
// PairingTask; the main loop calls publishPairShow, which composes the 8B
// PairShow payload and hands one frame per window to sendUnicast. Completion
// goes out as the empty PairDone frame via notifyPairingDone. No head linked
// -> the pump waits silently; window closed -> unlatch for the next window.
// Duplicate protection is the seq/ACK machinery on both ends.
//
// Carrier OTA trigger (same split): the `otahead` URL stage lives in
// OtaStaging; the main loop calls pumpOtaLink, which hands one OTA frame per
// command to sendUnicast. Latest command wins; a head mid-download ignores
// the re-trigger via its in-flight guard. URL bytes are validated by the
// shared otamanifest grammar and never logged.
//
// Direction map: PAIRSHOW + PAIRDONE + EXPR ride the core->head tx domain
// (sendUnicast). The three never share a counter.
//
// Auth: NONE beyond CRC+seq — transport-only trust, exactly like the AP
// passphrase. Never logs payload bytes beyond counters, never touches UART.

// Rules: WDT fed every tick, logWatermarks("LINK") every ~5s, task never
// halts. (A halted WDT-subscribed task becomes a panic-reboot loop.)
// Priority +2, same as audio, BELOW UdpServer +3. begin() runs AFTER
// UdpServer::begin and BEFORE the BOOT banner; failure LOG_Ws and the boot
// continues headless — the link must never brick driving.

// ── Arduino-free: seq verdict + TCP stream framing + type names ──────────
#include <stddef.h>
#include <stdint.h>

#include "common/udp_send.h"

// The catalog lands at global scope first: link_codec.h pulls it too, and a
// namespace-wrapped first inclusion would hide ::exprcat from other headers.
#include "link/expression_catalog.h"

// The frozen codec namespace `link` collides with POSIX ::link() via the
// WiFi/lwIP headers, so it is included once inside linkwrap__ and aliased to
// linkcode:: on ALL platforms (host goldens still pin the `link::` spelling).
namespace linkwrap__ {
#include "link/link_codec.h"
}  // namespace linkwrap__
namespace linkcode = linkwrap__::link;

#include "link/ota_manifest.h"

// Fallbacks keep the host tests + firmware compiling if config keys drift;
// in-project include/config.h always wins (same pattern as pairing_codec.h).
#ifndef CFG_LINK_UDP_PORT
#define CFG_LINK_UDP_PORT 3334
#endif
#ifndef CFG_LINK_TCP_PORT
#define CFG_LINK_TCP_PORT 3335
#endif
#ifndef CFG_LINK_STACK_WORDS
#define CFG_LINK_STACK_WORDS 4096
#endif
#ifndef CFG_LINK_PRIORITY_OFFSET
#define CFG_LINK_PRIORITY_OFFSET 2
#endif
#ifndef CFG_LINK_CORE
#define CFG_LINK_CORE 0
#endif
#ifndef CFG_LINK_LOOP_MS
#define CFG_LINK_LOOP_MS 10
#endif

namespace linkserver {

// Seq gate verdict for one inbound head->core frame (host-testable). Fresh =
// first-ever or wrap-aware strictly greater; Duplicate = exact retransmit
// (caller re-ACKs idempotently without re-executing); Stale = replay/older.
enum class SeqVerdict : uint8_t { kFresh, kDuplicate, kStale };

inline SeqVerdict classifySeq(bool haveSeen, uint16_t lastSeen, uint16_t got) {
  if (!haveSeen) {
    return SeqVerdict::kFresh;
  }
  if (got == lastSeen) {
    return SeqVerdict::kDuplicate;
  }
  return linkcode::isFreshSeq(lastSeen, got) ? SeqVerdict::kFresh : SeqVerdict::kStale;
}

// Boot-epoch gate (host-testable): the head sends its per-boot bootId in
// every HB. A changed (or first-seen) epoch resyncs: accept ANY seq (pin
// peer, ACK) instead of dropping low seqs as stale forever.
inline bool bootEpochChanged(bool haveBoot, uint32_t lastBoot, uint32_t gotBoot) {
  return !haveBoot || lastBoot != gotBoot;
}

// TCP stream framing result (host-testable): NeedMore = incomplete frame
// buffered; HaveFrame = one complete frame at buf[0]; Resync = bad magic or
// oversize len (caller drops one byte and retries).
enum class Pull : uint8_t { kNeedMore, kHaveFrame, kResync };

inline Pull pullFrame(const uint8_t* buf, size_t buffered, size_t& frameLenOut) {
  frameLenOut = 0;
  if (buf == nullptr || buffered < linkcode::kOverhead) {
    return Pull::kNeedMore;
  }
  if (buf[0] != linkcode::kMagic) {
    return Pull::kResync;
  }
  const size_t payloadLen = static_cast<size_t>(linkcode::getU16BE(buf + 6));
  if (payloadLen > linkcode::kMaxPayload) {
    return Pull::kResync;
  }
  const size_t total = linkcode::kOverhead + payloadLen;
  if (buffered < total) {
    return Pull::kNeedMore;
  }
  frameLenOut = total;
  return Pull::kHaveFrame;
}

// Log-only type name (never payload bytes). "UNKNOWN" outside the contract.
inline const char* typeName(uint8_t type) {
  switch (type) {
    case linkcode::kTypeFace:
      return "FACE";
    case linkcode::kTypeOled:
      return "OLED";
    case linkcode::kTypeCamCtl:
      return "CAMCTL";
    case linkcode::kTypePairShow:
      return "PAIRSHOW";
    case linkcode::kTypePairDone:
      return "PAIRDONE";
    case linkcode::kTypeExpr:
      return "EXPR";
    case linkcode::kTypeOta:
      return "OTA";
    case linkcode::kTypeJpeg:
      return "JPEG";
    case linkcode::kTypeHb:
      return "HB";
    case linkcode::kTypeHbAck:
      return "HB_ACK";
    case linkcode::kTypeAck:
      return "ACK";
    default:
      return "UNKNOWN";
  }
}

// OTA send gate (host-testable): staged && !alreadySent && haveHeadPeer.
inline bool otaReadyToSend(bool stagedValid, bool alreadySent, bool haveHeadPeer) {
  return stagedValid && !alreadySent && haveHeadPeer;
}

// HB peer pin rule (host-testable): every non-stale HB refreshes the target;
// stale replays never move it.
inline bool pinOnVerdict(SeqVerdict v) { return v != SeqVerdict::kStale; }

}  // namespace linkserver

#ifdef ARDUINO
// ── Firmware: link endpoint task (UDP + TCP sockets, Core 0) ───────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>

#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"

class LinkServer {
 public:
  static constexpr uint16_t kUdpPort = CFG_LINK_UDP_PORT;
  static constexpr uint16_t kTcpPort = CFG_LINK_TCP_PORT;
  static constexpr uint32_t kStackWords = CFG_LINK_STACK_WORDS;
  // Same level as audio +2, BELOW UdpServer +3: the phone-wire 5ms tick +
  // HMAC-verify budget must never be stolen by link work, and link work must
  // never preempt the Core 1 real-time slot.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + CFG_LINK_PRIORITY_OFFSET;
  static constexpr BaseType_t kCore = CFG_LINK_CORE;
  static constexpr uint32_t kLoopMs = CFG_LINK_LOOP_MS;
  // Static buffer caps (BSS members below, never stack/heap): the stream
  // buffer holds raw TCP bytes (many back-to-back 1034B-max frames fit); the
  // message buffer holds one reassembled JPEG (multi-frame head captures up
  // to the cap; larger messages drop via the Reassembler overflow reject).
  static constexpr size_t kTcpStreamCap = 16384;
  static constexpr size_t kJpegMsgCap = 16384;
  static constexpr uint32_t kJpegLogEveryN = 30;
  // HB_ACK era/resync field: no refresh domain on the transport-only link
  // (keyed link later) — always 0, echoed channel comes from the HB itself.
  static constexpr uint32_t kHbAckEra = 0;

  // Staged head->core command handoff. Bounded: payload never exceeds
  // kMaxPayload (enforced by the codec before the copy). No EventBus publish
  // by design — the staged struct IS the handoff.
  struct LastLinkCmd {
    bool valid = false;
    uint8_t type = 0;
    uint16_t seq = 0;
    uint16_t len = 0;
    uint8_t payload[linkcode::kMaxPayload] = {};
  };

  LinkServer() : task_(nullptr), mutex_(nullptr), server_(kTcpPort) {}

  // Binds UDP 3334 + TCP 3335 and pins the link task to Core 0. Call once in
  // setup(), AFTER UdpServer::begin (sockets ride the already-up stack; no
  // WiFi mode owned here) and BEFORE the BOOT banner lines, with logging up.
  // Logs ONE BOOT line (ports only). False + caller LOG_W on bind/mutex/task
  // failure — the caller continues headless, never bricks driving.
  bool begin() {
    const bool udpOk = udp_.begin(kUdpPort);
    DESKY_ASSERT(udpOk);
    if (!udpOk) {
      return false;
    }
    server_.begin();
    mutex_ = xSemaphoreCreateMutex();
    DESKY_ASSERT(mutex_ != nullptr);
    if (mutex_ == nullptr) {
      return false;
    }
    reasm_.reset(jpegMsg_, sizeof(jpegMsg_));
    LOG_I("BOOT", "link ready udp=%u tcp=%u", static_cast<unsigned>(kUdpPort), static_cast<unsigned>(kTcpPort));
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&LinkServer::taskEntry, "link", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  // Mutex-guarded copy of the last staged command for the main loop / future
  // OLED bridge. False when no command has staged yet (or begin failed).
  // Task context only, never ISR.
  bool snapshotCmd(LastLinkCmd& out) const {
    if (mutex_ == nullptr) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    out = lastCmd_;
    xSemaphoreGive(mutex_);
    return out.valid;
  }

  uint32_t framesRx() const { return framesRx_; }
  uint32_t bytesRx() const { return bytesRx_; }
  uint32_t lastFps() const { return lastFps_; }
  uint32_t udpRx() const { return udpRx_; }
  uint32_t udpDrop() const { return udpDrop_; }
  uint32_t hbRx() const { return hbRx_; }
  uint32_t cmdRx() const { return cmdRx_; }
  uint32_t ackRx() const { return ackRx_; }
  uint32_t ackTx() const { return ackTx_; }
  uint32_t hbAckTx() const { return hbAckTx_; }
  uint32_t otaTx() const { return otaTx_; }
  uint32_t txFail() const { return txFail_; }
  uint32_t tcpDrop() const { return tcpDrop_; }
  uint16_t lastSeq() const { return lastSeq_; }
  uint32_t lastBootId() const { return lastBootId_; }
  uint16_t txSeq() const { return txSeq_; }

  // HB-pinned head peer (snapshotCmd-style accessor): unicast target for
  // core->head frames. False while no head has checked in. Benign
  // tear-tolerant read like the counters above; task context only, never ISR.
  bool headPeer(IPAddress& ip, uint16_t& port) const {
    if (!haveHeadPeer_) {
      return false;
    }
    ip = headIp_;
    port = headPort_;
    return true;
  }

  bool hasHeadPeer() const { return haveHeadPeer_; }

  // Core->head unicast for the ability pumps (PairShow/PairDone + OTA stages
  // live ability-side — this class holds no business state, only sockets +
  // framing + the pinned peer + the tx counter). Mints the frame seq under
  // lock, encodes, and unicasts to the HB-pinned head peer. False when
  // pre-begin, peerless, or the radio refused the datagram. seqOut carries
  // the minted seq (0 when pre-begin/peerless). Empty payloads are legal.
  bool sendUnicast(uint8_t type, const uint8_t* payload, uint16_t payloadLen, uint16_t& seqOut) {
    seqOut = 0;
    if (mutex_ == nullptr || !haveHeadPeer_) {
      return false;
    }
    const char* what = "frame";
    switch (type) {
      case linkcode::kTypePairShow:
        what = "pairshow";
        break;
      case linkcode::kTypePairDone:
        what = "pairdone";
        break;
      case linkcode::kTypeExpr:
        what = "expr";
        break;
      case linkcode::kTypeOta:
        what = "ota";
        break;
      case linkcode::kTypeHbAck:
        what = "hb_ack";
        break;
      case linkcode::kTypeAck:
        what = "ack";
        break;
      default:
        break;
    }
    return sendTo(type, payload, payloadLen, headIp_, headPort_, seqOut, what) == SendVerdict::kOk;
  }

 private:
  // Single core->head send choke point (txSeq single owner): mints the seq
  // under lock, encodes, and unicasts — serializing the shared WiFiUDP
  // object across the link task and the main-loop ability pumps.
  enum class SendVerdict : uint8_t { kOk, kEncodeFail, kSendFail };

  SendVerdict sendTo(uint8_t type, const uint8_t* payload, uint16_t payloadLen, const IPAddress& ip, uint16_t port,
                     uint16_t& seqOut, const char* what) {
    seqOut = 0;
    if (mutex_ == nullptr) {
      return SendVerdict::kSendFail;
    }
    // Stack-budget guard: every UDP send on this task funnels through here,
    // so the encode buffer is capped at the largest legal link-UDP payload
    // instead of kMaxFrame. Oversize callers fail closed here (JPEG rides
    // TCP, never this path).
    DESKY_ASSERT(payloadLen <= linkcode::kOtaUrlMaxLen);
    DESKY_ASSERT(payload != nullptr || payloadLen == 0);
    if (payloadLen > linkcode::kOtaUrlMaxLen || (payload == nullptr && payloadLen != 0)) {
      return SendVerdict::kEncodeFail;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    seqOut = txSeq_++;
    uint8_t out[linkcode::kOverhead + linkcode::kOtaUrlMaxLen];
    const size_t n = linkcode::encodeFrame(type, seqOut, payload, payloadLen, 0, 1, out, sizeof(out));
    bool sent = false;
    if (n != 0) {
      sent = udpsend::udpSendChecked(udp_, ip, port, out, n);
      if (!sent) {
        ++txFail_;
        LOG_W("LINK", "udp send fail what=%s len=%u", what, static_cast<unsigned>(n));
      }
    }
    xSemaphoreGive(mutex_);
    if (n == 0) {
      return SendVerdict::kEncodeFail;
    }
    return sent ? SendVerdict::kOk : SendVerdict::kSendFail;
  }

  static void taskEntry(void* arg) { static_cast<LinkServer*>(arg)->loop(); }

  void loop() {
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    // Never returns / never halts. (A halted WDT-subscribed task becomes a
    // panic-reboot loop.)
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      pollUdp();
      pollTcp();
      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (5000u / kLoopMs) == 0) {
        Diagnostics::logWatermarks("LINK");
        logLinkAccounting();
      }
    }
  }

  // First occurrence per boot per class always logs at INFO; afterwards the
  // call-site LOG_EVERY_N throttle (DEBUG level) applies. Counters only.
  template <typename... Args>
  void dropFirst(bool& first, const char* fmt, Args... args) {
    if (first) {
      first = false;
      LOG_I("LINK", fmt, args...);
    }
  }

  // 5s link accounting (unconditional LOG_I: survives release, where
  // LOG_EVERY_N vanishes). Counters only + peer address + seq domains.
  void logLinkAccounting() {
    char peer[24] = "none";
    if (haveHeadPeer_) {
      snprintf(peer, sizeof(peer), "%u.%u.%u.%u:%u", static_cast<unsigned>(headIp_[0]),
               static_cast<unsigned>(headIp_[1]), static_cast<unsigned>(headIp_[2]), static_cast<unsigned>(headIp_[3]),
               static_cast<unsigned>(headPort_));
    }
    LOG_I("LINK",
          "acct udp=%lu/%lu hb=%lu cmd=%lu ack=%lu/%lu ha=%lu ota=%lu fail=%lu frm=%lu B=%lu fps=%lu tcp=%lu "
          "peer=%s sq=%u boot=%08lx tx=%u",
          static_cast<unsigned long>(udpRx_), static_cast<unsigned long>(udpDrop_), static_cast<unsigned long>(hbRx_),
          static_cast<unsigned long>(cmdRx_), static_cast<unsigned long>(ackRx_), static_cast<unsigned long>(ackTx_),
          static_cast<unsigned long>(hbAckTx_), static_cast<unsigned long>(otaTx_), static_cast<unsigned long>(txFail_),
          static_cast<unsigned long>(framesRx_), static_cast<unsigned long>(bytesRx_),
          static_cast<unsigned long>(lastFps_), static_cast<unsigned long>(tcpDrop_), peer,
          static_cast<unsigned>(lastSeq_), static_cast<unsigned long>(lastBootId_), static_cast<unsigned>(txSeq_));
  }

  // UDP: drain every pending datagram; decode via the codec, then demux by
  // type. CRC/seq failures and unknown types drop with throttled logging.
  void pollUdp() {
    int pending = 0;
    while ((pending = udp_.parsePacket()) > 0) {
      const IPAddress rip = udp_.remoteIP();
      const uint16_t rport = udp_.remotePort();
      uint8_t buf[linkcode::kMaxFrame];
      const int got = udp_.read(buf, sizeof(buf));
      if (pending > static_cast<int>(sizeof(buf)) || got <= 0) {
        dropFirst(firstMalformedDrop_, "drop link datagram pending=%d got=%d (first of boot)", pending, got);
        LOG_EVERY_N(20, "LINK", "drop link datagram pending=%d got=%d", pending, got);
        ++udpDrop_;
        continue;
      }
      handleUdpFrame(buf, static_cast<size_t>(got), rip, rport);
    }
  }

  void handleUdpFrame(const uint8_t* buf, size_t len, const IPAddress& rip, uint16_t rport) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    ++udpRx_;
    linkcode::Frame f;
    if (!linkcode::decodeFrame(buf, len, f)) {
      // Unknown-type probes get their own class (the frozen codec rejects
      // them); everything else (bad magic/CRC/truncation) is malformed.
      if (len >= 2 && !linkcode::isKnownType(buf[1])) {
        dropFirst(firstUnknownDrop_, "drop link unknown type=0x%02X (first of boot)", static_cast<unsigned>(buf[1]));
        LOG_EVERY_N(20, "LINK", "drop link unknown type=0x%02X", static_cast<unsigned>(buf[1]));
      } else {
        dropFirst(firstMalformedDrop_, "drop link malformed len=%u (first of boot)", static_cast<unsigned>(len));
        LOG_EVERY_N(20, "LINK", "drop link malformed len=%u", static_cast<unsigned>(len));
      }
      ++udpDrop_;
      return;
    }
    // UDP carries single-frame control traffic only; fragmentation is a
    // TCP-stream concern (see pollTcp).
    if (f.fragIdx != 0 || f.fragTotal != 1) {
      dropFirst(firstMalformedDrop_, "drop link frag idx=%u total=%u (first of boot)", static_cast<unsigned>(f.fragIdx),
                static_cast<unsigned>(f.fragTotal));
      LOG_EVERY_N(20, "LINK", "drop link frag idx=%u total=%u", static_cast<unsigned>(f.fragIdx),
                  static_cast<unsigned>(f.fragTotal));
      ++udpDrop_;
      return;
    }
    switch (f.type) {
      case linkcode::kTypeHb:
        handleHb(f, rip, rport);
        break;
      case linkcode::kTypeFace:
      case linkcode::kTypeOled:
      case linkcode::kTypeCamCtl:
        handleCmd(f, rip, rport);
        break;
      case linkcode::kTypeAck:
        handleAck(f);
        break;
      case linkcode::kTypeHbAck:
      case linkcode::kTypeJpeg:
      case linkcode::kTypeOta:
      case linkcode::kTypePairShow:
      case linkcode::kTypePairDone:
      case linkcode::kTypeExpr:
        // Core->head direction on the UDP socket: never valid inbound here.
        // Silent drop, throttled.
        dropFirst(firstUnexpectedDrop_, "drop link unexpected %s on udp (first of boot)", linkserver::typeName(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unexpected %s on udp", linkserver::typeName(f.type));
        ++udpDrop_;
        break;
      default:
        // Unreachable: decodeFrame rejects unknown types above. Kept so a
        // future codec bump fails closed here.
        dropFirst(firstUnknownDrop_, "drop link unknown type=0x%02X (first of boot)", static_cast<unsigned>(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unknown type=0x%02X", static_cast<unsigned>(f.type));
        ++udpDrop_;
        break;
    }
  }

  // HB (head->core): epoch-gated, then freshness-gated; fresh replies
  // HB_ACK, duplicates re-send HB_ACK idempotently, stale drops. A changed
  // bootId epoch resyncs so a head reboot never wedges as stale forever.
  // Counters only in logs.
  void handleHb(const linkcode::Frame& f, const IPAddress& rip, uint16_t rport) {
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint32_t freeHeap = 0;
    uint32_t bootId = 0;
    if (!linkcode::unpackHb(f.payload, f.payloadLen, rssi, channel, freeHeap, bootId)) {
      dropFirst(firstMalformedDrop_, "drop link hb unpack (first of boot)");
      LOG_EVERY_N(20, "LINK", "drop link hb unpack");
      ++udpDrop_;
      return;
    }
    // Decoded for the HB_ACK echo (channel) only; the counters-only log rule
    // keeps rssi/heap off the serial line. bootId is logged on resync only
    // (rare by construction).
    (void)rssi;
    (void)freeHeap;
    if (linkserver::bootEpochChanged(haveBoot_, lastBootId_, bootId)) {
      haveBoot_ = true;
      lastBootId_ = bootId;
      haveSeq_ = true;
      lastSeq_ = f.seq;
      ++hbRx_;
      pinHeadPeer(rip, rport);  // (Re)pin: head rebooted or first contact.
      sendHbAck(rip, rport, channel);
      LOG_I("LINK", "hb resync boot=%08lx seq=%u", static_cast<unsigned long>(bootId), static_cast<unsigned>(f.seq));
      return;
    }
    const linkserver::SeqVerdict v = linkserver::classifySeq(haveSeq_, lastSeq_, f.seq);
    if (v == linkserver::SeqVerdict::kStale) {
      dropFirst(firstStaleDrop_, "drop link stale HB seq=%u (first of boot)", static_cast<unsigned>(f.seq));
      LOG_EVERY_N(20, "LINK", "drop link stale HB seq=%u", static_cast<unsigned>(f.seq));
      ++udpDrop_;
      return;
    }
    if (v == linkserver::SeqVerdict::kDuplicate) {
      pinHeadPeer(rip, rport);  // Re-pin: the head is still here.
      sendHbAck(rip, rport, channel);
      LOG_EVERY_N(20, "LINK", "hb redeliver seq=%u", static_cast<unsigned>(f.seq));
      return;
    }
    haveSeq_ = true;
    lastSeq_ = f.seq;
    ++hbRx_;
    pinHeadPeer(rip, rport);  // Fresh HB: (re)pin the unicast target.
    sendHbAck(rip, rport, channel);
    dropFirst(firstHb_, "hb seq=%u (first of boot)", static_cast<unsigned>(f.seq));
    LOG_EVERY_N(10, "LINK", "hb seq=%u", static_cast<unsigned>(f.seq));
  }

  // FACE/OLED/CAMCTL: freshness-gated; fresh stages a bounded copy under
  // lock + ACKs + LOG_Is the command (never payload bytes); duplicates
  // re-ACK without re-staging; stale drops.
  void handleCmd(const linkcode::Frame& f, const IPAddress& rip, uint16_t rport) {
    const linkserver::SeqVerdict v = linkserver::classifySeq(haveSeq_, lastSeq_, f.seq);
    if (v == linkserver::SeqVerdict::kStale) {
      dropFirst(firstStaleDrop_, "drop link stale %s seq=%u (first of boot)", linkserver::typeName(f.type),
                static_cast<unsigned>(f.seq));
      LOG_EVERY_N(20, "LINK", "drop link stale %s seq=%u", linkserver::typeName(f.type), static_cast<unsigned>(f.seq));
      ++udpDrop_;
      return;
    }
    if (v == linkserver::SeqVerdict::kDuplicate) {
      sendAck(rip, rport, f.seq);
      LOG_EVERY_N(20, "LINK", "%s redeliver seq=%u", linkserver::typeName(f.type), static_cast<unsigned>(f.seq));
      return;
    }
    haveSeq_ = true;
    lastSeq_ = f.seq;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    lastCmd_.valid = true;
    lastCmd_.type = f.type;
    lastCmd_.seq = f.seq;
    lastCmd_.len = f.payloadLen;
    for (uint16_t i = 0; i < f.payloadLen; ++i) {
      lastCmd_.payload[i] = f.payload[i];
    }
    xSemaphoreGive(mutex_);
    ++cmdRx_;
    sendAck(rip, rport, f.seq);
    LOG_I("LINK", "cmd type=%s seq=%u len=%u", linkserver::typeName(f.type), static_cast<unsigned>(f.seq),
          static_cast<unsigned>(f.payloadLen));
  }

  // ACK: receipt for our HB_ACK/ACK transmissions. Counted, never re-ACKed
  // (no ACK storms), and outside the command seq domain.
  void handleAck(const linkcode::Frame& f) {
    uint16_t acked = 0;
    if (!linkcode::unpackAck(f.payload, f.payloadLen, acked)) {
      dropFirst(firstMalformedDrop_, "drop link ack unpack (first of boot)");
      LOG_EVERY_N(20, "LINK", "drop link ack unpack");
      ++udpDrop_;
      return;
    }
    ++ackRx_;
    LOG_EVERY_N(20, "LINK", "ack acked=%u seq=%u", static_cast<unsigned>(acked), static_cast<unsigned>(f.seq));
  }

  // ACK TX (unicast to the command sender): the frame's own seq rides our
  // core->head tx domain (sendTo mints it). Replies to the sender — NOT via
  // the pinned-peer sendUnicast — because handleCmd deliberately never pins
  // (only HBs pin). Encode failure stays silent.
  void sendAck(const IPAddress& ip, uint16_t port, uint16_t ackedSeq) {
    uint8_t payload[linkcode::kAckLen];
    if (!linkcode::packAck(ackedSeq, payload)) {
      return;
    }
    uint16_t seq = 0;
    if (sendTo(linkcode::kTypeAck, payload, sizeof(payload), ip, port, seq, "ack") == SendVerdict::kOk) {
      ++ackTx_;
    }
  }

  // HB peer pin (ACK/HB_ACK replies + ability-pump unicast target). Called
  // for fresh + duplicate HBs only — stale replays never move the peer.
  void pinHeadPeer(const IPAddress& ip, uint16_t port) {
    headIp_ = ip;
    headPort_ = port;
    haveHeadPeer_ = true;
  }

  // HB_ACK TX (core->head): echoes the HB's channel + the fixed era (no
  // refresh domain on the transport-only link). The HB pinned the peer just
  // before this call, so sendUnicast reaches exactly the HB sender.
  void sendHbAck(const IPAddress& ip, uint16_t port, uint8_t channel) {
    (void)ip;
    (void)port;
    uint8_t payload[linkcode::kHbAckLen];
    if (!linkcode::packHbAck(channel, kHbAckEra, payload)) {
      return;
    }
    uint16_t seq = 0;
    if (sendUnicast(linkcode::kTypeHbAck, payload, sizeof(payload), seq)) {
      ++hbAckTx_;
      LOG_EVERY_N(20, "LINK", "hb_ack tx seq=%u", static_cast<unsigned>(seq));
    }
  }

  // TCP: serve ONE JPEG client. Accept while idle, drain bytes into the
  // STATIC stream buffer, then pull + validate complete frames. Disconnect
  // resets stream + reassembly state.
  void pollTcp() {
    WiFiClient incoming = server_.accept();
    if (incoming) {
      if (tcpClient_ && tcpClient_.connected()) {
        incoming.stop();
        dropFirst(firstTcpDrop_, "drop link tcp extra client (first of boot)");
        LOG_EVERY_N(20, "LINK", "drop link tcp extra client");
      } else {
        tcpClient_ = incoming;
        streamLen_ = 0;
        reasm_.reset(jpegMsg_, sizeof(jpegMsg_));
        LOG_I("LINK", "tcp jpeg client connected");
      }
    }
    if (!(tcpClient_ && tcpClient_.connected())) {
      if (tcpClient_) {
        tcpClient_.stop();
        streamLen_ = 0;
        reasm_.reset(jpegMsg_, sizeof(jpegMsg_));
        LOG_I("LINK", "tcp jpeg client gone frames=%lu bytes=%lu", static_cast<unsigned long>(framesRx_),
              static_cast<unsigned long>(bytesRx_));
      }
      return;
    }
    while (tcpClient_.available() > 0 && streamLen_ < sizeof(tcpStream_)) {
      const size_t room = sizeof(tcpStream_) - streamLen_;
      const int got = tcpClient_.read(tcpStream_ + streamLen_, room);
      if (got <= 0) {
        break;
      }
      streamLen_ += static_cast<size_t>(got);
    }
    for (;;) {
      size_t frameLen = 0;
      const linkserver::Pull p = linkserver::pullFrame(tcpStream_, streamLen_, frameLen);
      if (p == linkserver::Pull::kNeedMore) {
        break;
      }
      if (p == linkserver::Pull::kResync) {
        dropFirst(firstTcpDrop_, "drop link tcp resync (first of boot)");
        LOG_EVERY_N(20, "LINK", "drop link tcp resync");
        ++tcpDrop_;
        consumeTcp(1);
        continue;
      }
      linkcode::Frame f;
      if (!linkcode::decodeFrame(tcpStream_, frameLen, f)) {
        dropFirst(firstTcpDrop_, "drop link tcp bad crc/type (first of boot)");
        LOG_EVERY_N(20, "LINK", "drop link tcp bad crc/type");
        ++tcpDrop_;
        consumeTcp(frameLen);
        continue;
      }
      handleTcpFrame(f);  // Copies payload bytes out before the consume.
      consumeTcp(frameLen);
    }
  }

  // TCP frame effect: JPEG only (anything else on the JPEG socket drops).
  // Every message restarts reassembly on frag idx 0; the Reassembler appends
  // strictly in order and rejects duplicates/gaps/overflows without mutating
  // state. Complete messages stop here: validated bytes are checksummed +
  // counted, then DROPPED (no display ability exists yet). A future display
  // ability consumes at this point; it must not re-own the stream framing.
  void handleTcpFrame(const linkcode::Frame& f) {
    if (f.type != linkcode::kTypeJpeg) {
      dropFirst(firstTcpDrop_, "drop link tcp non-jpeg %s (first of boot)", linkserver::typeName(f.type));
      LOG_EVERY_N(20, "LINK", "drop link tcp non-jpeg %s", linkserver::typeName(f.type));
      ++tcpDrop_;
      return;
    }
    if (f.fragIdx == 0) {
      reasm_.reset(jpegMsg_, sizeof(jpegMsg_));
    }
    if (!reasm_.push(f.fragIdx, f.fragTotal, f.payload, f.payloadLen)) {
      dropFirst(firstTcpDrop_, "drop link tcp frag idx=%u total=%u (first of boot)", static_cast<unsigned>(f.fragIdx),
                static_cast<unsigned>(f.fragTotal));
      LOG_EVERY_N(20, "LINK", "drop link tcp frag idx=%u total=%u", static_cast<unsigned>(f.fragIdx),
                  static_cast<unsigned>(f.fragTotal));
      ++tcpDrop_;
      return;
    }
    if (!reasm_.complete()) {
      return;
    }
    const size_t msgLen = reasm_.len();
    ++framesRx_;
    bytesRx_ += static_cast<uint32_t>(msgLen);
    const uint32_t nowMs = millis();
    ++fpsWindow_;
    if (nowMs - fpsWindowStartMs_ >= 1000u) {
      lastFps_ = fpsWindow_;
      fpsWindow_ = 0;
      fpsWindowStartMs_ = nowMs;
    }
    if ((framesRx_ % kJpegLogEveryN) == 0) {
      LOG_I("LINK", "jpeg frames=%lu bytes=%lu fps=%u", static_cast<unsigned long>(framesRx_),
            static_cast<unsigned long>(bytesRx_), static_cast<unsigned>(lastFps_));
    }
    reasm_.reset(jpegMsg_, sizeof(jpegMsg_));
  }

  void consumeTcp(size_t n) {
    if (n == 0 || n > streamLen_) {
      return;
    }
    const size_t remain = streamLen_ - n;
    for (size_t i = 0; i < remain; ++i) {
      tcpStream_[i] = tcpStream_[n + i];
    }
    streamLen_ = remain;
  }

  TaskHandle_t task_;
  SemaphoreHandle_t mutex_;
  WiFiUDP udp_;
  WiFiServer server_;
  WiFiClient tcpClient_;
  // Big buffers live in BSS (LinkServer is a setup()-time global): never
  // task stack, never heap.
  uint8_t tcpStream_[kTcpStreamCap];
  size_t streamLen_ = 0;
  uint8_t jpegMsg_[kJpegMsgCap];
  linkcode::Reassembler reasm_;
  LastLinkCmd lastCmd_;
  // HB-pinned head peer: unicast target for core->head frames. Pinned from
  // every non-stale HB.
  IPAddress headIp_;
  uint16_t headPort_ = 0;
  bool haveHeadPeer_ = false;
  // Seq domains: head->core (HB + staged commands share head's counter;
  // first-ever frame accepted) vs our core->head tx counter (single writer
  // in sendTo). Epoch domain: the head's per-boot bootId — a changed epoch
  // resyncs the seq gate.
  bool haveSeq_ = false;
  uint16_t lastSeq_ = 0;
  bool haveBoot_ = false;
  uint32_t lastBootId_ = 0;
  uint16_t txSeq_ = 0;
  // Counters (task-written; getters are benign tear-tolerant reads).
  uint32_t udpRx_ = 0;
  uint32_t udpDrop_ = 0;
  uint32_t hbRx_ = 0;
  uint32_t cmdRx_ = 0;
  uint32_t ackRx_ = 0;
  uint32_t ackTx_ = 0;
  uint32_t hbAckTx_ = 0;
  uint32_t otaTx_ = 0;
  uint32_t txFail_ = 0;
  uint32_t framesRx_ = 0;
  uint32_t bytesRx_ = 0;
  uint32_t lastFps_ = 0;
  uint32_t fpsWindow_ = 0;
  uint32_t fpsWindowStartMs_ = 0;
  uint32_t tcpDrop_ = 0;
  // First-drop flags: first occurrence per boot per class always logs.
  // Reset only by reboot.
  bool firstMalformedDrop_ = true;
  bool firstUnknownDrop_ = true;
  bool firstUnexpectedDrop_ = true;
  bool firstStaleDrop_ = true;
  bool firstHb_ = true;
  bool firstTcpDrop_ = true;
};

#endif  // ARDUINO
