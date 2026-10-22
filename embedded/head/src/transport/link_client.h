#pragma once
// desky-head WiFi link client (B-link) - TRANSPORT layer, dumb pipe.
// Sockets, framing, peer/assoc/HB state only; business arrives as injected
// LinkHandlers callbacks (stored, never owned, bound in main.cpp).
// UDP 3334: HB head->core at 1Hz + ACK replies; RX FACE/OLED/CAMCTL/
// PAIRSHOW/PAIRDONE/OTA + HB_ACK. Link down after 3 missed ACKs (~3s).
// Tx seq is monotonic within a boot; the per-boot bootId epoch lets the
// core resync after a head reboot. TCP 3335: JPEG stills, one frame per
// `snap` trigger. Logs are counters-only, never payload bytes.
// Association is async with throttled retry; absence of the AP never
// blocks boot. poll() + heartbeatTick() run on the Arduino loop.

#include <stddef.h>
#include <stdint.h>

#include "transport/link_codec.h"

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

// Injected business edges (stored, never owned). main.cpp binds them.
struct LinkHandlers {
  void* ctx = nullptr;
  // Validated FACE/OLED/CAMCTL frame. Handler maps, publishes, executes.
  void (*onCommand)(void* ctx, const linkcode::Frame& f) = nullptr;
  // Validated PAIRSHOW payload (digits + window).
  void (*onPairShow)(void* ctx, const uint8_t pin6[6], uint16_t validSecs) = nullptr;
  // Validated PAIRDONE frame (presence is the signal).
  void (*onPairDone)(void* ctx) = nullptr;
  // Raw OTA URL bytes (length-gated only). True = consumed.
  bool (*onOtaUrl)(void* ctx, const uint8_t* url, uint16_t len) = nullptr;
  // Camera gate for the TCP stills pump.
  bool (*isCameraOn)(void* ctx) = nullptr;
};

#ifdef ARDUINO
// ── Firmware: WiFi STA link client (UDP HB/commands + TCP JPEG stills) ─────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_camera.h>
#include <esp_system.h>

#include "common/fault_manager.h"
#include "common/logger.h"
#include "common/udp_send.h"
#include "config.h"
#include "head_context.h"

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

  // Call once in setup(). Never blocks boot: association is async.
  void begin() {
    bootId_ = esp_random();  // Per-boot epoch; never reseeded (core resyncs on it).
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    udp_.begin(CFG_LINK_UDP_PORT);
    LOG_I("LINK", "wifi sta up udp=%u tcp=%u ap=%s", static_cast<unsigned>(CFG_LINK_UDP_PORT),
          static_cast<unsigned>(CFG_LINK_TCP_PORT), CFG_LINK_SSID);
    beginAttempt(millis());
  }

  // Stored, never owned. Call once in setup() after begin().
  void setHandlers(const LinkHandlers& h) { handlers_ = h; }

  // Drains inbound UDP, drives association while down, services one still.
  void poll() {
    const uint32_t now = millis();
    if (!associated_) {
      // Never re-begin() an in-flight handshake here: that aborts it.
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
          case wifilink::AssocAction::kBegin:
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

  // 1Hz HB TX + link-down watch (3 missed ACKs -> re-associate). Called every loop() after poll().
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
    }
  }

  // One JPEG still on the next poll() (`snap` trigger, never continuous).
  void requestStill() {
    if (!linkUp_) {
      LOG_W("LINK", "snap ignored, link down");
      ++stillDrop_;
      return;
    }
    stillPending_ = true;
  }

  // CLI `link` / `status` door: counters only.
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

  uint32_t txFail() const { return txFail_; }

 private:
  // First drop per boot per class logs at INFO; afterwards throttled.
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
    haveCmdSeq_ = false;
    hbSinceAck_ = 0;
    lastAckMs_ = now;
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
    connecting_ = false;
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
        ++hbAckRx_;  // counted, never re-ACKed
        LOG_EVERY_N(20, "LINK", "ack acked=%u", static_cast<unsigned>(f.seq));
        break;
      case linkcode::kTypeHb:
      case linkcode::kTypeJpeg:
      case linkcode::kTypeQrBitmap:
        // Wrong direction on this socket: throttled drop, counters only.
        dropFirst(firstDrop_, "drop link unexpected %s on udp (first of boot)", wifilink::typeName(f.type));
        LOG_EVERY_N(20, "LINK", "drop link unexpected %s on udp", wifilink::typeName(f.type));
        ++udpDrop_;
        break;
      default:
        // Unreachable: decodeFrame rejects unknown types above (fails closed).
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
    (void)era;
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

  // Fresh executes once via the injected edges + ACKs; duplicates only
  // re-ACK, never re-execute; stale drops. Unappliable frames get no ACK.
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
    // OTA length gate (mirrors wifilink::isOtaFrame).
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
    // PAIRSHOW length gate (mirrors wifilink::isPairShowFrame).
    if (f.type == linkcode::kTypePairShow && f.payloadLen == linkcode::kPairShowLen) {
      uint8_t pin[6] = {};
      uint16_t validSecs = 0;
      if (!linkcode::unpackPairShow(f.payload, f.payloadLen, pin, validSecs)) {
        // Bad digits: unexecutable, no ACK for a frame we cannot apply.
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
      if (handlers_.onPairShow != nullptr) {
        handlers_.onPairShow(handlers_.ctx, pin, validSecs);
      }
      LOG_I("LINK", "pairshow seq=%u valid=%us", static_cast<unsigned>(f.seq), static_cast<unsigned>(validSecs));
      return;
    }
    // PAIRDONE length gate (mirrors wifilink::isPairDoneFrame).
    if (f.type == linkcode::kTypePairDone && f.payloadLen == linkcode::kPairDoneLen) {
      haveCmdSeq_ = true;
      lastCmdSeq_ = f.seq;
      ++pairDoneRx_;
      ++cmdRx_;
      sendAck(rip, rport, f.seq);
      if (handlers_.onPairDone != nullptr) {
        handlers_.onPairDone(handlers_.ctx);
      }
      LOG_I("LINK", "pairdone seq=%u", static_cast<unsigned>(f.seq));
      return;
    }
    // FACE/OLED/CAMCTL only. Value-checked through the shared codec
    // vocabulary (fail-closed); the handler re-maps via wifilink::mapCmd.
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
      return;
    }
    haveCmdSeq_ = true;
    lastCmdSeq_ = f.seq;
    // Publish rides the injected edge: the wire type is NOT the CmdTarget value.
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

  // One pending still -> TCP 3335, chunked from the camera fb. Failure counts, never asserts.
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

  LinkHandlers handlers_;
  WiFiUDP udp_;
  uint8_t rxBuf_[linkcode::kMaxFrame];
  uint8_t txBuf_[linkcode::kMaxFrame];
  bool associated_;
  bool linkUp_;
  uint32_t lastAssocTryMs_;
  bool connecting_;
  uint32_t attemptStartMs_;
  bool firstAttempt_;
  uint32_t lastHbMs_;
  uint32_t lastAckMs_;
  uint8_t hbSinceAck_;
  uint32_t bootId_;  // Per-boot epoch; never reseeded.
  uint16_t txSeq_;   // Monotonic within a boot; never reset on re-assoc.
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
