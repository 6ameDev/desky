#pragma once
// desky-head WiFi link client (H2.3 B-link) — TRANSPORT layer, dumb pipe.
//
// Sockets, framing, peer/assoc/HB state only; business decisions arrive as
// injected callbacks (LinkHandlers, stored never owned, bound in main.cpp).
// No PowerManager / OtaUpdater / CameraDriver / FaceRenderer includes, no
// direct business calls. Split from the monolith wifi_link.h (Task 2); the
// business mapping helpers live in their canonical homes (wifilink::mapCmd +
// isOtaFrame in transport/link_codec.h, pairing gates in
// abilities/pairing/pairing_codec.h).
//
//   UDP CFG_LINK_UDP_PORT (3334): HB head->core at 1Hz (rssi/channel/freeHeap/
//     bootId per the codec HB type) + ACK replies; RX FACE/OLED/CAMCTL/
//     PAIRSHOW/PAIRDONE commands + HB_ACK on the same socket. HB_ACK tracked: linkDown after
//     3 missed ACKs (~3s) -> re-associate + re-AWAKE. The tx seq domain is
//     MONOTONIC within a boot (never reset on re-assoc); the per-boot bootId
//     epoch lets the core resync after a head reboot instead of dropping low
//     seqs as stale forever.
//   TCP CFG_LINK_TCP_PORT (3335): JPEG stills head->core, chunked per the
//     README MTU rule (frag idx/total in every frame). Stills-first: 1 frame
//     per CLI `snap` trigger in this task, NO continuous streaming yet
//     (continuous fps comes with the H2.4 bench gates).
//
// Power / coexistence notes (see also include/mcu/esp32cam_ov3660.h):
// - Camera data/clock pins are DIGITAL-only (no ADC2 use anywhere on head),
//   so there is NO ADC2-vs-WiFi conflict — WiFi + camera coexist by design.
// - Star 5V rail budgets module + camera + OLED + radio peak together; the
//   modem-sleep-between-HBs policy keeps the radio at its idle floor except
//   during the 1Hz HB blip and on-demand still uploads.
// - Channel follows the core AP (head never selects a channel; the STA
//   inherits the AP channel on associate).
// - MB-board rule preserved: with no AP visible (every USB run) association
//   fails gracefully with a throttled LOG_W + retry — never asserts, never
//   blocks boot, camera + faces keep working standalone.
//
// Scheduling: NO dedicated task in this task — poll() + heartbeatTick() run
// on the Arduino loop (Core 1 by construction), so the Core 0 render task
// (CFG_FACE_PRIORITY_OFFSET +2) is never preempted by link work. If link work
// ever moves to its own task, pin it to Core 0 at offset +1 (BELOW face +2,
// same level core gave the link below its UDP server +3).
//
// POSIX gotcha (discovered on core, not rediscovered here): newlib declares
// ::link(), so `namespace link` at global scope BREAKS Arduino TUs. The
// codec is included ONCE inside linkwrap__ (see transport/link_codec.h) and
// used via the `linkcode` alias uniformly (same workaround as core's
// link_server.h).

#include <stddef.h>
#include <stdint.h>

#include "transport/link_codec.h"

// Fallbacks keep the host tests + firmware compiling if config keys drift;
// in-project include/config.h always wins (same pattern as link_stub.h).
#ifndef CFG_LINK_SSID
#define CFG_LINK_SSID "desky"
#endif
#ifndef CFG_LINK_PASS
#define CFG_LINK_PASS "desky1234"
#endif
#ifndef CFG_LINK_HOST
#define CFG_LINK_HOST "192.168.4.1"
#endif
#ifndef CFG_LINK_UDP_PORT
#define CFG_LINK_UDP_PORT 3334
#endif
#ifndef CFG_LINK_TCP_PORT
#define CFG_LINK_TCP_PORT 3335
#endif
#ifndef CFG_LINK_ASSOC_RETRY_MS
#define CFG_LINK_ASSOC_RETRY_MS 5000
#endif

// Injected business edges (stored, never owned). main.cpp binds these to the
// PowerManager / OtaUpdater / CameraDriver; transport never includes those
// headers and never calls them directly.
struct LinkHandlers {
  void* ctx = nullptr;
  // Validated FACE/OLED/CAMCTL frame (decode + frag gate + seq gate +
  // codec value check). Handler maps (wifilink::mapCmd in
  // transport/link_codec.h), publishes HEAD_EVENT_LINK_CMD, and executes.
  void (*onCommand)(void* ctx, const linkcode::Frame& f) = nullptr;
  // Validated PAIRSHOW payload (digits + window unpacked by transport).
  void (*onPairShow)(void* ctx, const uint8_t pin6[6], uint16_t validSecs) = nullptr;
  // Validated PAIRDONE frame (presence is the signal).
  void (*onPairDone)(void* ctx) = nullptr;
  // Raw OTA URL bytes (length-gated only). Handler owns the grammar
  // (otamanifest::copyOtaUrl) + updater trigger. True = consumed (the seq
  // was already consumed + ACKed by transport before this call).
  bool (*onOtaUrl)(void* ctx, const uint8_t* url, uint16_t len) = nullptr;
  // Camera gate for the TCP stills pump (replaces direct cam_ calls;
  // the fb acquire/release itself stays here via esp_camera.h).
  bool (*isCameraOn)(void* ctx) = nullptr;
};

#ifdef ARDUINO
// ── Firmware: WiFi STA link client (UDP HB/commands + TCP JPEG stills) ─────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_camera.h>
#include <esp_system.h>  // esp_random() for the per-boot bootId epoch

#include "common/fault_manager.h"
#include "common/logger.h"
#include "common/udp_send.h"
#include "config.h"
#include "head_context.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_LINK_HEARTBEAT_MS
#define CFG_LINK_HEARTBEAT_MS 1000
#endif
#ifndef CFG_LINK_ASSOC_TIMEOUT_MS
#define CFG_LINK_ASSOC_TIMEOUT_MS 10000
#endif

class LinkClient {
 public:
  LinkClient()
      : handlers_(),
        associated_(false),
        linkUp_(false),
        lastAssocTryMs_(0),
        connecting_(false),
        attemptStartMs_(0),
        firstAttempt_(true),
        lastHbMs_(0),
        lastAckMs_(0),
        hbSinceAck_(0),
        bootId_(0),
        txSeq_(0),
        haveCmdSeq_(false),
        lastCmdSeq_(0),
        stillPending_(false),
        lastRssi_(0),
        lastChannel_(0),
        hbSent_(0),
        hbAckRx_(0),
        cmdRx_(0),
        ackTx_(0),
        framesSent_(0),
        stillDrop_(0),
        udpDrop_(0),
        firstAssocFail_(true),
        firstHb_(true),
        firstDrop_(true),
        firstStale_(true) {}

  // Joins the core AP (STA only, modem-sleep) and binds UDP. Never blocks
  // boot: association is async, absence of the AP only logs. Call once in
  // setup(), AFTER g_link (stub) init — the stub stays PARKED and this
  // client is the live path. Business edges arrive via setHandlers() (main
  // binds PowerManager/OtaUpdater/CameraDriver there). Logging must be up.
  void begin() {
    // Boot epoch for the HB seq domain (see linkcode::kHbLen note): drawn
    // once here, never reseeded — the core uses it to resync after a head
    // reboot instead of dropping our low seqs as stale forever.
    bootId_ = esp_random();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);  // modem-sleep between HBs (idle floor, see header)
    udp_.begin(CFG_LINK_UDP_PORT);
    LOG_I("LINK", "wifi sta up udp=%u tcp=%u ap=%s", static_cast<unsigned>(CFG_LINK_UDP_PORT),
          static_cast<unsigned>(CFG_LINK_TCP_PORT), CFG_LINK_SSID);
    beginAttempt(millis());
  }

  // Business-edge binder (stored, never owned). Call once in setup() after
  // begin(); main.cpp owns the handler targets.
  void setHandlers(const LinkHandlers& h) { handlers_ = h; }

  // Drains inbound UDP (commands/HB_ACK/ACK), drives association while
  // down, and services one pending still. Quick by design — called every
  // loop() alongside the stub poll; ordering in main.cpp is preserved.
  void poll() {
    const uint32_t now = millis();
    if (!associated_) {
      // Attempt in flight: only CHECK it (cheap status read). A NEW attempt
      // starts on the retry cadence — never re-begin() here: that aborts the
      // in-flight handshake, so same-tick re-begin loops can never connect.
      if (!connecting_) {
        if (wifilink::retryDue(now, lastAssocTryMs_, CFG_LINK_ASSOC_RETRY_MS)) {
          beginAttempt(now);
        }
      } else {
        switch (wifilink::assocStep(connecting_, WiFi.status() == WL_CONNECTED, now, attemptStartMs_,
                                    static_cast<uint32_t>(CFG_LINK_ASSOC_TIMEOUT_MS))) {
          case wifilink::AssocAction::kConnected:
            connecting_ = false;
            onAssoc(now);
            break;
          case wifilink::AssocAction::kRestart:
            // Handshake never completed: drop it and start fresh on the
            // retry cadence. Only NOW may we say the AP is not visible.
            WiFi.disconnect();
            connecting_ = false;
            lastAssocTryMs_ = now;
            if (firstAssocFail_) {
              firstAssocFail_ = false;
              LOG_W("LINK", "no AP visible, retry (assoc timeout)");
            } else {
              LOG_EVERY_N(12, "LINK", "no AP visible, retry (assoc timeout)");
            }
            break;
          case wifilink::AssocAction::kBegin:  // unreachable: connecting_ is true
          case wifilink::AssocAction::kWait:
            break;
        }
      }
    } else if (WiFi.status() != WL_CONNECTED) {
      markDown(now);
    }
    int pending = 0;
    while ((pending = udp_.parsePacket()) > 0) {
      uint8_t* buf = rxBuf_;
      const int got = udp_.read(buf, sizeof(rxBuf_));
      if (pending > static_cast<int>(sizeof(rxBuf_)) || got <= 0) {
        dropFirst(firstDrop_, "drop link datagram pending=%d got=%d (first of boot)", pending, got);
        LOG_EVERY_N(20, "LINK", "drop link datagram pending=%d got=%d", pending, got);
        ++udpDrop_;
        continue;
      }
      handleFrame(buf, static_cast<size_t>(got), udp_.remoteIP(), udp_.remotePort());
    }
    serviceStill();
  }

  // 1Hz HB TX + link-down watch (~3s / 3 missed ACKs -> re-associate,
  // re-AWAKE + seq resync from 0). Called every loop() after poll().
  void heartbeatTick() {
    const uint32_t now = millis();
    if (!linkUp_) {
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      markDown(now);
      return;
    }
    if (wifilink::shouldSendHb(now, lastHbMs_, CFG_LINK_HEARTBEAT_MS)) {
      lastHbMs_ = now;
      sendHb();
    }
    if (wifilink::linkDownAfter(hbSinceAck_) &&
        (now - lastAckMs_) >= 3u * static_cast<uint32_t>(CFG_LINK_HEARTBEAT_MS)) {
      LOG_W("LINK", "link down miss=%u hb=%lu ack=%lu (re-associate)", static_cast<unsigned>(hbSinceAck_),
            static_cast<unsigned long>(hbSent_), static_cast<unsigned long>(hbAckRx_));
      WiFi.disconnect();
      markDown(now);
      // NOTE: txSeq_ is NOT reset (stays monotonic for this boot). The core
      // gates strictly-greater within our bootId epoch, so the post-reassoc
      // HBs are fresh and the link self-heals. Resetting here used to wedge
      // the link permanently (every re-assoc HB dropped as stale).
    }
  }

  // One JPEG still on the next poll() (CLI `snap` trigger). Stills-first:
  // at most one frame per trigger, never continuous.
  void requestStill() {
    if (!linkUp_) {
      LOG_W("LINK", "snap ignored, link down");
      ++stillDrop_;
      return;
    }
    stillPending_ = true;
  }

  // CLI `link` / `status` door: one counters-only line (never payload bytes).
  void logStatus() const {
    LOG_I("LINK",
          "link state=%s assoc=%d rssi=%d ch=%u hb=%lu/%lu miss=%u cmd=%lu pair=%lu/%lu ota=%lu still=%lu/%lu "
          "fail=%lu ackT=%lu drop=%lu tx=%u boot=%08lx",
          linkUp_ ? "up" : "down", associated_ ? 1 : 0, static_cast<int>(lastRssi_),
          static_cast<unsigned>(lastChannel_), static_cast<unsigned long>(hbSent_),
          static_cast<unsigned long>(hbAckRx_), static_cast<unsigned>(hbSinceAck_), static_cast<unsigned long>(cmdRx_),
          static_cast<unsigned long>(pairShowRx_), static_cast<unsigned long>(pairDoneRx_),
          static_cast<unsigned long>(otaRx_), static_cast<unsigned long>(framesSent_),
          static_cast<unsigned long>(stillDrop_), static_cast<unsigned long>(txFail_),
          static_cast<unsigned long>(ackTx_), static_cast<unsigned long>(udpDrop_), static_cast<unsigned>(txSeq_),
          static_cast<unsigned long>(bootId_));
  }

  // Checked-send failures (udpsend::udpSendChecked false): rare/backpressure
  // class, WARNed at every occurrence (release-visible). Benign tear-tolerant
  // read; loop context only, never ISR.
  uint32_t txFail() const { return txFail_; }

 private:
  // Security-drop logging (bench lesson, same idiom as core's link_server.h):
  // the first occurrence per boot per class always logs at INFO; afterwards
  // the call-site LOG_EVERY_N throttle applies. Counters only here.
  template <typename... Args>
  void dropFirst(bool& first, const char* fmt, Args... args) {
    if (first) {
      first = false;
      LOG_I("LINK", fmt, args...);
    }
  }

  void beginAttempt(uint32_t now) {
    lastAssocTryMs_ = now;
    attemptStartMs_ = now;
    connecting_ = true;
    WiFi.begin(CFG_LINK_SSID, CFG_LINK_PASS);
    dropFirst(firstAttempt_, "assoc attempt ssid=%s", CFG_LINK_SSID);
  }

  void onAssoc(uint32_t now) {
    associated_ = true;
    linkUp_ = true;
    // txSeq_ is deliberately NOT reset here: the seq domain is monotonic
    // within a boot (the epoch is bootId, sent in every HB). Resetting it
    // would make every post-assoc HB look stale to the core's
    // strictly-greater gate — an unrecoverable wedge (bench-proven).
    haveCmdSeq_ = false;
    hbSinceAck_ = 0;
    lastAckMs_ = now;  // grace window for the first HB_ACK
    lastHbMs_ = now;
    const int rssiRaw = WiFi.RSSI();
    lastRssi_ = static_cast<int8_t>(rssiRaw < -128 ? -128 : (rssiRaw > 127 ? 127 : rssiRaw));
    lastChannel_ = static_cast<uint8_t>(WiFi.channel());
    LOG_I("LINK", "AWAKE assoc ch=%u seq=%u", static_cast<unsigned>(lastChannel_), static_cast<unsigned>(txSeq_));
    sendHb();
  }

  void markDown(uint32_t now) {
    associated_ = false;
    linkUp_ = false;
    connecting_ = false;  // next poll() starts a fresh attempt on cadence
    lastAssocTryMs_ = now;
    if (stillPending_) {
      stillPending_ = false;
      ++stillDrop_;
    }
  }

  void handleFrame(const uint8_t* buf, size_t len, const IPAddress& rip, uint16_t rport) {
    if (buf == nullptr) {
      return;
    }
    linkcode::Frame f;
    if (!linkcode::decodeFrame(buf, len, f)) {
      if (len >= 2 && !linkcode::isKnownType(buf[1])) {
        dropFirst(firstDrop_, "drop link unknown type=0x%02X (first of boot)", static_cast<unsigned>(buf[1]));
        LOG_EVERY_N(20, "LINK", "drop link unknown type=0x%02X", static_cast<unsigned>(buf[1]));
      } else {
        dropFirst(firstDrop_, "drop link malformed len=%u (first of boot)", static_cast<unsigned>(len));
        LOG_EVERY_N(20, "LINK", "drop link malformed len=%u", static_cast<unsigned>(len));
      }
      ++udpDrop_;
      return;
    }
    if (f.fragIdx != 0 || f.fragTotal != 1) {
      dropFirst(firstDrop_, "drop link frag idx=%u total=%u (first of boot)", static_cast<unsigned>(f.fragIdx),
                static_cast<unsigned>(f.fragTotal));
      LOG_EVERY_N(20, "LINK", "drop link frag idx=%u total=%u", static_cast<unsigned>(f.fragIdx),
                  static_cast<unsigned>(f.fragTotal));
      ++udpDrop_;
      return;
    }
    switch (f.type) {
      case linkcode::kTypeHbAck:
        handleHbAck(f);
        break;
      case linkcode::kTypeFace:
      case linkcode::kTypeOled:
      case linkcode::kTypeCamCtl:
      case linkcode::kTypePairShow:
      case linkcode::kTypePairDone:
      case linkcode::kTypeOta:
        handleCmd(f, rip, rport);
        break;
      case linkcode::kTypeAck:
        ++hbAckRx_;  // receipt for our HB/ACK frames; counted, never re-ACKed
        LOG_EVERY_N(20, "LINK", "ack acked=%u", static_cast<unsigned>(f.seq));
        break;
      case linkcode::kTypeHb:
      case linkcode::kTypeJpeg:
      case linkcode::kTypeQrBitmap:
        // Wrong direction on this socket (HB is ours to send, JPEG rides
        // TCP, QRBITMAP is reserved and never sent since the flag-day
        // removal). Silent throttled drop, counters only.
        dropFirst(firstDrop_, "drop link unexpected %s on udp (first of boot)", wifilink::typeName(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unexpected %s on udp", wifilink::typeName(f.type));
        ++udpDrop_;
        break;
      default:
        // Unreachable: decodeFrame rejects unknown types above. Kept so a
        // future codec bump fails closed instead of falling through.
        dropFirst(firstDrop_, "drop link unknown type=0x%02X (first of boot)", static_cast<unsigned>(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unknown type=0x%02X", static_cast<unsigned>(f.type));
        ++udpDrop_;
        break;
    }
  }

  void handleHbAck(const linkcode::Frame& f) {
    uint8_t channel = 0;
    uint32_t era = 0;
    if (f.payloadLen != linkcode::kHbAckLen || !linkcode::unpackHbAck(f.payload, f.payloadLen, channel, era)) {
      dropFirst(firstDrop_, "drop link hb_ack unpack (first of boot)");
      LOG_EVERY_N(20, "LINK", "drop link hb_ack unpack");
      ++udpDrop_;
      return;
    }
    (void)era;  // transport-only link: no rekey domain (core sends 0)
    lastChannel_ = channel;
    lastAckMs_ = millis();
    hbSinceAck_ = 0;
    ++hbAckRx_;
    if (firstHb_) {
      firstHb_ = false;
      LOG_I("LINK", "hb_ack seq=%u (first of boot)", static_cast<unsigned>(f.seq));
    } else {
      LOG_EVERY_N(10, "LINK", "hb_ack seq=%u", static_cast<unsigned>(f.seq));
    }
  }

  // Core->head command: freshness-gated; fresh executes once via the injected
  // edges + ACKs; duplicates only re-ACK, never re-execute; stale drops.
  // FACE/OLED/CAMCTL frames are value-checked through the shared codec
  // vocabulary (fail-closed, same drop shape as the old mapCmd gate) and
  // handed to onCommand; PAIRSHOW hands its 6 digits + validity window to
  // onPairShow + ACKs; PAIRDONE hands completion to onPairDone + ACKs;
  // OTA hands its URL bytes to onOtaUrl + ACKs. Duplicates of any of these
  // only re-ACK, never re-trigger.
  void handleCmd(const linkcode::Frame& f, const IPAddress& rip, uint16_t rport) {
    const wifilink::RxVerdict v = wifilink::classifyRx(haveCmdSeq_, lastCmdSeq_, f.seq);
    if (v == wifilink::RxVerdict::kStale) {
      dropFirst(firstStale_, "drop link stale seq=%u (first of boot)", static_cast<unsigned>(f.seq));
      LOG_EVERY_N(20, "LINK", "drop link stale seq=%u", static_cast<unsigned>(f.seq));
      ++udpDrop_;
      return;
    }
    if (v == wifilink::RxVerdict::kDuplicate) {
      sendAck(rip, rport, f.seq);  // re-ACK, never re-execute
      LOG_EVERY_N(20, "LINK", "redeliver seq=%u", static_cast<unsigned>(f.seq));
      return;
    }
    // Length gate mirrors wifilink::isOtaFrame (transport/link_codec.h); the
    // byte grammar + updater trigger are
    // the injected onOtaUrl edge (main binds OtaUpdater there).
    if (f.type == linkcode::kTypeOta && f.payloadLen >= linkcode::kOtaUrlMinLen &&
        f.payloadLen <= linkcode::kOtaUrlMaxLen) {
      haveCmdSeq_ = true;
      lastCmdSeq_ = f.seq;
      ++otaRx_;
      ++cmdRx_;
      sendAck(rip, rport, f.seq);
      if (handlers_.onOtaUrl == nullptr) {
        LOG_W("LINK", "ota not attached");
        return;
      }
      if (!handlers_.onOtaUrl(handlers_.ctx, f.payload, f.payloadLen)) {
        dropFirst(firstDrop_, "drop link ota grammar seq=%u (first of boot)", static_cast<unsigned>(f.seq));
        LOG_EVERY_N(20, "LINK", "drop link ota grammar seq=%u", static_cast<unsigned>(f.seq));
        ++udpDrop_;
        return;
      }
      LOG_I("LINK", "ota seq=%u len=%u", static_cast<unsigned>(f.seq), static_cast<unsigned>(f.payloadLen));
      return;
    }
    // Length gate mirrors wifilink::isPairShowFrame
    // (abilities/pairing/pairing_codec.h); the 6-digit vocabulary lives once in unpackPairShow (shared
    // digits+valid-secs contract). Duplicates only re-ACK, never re-render.
    if (f.type == linkcode::kTypePairShow && f.payloadLen == linkcode::kPairShowLen) {
      uint8_t pin[6] = {};
      uint16_t validSecs = 0;
      if (!linkcode::unpackPairShow(f.payload, f.payloadLen, pin, validSecs)) {
        // Bad digits: unexecutable, no ACK for a frame we cannot apply
        // (same fail-closed shape as the bad-value FACE/OLED/CAMCTL drop).
        dropFirst(firstDrop_, "drop link bad pairshow seq=%u (first of boot)", static_cast<unsigned>(f.seq));
        LOG_EVERY_N(20, "LINK", "drop link bad pairshow seq=%u", static_cast<unsigned>(f.seq));
        ++udpDrop_;
        return;
      }
      haveCmdSeq_ = true;
      lastCmdSeq_ = f.seq;
      ++pairShowRx_;
      ++cmdRx_;
      sendAck(rip, rport, f.seq);
      // Handoff (Pre-2): the 8B payload hands digits + window to the
      // injected edge synchronously (stack pin outlives this call; no heap,
      // no retained pointer). Any FACE/OTA command or the valid-secs
      // deadline clears it downstream. Re-arms on every fresh show.
      if (handlers_.onPairShow != nullptr) {
        handlers_.onPairShow(handlers_.ctx, pin, validSecs);
      }
      LOG_I("LINK", "pairshow seq=%u valid=%us", static_cast<unsigned>(f.seq), static_cast<unsigned>(validSecs));
      return;
    }
    // Length gate mirrors wifilink::isPairDoneFrame (canonical in the shim).
    // Duplicates only re-ACK, never re-clear.
    if (f.type == linkcode::kTypePairDone && f.payloadLen == linkcode::kPairDoneLen) {
      haveCmdSeq_ = true;
      lastCmdSeq_ = f.seq;
      ++pairDoneRx_;
      ++cmdRx_;
      sendAck(rip, rport, f.seq);
      // Handoff (Pre-2): pairing complete — the injected edge clears the
      // PIN/QR show (no-op when nothing is up, so only real transitions log
      // downstream).
      if (handlers_.onPairDone != nullptr) {
        handlers_.onPairDone(handlers_.ctx);
      }
      LOG_I("LINK", "pairdone seq=%u", static_cast<unsigned>(f.seq));
      return;
    }
    // FACE/OLED/CAMCTL only (the handleFrame switch routes everything else
    // elsewhere). Value-checked through the shared codec vocabulary — the
    // wire values ARE the vocabulary, validated once in link/link_codec.h,
    // never re-derived. False = unexecutable drop, no ACK for a frame we
    // cannot apply (same fail-closed shape as the old mapCmd gate; the
    // handler re-maps via wifilink::mapCmd, which cannot fail here).
    bool ok = false;
    switch (f.type) {
      case linkcode::kTypeFace: {
        uint8_t id = 0;
        ok = (f.payloadLen == linkcode::kFaceLen && f.payload != nullptr && linkcode::unpackFace(f.payload[0], id));
        break;
      }
      case linkcode::kTypeOled: {
        uint8_t mode = 0;
        ok = (f.payloadLen == linkcode::kOledLen && f.payload != nullptr && linkcode::unpackOled(f.payload[0], mode));
        break;
      }
      case linkcode::kTypeCamCtl: {
        uint8_t wire = 0;
        ok = (f.payloadLen == linkcode::kCamCtlLen && f.payload != nullptr &&
              linkcode::unpackCamCtl(f.payload[0], wire));
        break;
      }
      default:
        ok = false;
        break;
    }
    if (!ok) {
      dropFirst(firstDrop_, "drop link bad value seq=%u (first of boot)", static_cast<unsigned>(f.seq));
      LOG_W("LINK", "drop bad value seq=%u", static_cast<unsigned>(f.seq));
      ++udpDrop_;
      return;  // unexecutable: no ACK for a frame we cannot apply
    }
    haveCmdSeq_ = true;
    lastCmdSeq_ = f.seq;
    // LINK_CMD publish rides the injected edge (main owns the bus + the ONE
    // command table there): the wire type is NOT the CmdTarget value
    // (kTypeCamCtl=0x03 vs CmdTarget::CAM=0), so transport must not publish.
    if (handlers_.onCommand != nullptr) {
      handlers_.onCommand(handlers_.ctx, f);
    }
    ++cmdRx_;
    sendAck(rip, rport, f.seq);
    LOG_I("LINK", "cmd %s seq=%u", wifilink::typeName(f.type), static_cast<unsigned>(f.seq));
  }

  void sendAck(const IPAddress& ip, uint16_t port, uint16_t ackedSeq) {
    uint8_t payload[linkcode::kAckLen];
    if (!linkcode::packAck(ackedSeq, payload)) {
      return;
    }
    const size_t n =
        linkcode::encodeFrame(linkcode::kTypeAck, txSeq_++, payload, sizeof(payload), 0, 1, txBuf_, sizeof(txBuf_));
    if (n == 0) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, ip, port, txBuf_, n)) {
      ++txFail_;
      LOG_W("LINK", "udp send fail what=%s len=%u", "ack", static_cast<unsigned>(n));
      return;
    }
    ++ackTx_;
  }

  void sendHb() {
    const int rssiRaw = WiFi.RSSI();
    lastRssi_ = static_cast<int8_t>(rssiRaw < -128 ? -128 : (rssiRaw > 127 ? 127 : rssiRaw));
    lastChannel_ = static_cast<uint8_t>(WiFi.channel());
    uint8_t payload[linkcode::kHbLen];
    if (!linkcode::packHb(lastRssi_, lastChannel_, ESP.getFreeHeap(), bootId_, payload)) {
      return;
    }
    const size_t n =
        linkcode::encodeFrame(linkcode::kTypeHb, txSeq_++, payload, sizeof(payload), 0, 1, txBuf_, sizeof(txBuf_));
    if (n == 0) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, CFG_LINK_HOST, CFG_LINK_UDP_PORT, txBuf_, n)) {
      ++txFail_;
      // Never ACKed: counts as missed so the ~3s link-down watch stays live.
      ++hbSinceAck_;
      LOG_W("LINK", "udp send fail what=%s len=%u", "hb", static_cast<unsigned>(n));
      return;
    }
    ++hbSent_;
    ++hbSinceAck_;
  }

  // One pending still -> TCP 3335, chunked per the README MTU rule straight
  // out of the camera fb (PSRAM) through the single static send buffer. Never
  // holds two full frames. Feeds the WDT across the upload; failure only
  // counts (never asserts, never blocks boot/loop). The camera power gate is
  // the injected isCameraOn edge (main binds CameraDriver there).
  void serviceStill() {
    if (!stillPending_) {
      return;
    }
    stillPending_ = false;
    const bool camOn = (handlers_.isCameraOn != nullptr) ? handlers_.isCameraOn(handlers_.ctx) : false;
    if (!linkUp_ || !camOn) {
      LOG_W("LINK", "snap dropped (link %s, camera %s)", linkUp_ ? "up" : "down", camOn ? "on" : "off");
      ++stillDrop_;
      return;
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr || fb->buf == nullptr) {
      if (fb != nullptr) {
        esp_camera_fb_return(fb);
      }
      LOG_W("LINK", "snap dropped (no frame)");
      ++stillDrop_;
      return;
    }
    const size_t totalLen = fb->len;
    const size_t total = wifilink::fragTotalFor(totalLen);
    if (total == 0 || total > 255) {
      LOG_W("LINK", "snap dropped len=%u frags=%u", static_cast<unsigned>(totalLen), static_cast<unsigned>(total));
      ++stillDrop_;
      esp_camera_fb_return(fb);
      return;
    }
    WiFiClient tcp;
    tcp.setTimeout(1000);
    if (tcp.connect(CFG_LINK_HOST, CFG_LINK_TCP_PORT, 1000) != 1) {
      LOG_W("LINK", "snap dropped (tcp refused)");
      ++stillDrop_;
      esp_camera_fb_return(fb);
      return;
    }
    const size_t per = wifilink::perFramePayload();
    const uint8_t fragTotal = static_cast<uint8_t>(total);
    bool ok = true;
    for (uint8_t idx = 0; idx < fragTotal; ++idx) {
      size_t off = 0;
      size_t chunk = 0;
      if (!wifilink::fragSlice(totalLen, per, idx, fragTotal, off, chunk)) {
        ok = false;
        break;
      }
      const size_t n = linkcode::encodeFrame(linkcode::kTypeJpeg, txSeq_++, fb->buf + off, static_cast<uint16_t>(chunk),
                                             idx, fragTotal, txBuf_, sizeof(txBuf_));
      if (n == 0 || tcp.write(txBuf_, n) != n) {
        ok = false;
        break;
      }
      FaultManager::watchdogFeed();
    }
    tcp.stop();
    esp_camera_fb_return(fb);
    if (ok) {
      ++framesSent_;
      LOG_I("LINK", "still bytes=%u frags=%u", static_cast<unsigned>(totalLen), static_cast<unsigned>(total));
    } else {
      LOG_W("LINK", "snap dropped mid-send bytes=%u", static_cast<unsigned>(totalLen));
      ++stillDrop_;
    }
  }

  LinkHandlers handlers_;  // injected business edges (main binds, never owned here)
  WiFiUDP udp_;
  // Static frame buffers (LinkClient is a setup()-time global -> BSS, never
  // task stack, never heap). JPEG bytes stream from the camera fb in
  // MTU-sized chunks; only one in-flight frame buffer exists.
  uint8_t rxBuf_[linkcode::kMaxFrame];
  uint8_t txBuf_[linkcode::kMaxFrame];
  bool associated_;
  bool linkUp_;
  uint32_t lastAssocTryMs_;
  // Async-assoc attempt state: connecting_ is true between WiFi.begin() and
  // the connect/timeout verdict (see assocStep). attemptStartMs_ bounds the
  // attempt at CFG_LINK_ASSOC_TIMEOUT_MS.
  bool connecting_;
  uint32_t attemptStartMs_;
  bool firstAttempt_;
  uint32_t lastHbMs_;
  uint32_t lastAckMs_;
  uint8_t hbSinceAck_;
  // Boot epoch for the HB seq domain (see linkcode::kHbLen note): drawn once
  // per boot so the core resyncs instead of wedging as stale after a head
  // reboot. NEVER reseeded, never zeroed after begin.
  uint32_t bootId_;
  // Link tx counter: MONOTONIC within a boot — never reset on re-assoc (the
  // core gates strictly-greater within our bootId epoch; resetting here used
  // to wedge the link permanently after every re-associate).
  uint16_t txSeq_;
  bool haveCmdSeq_;
  uint16_t lastCmdSeq_;
  bool stillPending_;
  int8_t lastRssi_;
  uint8_t lastChannel_;
  uint32_t hbSent_;
  uint32_t hbAckRx_;
  uint32_t cmdRx_;
  uint32_t ackTx_;
  uint32_t pairShowRx_ = 0;
  uint32_t pairDoneRx_ = 0;
  uint32_t otaRx_ = 0;
  uint32_t framesSent_;
  uint32_t stillDrop_;
  uint32_t udpDrop_;
  uint32_t txFail_ = 0;
  bool firstAssocFail_;
  bool firstHb_;
  bool firstDrop_;
  bool firstStale_;
};

#endif  // ARDUINO
