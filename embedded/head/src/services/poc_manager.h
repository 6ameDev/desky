#pragma once
// desky-head UART POC sender — header-only service (poc-comm-link spike).
//
// Owns the Head side of the barebones bi-directional UART transport:
//  - Serial2 link (TX=MCU_LINK_UART_TX=12, RX=MCU_LINK_UART_RX=13) + USB CLI.
//  - Generator task (Core 0): one source frame per fps tick, fragmented into
//    chunk_bytes CHUNK frames (LAST_CHUNK on final, SYNTHETIC for modes 1/2),
//    pace_us gap between chunks. Modes: 0 TEXT, 1 RAMP, 2 SYNTH_JPEG
//    (pseudorandom + SOI/EOI markers only, NOT decodable), 3 HW_CAM (lazy
//    camera init on first mode-3 tick; PSRAM fb slices copied into INTERNAL
//    staging before Serial2.write).
//  - CMD RX on Serial2 -> apply -> RESP ACK/NACK; deferred baud switch
//    (RESP at old baud, both switch after delay, 3s rollback to 115200).
//
// Memory: TX encode + chunk staging buffers are heap_caps INTERNAL, never
// PSRAM (asserted via esp_ptr_internal where available). No String/heap in
// the loop path. Idle HB at 1Hz when streaming is stopped or held.

#ifdef ARDUINO

#include <Arduino.h>
#include <esp_camera.h>
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
#define MCU_LINK_UART_TX 12
#endif
#ifndef MCU_LINK_UART_RX
#define MCU_LINK_UART_RX 13
#endif
#ifndef CFG_CAMERA_JPEG_QUALITY
#define CFG_CAMERA_JPEG_QUALITY 12
#endif

class PocManager {
 public:
  PocManager()
      : txBuf_(nullptr),
        chunkBuf_(nullptr),
        streaming_(true),
        txHold_(false),
        txFrameId_(0),
        nextTickMs_(0),
        lineNo_(0),
        camInit_(false),
        camOk_(false),
        lastCamWarnMs_(0),
        baudSwitchAtMs_(0),
        pendingBaud_(115200),
        waitingForLink_(false),
        waitLinkUntilMs_(0),
        lastValidRxMs_(0),
        lastHbMs_(0),
        txFrames_(0),
        txChunks_(0),
        txBytes_(0),
        rxCmds_(0),
        rxInvalid_(0),
        usbLen_(0),
        genTask_(nullptr) {}

  void begin() {
    // Setup order (core convention): logger + fault already up in main before
    // this runs; config defaults live in cfg_; banner is main's job.
    txBuf_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxFrameLen, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(txBuf_ != nullptr);
    chunkBuf_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxPayload, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(chunkBuf_ != nullptr);
#if POC_HAVE_INTERNAL_CHECK
    DESKY_ASSERT(esp_ptr_internal(txBuf_) && esp_ptr_internal(chunkBuf_));
#endif
    Serial2.begin(cfg_.baud, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    linkDec_.reset();
    const uint32_t now = millis();
    nextTickMs_ = now;
    lastHbMs_ = now;
    lastValidRxMs_ = now;
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&PocManager::genTaskThunk, "pocgen", 4096, this, tskIDLE_PRIORITY + 2, &genTask_, 0);
    DESKY_ASSERT(ok == pdPASS);
    LOG_I("POC", "head up link tx=%d rx=%d baud=%u chunk=%u pace=%u mode=%u fps=%u", MCU_LINK_UART_TX, MCU_LINK_UART_RX,
          static_cast<unsigned>(cfg_.baud), cfg_.chunk_bytes, static_cast<unsigned>(cfg_.pace_us), cfg_.mode, cfg_.fps);
  }

  // Called from loop(): USB CLI + link CMD RX + HB tick + baud state machine.
  void poll() {
    pollUsb();
    pollLink();
    const uint32_t now = millis();
    if (baudSwitchAtMs_ != 0 && (int32_t)(now - baudSwitchAtMs_) >= 0) {
      baudSwitchAtMs_ = 0;
      Serial2.updateBaudRate(pendingBaud_);
      cfg_.baud = pendingBaud_;
      lastAppliedBaud_ = pendingBaud_;
      waitingForLink_ = true;
      waitLinkUntilMs_ = now + 3000;
      LOG_I("POC", "baud switched to %u, waiting for link", static_cast<unsigned>(cfg_.baud));
    }
    if (waitingForLink_ && (int32_t)(now - waitLinkUntilMs_) >= 0) {
      waitingForLink_ = false;
      txHold_ = false;
      cfg_.baud = 115200;
      lastAppliedBaud_ = 115200;
      Serial2.updateBaudRate(115200);
      LOG_W("POC", "no link at new baud, rolled back to 115200");
      sendHb();
      sendHb();
    }
    if ((!streaming_ || txHold_ || waitingForLink_) && now - lastHbMs_ >= 1000) {
      lastHbMs_ = now;
      sendHb();
    }
  }

 private:
  static constexpr uint32_t kTextTotal = 512;
  static constexpr uint32_t kRampTotal = 1024;
  static constexpr uint32_t kJpegTotal = 2048;

  static void genTaskThunk(void* arg) { static_cast<PocManager*>(arg)->runGen(); }

  // Generator body (Core 0, never WDT-subscribed, never returns).
  void runGen() {
    for (;;) {
      const bool hold = txHold_ || waitingForLink_;
      if (streaming_ && !hold) {
        const uint32_t now = millis();
        const uint16_t fps = cfg_.fps < uartpoc::kFpsMin ? uartpoc::kFpsMin : cfg_.fps;
        if ((int32_t)(now - nextTickMs_) >= 0) {
          nextTickMs_ = now + 1000 / fps;
          const uint8_t mode = cfg_.mode;
          const uint16_t chunk = cfg_.chunk_bytes;
          const uint32_t pace = cfg_.pace_us;
          const uint16_t fid = txFrameId_++;
          sendSourceFrame(mode, fid, chunk, pace);
        }
      }
      delay(2);
    }
  }

  void sendSourceFrame(uint8_t mode, uint16_t fid, uint16_t chunk, uint32_t pace) {
    if (mode == uartpoc::MODE_HW_CAM) {
      sendCameraFrame(fid, chunk, pace);
      return;
    }
    uint32_t total = kTextTotal;
    uint8_t flags = 0;
    if (mode == uartpoc::MODE_SYNTH_RAMP) {
      total = kRampTotal;
      flags = uartpoc::FLAG_SYNTHETIC;
    } else if (mode == uartpoc::MODE_SYNTH_JPEG) {
      total = kJpegTotal;
      flags = uartpoc::FLAG_SYNTHETIC;
    }
    const uint16_t stride = (chunk < uartpoc::kChunkMin) ? uartpoc::kChunkMin : chunk;
    const uint32_t nChunks = (total + stride - 1) / stride;
    for (uint32_t idx = 0; idx < nChunks; ++idx) {
      const uint32_t off = idx * stride;
      uint16_t n = stride;
      if (off + n > total) {
        n = static_cast<uint16_t>(total - off);
      }
      fillSynthetic(mode, fid, off, chunkBuf_, n, total);
      uint8_t fl = flags;
      if (idx + 1 >= nChunks) {
        fl |= uartpoc::FLAG_LAST_CHUNK;
      }
      writeChunk(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), chunkBuf_, n);
      if (idx + 1 < nChunks && pace > 0) {
        delayMicroseconds(pace);
      }
    }
    ++txFrames_;
  }

  void fillSynthetic(uint8_t mode, uint16_t fid, uint32_t off, uint8_t* dst, uint16_t n, uint32_t total) {
    if (mode == uartpoc::MODE_SYNTH_RAMP) {
      for (uint16_t i = 0; i < n; ++i) {
        dst[i] = static_cast<uint8_t>((off + i) & 0xFF);
      }
      return;
    }
    if (mode == uartpoc::MODE_SYNTH_JPEG) {
      // Xorshift32 regenerated per chunk (deterministic in offset); SOI/EOI
      // markers only — payload is NOT decodable JPEG by design.
      uint32_t x = static_cast<uint32_t>(fid) * 2654435761UL + 1;
      if (x == 0) {
        x = 1;
      }
      for (uint32_t i = 0; i < off + n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if (i >= off) {
          dst[i - off] = static_cast<uint8_t>(x & 0xFF);
        }
      }
      if (off == 0 && n >= 2) {
        dst[0] = 0xFF;
        dst[1] = 0xD8;
      }
      if (off + n >= total && n >= 2) {
        dst[n - 2] = 0xFF;
        dst[n - 1] = 0xD9;
      }
      return;
    }
    // MODE_TEXT: incrementing 32B lines "TXT fid:off " + alpha filler.
    for (uint16_t i = 0; i < n;) {
      char head[24];
      const uint32_t lineOff = off + i;
      snprintf(head, sizeof(head), "TXT %04u:%04u ", static_cast<unsigned>(fid), static_cast<unsigned>(lineOff));
      uint16_t h = 0;
      while (head[h] != '\0' && i < n) {
        dst[i++] = static_cast<uint8_t>(head[h++]);
      }
      while (i < n && ((off + i) % 32) != 31) {
        dst[i] = static_cast<uint8_t>('a' + ((off + i) % 26));
        ++i;
      }
      if (i < n) {
        dst[i++] = '\n';
      }
    }
    ++lineNo_;
  }

  void sendCameraFrame(uint16_t fid, uint16_t chunk, uint32_t pace) {
    if (!ensureCamera()) {
      return;
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      return;
    }
    const uint16_t stride = (chunk < uartpoc::kChunkMin) ? uartpoc::kChunkMin : chunk;
    const uint32_t total = static_cast<uint32_t>(fb->len);
    const uint32_t nChunks = (total + stride - 1) / stride;
    if (nChunks <= uartpoc::Reassembler::kMaxChunks) {
      for (uint32_t idx = 0; idx < nChunks; ++idx) {
        const uint32_t off = idx * stride;
        uint16_t n = stride;
        if (off + n > total) {
          n = static_cast<uint16_t>(total - off);
        }
        for (uint16_t i = 0; i < n; ++i) {
          chunkBuf_[i] = fb->buf[off + i];  // PSRAM fb -> INTERNAL staging.
        }
        uint8_t fl = 0;
        if (idx + 1 >= nChunks) {
          fl |= uartpoc::FLAG_LAST_CHUNK;
        }
        writeChunk(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), chunkBuf_, n);
        if (idx + 1 < nChunks && pace > 0) {
          delayMicroseconds(pace);
        }
      }
      ++txFrames_;
    }
    esp_camera_fb_return(fb);
  }

  bool ensureCamera() {
    if (camInit_) {
      if (!camOk_) {
        const uint32_t now = millis();
        if (now - lastCamWarnMs_ >= 2000) {
          lastCamWarnMs_ = now;
          LOG_W("POC", "camera unavailable, mode 3 frames skipped");
        }
      }
      return camOk_;
    }
    camInit_ = true;
    camera_config_t cc = {};
    cc.ledc_channel = LEDC_CHANNEL_0;
    cc.ledc_timer = LEDC_TIMER_0;
    cc.pin_d0 = MCU_CAM_PIN_Y2;
    cc.pin_d1 = MCU_CAM_PIN_Y3;
    cc.pin_d2 = MCU_CAM_PIN_Y4;
    cc.pin_d3 = MCU_CAM_PIN_Y5;
    cc.pin_d4 = MCU_CAM_PIN_Y6;
    cc.pin_d5 = MCU_CAM_PIN_Y7;
    cc.pin_d6 = MCU_CAM_PIN_Y8;
    cc.pin_d7 = MCU_CAM_PIN_Y9;
    cc.pin_xclk = MCU_CAM_PIN_XCLK;
    cc.pin_pclk = MCU_CAM_PIN_PCLK;
    cc.pin_vsync = MCU_CAM_PIN_VSYNC;
    cc.pin_href = MCU_CAM_PIN_HREF;
    cc.pin_sccb_sda = MCU_CAM_PIN_SIOD;
    cc.pin_sccb_scl = MCU_CAM_PIN_SIOC;
    cc.pin_pwdn = MCU_CAM_PIN_PWDN;
    cc.pin_reset = MCU_CAM_PIN_RESET;
    cc.xclk_freq_hz = 20000000;
    cc.pixel_format = PIXFORMAT_JPEG;
    cc.frame_size = FRAMESIZE_QVGA;
    cc.jpeg_quality = CFG_CAMERA_JPEG_QUALITY;
    cc.fb_count = 1;
    if (esp_camera_init(&cc) == ESP_OK) {
      camOk_ = true;
      LOG_I("POC", "camera lazy-init ok (qvga jpeg)");
    } else {
      LOG_W("POC", "camera lazy-init failed, mode 3 frames skipped");
    }
    return camOk_;
  }

  void writeChunk(uint8_t type, uint8_t flags, uint16_t fid, uint16_t idx, const uint8_t* payload, uint16_t n) {
    size_t outLen = 0;
    if (!uartpoc::encodeFrame(type, flags, fid, idx, payload, n, txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      return;
    }
    txBytes_ += Serial2.write(txBuf_, outLen);
    ++txChunks_;
  }

  void sendHb() {
    size_t outLen = 0;
    if (uartpoc::encodeFrame(uartpoc::MSG_HB, 0, txFrameId_++, 0, nullptr, 0, txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      txBytes_ += Serial2.write(txBuf_, outLen);
    }
  }

  void sendResp(uint16_t cmdId, const char* text) {
    size_t n = 0;
    while (text[n] != '\0') {
      ++n;
    }
    size_t outLen = 0;
    if (uartpoc::encodeFrame(uartpoc::MSG_RESP, 0, cmdId, 0, reinterpret_cast<const uint8_t*>(text),
                             static_cast<uint16_t>(n), txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      txBytes_ += Serial2.write(txBuf_, outLen);
    }
  }

  void pollLink() {
    while (Serial2.available() > 0) {
      const uint8_t b = static_cast<uint8_t>(Serial2.read());
      size_t consumed = 0;
      uartpoc::DecodedFrame fr;
      const uartpoc::DecodeStatus st = linkDec_.feed(&b, 1, consumed, fr);
      if (st == uartpoc::DecodeStatus::OK) {
        lastValidRxMs_ = millis();
        if (waitingForLink_) {  // Any valid frame at the new baud resumes us.
          waitingForLink_ = false;
          txHold_ = false;
          LOG_I("POC", "link confirmed at %u baud, streaming resumed", static_cast<unsigned>(cfg_.baud));
        }
        if (fr.type == uartpoc::MSG_CMD) {
          ++rxCmds_;
          handleCmd(fr);
        }
      } else if (st == uartpoc::DecodeStatus::ERR_HDR_CRC || st == uartpoc::DecodeStatus::ERR_PAY_CRC) {
        ++rxInvalid_;
      }
    }
  }

  void handleCmd(const uartpoc::DecodedFrame& fr) {
    if (fr.payloadLen >= sizeof(cmdBuf_)) {
      sendResp(fr.frameId, "NACK too_long");
      return;
    }
    for (uint16_t i = 0; i < fr.payloadLen; ++i) {
      cmdBuf_[i] = static_cast<char>(fr.payload[i]);
    }
    cmdBuf_[fr.payloadLen] = '\0';
    const char* p = cmdBuf_;
    if (startsWith_(p, "SET ")) {
      char key[16];
      char val[16];
      char extra[16];
      const int ntok = splitTokens_(p + 4, key, sizeof(key), val, sizeof(val), extra, sizeof(extra));
      if (ntok < 2) {
        sendResp(fr.frameId, "NACK bad_value");
        return;
      }
      const bool isBaud = uartpoc::keyEq(key, "baud");
      char msg[64];
      // Validate first WITHOUT applying (baud needs the deferred switch).
      uartpoc::PocConfig probe = cfg_;
      if (!uartpoc::parseSet(key, val, probe, msg, sizeof(msg))) {
        sendResp(fr.frameId, msg);
        return;
      }
      uint32_t applyMs = 100;
      if (ntok >= 3) {
        uint32_t parsed = 0;
        if (!uartpoc::parseU32(extra, parsed)) {
          sendResp(fr.frameId, "NACK bad_value");
          return;
        }
        applyMs = uartpoc::clampU32(parsed, 10, 2000);
      }
      if (isBaud && probe.baud == lastAppliedBaud_) {
        sendResp(fr.frameId, msg);  // No-op switch: ACK, stay put.
        return;
      }
      if (!isBaud) {
        cfg_ = probe;
      }
      sendResp(fr.frameId, msg);  // RESP goes out at the OLD baud.
      if (isBaud) {
        txHold_ = true;  // Silent until the peer confirms the new baud.
        pendingBaud_ = probe.baud;
        baudSwitchAtMs_ = millis() + applyMs;
        // cfg_.baud keeps the old rate until the switch fires (STATS honesty).
      }
      return;
    }
    if (startsWith_(p, "GET ")) {
      char key[16];
      char val[16];
      char extra[16];
      if (splitTokens_(p + 4, key, sizeof(key), val, sizeof(val), extra, sizeof(extra)) != 1) {
        sendResp(fr.frameId, "NACK bad_value");
        return;
      }
      char kv[96];
      uartpoc::formatGet(cfg_, key, kv, sizeof(kv));
      if (startsWith_(kv, "NACK")) {
        sendResp(fr.frameId, kv);
        return;
      }
      char msg[112];
      snprintf(msg, sizeof(msg), "ACK %s", kv);
      sendResp(fr.frameId, msg);
      return;
    }
    if (isWord_(p, "STATS")) {
      char msg[160];
      buildStats_(msg, sizeof(msg));
      sendResp(fr.frameId, msg);
      return;
    }
    sendResp(fr.frameId, "NACK unknown_cmd");
  }

  uint32_t currentBaud_() {
    // Serial2 has no baud getter; track logically (updated on real switches).
    return lastAppliedBaud_;
  }

  void pollUsb() {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\n') {
        usbBuf_[usbLen_] = '\0';
        handleUsbLine(usbBuf_);
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

  void handleUsbLine(const char* line) {
    if (startsWith_(line, "SET ")) {
      char key[16];
      char val[16];
      char extra[16];
      if (splitTokens_(line + 4, key, sizeof(key), val, sizeof(val), extra, sizeof(extra)) < 2) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      char msg[64];
      if (!parseLocalSet(key, val)) {
        uartpoc::PocConfig probe = cfg_;
        uartpoc::parseSet(key, val, probe, msg, sizeof(msg));
        LOG_W("POC", "%s", msg);
        return;
      }
      uartpoc::formatGet(cfg_, key, msg, sizeof(msg));
      LOG_I("POC", "ACK %s", msg);
      if (uartpoc::keyEq(key, "baud")) {
        LOG_I("POC", "MONITOR: switch USB monitor to %u baud", static_cast<unsigned>(cfg_.baud));
      }
      return;
    }
    if (startsWith_(line, "GET ")) {
      char key[16];
      char val[16];
      char extra[16];
      if (splitTokens_(line + 4, key, sizeof(key), val, sizeof(val), extra, sizeof(extra)) != 1) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      char kv[96];
      uartpoc::formatGet(cfg_, key, kv, sizeof(kv));
      LOG_I("POC", "%s", kv);
      return;
    }
    if (isWord_(line, "STATS")) {
      char msg[160];
      buildStats_(msg, sizeof(msg));
      LOG_I("POC", "%s", msg);
      return;
    }
    if (isWord_(line, "START")) {
      streaming_ = true;
      LOG_I("POC", "streaming started");
      return;
    }
    if (isWord_(line, "STOP")) {
      streaming_ = false;
      LOG_I("POC", "streaming stopped");
      return;
    }
    if (isWord_(line, "HELP")) {
      LOG_I("POC", "cmds: SET k v | GET k | GET all | STATS | START | STOP | HELP");
      LOG_I("POC",
            "keys: chunk|chunk_bytes 16..1024 pace|pace_us 0..50000 baud 9600|57600|115200|230400|460800|921600 mode "
            "0..3 fps 1..30");
      return;
    }
    if (line[0] != '\0') {
      LOG_W("POC", "unknown cmd (try HELP)");
    }
  }

  // Local SET: baud switches the UART immediately (operator-owned); rest apply.
  bool parseLocalSet(const char* key, const char* val) {
    uartpoc::PocConfig probe = cfg_;
    char msg[64];
    if (!uartpoc::parseSet(key, val, probe, msg, sizeof(msg))) {
      return false;
    }
    cfg_ = probe;
    if (uartpoc::keyEq(key, "baud")) {
      Serial2.updateBaudRate(cfg_.baud);
      lastAppliedBaud_ = cfg_.baud;
    }
    return true;
  }

  void buildStats_(char* out, size_t cap) {
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(out, cap,
             "STATS txf=%lu txc=%lu txb=%lu rxcmd=%lu rxe=%lu mode=%u chunk=%u pace=%lu baud=%lu fps=%u intfree=%u",
             static_cast<unsigned long>(txFrames_), static_cast<unsigned long>(txChunks_),
             static_cast<unsigned long>(txBytes_), static_cast<unsigned long>(rxCmds_),
             static_cast<unsigned long>(rxInvalid_), cfg_.mode, cfg_.chunk_bytes,
             static_cast<unsigned long>(cfg_.pace_us), static_cast<unsigned long>(currentBaud_()), cfg_.fps,
             static_cast<unsigned>(intFree));
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
    while (*s == ' ' || *s == '\t') {
      ++s;
    }
    return *s == '\0';
  }

  // Split "a b c" into up to 3 tokens; returns token count (0..3).
  static int splitTokens_(const char* s, char* t1, size_t c1, char* t2, size_t c2, char* t3, size_t c3) {
    char* toks[3] = {t1, t2, t3};
    const size_t caps[3] = {c1, c2, c3};
    int n = 0;
    while (*s != '\0' && n < 3) {
      while (*s == ' ' || *s == '\t') {
        ++s;
      }
      if (*s == '\0') {
        break;
      }
      size_t i = 0;
      while (*s != '\0' && *s != ' ' && *s != '\t') {
        if (i + 1 < caps[n]) {
          toks[n][i++] = *s;
        }
        ++s;
      }
      toks[n][i] = '\0';
      ++n;
    }
    while (*s == ' ' || *s == '\t') {
      ++s;
    }
    if (*s != '\0' || (n > 0 && toks[n - 1][0] == '\0')) {
      return -1;  // Trailing garbage / empty token.
    }
    return n;
  }

  uartpoc::PocConfig cfg_;
  uartpoc::Decoder linkDec_;
  uint8_t* txBuf_;     // INTERNAL encode/staging (kMaxFrameLen).
  uint8_t* chunkBuf_;  // INTERNAL raw chunk staging (kMaxPayload).
  bool streaming_;
  bool txHold_;  // Silent window across baud switches.
  uint16_t txFrameId_;
  uint32_t nextTickMs_;
  uint32_t lineNo_;
  bool camInit_;
  bool camOk_;
  uint32_t lastCamWarnMs_;
  uint32_t baudSwitchAtMs_;
  uint32_t pendingBaud_;
  uint32_t lastAppliedBaud_ = 115200;
  bool waitingForLink_;
  uint32_t waitLinkUntilMs_;
  uint32_t lastValidRxMs_;
  uint32_t lastHbMs_;
  uint32_t txFrames_;
  uint32_t txChunks_;
  uint32_t txBytes_;
  uint32_t rxCmds_;
  uint32_t rxInvalid_;
  char usbBuf_[96];
  size_t usbLen_;
  char cmdBuf_[160];
  TaskHandle_t genTask_;
};

#endif  // ARDUINO
