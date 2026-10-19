#pragma once
// desky v2 core<->head link endpoint (H2.2, B-link) — header-only.
//
// Owns the core-side link sockets + one Core 0 FreeRTOS task. Sockets ONLY;
// ESP-NOW is NOT a transport for this codec (see shared/link/README — the
// FROZEN B-link contract implemented here; the codec and README are never
// touched from this file).
//
//   UDP 3334 (CFG_LINK_UDP_PORT): RX link frames (FACE/OLED/CAMCTL/QRBITMAP/
//     HB/ACK per link_codec.h), CRC/seq validated via the codec. Seq freshness
//     via linkcode::isFreshSeq on a single shared head->core seq domain (HB +
//     staged commands share head's counter; received ACKs ride outside it —
//     see handleAck). Duplicate seq re-ACKs idempotently WITHOUT re-executing
//     (mirror of the REKEY duplicate rule in udp_server.h). Valid HB replies
//     HB_ACK (core->head). Valid FACE/OLED/CAMCTL/QRBITMAP stage into
//     LastLinkCmd (bounded copy under lock) for the main loop / future OLED
//     bridge — applying, for now, is the LOG_I command line. Unknown types
//     are rejected by the frozen codec and land in the unknown-type drop
//     class (first-of-boot INFO, then throttled).
//   TCP 3335 (CFG_LINK_TCP_PORT): JPEG head->core stream (the camera lives on
//     head; the codec's JPEG length gate is direction-free, so the envelope
//     is reused as-is). ONE client at a time; extras are refused while a head
//     is served. Length-framed via the link header (len field): bytes drain
//     straight into a STATIC 16KB stream buffer, complete frames are pulled
//     with linkserver::pullFrame, per-frame CRC via the codec, and fragmented
//     messages reassemble via linkcode::Reassembler into a second STATIC 16KB
//     message buffer. Validated JPEG bytes are checksummed (CRC) + counted
//     (framesRx_/bytesRx_/lastFps) and DROPPED after validation — no consumer
//     yet (H2.5/display consumes later).
//
// Buffer choice (documented): a 24KB-ish recv buffer is far too big for the
// 4096-word task stack, so BOTH big buffers are LinkServer MEMBERS (the server
// is a setup()-time global -> BSS, never stack, never heap). The task stack
// stays 4096 words; the UDP path uses a 1034B stack frame (kMaxFrame) and
// everything else is counters. No heap/String anywhere in the RX/TX path.
//
// H2.5 QR send: the main loop stages the pairing window's 63B bitmap
// (stageQrBitmap, policy side, never sockets); this task unicasts it once
// per window to the HB-pinned head peer (trySendQr, transport side — one
// small unicast after the UDP drain, so the RX tick can never stall).
// No head linked -> the stage waits silently; window closed -> clearQr.
// Duplicate protection is the existing seq/ACK machinery on both ends.

// Auth: NONE in this task beyond CRC+seq — transport-only trust, exactly like
// the AP passphrase (documented in shared/link/README). A keyed link can come
// later. This header never logs payload bytes beyond counters (the link
// carries no keys; still counters-only by rule) and never touches UART copper
// (head copper stays absent per hardware/INTERFACE.md section 0).
//
// Structure mirrors udp_server.h: the top half (namespace linkserver) is
// Arduino-free pure logic (stdint + link_codec.h only) so host Unity tests
// include this header directly. The bottom half (class LinkServer) is
// firmware-only (#ifdef ARDUINO): sockets + Core 0 task.
//
// Rules: WDT fed every tick, Diagnostics::logWatermarks("LINK") every ~5s,
// task never halts (a halted WDT-subscribed task becomes a panic-reboot loop,
// so halting would require feed-then-delete first — this task simply never
// halts). Priority tskIDLE_PRIORITY + 2, same as audio, BELOW UdpServer +3:
// the UDP server's HMAC-verify budget + 5ms UDP tick must never be stolen by
// link work. begin() runs AFTER UdpServer::begin (the link binds sockets on
// the already-up stack and owns no WiFi mode itself) and BEFORE the BOOT
// banner lines; failure LOG_Ws and the boot continues headless — the link
// must never brick driving.

// ── Arduino-free: seq verdict + TCP stream framing + type names ──────────
#include <stddef.h>
#include <stdint.h>

#include "common/udp_send.h"

// The frozen codec namespace `link` collides with POSIX ::link() (declared
// via the WiFi/lwIP stack: IPAddress.h -> lwIP opt.h -> toolchain unistd.h ->
// newlib sys/unistd.h) in any Arduino TU, and the codec itself must not be
// touched. So the codec is included once here inside linkwrap__ and aliased
// back on ALL platforms (uniform spelling, immune to include order and to a
// host unistd.h declaring ::link). All uses below go through the `linkcode::`
// alias (identical entities; host-pinned goldens in test_link_codec.cpp still
// cover the canonical `link::` spelling, which keeps working in
// Arduino-free TUs).
namespace linkwrap__ {
#include "link/link_codec.h"
}  // namespace linkwrap__
namespace linkcode = linkwrap__::link;

#include "link/ota_manifest.h"

// Fallbacks keep the host tests + firmware compiling if config keys drift;
// in-project include/config.h always wins (same pattern as pairing.h).
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
// first-ever frame or wrap-aware strictly greater (linkcode::isFreshSeq);
// Duplicate = exact retransmit of the last accepted seq (caller re-ACKs
// idempotently without re-executing); Stale = anything else (replay/older,
// silent drop).
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

// Boot-epoch gate (host-testable): the head draws bootId = esp_random() per
// boot and sends it in every HB. A changed (or first-seen) epoch means the
// head rebooted — accept ANY seq as a fresh resync (pin peer, ACK) instead
// of dropping low seqs as stale forever. Same-epoch frames keep the
// strictly-greater gate above. Replaying an old epoch's newest frame only
// buys one idempotent HB_ACK (counters only) — acceptable under the
// transport-only trust model (stated in the codec).
inline bool bootEpochChanged(bool haveBoot, uint32_t lastBoot, uint32_t gotBoot) {
  return !haveBoot || lastBoot != gotBoot;
}

// TCP stream framing result (host-testable): NeedMore = fewer than
// kOverhead bytes buffered, or a header whose full frame has not arrived yet;
// HaveFrame = one complete frame sits at buf[0] (frameLenOut = its total
// bytes; CRC/type still to be validated by decodeFrame); Resync = buf[0] is
// not the magic, or the len field claims more than kMaxPayload (caller drops
// one byte and retries — never skips ahead blindly).
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

// Log-only type name (never payload bytes — see the counters-only rule
// above). Returns "UNKNOWN" for anything outside the frozen contract.
inline const char* typeName(uint8_t type) {
  switch (type) {
    case linkcode::kTypeFace:
      return "FACE";
    case linkcode::kTypeOled:
      return "OLED";
    case linkcode::kTypeCamCtl:
      return "CAMCTL";
    case linkcode::kTypeQrBitmap:
      return "QRBITMAP";
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

// QR send gate (host-testable): the link task unicasts the staged 63B
// bitmap exactly once per window (alreadySent latches after the send) and
// only to an HB-pinned head peer. Staging without a peer waits (late
// joiners covered); no peer never blocks the RX tick — the skip stays a
// throttled LOG_D on the task side.
inline bool qrReadyToSend(bool stagedValid, bool alreadySent, bool haveHeadPeer) {
  return stagedValid && !alreadySent && haveHeadPeer;
}

// OTA send gate (host-testable): same once-per-command shape as the QR gate
// above — one unicast of the staged URL to the HB-pinned head, silent wait
// while no peer is linked.
inline bool otaReadyToSend(bool stagedValid, bool alreadySent, bool haveHeadPeer) {
  return stagedValid && !alreadySent && haveHeadPeer;
}

// HB peer pin rule (host-testable): every non-stale HB (fresh + duplicate
// re-pin) refreshes the unicast target; stale replays never move it.
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
  // HB_ACK era/resync field: no rekey domain on the transport-only link
  // (keyed link later) — always 0, echoed channel comes from the HB itself.
  static constexpr uint32_t kHbAckEra = 0;

  // Staged head->core command handoff (H2.5/OLED-bridge reads via
  // snapshotCmd). Bounded: payload never exceeds kMaxPayload (enforced by
  // the codec before the copy). No EventBus publish by design: no fitting
  // EventType exists and this task must not invent a second bus — the staged
  // struct IS the handoff.
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
  uint32_t qrTx() const { return qrTx_; }
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

  // Stage the pairing window's 63B QR bitmap (main-loop policy side; copies
  // under lock, never touches sockets, so it cannot disturb the UDP RX
  // tick). The link task unicasts it once to the HB-pinned head. Null or
  // pre-begin (no mutex) -> false, fail-closed.
  // Producer vocabulary note: the future stageFace/stageOled/stageCam MUST
  // pack via link::packOled/packCamCtl/unpack* (literal-byte tests pin the wire);
  // the stageOtaUrl producer next to this one validates via the shared
  // otamanifest::isValidOtaUrlBytes helper (single-sourced, never re-derived).
  bool stageQrBitmap(const uint8_t qr63[linkcode::kQrBitmapLen]) {
    if (qr63 == nullptr || mutex_ == nullptr) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!qrSent_) {
      for (size_t i = 0; i < linkcode::kQrBitmapLen; ++i) {
        stagedQr_[i] = qr63[i];
      }
      stagedValid_ = true;
    }
    xSemaphoreGive(mutex_);
    return true;
  }

  // Stage a carrier OTA URL for the head pull (main-loop policy side, fed by
  // the `otahead <url>` console verb; copies under lock, never touches
  // sockets). The link task unicasts it once as a kTypeOta frame to the
  // HB-pinned head. Validation is the shared otamanifest grammar (1..128
  // printable-ASCII bytes, no '@'/whitespace — credentials never ride or
  // log); invalid/null/pre-begin -> false, fail-closed. Every valid command
  // REPLACES the stage and re-arms the send (console-verb semantics: latest
  // command wins — a head mid-download ignores the re-trigger via its
  // in-flight guard, an idle head pulls the newest URL); clearOta re-arms
  // for the next command.
  bool stageOtaUrl(const char* url) {
    if (url == nullptr || mutex_ == nullptr) {
      return false;
    }
    size_t len = 0;
    while (len <= otamanifest::kUrlMax && url[len] != '\0') {
      ++len;
    }
    if (len == 0 || len > otamanifest::kUrlMax) {
      return false;
    }
    if (!otamanifest::isValidOtaUrlBytes(reinterpret_cast<const uint8_t*>(url), len)) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (size_t i = 0; i < len; ++i) {
      stagedOta_[i] = static_cast<uint8_t>(url[i]);
    }
    stagedOtaLen_ = len;
    stagedOtaValid_ = true;
    otaSent_ = false;
    xSemaphoreGive(mutex_);
    return true;
  }

  // Drop any staged/unsent OTA (torn download repeat path) and unlatch the
  // once-per-command gate. Safe pre-begin.
  void clearOta() {
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    stagedOtaValid_ = false;
    otaSent_ = false;
    stagedOtaLen_ = 0;
    for (size_t i = 0; i < linkcode::kOtaUrlMaxLen; ++i) {
      stagedOta_[i] = 0;
    }
    xSemaphoreGive(mutex_);
  }

  // Staged OTA length for the console door (counters only). 0 pre-begin.
  size_t stagedOtaLen() const {
    if (mutex_ == nullptr) {
      return 0;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const size_t len = stagedOtaValid_ ? stagedOtaLen_ : 0;
    xSemaphoreGive(mutex_);
    return len;
  }

  // Drop any staged/unsent QR (window closed: bonded/burned/cleared) and
  // unlatch the once-per-window gate for the next window. Safe pre-begin.
  void clearQr() {
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    stagedValid_ = false;
    qrSent_ = false;
    for (size_t i = 0; i < linkcode::kQrBitmapLen; ++i) {
      stagedQr_[i] = 0;
    }
    xSemaphoreGive(mutex_);
  }

 private:
  static void taskEntry(void* arg) { static_cast<LinkServer*>(arg)->loop(); }

  void loop() {
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    // Never returns / never halts by design (see coordinator.h: a halted
    // WDT-subscribed task becomes a panic-reboot loop).
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      pollUdp();
      trySendQr();
      trySendOta();
      pollTcp();
      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (5000u / kLoopMs) == 0) {
        Diagnostics::logWatermarks("LINK");
        logLinkAccounting();
      }
    }
  }

  // Security-drop logging (bench lesson, Task 10 — same idiom as udp_server.h):
  // LOG_EVERY_N alone hides the first N-1 occurrences per call site, so the
  // first occurrence per boot per class always logs at INFO; afterwards the
  // call-site LOG_EVERY_N throttle (DEBUG level) applies. Counters only here.
  template <typename... Args>
  void dropFirst(bool& first, const char* fmt, Args... args) {
    if (first) {
      first = false;
      LOG_I("LINK", fmt, args...);
    }
  }

  // 5s link accounting (unconditional LOG_I: survives release, where
  // LOG_EVERY_N vanishes). Counters only + peer address + seq domains — never
  // payload bytes. ~140B: fits the 192B log line with the logger prefix.
  void logLinkAccounting() {
    char peer[24] = "none";
    if (haveHeadPeer_) {
      snprintf(peer, sizeof(peer), "%u.%u.%u.%u:%u", static_cast<unsigned>(headIp_[0]),
               static_cast<unsigned>(headIp_[1]), static_cast<unsigned>(headIp_[2]), static_cast<unsigned>(headIp_[3]),
               static_cast<unsigned>(headPort_));
    }
    LOG_I("LINK",
          "acct udp=%lu/%lu hb=%lu cmd=%lu ack=%lu/%lu ha=%lu qr=%lu ota=%lu fail=%lu frm=%lu B=%lu fps=%lu tcp=%lu "
          "peer=%s sq=%u boot=%08lx tx=%u",
          static_cast<unsigned long>(udpRx_), static_cast<unsigned long>(udpDrop_), static_cast<unsigned long>(hbRx_),
          static_cast<unsigned long>(cmdRx_), static_cast<unsigned long>(ackRx_), static_cast<unsigned long>(ackTx_),
          static_cast<unsigned long>(hbAckTx_), static_cast<unsigned long>(qrTx_), static_cast<unsigned long>(otaTx_),
          static_cast<unsigned long>(txFail_), static_cast<unsigned long>(framesRx_),
          static_cast<unsigned long>(bytesRx_), static_cast<unsigned long>(lastFps_),
          static_cast<unsigned long>(tcpDrop_), peer, static_cast<unsigned>(lastSeq_),
          static_cast<unsigned long>(lastBootId_), static_cast<unsigned>(txSeq_));
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
      case linkcode::kTypeQrBitmap:
        handleCmd(f, rip, rport);
        break;
      case linkcode::kTypeAck:
        handleAck(f);
        break;
      case linkcode::kTypeHbAck:
      case linkcode::kTypeJpeg:
      case linkcode::kTypeOta:
        // Core->head direction on the UDP socket: never valid inbound here
        // (JPEG rides TCP; OTA is staged core-side and sent by trySendOta).
        // Silent drop, throttled.
        dropFirst(firstUnexpectedDrop_, "drop link unexpected %s on udp (first of boot)", linkserver::typeName(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unexpected %s on udp", linkserver::typeName(f.type));
        ++udpDrop_;
        break;
      default:
        // Unreachable: decodeFrame rejects unknown types above. Kept so a
        // future codec bump fails closed here instead of falling through.
        dropFirst(firstUnknownDrop_, "drop link unknown type=0x%02X (first of boot)", static_cast<unsigned>(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unknown type=0x%02X", static_cast<unsigned>(f.type));
        ++udpDrop_;
        break;
    }
  }

  // HB (head->core): epoch-gated, then freshness-gated; fresh replies
  // HB_ACK, duplicates re-send HB_ACK idempotently (mirror of the REKEY
  // duplicate rule), stale drops. A changed bootId epoch resyncs (accept any
  // seq, pin peer) so a head reboot never wedges as stale forever.
  // Counters only in logs — never payload bytes.
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

  // FACE/OLED/CAMCTL/QRBITMAP: freshness-gated; fresh stages a bounded copy
  // under lock + ACKs + LOG_Is the command (type/seq/len only — never payload
  // bytes); duplicates re-ACK WITHOUT re-staging or re-logging at INFO;
  // stale drops.
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

  // ACK (either direction): receipt for our HB_ACK/ACK transmissions.
  // Counted, never re-ACKed (no ACK storms), and outside the command seq
  // domain — a replayed receipt triggers no effect, so gating it would only
  // wedge the shared counter on reordered receipts.
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

  // ACK TX (unicast to the command sender): payload is the acked seq; the
  // frame's own seq rides our core->head tx domain (txSeq_++ per send, so a
  // duplicate re-ACK is a fresh frame carrying the same receipt).
  void sendAck(const IPAddress& ip, uint16_t port, uint16_t ackedSeq) {
    uint8_t payload[linkcode::kAckLen];
    uint8_t out[linkcode::kOverhead + linkcode::kAckLen];
    if (!linkcode::packAck(ackedSeq, payload)) {
      return;
    }
    const size_t n =
        linkcode::encodeFrame(linkcode::kTypeAck, txSeq_++, payload, sizeof(payload), 0, 1, out, sizeof(out));
    if (n == 0) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, ip, port, out, n)) {
      ++txFail_;
      LOG_W("LINK", "udp send fail what=%s len=%u", "ack", static_cast<unsigned>(n));
      return;
    }
    ++ackTx_;
  }

  // HB peer pin (H2.5 QR unicast target). Called for fresh + duplicate
  // HBs only — the stale path returns before it, so replays never move the
  // peer. Counters-only rule: no payload bytes logged here.
  void pinHeadPeer(const IPAddress& ip, uint16_t port) {
    headIp_ = ip;
    headPort_ = port;
    haveHeadPeer_ = true;
  }

  // QRBITMAP send (H2.5, link-task side): unicasts the staged 63B bitmap
  // once per window to the HB-pinned head as one QRBITMAP frame. No head
  // linked -> silent pending skip + throttled LOG_D (the serial QR row=
  // dump stays the dev path); the stage waits for a late joiner. Encode
  // failure -> throttled drop, never a fault, never a brick of driving.
  // Duplicate protection is the existing seq/ACK machinery (head re-ACKs
  // duplicates); the qrSent_ latch keeps this to one send per window.
  void trySendQr() {
    uint8_t staged[linkcode::kQrBitmapLen] = {};
    bool doSend = false;
    bool pendingNoPeer = false;
    uint16_t seq = 0;
    IPAddress peer;
    uint16_t port = 0;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      pendingNoPeer = stagedValid_ && !qrSent_ && !haveHeadPeer_;
      doSend = linkserver::qrReadyToSend(stagedValid_, qrSent_, haveHeadPeer_);
      if (doSend) {
        for (size_t i = 0; i < linkcode::kQrBitmapLen; ++i) {
          staged[i] = stagedQr_[i];
        }
        seq = txSeq_++;
        peer = headIp_;
        port = headPort_;
        stagedValid_ = false;
        qrSent_ = true;
      }
      xSemaphoreGive(mutex_);
    }
    if (pendingNoPeer) {
      LOG_EVERY_N(20, "LINK", "qr pending, no head peer");
      return;
    }
    if (!doSend) {
      return;
    }
    uint8_t out[linkcode::kOverhead + linkcode::kQrBitmapLen];
    const size_t n =
        linkcode::encodeFrame(linkcode::kTypeQrBitmap, seq, staged, sizeof(staged), 0, 1, out, sizeof(out));
    if (n == 0) {
      LOG_EVERY_N(20, "LINK", "qr encode failed seq=%u", static_cast<unsigned>(seq));
      return;
    }
    if (!udpsend::udpSendChecked(udp_, peer, port, out, n)) {
      ++txFail_;
      LOG_W("LINK", "udp send fail what=%s len=%u", "qr", static_cast<unsigned>(n));
      return;
    }
    ++qrTx_;
    LOG_I("LINK", "qr sent seq=%u len=%u", static_cast<unsigned>(seq), static_cast<unsigned>(sizeof(staged)));
  }

  // OTA URL send (carrier trigger, task side): unicasts the staged URL once
  // per `otahead` command to the HB-pinned head as one OTA frame. No head
  // linked -> silent pending skip + throttled LOG_D (same late-joiner wait
  // as the QR path). Encode failure -> throttled drop, never a fault, never
  // a brick of driving. Duplicate protection is the existing seq/ACK
  // machinery (head re-ACKs duplicates); the otaSent_ latch keeps this to
  // one send per command. Never logs URL bytes (length only).
  void trySendOta() {
    uint8_t staged[linkcode::kOtaUrlMaxLen] = {};
    size_t stagedLen = 0;
    bool doSend = false;
    bool pendingNoPeer = false;
    uint16_t seq = 0;
    IPAddress peer;
    uint16_t port = 0;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      pendingNoPeer = stagedOtaValid_ && !otaSent_ && !haveHeadPeer_;
      doSend = linkserver::otaReadyToSend(stagedOtaValid_, otaSent_, haveHeadPeer_);
      if (doSend) {
        for (size_t i = 0; i < stagedOtaLen_; ++i) {
          staged[i] = stagedOta_[i];
        }
        stagedLen = stagedOtaLen_;
        seq = txSeq_++;
        peer = headIp_;
        port = headPort_;
        stagedOtaValid_ = false;
        otaSent_ = true;
      }
      xSemaphoreGive(mutex_);
    }
    if (pendingNoPeer) {
      LOG_EVERY_N(20, "LINK", "ota pending, no head peer");
      return;
    }
    if (!doSend) {
      return;
    }
    uint8_t out[linkcode::kOverhead + linkcode::kOtaUrlMaxLen];
    const size_t n = linkcode::encodeFrame(linkcode::kTypeOta, seq, staged, static_cast<uint16_t>(stagedLen), 0, 1, out,
                                           sizeof(out));
    if (n == 0) {
      LOG_EVERY_N(20, "LINK", "ota encode failed seq=%u", static_cast<unsigned>(seq));
      return;
    }
    if (!udpsend::udpSendChecked(udp_, peer, port, out, n)) {
      ++txFail_;
      LOG_W("LINK", "udp send fail what=%s len=%u", "ota", static_cast<unsigned>(n));
      return;
    }
    ++otaTx_;
    LOG_I("LINK", "ota sent seq=%u len=%u", static_cast<unsigned>(seq), static_cast<unsigned>(stagedLen));
  }

  // HB_ACK TX (core->head): echoes the HB's channel + the fixed era (no
  // rekey domain on the transport-only link). Carries no secret.
  void sendHbAck(const IPAddress& ip, uint16_t port, uint8_t channel) {
    uint8_t payload[linkcode::kHbAckLen];
    uint8_t out[linkcode::kOverhead + linkcode::kHbAckLen];
    if (!linkcode::packHbAck(channel, kHbAckEra, payload)) {
      return;
    }
    const size_t n =
        linkcode::encodeFrame(linkcode::kTypeHbAck, txSeq_++, payload, sizeof(payload), 0, 1, out, sizeof(out));
    if (n == 0) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, ip, port, out, n)) {
      ++txFail_;
      LOG_W("LINK", "udp send fail what=%s len=%u", "hb_ack", static_cast<unsigned>(n));
      return;
    }
    ++hbAckTx_;
    LOG_EVERY_N(20, "LINK", "hb_ack tx seq=%u", static_cast<unsigned>(txSeq_ - 1));
  }

  // TCP: serve ONE JPEG client. Accept while idle (extras refused while a
  // head is served), drain available bytes straight into the STATIC stream
  // buffer (zero heap, zero stack temp), then pull + validate complete
  // frames. Disconnect resets stream + reassembly state.
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
  // Every message restarts reassembly on frag idx 0 (single-frame 0/1
  // included); the Reassembler then appends strictly in order and rejects
  // duplicates/gaps/total-mismatches/overflows WITHOUT mutating state.
  // Restart-on-0 keeps a lost tail from wedging the stream — a duplicate
  // idx-0 retransmit just rewrites identical bytes. Complete messages are
  // checksummed (per-frame CRC via the codec) + counted, then DROPPED: no
  // consumer yet (H2.5/display consumes later). Counters only in logs.
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
  // Big buffers: LinkServer is a setup()-time global, so these members live
  // in BSS (STATIC, never task stack, never heap). 16KB + 16KB + 1KB would
  // overflow the 4096-word stack in a single frame — hence members.
  uint8_t tcpStream_[kTcpStreamCap];
  size_t streamLen_ = 0;
  uint8_t jpegMsg_[kJpegMsgCap];
  linkcode::Reassembler reasm_;
  LastLinkCmd lastCmd_;
  // HB-pinned head peer: unicast target for core->head frames (the QRBITMAP
  // send below). Pinned from every non-stale HB (fresh + duplicate re-pin;
  // stale never moves it) — the link task's only notion of "a head is here".
  IPAddress headIp_;
  uint16_t headPort_ = 0;
  bool haveHeadPeer_ = false;
  // Staged QR bitmap (H2.5): the main loop stages the open window's 63B
  // bitmap (policy side, never sockets); the link task unicasts it ONCE to
  // the pinned head (qrSent_ latches until clearQr). Waiting for a late
  // joiner is the silent pending path, never a fault.
  uint8_t stagedQr_[linkcode::kQrBitmapLen] = {};
  bool stagedValid_ = false;
  bool qrSent_ = false;
  // Staged carrier OTA URL: the `otahead` console verb stages the pull URL
  // (policy side, never sockets); the link task unicasts it ONCE to the
  // pinned head (otaSent_ latches until clearOta). Same shape as the QR
  // stage above; the URL bytes are validated by stageOtaUrl and never logged.
  uint8_t stagedOta_[linkcode::kOtaUrlMaxLen] = {};
  size_t stagedOtaLen_ = 0;
  bool stagedOtaValid_ = false;
  bool otaSent_ = false;
  // Seq domains: head->core (HB + staged commands share head's counter;
  // first-ever frame accepted regardless of value) vs our core->head tx
  // counter for ACK/HB_ACK frames. Epoch domain: the head's per-boot bootId
  // (HB payload) — a changed epoch resyncs the seq gate (head rebooted),
  // same-epoch frames keep the strictly-greater gate.
  bool haveSeq_ = false;
  uint16_t lastSeq_ = 0;
  bool haveBoot_ = false;
  uint32_t lastBootId_ = 0;
  uint16_t txSeq_ = 0;
  // Counters (32-bit, task-written; getters are benign tear-tolerant reads).
  uint32_t udpRx_ = 0;
  uint32_t udpDrop_ = 0;
  uint32_t hbRx_ = 0;
  uint32_t cmdRx_ = 0;
  uint32_t ackRx_ = 0;
  uint32_t ackTx_ = 0;
  uint32_t hbAckTx_ = 0;
  uint32_t qrTx_ = 0;
  uint32_t otaTx_ = 0;
  uint32_t txFail_ = 0;
  uint32_t framesRx_ = 0;
  uint32_t bytesRx_ = 0;
  uint32_t lastFps_ = 0;
  uint32_t fpsWindow_ = 0;
  uint32_t fpsWindowStartMs_ = 0;
  uint32_t tcpDrop_ = 0;
  // First-drop flags for dropFirst() (bench lesson: first occurrence per boot
  // per class always logs). Reset only by reboot.
  bool firstMalformedDrop_ = true;
  bool firstUnknownDrop_ = true;
  bool firstUnexpectedDrop_ = true;
  bool firstStaleDrop_ = true;
  bool firstHb_ = true;
  bool firstTcpDrop_ = true;
};

#endif  // ARDUINO
