#pragma once
// desky-core UART POC receiver — header-only service (poc-comm-link spike).
//
// Owns the S3 side of the barebones bi-directional UART transport:
//  - Serial1 link (RX=MCU_LINK_UART_RX=18, TX=MCU_LINK_UART_TX=17). TX stays
//    TRISTATED (RX-only begin, TX pin unassigned) until the first valid Head
//    frame — the S3 must never drive the CAM's GPIO12 strapping pin during
//    CAM reset. First valid frame attaches TX.
//  - Reassembly in one INTERNAL frame slot (~64KB cap, else drop).
//    Out-of-order tolerated, duplicates ignored, missing-at-LAST drops.
//  - Counters + machine-parseable single-line STATS for the sweep script.
//  - USB CLI: STATS | RESET | SET k v | GET k | GET all | HEAD SET k v |
//    HEAD GET k | HELP. Baud changes always run the deferred ACK-then-switch
//    protocol (CMD at old baud, 2s ACK wait else abort, both switch after a
//    delay, USB reminder to switch the monitor).
//
// Memory: RX staging + reassembly slot + TX encode buffer are heap_caps
// INTERNAL, never PSRAM (asserted via esp_ptr_internal where available).

#ifdef ARDUINO

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>

#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "link/poc_config.h"
#include "link/uart_frame.h"

#if __has_include(<esp_memory_utils.h>)
#include <esp_memory_utils.h>
#define POC_HAVE_INTERNAL_CHECK 1
#else
#define POC_HAVE_INTERNAL_CHECK 0
#endif

#ifndef MCU_LINK_UART_TX
#define MCU_LINK_UART_TX 17
#endif
#ifndef MCU_LINK_UART_RX
#define MCU_LINK_UART_RX 18
#endif

class PocLink {
 public:
  static constexpr size_t kSlotCap = 65536;  // One frame slot; bigger frames drop.
  static constexpr size_t kRxBuf = 2048;
  static constexpr size_t kRxRing = 4096;         // UART driver ring (INTERNAL); margin for pace-0 bursts at 2M+.
  static constexpr size_t kRxOvfWarn = 3584;      // available() at/above this counts an overflow-pressure event.
  static constexpr uint32_t kStaleChunkMs = 100;  // Partial frame idle this long => flush slot, count a drop.

  PocLink()
      : rxBuf_(nullptr),
        slot_(nullptr),
        txBuf_(nullptr),
        txEnabled_(false),
        txFrameId_(0),
        framesOk_(0),
        chunksRx_(0),
        hdrErr_(0),
        payErr_(0),
        drops_(0),
        ooo_(0),
        dups_(0),
        ovf_(0),
        hbRx_(0),
        bytesRx_(0),
        tFirstMs_(0),
        lastRxMs_(0),
        lastChunkMs_(0),
        oooFrame_(0xFFFF),
        nextExpected_(0),
        waitingResp_(false),
        respGot_(false),
        usbLen_(0) {
    respBuf_[0] = '\0';
  }

  void begin() {
    rxBuf_ = static_cast<uint8_t*>(heap_caps_malloc(kRxBuf, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(rxBuf_ != nullptr);
    slot_ = static_cast<uint8_t*>(heap_caps_malloc(kSlotCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(slot_ != nullptr);
    txBuf_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxFrameLen, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(txBuf_ != nullptr);
#if POC_HAVE_INTERNAL_CHECK
    DESKY_ASSERT(esp_ptr_internal(rxBuf_) && esp_ptr_internal(slot_) && esp_ptr_internal(txBuf_));
#endif
    reasm_.attach(slot_, kSlotCap);
    // RX-only: TX pin unassigned (-1) keeps GPIO17 high-impedance so the S3
    // never drives CAM GPIO12 during CAM reset (strapping requirement).
    Serial1.setRxBufferSize(kRxRing);  // 4KB INTERNAL ring: margin for pace-0 bursts at 2M+.
    Serial1.begin(cfg_.baud, SERIAL_8N1, MCU_LINK_UART_RX, -1);
    LOG_I("POC", "s3 up link rx=%d tx=tristated(until first head frame) baud=%u slot=%uKB", MCU_LINK_UART_RX,
          static_cast<unsigned>(cfg_.baud), static_cast<unsigned>(kSlotCap / 1024));
  }

  // Called from loop(): link RX pump + stale-partial flush + USB CLI.
  void poll() {
    pollLink();
    staleFlush_();
    pollUsb();
  }

  // A partial frame idle >kStaleChunkMs is dead (sender moved on): flush the slot,
  // count one drop. Without this a stalled partial squats reassembly until STALE.
  void staleFlush_() {
    if (uartpoc::reasmStaleDue(reasm_.active(), lastChunkMs_, millis(), kStaleChunkMs)) {
      reasm_.reset();
      ++drops_;
      lastChunkMs_ = millis();
    }
  }

 private:
  void enableTx_() {
    txEnabled_ = true;
    Serial1.end();
    Serial1.setRxBufferSize(kRxRing);
    Serial1.begin(cfg_.baud, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    LOG_I("POC", "first head frame heard, tx attached on gpio%d", MCU_LINK_UART_TX);
  }

  void pollLink() {
    int avail = Serial1.available();
    if (avail >= static_cast<int>(kRxOvfWarn)) {
      ++ovf_;  // At most once per poll: ring is nearly full, overflow pressure.
    }
    while (avail > 0) {
      size_t n = static_cast<size_t>(avail > 256 ? 256 : avail);
      if (n > kRxBuf) {
        n = kRxBuf;
      }
      for (size_t i = 0; i < n; ++i) {
        const int c = Serial1.read();
        if (c < 0) {
          n = i;
          break;
        }
        rxBuf_[i] = static_cast<uint8_t>(c);
      }
      size_t off = 0;
      while (off < n) {
        size_t consumed = 0;
        uartpoc::DecodedFrame fr;
        const uartpoc::DecodeStatus st = linkDec_.feed(rxBuf_ + off, n - off, consumed, fr);
        off += consumed;
        if (st == uartpoc::DecodeStatus::OK) {
          onFrame_(fr);
        } else if (st == uartpoc::DecodeStatus::ERR_HDR_CRC) {
          ++hdrErr_;
        } else if (st == uartpoc::DecodeStatus::ERR_PAY_CRC) {
          ++payErr_;
        } else {
          break;  // NEED_MORE: wait for more bytes.
        }
      }
      avail = Serial1.available();
    }
  }

  void onFrame_(const uartpoc::DecodedFrame& fr) {
    const uint32_t now = millis();
    lastRxMs_ = now;
    if (tFirstMs_ == 0) {
      tFirstMs_ = now;
    }
    if (!txEnabled_) {
      enableTx_();
    }
    if (fr.type == uartpoc::MSG_CHUNK) {
      ++chunksRx_;
      lastChunkMs_ = now;
      size_t total = 0;
      uartpoc::Reassembler::Push res = reasm_.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, total);
      if (res == uartpoc::Reassembler::Push::STALE) {
        ++drops_;  // Previous partial abandoned; re-push starts the new frame.
        res = reasm_.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, total);
      }
      if (res == uartpoc::Reassembler::Push::COMPLETE) {
        ++framesOk_;
        bytesRx_ += total;
        oooFrame_ = 0xFFFF;  // Fresh frame resets the OOO heuristic.
      } else if (res == uartpoc::Reassembler::Push::DROPPED) {
        ++drops_;
        oooFrame_ = 0xFFFF;
      } else if (res == uartpoc::Reassembler::Push::DUPLICATE) {
        ++dups_;
      } else {  // ACCEPTED: out-of-order heuristic (first chunk of a frame is free).
        if (fr.frameId != oooFrame_) {
          oooFrame_ = fr.frameId;
          nextExpected_ = 0;
        }
        if (fr.chunkIdx != nextExpected_) {
          ++ooo_;
        }
        if (fr.chunkIdx >= nextExpected_) {
          nextExpected_ = static_cast<uint16_t>(fr.chunkIdx + 1);
        }
      }
    } else if (fr.type == uartpoc::MSG_HB) {
      ++hbRx_;
    } else if (fr.type == uartpoc::MSG_RESP) {
      if (waitingResp_) {
        size_t i = 0;
        while (i + 1 < sizeof(respBuf_) && i < fr.payloadLen) {
          respBuf_[i] = static_cast<char>(fr.payload[i]);
          ++i;
        }
        respBuf_[i] = '\0';
        respGot_ = true;
      }
    }
    // MSG_CMD is unexpected on the S3 side: counted as traffic, otherwise ignored.
  }

  // Send a CMD frame and wait ≤2s for its RESP. True + respOut on reply.
  bool sendCmdAndWait_(const char* cmd, char* respOut, size_t respCap) {
    if (!txEnabled_) {
      LOG_W("POC", "no link yet (tx tristated), bridge refused");
      return false;
    }
    size_t n = 0;
    while (cmd[n] != '\0') {
      ++n;
    }
    size_t outLen = 0;
    if (!uartpoc::encodeFrame(uartpoc::MSG_CMD, 0, txFrameId_++, 0, reinterpret_cast<const uint8_t*>(cmd),
                              static_cast<uint16_t>(n), txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      LOG_W("POC", "cmd too long, refused");
      return false;
    }
    waitingResp_ = false;
    respGot_ = false;
    respBuf_[0] = '\0';
    Serial1.write(txBuf_, outLen);
    waitingResp_ = true;
    const uint32_t deadline = millis() + 2000;
    while (!respGot_ && (int32_t)(deadline - millis()) > 0) {
      pollLink();
      delay(1);
    }
    waitingResp_ = false;
    if (!respGot_) {
      return false;
    }
    size_t i = 0;
    while (i + 1 < respCap && respBuf_[i] != '\0') {
      respOut[i] = respBuf_[i];
      ++i;
    }
    respOut[i] = '\0';
    return true;
  }

  // Deferred baud protocol: CMD at old baud, ACK ≤2s else abort, then both
  // switch (head already switched at +100ms and waits silently for our HB).
  void baudProtocol_(uint32_t target) {
    if (target == cfg_.baud) {
      LOG_I("POC", "already at %u baud", static_cast<unsigned>(target));
      return;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "SET baud %u 100", static_cast<unsigned>(target));
    char resp[128];
    LOG_I("POC", "baud switch -> %u (cmd at %u)...", static_cast<unsigned>(target), static_cast<unsigned>(cfg_.baud));
    if (!sendCmdAndWait_(cmd, resp, sizeof(resp))) {
      LOG_W("POC", "baud switch aborted: RESP timeout (staying at %u)", static_cast<unsigned>(cfg_.baud));
      return;
    }
    if (!startsWith_(resp, "ACK")) {
      LOG_W("POC", "baud switch aborted: %s", resp);
      return;
    }
    delay(250);  // Head switched at ACK+100ms; we switch at ACK+250ms, no garbage window.
    cfg_.baud = target;
    Serial1.updateBaudRate(target);
    // Announce at the new baud so a freshly-switched head resumes streaming.
    sendHb_();
    sendHb_();
    LOG_I("POC", "baud now %u. MONITOR: switch USB monitor to %u baud", static_cast<unsigned>(target),
          static_cast<unsigned>(target));
  }

  void sendHb_() {
    size_t outLen = 0;
    if (uartpoc::encodeFrame(uartpoc::MSG_HB, 0, txFrameId_++, 0, nullptr, 0, txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      Serial1.write(txBuf_, outLen);
    }
  }

  void pollUsb() {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\n') {
        usbBuf_[usbLen_] = '\0';
        handleUsbLine_(usbBuf_);
        usbLen_ = 0;
      } else if (c != '\r') {
        if (usbLen_ + 1 < sizeof(usbBuf_)) {
          usbBuf_[usbLen_++] = c;
        } else {
          LOG_W("POC", "usb line too long, dropped");
          usbLen_ = 0;
        }
      }
    }
  }

  void handleUsbLine_(const char* line) {
    if (startsWith_(line, "HEAD ")) {
      const char* rest = line + 5;
      if (startsWith_(rest, "SET ") || startsWith_(rest, "GET ")) {
        // Baud bridges always carry the deferred-switch apply delay.
        if (startsWith_(rest, "SET baud ")) {
          uint32_t target = 0;
          if (uartpoc::parseU32(skipSpaces_(rest + 9), target) && uartpoc::isValidBaud(target)) {
            baudProtocol_(target);
          } else {
            LOG_W("POC", "NACK bad_baud");
          }
          return;
        }
        char resp[128];
        if (sendCmdAndWait_(rest, resp, sizeof(resp))) {
          LOG_I("POC", "HEAD RESP: %s", resp);
        } else {
          LOG_W("POC", "HEAD RESP: TIMEOUT");
        }
        return;
      }
      LOG_W("POC", "usage: HEAD SET k v | HEAD GET k");
      return;
    }
    if (startsWith_(line, "SET ")) {
      const char* kv = line + 4;
      char key[16];
      char val[16];
      if (splitKV_(kv, key, sizeof(key), val, sizeof(val)) != 2) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      if (uartpoc::keyEq(key, "baud")) {
        uint32_t target = 0;
        if (uartpoc::parseU32(val, target) && uartpoc::isValidBaud(target)) {
          baudProtocol_(target);
        } else {
          LOG_W("POC", "NACK bad_baud");
        }
        return;
      }
      char msg[64];
      uartpoc::PocConfig probe = cfg_;
      if (uartpoc::parseSet(key, val, probe, msg, sizeof(msg))) {
        cfg_ = probe;
      }
      LOG_I("POC", "%s", msg);
      return;
    }
    if (startsWith_(line, "GET ")) {
      char key[16];
      if (splitKey_(line + 4, key, sizeof(key)) != 1) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      char kv[96];
      uartpoc::formatGet(cfg_, key, kv, sizeof(kv));
      LOG_I("POC", "%s", kv);
      return;
    }
    if (isWord_(line, "STATS")) {
      char msg[224];
      buildStats_(msg, sizeof(msg));
      LOG_I("POC", "%s", msg);
      return;
    }
    if (isWord_(line, "RESET")) {
      framesOk_ = chunksRx_ = hdrErr_ = payErr_ = drops_ = ooo_ = dups_ = ovf_ = hbRx_ = bytesRx_ = 0;
      tFirstMs_ = 0;
      lastChunkMs_ = 0;
      oooFrame_ = 0xFFFF;
      reasm_.reset();
      linkDec_.reset();
      LOG_I("POC", "counters reset");
      return;
    }
    if (isWord_(line, "HELP")) {
      LOG_I("POC", "cmds: STATS | RESET | SET k v | GET k | GET all | HEAD SET k v | HEAD GET k | HELP");
      LOG_I("POC", "keys: chunk|chunk_bytes 16..1024 pace|pace_us 0..50000 baud <list incl 1M-5M> mode 0..3 fps 1..30");
      return;
    }
    if (line[0] != '\0') {
      LOG_W("POC", "unknown cmd (try HELP)");
    }
  }

  void buildStats_(char* out, size_t cap) {
    const uint32_t now = millis();
    const uint32_t elapsed = (tFirstMs_ == 0 || now <= tFirstMs_) ? 0 : now - tFirstMs_;
    const unsigned long kbps = (elapsed < 1000) ? 0 : static_cast<unsigned long>(bytesRx_ * 8 / elapsed);
    const unsigned long kbs = (elapsed < 1000) ? 0 : static_cast<unsigned long>(bytesRx_ / elapsed);
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // Single machine-parseable line (sweep script scrapes k=v tokens).
    // kbps = kilobits/s, kbs = KB/s per spec (bytes/elapsed).
    snprintf(out, cap,
             "STATS ok=%lu chunks=%lu herr=%lu perr=%lu drops=%lu ooo=%lu dups=%lu ovf=%lu hb=%lu bytes=%lu kbps=%lu "
             "kbs=%lu intfree=%u up=%lu",
             static_cast<unsigned long>(framesOk_), static_cast<unsigned long>(chunksRx_),
             static_cast<unsigned long>(hdrErr_), static_cast<unsigned long>(payErr_),
             static_cast<unsigned long>(drops_), static_cast<unsigned long>(ooo_), static_cast<unsigned long>(dups_),
             static_cast<unsigned long>(ovf_), static_cast<unsigned long>(hbRx_), static_cast<unsigned long>(bytesRx_),
             kbps, kbs, static_cast<unsigned>(intFree), static_cast<unsigned long>(elapsed));
  }

  static const char* skipSpaces_(const char* s) {
    while (*s == ' ' || *s == '\t') {
      ++s;
    }
    return s;
  }

  static bool startsWith_(const char* s, const char* prefix) {
    while (*prefix != '\0') {
      if (*s++ != *prefix++) {
        return false;
      }
    }
    return true;
  }

  static bool isWord_(const char* s, const char* word) {
    while (*word != '\0') {
      if (*s++ != *word++) {
        return false;
      }
    }
    s = skipSpaces_(s);
    return *s == '\0';
  }

  static int splitKV_(const char* s, char* key, size_t keyCap, char* val, size_t valCap) {
    s = skipSpaces_(s);
    size_t i = 0;
    while (*s != '\0' && *s != ' ' && *s != '\t') {
      if (i + 1 < keyCap) {
        key[i++] = *s;
      }
      ++s;
    }
    key[i] = '\0';
    s = skipSpaces_(s);
    if (*s == '\0') {
      return 1;
    }
    i = 0;
    while (*s != '\0' && *s != ' ' && *s != '\t') {
      if (i + 1 < valCap) {
        val[i++] = *s;
      }
      ++s;
    }
    val[i] = '\0';
    s = skipSpaces_(s);
    if (*s != '\0') {
      return -1;
    }
    return 2;
  }

  static int splitKey_(const char* s, char* key, size_t keyCap) {
    s = skipSpaces_(s);
    size_t i = 0;
    while (*s != '\0' && *s != ' ' && *s != '\t') {
      if (i + 1 < keyCap) {
        key[i++] = *s;
      }
      ++s;
    }
    key[i] = '\0';
    s = skipSpaces_(s);
    if (*s != '\0' || key[0] == '\0') {
      return -1;
    }
    return 1;
  }

  uartpoc::PocConfig cfg_;
  uartpoc::Decoder linkDec_;
  uartpoc::Reassembler reasm_;
  uint8_t* rxBuf_;  // INTERNAL UART read staging (kRxBuf).
  uint8_t* slot_;   // INTERNAL one-frame reassembly slot (kSlotCap).
  uint8_t* txBuf_;  // INTERNAL CMD/HB encode staging (kMaxFrameLen).
  bool txEnabled_;
  uint16_t txFrameId_;
  uint32_t framesOk_;
  uint32_t chunksRx_;
  uint32_t hdrErr_;
  uint32_t payErr_;
  uint32_t drops_;
  uint32_t ooo_;
  uint32_t dups_;
  uint32_t ovf_;  // RX overflow-pressure events (available() >= kRxOvfWarn at poll entry).
  uint32_t hbRx_;
  uint32_t bytesRx_;
  uint32_t tFirstMs_;
  uint32_t lastRxMs_;
  uint32_t lastChunkMs_;  // Last CHUNK arrival; drives the stale-partial flush.
  uint16_t oooFrame_;
  uint16_t nextExpected_;
  bool waitingResp_;
  bool respGot_;
  char respBuf_[128];
  char usbBuf_[128];
  size_t usbLen_;
};

#endif  // ARDUINO
