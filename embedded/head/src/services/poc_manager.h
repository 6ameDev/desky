#pragma once
// desky-head UART POC sender — header-only service (poc-comm-link spike).
//
// Owns the Head side of the barebones bi-directional UART transport:
//  - Serial2 link (TX=MCU_LINK_UART_TX=12, RX=MCU_LINK_UART_RX=13) + USB CLI.
//  - Generator task (Core 0): one source frame per fps tick, fragmented into
//    chunk_bytes CHUNK frames (LAST_CHUNK on final, SYNTHETIC for modes 1/2),
//    pace_us gap between chunks. When fec_k>0 and the frame passes the
//    FEC-skip policy (see kFecDataCap), K parity CHUNKs follow the N data
//    chunks (IS_PARITY flag, never LAST_CHUNK, full-stride payloads, same
//    pacing). Modes: 0 TEXT, 1 RAMP, 2 SYNTH_JPEG
//    (pseudorandom + SOI/EOI markers only, NOT decodable), 3 HW_CAM (lazy
//    camera init on first mode-3 tick; PSRAM fb slices copied into INTERNAL
//    staging before Serial2.write).
//  - CMD RX on Serial2 -> apply -> RESP ACK/NACK; deferred baud switch
//    (RESP at old baud, both switch after delay, 3s rollback to 115200).
//  - SELFTEST verbs (USB CLI): MEM (in-memory codec round-trip, zero Serial2
//    traffic), WIRE (physical TX12-RX13 jumper loopback through silicon),
//    SWEEP (WIRE matrix for the no-PC stage). USB stays 115200 throughout;
//    only Serial2 changes baud, always restored with streaming resumed.
//
// Memory: TX encode + chunk staging buffers are heap_caps INTERNAL, never
// PSRAM (asserted via esp_ptr_internal where available), plus one 64KB
// verify slot (matches the S3 kSlotCap so Head loopback predicts S3 1:1) +
// one verify chunk buffer for the selftests + one 20KB FEC scratch (16KB
// data staging + 4KB parity, allocated once, no per-frame heap). No String/heap in
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
#include "link/poc_synth.h"
#include "link/uart_fec.h"
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
        verifySlot_(nullptr),
        verifyChunk_(nullptr),
        fecBuf_(nullptr),
        streaming_(true),
        txHold_(false),
        testActive_(false),
        txFrameId_(0),
        nextTickMs_(0),
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
        txParity_(0),
        txBytes_(0),
        rxCmds_(0),
        rxInvalid_(0),
        usbLen_(0),
        genTask_(nullptr),
        txMutex_(nullptr) {}

  void begin() {
    // Setup order (core convention): logger + fault already up in main before
    // this runs; config defaults live in cfg_; banner is main's job.
    txMutex_ = xSemaphoreCreateMutex();  // Serializes ALL Serial2 TX (generator task vs loop task).
    DESKY_ASSERT(txMutex_ != nullptr);
    txBuf_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxFrameLen, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(txBuf_ != nullptr);
    chunkBuf_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxPayload, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(chunkBuf_ != nullptr);
    verifySlot_ = static_cast<uint8_t*>(heap_caps_malloc(kVerifySlotCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(verifySlot_ != nullptr);
    verifyChunk_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxPayload, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(verifyChunk_ != nullptr);
    fecBuf_ = static_cast<uint8_t*>(heap_caps_malloc(kFecScratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(fecBuf_ != nullptr);
#if POC_HAVE_INTERNAL_CHECK
    DESKY_ASSERT(esp_ptr_internal(txBuf_) && esp_ptr_internal(chunkBuf_) && esp_ptr_internal(verifySlot_) &&
                 esp_ptr_internal(verifyChunk_) && esp_ptr_internal(fecBuf_));
#endif
    verifyReasm_.attach(verifySlot_, kVerifySlotCap);
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
      // Drain + end/begin (NEVER updateBaudRate mid-stream: reconfiguring with a
      // live FIFO intermittently wedges UART TX silent with zero counters and
      // climbing txf — measured repeatedly. end/begin has 200+ clean runs).
      Serial2.flush();  // Blocks until TX FIFO+ring drain at the OLD rate (~25ms max).
      Serial2.end();
      Serial2.begin(pendingBaud_, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
      linkDec_.reset();  // Old-rate bytes must not seed new-rate decodes.
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
      Serial2.flush();
      Serial2.end();
      Serial2.begin(115200, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
      linkDec_.reset();
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
  static constexpr size_t kVerifySlotCap = 65536;  // Matches the S3 kSlotCap: Head loopback predicts S3 1:1.

  // FEC-skip policy (INTERNAL pressure): parity stages N*stride data bytes +
  // up to K*stride parity bytes in one INTERNAL scratch buffer allocated once
  // in begin(). kFecDataCap = fec::kEmitDataCap = 16KB: every synth total
  // (max 2048B) always qualifies, QVGA-class camera JPEGs (~10-15KB, POC §5)
  // qualify at any stride, and anything bigger SKIPS (wire stays
  // byte-identical to K=0, zero behavior change). Sizing: scratch = 16KB +
  // 4*1KB = 20KB; existing INTERNAL is ~67KB (64KB verify slot + tx/chunk
  // staging) against intfree 182-267KB observed -> worst case ~87KB used,
  // ~95KB headroom. K=0 default keeps the wire identical; S3-side decode is
  // task 3 (today's S3 would mis-reassemble parity chunks as data).
  static constexpr size_t kFecDataCap = uartpoc::fec::kEmitDataCap;
  static constexpr size_t kFecScratchBytes = kFecDataCap + uartpoc::fec::kMaxParity * uartpoc::kMaxPayload;

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
    if (mode > uartpoc::MODE_SYNTH_JPEG) {
      mode = uartpoc::MODE_TEXT;  // Unreachable via clamped knobs; preserves the TEXT fallback.
    }
    const uint32_t total = pocself::synthTotal(mode);
    const uint8_t flags = pocself::synthFlags(mode);
    const uint16_t stride = (chunk < uartpoc::kChunkMin) ? uartpoc::kChunkMin : chunk;
    const uint32_t nChunks = (total + stride - 1) / stride;
    const uartpoc::fec::EmitPlan fec = uartpoc::fec::planEmit(cfg_.fec_k, nChunks, stride);
    for (uint32_t idx = 0; idx < nChunks; ++idx) {
      const uint32_t off = idx * stride;
      uint16_t n = stride;
      if (off + n > total) {
        n = static_cast<uint16_t>(total - off);
      }
      pocself::fillSynthetic(mode, fid, off, chunkBuf_, n, total);
      uint8_t fl = flags;
      if (idx + 1 >= nChunks) {
        fl |= uartpoc::FLAG_LAST_CHUNK;
      }
      writeChunk(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), chunkBuf_, n);
      if (pace > 0 && (idx + 1 < nChunks || fec.emit)) {
        delayMicroseconds(pace);  // Parity continues the uniform inter-chunk gap; skipped plans pace as before.
      }
    }
    if (fec.emit && stageSynthParity_(mode, fid, stride, nChunks, total, fec.k)) {
      sendParityChunks_(fid, nChunks, stride, fec.k, pace);
    }
    ++txFrames_;
  }

  // Regenerate all N synth data chunks into the fecBuf_ staging area and solve
  // K parity blocks in place. No heap in the loop path: pointer/lens tables
  // are members, the scratch was allocated once in begin(). Returns
  // encode()'s verdict (false -> caller sends data-only, wire unchanged).
  bool stageSynthParity_(uint8_t mode, uint16_t fid, uint16_t stride, uint32_t nChunks, uint32_t total, uint8_t k) {
    for (uint32_t i = 0; i < nChunks; ++i) {
      const uint32_t off = i * stride;
      uint16_t n = stride;
      if (off + n > total) {
        n = static_cast<uint16_t>(total - off);
      }
      uint8_t* dst = fecBuf_ + i * stride;
      pocself::fillSynthetic(mode, fid, off, dst, n, total);
      fecDataPtrs_[i] = dst;
      fecLens_[i] = n;
    }
    return stageParity_(nChunks, stride, k);
  }

  // Solve K parity blocks from the staged fecDataPtrs_/fecLens_ tables into
  // the fecBuf_ parity area (past kFecDataCap). Called with a held fb for
  // camera frames, with regenerated synth bytes otherwise.
  bool stageParity_(uint32_t nChunks, uint16_t stride, uint8_t k) {
    for (uint8_t j = 0; j < k; ++j) {
      fecParPtrs_[j] = fecBuf_ + kFecDataCap + static_cast<size_t>(j) * stride;
    }
    return uartpoc::fec::encode(fecDataPtrs_, static_cast<uint8_t>(nChunks), k, stride, fecLens_, fecParPtrs_);
  }

  // Emit K parity chunks after the N data chunks: chunkIdx N..N+K-1, flags
  // exactly IS_PARITY (never LAST_CHUNK — LAST stays on data chunk N-1),
  // payloadLen the full stride, paced like data chunks through the existing
  // txWriteRaw_() mutex funnel.
  void sendParityChunks_(uint16_t fid, uint32_t nChunks, uint16_t stride, uint8_t k, uint32_t pace) {
    for (uint8_t j = 0; j < k; ++j) {
      writeParityChunk_(uartpoc::MSG_CHUNK, uartpoc::fec::parityChunkFlags(), fid,
                        uartpoc::fec::parityChunkIdx(nChunks, j), fecParPtrs_[j], stride);
      if (j + 1 < k && pace > 0) {
        delayMicroseconds(pace);
      }
    }
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
      const uartpoc::fec::EmitPlan fec = uartpoc::fec::planEmit(cfg_.fec_k, nChunks, stride);
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
        if (pace > 0 && (idx + 1 < nChunks || fec.emit)) {
          delayMicroseconds(pace);  // Parity continues the uniform inter-chunk gap; skipped plans pace as before.
        }
      }
      if (fec.emit) {
        // Parity straight from the fb slices (true lens[]; encode() owns the
        // short-LAST padding). fb is still held: returned after the last
        // write below, as today.
        for (uint32_t i = 0; i < nChunks; ++i) {
          const uint32_t off = i * stride;
          uint16_t n = stride;
          if (off + n > total) {
            n = static_cast<uint16_t>(total - off);
          }
          fecDataPtrs_[i] = fb->buf + off;
          fecLens_[i] = n;
        }
        if (stageParity_(nChunks, stride, fec.k)) {
          sendParityChunks_(fid, nChunks, stride, fec.k, pace);
        }
      }
      ++txFrames_;
    }
    esp_camera_fb_return(fb);
  }

  // Resolution index (cfg_.framesize, 0..6) -> ESP framesize_t (OV3660 order).
  static framesize_t framesizeEsp_(uint8_t idx) {
    switch (idx) {
      case 1:
        return FRAMESIZE_VGA;
      case 2:
        return FRAMESIZE_SVGA;
      case 3:
        return FRAMESIZE_XGA;
      case 4:
        return FRAMESIZE_SXGA;
      case 5:
        return FRAMESIZE_UXGA;
      case 6:
        return FRAMESIZE_QXGA;
      default:
        return FRAMESIZE_QVGA;
    }
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
    if (initCamera_(framesizeEsp_(uartpoc::clampFramesize(cfg_.framesize)))) {
      camOk_ = true;
      LOG_I("POC", "camera lazy-init ok (%s jpeg)", uartpoc::framesizeName(cfg_.framesize));
    } else {
      LOG_W("POC", "camera lazy-init failed, mode 3 frames skipped");
    }
    return camOk_;
  }

  // Raw camera (re-)init at one resolution. Keeps fb_count=2 + jpeg_quality 12
  // (no quality knob: scope control). Returns esp_camera_init's verdict.
  bool initCamera_(framesize_t fs) {
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
    cc.frame_size = fs;
    cc.jpeg_quality = CFG_CAMERA_JPEG_QUALITY;
    cc.fb_count = 2;  // Double-buffered: DMA fills Frame B in PSRAM while Frame A chunks out over UART.
    return esp_camera_init(&cc) == ESP_OK;
  }

  // Runtime resolution switch: deinit + re-init at the new size. The generator
  // task is suspended across the switch so no fb grab races the deinit; the
  // prior txHold_ is saved/restored (a baud switch may own it). On new-size
  // failure the old size is re-inited (best effort) and cfg_ is left untouched,
  // so the camera path is never bricked — caller NACKs.
  bool switchFramesize_(uint8_t idx) {
    const uint8_t prev = cfg_.framesize;
    const bool holdSave = txHold_;
    if (genTask_ != nullptr) {
      vTaskSuspend(genTask_);
    }
    txHold_ = true;
    if (camInit_) {
      esp_camera_deinit();
    }
    camInit_ = true;
    bool ok = false;
    if (initCamera_(framesizeEsp_(idx))) {
      cfg_.framesize = idx;
      camOk_ = true;
      ok = true;
      LOG_I("POC", "framesize now %s", uartpoc::framesizeName(idx));
    } else {
      if (initCamera_(framesizeEsp_(prev))) {
        camOk_ = true;
        LOG_W("POC", "framesize re-init failed, kept %s", uartpoc::framesizeName(prev));
      } else {
        camOk_ = false;
        LOG_W("POC", "framesize re-init failed, camera unavailable");
      }
    }
    txHold_ = holdSave;
    if (genTask_ != nullptr) {
      vTaskResume(genTask_);
    }
    return ok;
  }

  // SET framesize plumbing shared by USB CLI and link CMD: validate the name,
  // no-op ACK when already there and healthy, else switch. msg carries the
  // ACK/NACK line; returns the switch verdict.
  bool applyFramesizeCmd_(const char* val, char* msg, size_t cap) {
    const int8_t fi = uartpoc::parseFramesize(val);
    if (fi < 0) {
      uartpoc::writeStr(msg, cap, "NACK bad_framesize qvga|vga|svga|xga|sxga|uxga|qxga");
      return false;
    }
    const uint8_t idx = static_cast<uint8_t>(fi);
    if (idx == cfg_.framesize && camOk_) {
      snprintf(msg, cap, "ACK framesize %s", uartpoc::framesizeName(idx));
      return true;
    }
    if (switchFramesize_(idx)) {
      snprintf(msg, cap, "ACK framesize %s", uartpoc::framesizeName(idx));
      return true;
    }
    uartpoc::writeStr(msg, cap, "NACK camera_reinit_failed");
    return false;
  }

  // ALL Serial2 TX funnels through here. The generator task (streaming chunks) and
  // the loop task (HB/RESP/tests) share one UART: unguarded writes interleave
  // mid-frame (measured: 20% RESP loss + S3-side perr/drops with streaming on,
  // 10/10 ACK with it stopped). FreeRTOS mutex (blocking, not spin); worst hold
  // ~12ms (144B @115200). No lock ordering issues: single mutex, TX ring drains
  // via ISR with no dependency on either task.
  size_t txWriteRaw_(const uint8_t* buf, size_t len) {
    if (txMutex_ == nullptr || buf == nullptr) {
      return 0;
    }
    size_t n = 0;
    if (xSemaphoreTake(txMutex_, portMAX_DELAY) == pdTRUE) {
      n = Serial2.write(buf, len);
      xSemaphoreGive(txMutex_);
    }
    return n;
  }

  void writeChunk(uint8_t type, uint8_t flags, uint16_t fid, uint16_t idx, const uint8_t* payload, uint16_t n) {
    size_t outLen = 0;
    if (!uartpoc::encodeFrame(type, flags, fid, idx, payload, n, txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      return;
    }
    txBytes_ += txWriteRaw_(txBuf_, outLen);
    ++txChunks_;
  }

  // Parity-chunk twin of writeChunk: same encode + txWriteRaw_() mutex funnel,
  // but counted separately in txParity_ (txChunks_ keeps counting data chunks
  // only, so K=0 STATS lines are unchanged).
  void writeParityChunk_(uint8_t type, uint8_t flags, uint16_t fid, uint16_t idx, const uint8_t* payload, uint16_t n) {
    size_t outLen = 0;
    if (!uartpoc::encodeFrame(type, flags, fid, idx, payload, n, txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      return;
    }
    txBytes_ += txWriteRaw_(txBuf_, outLen);
    ++txParity_;
  }

  void sendHb() {
    size_t outLen = 0;
    if (uartpoc::encodeFrame(uartpoc::MSG_HB, 0, txFrameId_++, 0, nullptr, 0, txBuf_, uartpoc::kMaxFrameLen, outLen)) {
      txBytes_ += txWriteRaw_(txBuf_, outLen);
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
      txBytes_ += txWriteRaw_(txBuf_, outLen);
    }
  }

  void pollLink() {
    if (testActive_) {
      return;  // WIRE/SWEEP own Serial2: test bytes stay buffered for the test pump, never CMDs.
    }
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
      if (uartpoc::keyEq(key, "framesize")) {
        // Resolution switch needs the camera re-init side effect: parseSet
        // alone must never apply it (it would store without re-initing).
        char msg[64];
        applyFramesizeCmd_(val, msg, sizeof(msg));
        sendResp(fr.frameId, msg);
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
      char msg[192];
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
      if (uartpoc::keyEq(key, "framesize")) {
        // Same re-init path as the link CMD: never store without re-initing.
        char msg[64];
        if (applyFramesizeCmd_(val, msg, sizeof(msg))) {
          LOG_I("POC", "%s", msg);
        } else {
          LOG_W("POC", "%s", msg);
        }
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
      char msg[192];
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
            "keys: chunk|chunk_bytes 16..1024 pace|pace_us 0..50000 baud "
            "9600|57600|115200|460800|921600|1M|1.5M|2M|3M|4M|5M (230400 BANNED) mode "
            "0..3 fps 1..30 fec|fec_k 0..4 framesize qvga|vga|svga|xga|sxga|uxga|qxga (mode-3 re-init)");
      LOG_I("POC", "selftest: SELFTEST MEM <mode 0-3> <chunk 16-1024> <pace 0-50000> <nframes 1-50>");
      LOG_I("POC", "selftest: SELFTEST WIRE <baud> <mode> <chunk> <pace> <nframes 1-50> (needs TX12-RX13 jumper)");
      LOG_I("POC", "selftest: SELFTEST SWEEP [QUICK|FULL] (WIRE matrix, USB stays 115200)");
      return;
    }
    if (startsWith_(line, "SELFTEST ")) {
      handleSelftest_(line + 9);
      return;
    }
    if (line[0] != '\0') {
      LOG_W("POC", "unknown cmd (try HELP)");
    }
  }

  // Local SET: baud switches the UART immediately (operator-owned); rest apply.
  // framesize is REFUSED here by design: it must go through applyFramesizeCmd_
  // (camera re-init), never a bare store.
  bool parseLocalSet(const char* key, const char* val) {
    if (uartpoc::keyEq(key, "framesize")) {
      return false;
    }
    uartpoc::PocConfig probe = cfg_;
    char msg[64];
    if (!uartpoc::parseSet(key, val, probe, msg, sizeof(msg))) {
      return false;
    }
    cfg_ = probe;
    if (uartpoc::keyEq(key, "baud")) {
      Serial2.flush();
      Serial2.end();
      Serial2.begin(cfg_.baud, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
      linkDec_.reset();
      lastAppliedBaud_ = cfg_.baud;
    }
    return true;
  }

  // ── SELFTEST (Head-only loopback verification) ──
  // MEM: in-memory codec round-trip, zero Serial2 traffic. WIRE: physical
  // TX12-RX13 jumper loopback through silicon. SWEEP: WIRE matrix for the
  // no-PC stage (Nano is capture-only). USB stays 115200 throughout; only
  // Serial2 changes baud, always restored with streaming resumed. All tests
  // run synchronously in the loop task with per-chunk watchdog feeds; the
  // generator is frozen via txHold_ (streaming_ flag itself untouched) and
  // WIRE/SWEEP additionally set testActive_ so pollLink() never eats test
  // bytes as CMDs. Decoders are test-local; only verifyReasm_ (reset per
  // test) and the verify buffers are shared, never with the generator.

  struct SelfRes {
    uint32_t ok;
    uint32_t herr;
    uint32_t perr;
    uint32_t drops;
    uint32_t mismatch;
    uint32_t timeouts;
    uint32_t rxbytes;
    uint32_t maxEncUs;
    uint32_t ms;
    uint32_t kbs;
    uint32_t skipped;   // Big-frame skips (fb->len > verify slot): neither ok nor bad.
    uint32_t skipSize;  // Last skipped frame's byte size (for the SKIP line).
    bool camUnavail;
  };

  void handleSelftest_(const char* args) {
    if (startsWith_(args, "MEM ")) {
      char t[4][16];
      if (splitMany_(args + 4, t, 4) != 4) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      uint32_t mode = 0, chunk = 0, pace = 0, nframes = 0;
      if (!uartpoc::parseU32(t[0], mode) || !uartpoc::parseU32(t[1], chunk) || !uartpoc::parseU32(t[2], pace) ||
          !uartpoc::parseU32(t[3], nframes)) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      if (nframes < 1 || nframes > 50) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      runSelfMem_(static_cast<uint8_t>(uartpoc::clampU32(mode, uartpoc::kModeMin, uartpoc::kModeMax)),
                  static_cast<uint16_t>(uartpoc::clampU32(chunk, uartpoc::kChunkMin, uartpoc::kChunkMax)),
                  uartpoc::clampU32(pace, uartpoc::kPaceMin, uartpoc::kPaceMax), nframes);
      return;
    }
    if (startsWith_(args, "WIRE ")) {
      char t[5][16];
      if (splitMany_(args + 5, t, 5) != 5) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      uint32_t baud = 0, mode = 0, chunk = 0, pace = 0, nframes = 0;
      if (!uartpoc::parseU32(t[0], baud) || !uartpoc::parseU32(t[1], mode) || !uartpoc::parseU32(t[2], chunk) ||
          !uartpoc::parseU32(t[3], pace) || !uartpoc::parseU32(t[4], nframes)) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      if (!uartpoc::isValidBaud(baud)) {
        LOG_W("POC", "NACK bad_baud");
        return;
      }
      if (nframes < 1 || nframes > 50) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      const SelfRes r =
          runSelfWireRes_(baud, static_cast<uint8_t>(uartpoc::clampU32(mode, uartpoc::kModeMin, uartpoc::kModeMax)),
                          static_cast<uint16_t>(uartpoc::clampU32(chunk, uartpoc::kChunkMin, uartpoc::kChunkMax)),
                          uartpoc::clampU32(pace, uartpoc::kPaceMin, uartpoc::kPaceMax), nframes);
      logWireRes_(r, 0, 0, 0, 0, false);
      return;
    }
    if (isWord_(args, "SWEEP")) {
      runSelfSweep_(false);
      return;
    }
    if (startsWith_(args, "SWEEP ")) {
      char t[1][16];
      if (splitMany_(args + 6, t, 1) != 1) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      if (uartpoc::keyEq(t[0], "QUICK")) {
        runSelfSweep_(false);
        return;
      }
      if (uartpoc::keyEq(t[0], "FULL")) {
        runSelfSweep_(true);
        return;
      }
      LOG_W("POC", "NACK bad_value");
      return;
    }
    LOG_W("POC", "NACK unknown_cmd");
  }

  // Split s into at most maxToks whitespace tokens (each <16 chars, silently
  // truncated like splitTokens_); returns the count, or -1 on trailing
  // garbage (same contract as splitTokens_).
  static int splitMany_(const char* s, char toks[][16], int maxToks) {
    int n = 0;
    while (*s != '\0' && n < maxToks) {
      while (*s == ' ' || *s == '\t') {
        ++s;
      }
      if (*s == '\0') {
        break;
      }
      size_t i = 0;
      while (*s != '\0' && *s != ' ' && *s != '\t') {
        if (i + 1 < 16) {
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

  void runSelfMem_(uint8_t mode, uint16_t chunk, uint32_t pace, uint32_t nframes) {
    SelfRes r = {};
    const uint32_t t0 = millis();
    const bool holdSave = txHold_;
    txHold_ = true;                      // Freeze the generator (streaming_ untouched, restored below).
    uartpoc::Decoder dec;                // Test-local: the shared linkDec_ is never touched.
    uint8_t enc[uartpoc::kMaxFrameLen];  // Stack staging: txBuf_ stays with the generator.
    for (uint32_t f = 0; f < nframes; ++f) {
      const uint16_t fid = static_cast<uint16_t>(f);
      bool done = false;
      const uint32_t sk0 = r.skipped;
      if (mode == uartpoc::MODE_HW_CAM) {
        if (!testFrameCam_(fid, chunk, pace, dec, enc, r, false, 0, done)) {
          break;  // CAMUNAVAIL: r.camUnavail set, stop.
        }
      } else {
        done = testFrameSynth_(mode, fid, chunk, pace, dec, enc, r, false, 0);
      }
      if (r.camUnavail) {
        break;
      }
      if (done) {
        ++r.ok;
      } else if (r.skipped == sk0) {
        ++r.drops;  // A big-frame skip is counted in r.skipped, never as a drop.
      }
    }
    txHold_ = holdSave;
    r.ms = millis() - t0;
    if (r.camUnavail) {
      LOG_W("POC", "SELFTEST MEM CAMUNAVAIL");
      return;
    }
    // Resolution is fixed for the whole run, so skips are all-or-nothing in
    // practice: any skip with zero completions reports SKIP, not failure.
    if (r.skipped > 0 && r.ok == 0) {
      LOG_W("POC", "SELFTEST MEM SKIP_BIGFRAME size=%lu", static_cast<unsigned long>(r.skipSize));
      return;
    }
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    LOG_I("POC", "SELFTEST MEM ok=%lu herr=%lu perr=%lu drops=%lu mismatch=%lu maxEncUs=%lu intfree=%u ms=%lu",
          static_cast<unsigned long>(r.ok), static_cast<unsigned long>(r.herr), static_cast<unsigned long>(r.perr),
          static_cast<unsigned long>(r.drops), static_cast<unsigned long>(r.mismatch),
          static_cast<unsigned long>(r.maxEncUs), static_cast<unsigned>(intFree), static_cast<unsigned long>(r.ms));
  }

  // One synthetic test frame (modes 0-2). wire=false feeds encoded bytes back
  // in kSelftestSlice slices (unaligned-delivery coverage, zero Serial2
  // traffic); wire=true pushes bytes through silicon with an interleaved RX
  // drain plus a post-TX drain until deadline. Returns true on COMPLETE with
  // the expected total (else the caller counts drop/timeout).
  bool testFrameSynth_(uint8_t mode, uint16_t fid, uint16_t chunk, uint32_t pace, uartpoc::Decoder& dec, uint8_t* enc,
                       SelfRes& r, bool wire, uint32_t deadline) {
    verifyReasm_.reset();
    const uint32_t total = pocself::synthTotal(mode);
    const uint8_t flags = pocself::synthFlags(mode);
    const uint32_t nChunks = (total + chunk - 1) / chunk;
    bool done = false;
    size_t outTotal = 0;
    for (uint32_t idx = 0; idx < nChunks && !done; ++idx) {
      if (wire && (int32_t)(millis() - deadline) >= 0) {
        break;
      }
      const uint32_t off = idx * chunk;
      uint16_t n = chunk;
      if (off + n > total) {
        n = static_cast<uint16_t>(total - off);
      }
      pocself::fillSynthetic(mode, fid, off, verifyChunk_, n, total);
      uint8_t fl = flags;
      if (idx + 1 >= nChunks) {
        fl |= uartpoc::FLAG_LAST_CHUNK;
      }
      const uint32_t e0 = micros();
      size_t encLen = 0;
      const bool encOk = uartpoc::encodeFrame(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), verifyChunk_, n,
                                              enc, uartpoc::kMaxFrameLen, encLen);
      const uint32_t encUs = micros() - e0;
      if (!encOk) {
        ++r.drops;
        continue;
      }
      if (encUs > r.maxEncUs) {
        r.maxEncUs = encUs;
      }
      if (wire) {
        txWriteRaw_(enc, encLen);
        wireDrain_(dec, mode, chunk, total, r, done, outTotal);
      } else {
        pumpSlice_(dec, mode, chunk, total, enc, encLen, r, done, outTotal);
      }
      if (pace > 0 && idx + 1 < nChunks) {
        delayMicroseconds(pace);
      }
      FaultManager::watchdogFeed();
    }
    if (wire && !done) {
      while (!done && (int32_t)(deadline - millis()) > 0) {
        wireDrain_(dec, mode, chunk, total, r, done, outTotal);
        if (!done) {
          delay(1);
          FaultManager::watchdogFeed();
        }
      }
    }
    if (done && outTotal != total) {  // COMPLETE but short: firmware bug, demote.
      ++r.mismatch;
      done = false;
    }
    return done;
  }

  // One camera test frame (mode 3): one fb grab, CRC32 over source vs
  // reassembled. Returns false on CAMUNAVAIL (r.camUnavail set); otherwise
  // sets done like testFrameSynth_.
  bool testFrameCam_(uint16_t fid, uint16_t chunk, uint32_t pace, uartpoc::Decoder& dec, uint8_t* enc, SelfRes& r,
                     bool wire, uint32_t deadline, bool& done) {
    done = false;
    if (!ensureCamera()) {
      r.camUnavail = true;
      return false;
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      r.camUnavail = true;
      return false;
    }
    const uint32_t total = static_cast<uint32_t>(fb->len);
    const uint32_t srcCrc = uartpoc::crc32Ieee(fb->buf, total);
    if (total > kVerifySlotCap) {
      // Honesty rule: a frame that cannot fit the 64KB verify slot (== S3
      // kSlotCap) is SKIPPED, never half-verified. Counted separately (neither
      // ok nor bad); callers report SKIP_BIGFRAME with the size.
      ++r.skipped;
      r.skipSize = total;
      esp_camera_fb_return(fb);
      return true;
    }
    verifyReasm_.reset();
    size_t outTotal = 0;
    const uint32_t nChunks = (total + chunk - 1) / chunk;
    // Mirror the generator: frames past the reassembly chunk cap are skipped.
    if (total > 0 && nChunks <= uartpoc::Reassembler::kMaxChunks) {
      for (uint32_t idx = 0; idx < nChunks && !done; ++idx) {
        if (wire && (int32_t)(millis() - deadline) >= 0) {
          break;
        }
        const uint32_t off = idx * chunk;
        uint16_t n = chunk;
        if (off + n > total) {
          n = static_cast<uint16_t>(total - off);
        }
        for (uint16_t i = 0; i < n; ++i) {
          verifyChunk_[i] = fb->buf[off + i];  // PSRAM fb -> INTERNAL staging.
        }
        uint8_t fl = 0;
        if (idx + 1 >= nChunks) {
          fl |= uartpoc::FLAG_LAST_CHUNK;
        }
        const uint32_t e0 = micros();
        size_t encLen = 0;
        const bool encOk = uartpoc::encodeFrame(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), verifyChunk_,
                                                n, enc, uartpoc::kMaxFrameLen, encLen);
        const uint32_t encUs = micros() - e0;
        if (!encOk) {
          ++r.drops;
          continue;
        }
        if (encUs > r.maxEncUs) {
          r.maxEncUs = encUs;
        }
        if (wire) {
          txWriteRaw_(enc, encLen);
          wireDrain_(dec, uartpoc::MODE_HW_CAM, chunk, total, r, done, outTotal);
        } else {
          pumpSlice_(dec, uartpoc::MODE_HW_CAM, chunk, total, enc, encLen, r, done, outTotal);
        }
        if (pace > 0 && idx + 1 < nChunks) {
          delayMicroseconds(pace);
        }
        FaultManager::watchdogFeed();
      }
      if (wire && !done) {
        while (!done && (int32_t)(deadline - millis()) > 0) {
          wireDrain_(dec, uartpoc::MODE_HW_CAM, chunk, total, r, done, outTotal);
          if (!done) {
            delay(1);
            FaultManager::watchdogFeed();
          }
        }
      }
    }
    if (done) {
      const uint32_t rxCrc = uartpoc::crc32Ieee(verifySlot_, outTotal);
      if (outTotal != total || rxCrc != srcCrc) {
        ++r.mismatch;
        done = false;
      }
    }
    esp_camera_fb_return(fb);
    return true;
  }

  // Handle one decoded test CHUNK: reassemble with S3-identical STALE
  // handling, then (synthetic modes) regenerate expected bytes and memcmp.
  // Sets done/outTotal on COMPLETE.
  void onTestChunk_(uint8_t mode, uint16_t stride, uint32_t total, const uartpoc::DecodedFrame& fr, SelfRes& r,
                    bool& done, size_t& outTotal) {
    size_t tot = 0;
    uartpoc::Reassembler::Push pr =
        verifyReasm_.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, tot);
    if (pr == uartpoc::Reassembler::Push::STALE) {
      ++r.drops;
      pr = verifyReasm_.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, tot);
    }
    if (pr == uartpoc::Reassembler::Push::DROPPED || pr == uartpoc::Reassembler::Push::OVERSIZE) {
      ++r.drops;
      return;
    }
    if (mode != uartpoc::MODE_HW_CAM) {
      const uint32_t off = static_cast<uint32_t>(fr.chunkIdx) * stride;
      uint16_t want = 0;
      bool lenOk = false;
      if (off < total) {
        want = stride;
        if (off + want > total) {
          want = static_cast<uint16_t>(total - off);
        }
        lenOk = (fr.payloadLen == want);
      }
      if (!lenOk) {
        ++r.mismatch;
      } else {
        pocself::fillSynthetic(mode, fr.frameId, off, verifyChunk_, want, total);
        for (uint16_t i = 0; i < want; ++i) {
          if (verifyChunk_[i] != fr.payload[i]) {
            ++r.mismatch;
            break;
          }
        }
      }
    }
    if (pr == uartpoc::Reassembler::Push::COMPLETE) {
      outTotal = tot;
      done = true;
    }
  }

  // Feed one encoded frame in kSelftestSlice slices (unaligned-delivery
  // coverage), honoring the Decoder re-feed protocol after ERR_*.
  void pumpSlice_(uartpoc::Decoder& dec, uint8_t mode, uint16_t stride, uint32_t total, const uint8_t* data, size_t len,
                  SelfRes& r, bool& done, size_t& outTotal) {
    size_t pos = 0;
    while (pos < len && !done) {
      size_t sl = len - pos;
      if (sl > pocself::kSelftestSlice) {
        sl = pocself::kSelftestSlice;
      }
      size_t o = 0;
      while (o < sl && !done) {
        size_t consumed = 0;
        uartpoc::DecodedFrame fr;
        const uartpoc::DecodeStatus st = dec.feed(data + pos + o, sl - o, consumed, fr);
        o += consumed;
        if (st == uartpoc::DecodeStatus::OK) {
          onTestChunk_(mode, stride, total, fr, r, done, outTotal);
        } else if (st == uartpoc::DecodeStatus::ERR_HDR_CRC) {
          ++r.herr;
        } else if (st == uartpoc::DecodeStatus::ERR_PAY_CRC) {
          ++r.perr;
        } else {
          break;  // NEED_MORE: decoder buffered all input; next slice.
        }
        if (consumed == 0) {
          break;  // Defensive: never spin on a zero-progress feed.
        }
      }
      pos += sl;
    }
  }

  // Drain all currently-available Serial2 bytes through the test decoder.
  void wireDrain_(uartpoc::Decoder& dec, uint8_t mode, uint16_t stride, uint32_t total, SelfRes& r, bool& done,
                  size_t& outTotal) {
    FaultManager::watchdogFeed();
    while (Serial2.available() > 0 && !done) {
      const int c = Serial2.read();
      if (c < 0) {
        break;
      }
      ++r.rxbytes;
      const uint8_t b = static_cast<uint8_t>(c);
      size_t consumed = 0;
      uartpoc::DecodedFrame fr;
      const uartpoc::DecodeStatus st = dec.feed(&b, 1, consumed, fr);
      if (st == uartpoc::DecodeStatus::OK) {
        onTestChunk_(mode, stride, total, fr, r, done, outTotal);
      } else if (st == uartpoc::DecodeStatus::ERR_HDR_CRC) {
        ++r.herr;
      } else if (st == uartpoc::DecodeStatus::ERR_PAY_CRC) {
        ++r.perr;
      }
    }
  }

  void flushSerial2_() {
    while (Serial2.available() > 0) {
      Serial2.read();
    }
  }

  // Throwaway HB round-trip on the live Serial2 rate: any residual settle glitch lands here,
  // decoded by a junk decoder, never touching test state. Bounded (~200ms), WDT-fed.
  void wireWarmup_() {
    uint8_t hb[uartpoc::kHeaderLen + uartpoc::kTailLen];
    size_t hbLen = 0;
    if (!uartpoc::encodeFrame(uartpoc::MSG_HB, 0, 0xFFFF, 0, nullptr, 0, hb, sizeof(hb), hbLen)) {
      return;
    }
    txWriteRaw_(hb, hbLen);
    uartpoc::Decoder junk;
    const uint32_t end = millis() + 200;
    while ((int32_t)(end - millis()) > 0) {
      while (Serial2.available() > 0) {
        const int c = Serial2.read();
        if (c < 0) {
          break;
        }
        const uint8_t b = static_cast<uint8_t>(c);
        size_t consumed = 0;
        uartpoc::DecodedFrame fr;
        junk.feed(&b, 1, consumed, fr);
      }
      delay(1);
      FaultManager::watchdogFeed();
    }
    flushSerial2_();
  }

  // Physical loopback core: saves state, owns Serial2 at <baud> with an
  // enlarged RX buffer (self-echo of a full-size chunk must not overrun the
  // 256B Arduino default), restores everything. USB (Serial) baud never
  // changes; cfg_ is untouched (only the live UART rate moves).
  SelfRes runSelfWireRes_(uint32_t baud, uint8_t mode, uint16_t chunk, uint32_t pace, uint32_t nframes) {
    SelfRes r = {};
    const uint32_t t0 = millis();
    const bool streamSave = streaming_;
    const bool holdSave = txHold_;
    streaming_ = false;
    txHold_ = true;      // Generator silent: no interleaved Serial2 traffic.
    testActive_ = true;  // pollLink() hands off: test bytes are never CMDs.
    Serial2.end();
    Serial2.setRxBufferSize(4096);
    Serial2.begin(baud, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    delay(50);                           // Let the new baud settle: reconfig can spit a framing glitch that would
    flushSerial2_();                     // otherwise eat frame 0's magic (silent resync, fatal). Flush AFTER settling.
    uartpoc::Decoder dec;                // Test-local: the shared linkDec_ is never touched.
    wireWarmup_();                       // Throwaway HB round-trip: proves the path clean before frame 0 counts.
    uint8_t enc[uartpoc::kMaxFrameLen];  // Stack staging: txBuf_ stays with the generator.
    for (uint32_t f = 0; f < nframes; ++f) {
      const uint16_t fid = static_cast<uint16_t>(f);
      bool done = false;
      const uint32_t sk0 = r.skipped;
      bool frameSkipped = false;
      for (int att = 0; att < 2 && !done && !frameSkipped; ++att) {  // One retry: a settle-glitch casualty
        if (att > 0) {                                               // must not doom the whole test.
          flushSerial2_();  // (Retry-attempt wire errors still count: real events.)
          dec.reset();
        }
        const uint32_t deadline = millis() + 2000;
        if (mode == uartpoc::MODE_HW_CAM) {
          if (!testFrameCam_(fid, chunk, pace, dec, enc, r, true, deadline, done)) {
            break;  // CAMUNAVAIL.
          }
        } else {
          done = testFrameSynth_(mode, fid, chunk, pace, dec, enc, r, true, deadline);
        }
        if (r.camUnavail) {
          break;
        }
        frameSkipped = (r.skipped != sk0);  // Big frame: nothing went on the wire, retry is pointless.
      }
      if (r.camUnavail) {
        break;
      }
      if (done) {
        ++r.ok;
      } else if (!frameSkipped) {
        ++r.timeouts;  // Abort remaining frames on timeout.
        break;
      }  // Skipped frames are counted in r.skipped: neither ok nor timeout.
    }
    Serial2.end();
    Serial2.setRxBufferSize(256);  // Back to the Arduino default.
    Serial2.begin(cfg_.baud, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    flushSerial2_();
    linkDec_.reset();
    testActive_ = false;
    streaming_ = streamSave;
    txHold_ = holdSave;
    r.ms = millis() - t0;
    r.kbs = (r.ms < 1000) ? 0 : static_cast<uint32_t>(r.rxbytes / r.ms);
    return r;
  }

  void logWireRes_(const SelfRes& r, uint32_t baud, uint16_t chunk, uint32_t pace, uint8_t mode, bool combo) {
    if (r.camUnavail) {
      LOG_W("POC", "SELFTEST WIRE CAMUNAVAIL");
      return;
    }
    // Big-frame skips report SKIP_BIGFRAME, never ok/timeout lines. Resolution
    // is fixed per run, so this is all-or-nothing in practice.
    if (r.skipped > 0 && r.ok == 0) {
      if (combo) {
        LOG_W("POC", "SELFTEST WIRE SKIP_BIGFRAME size=%lu baud=%lu chunk=%u pace=%lu mode=%u",
              static_cast<unsigned long>(r.skipSize), static_cast<unsigned long>(baud), chunk,
              static_cast<unsigned long>(pace), mode);
        return;
      }
      LOG_W("POC", "SELFTEST WIRE SKIP_BIGFRAME size=%lu", static_cast<unsigned long>(r.skipSize));
      return;
    }
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (combo) {
      LOG_I("POC",
            "SELFTEST WIRE ok=%lu herr=%lu perr=%lu drops=%lu mismatch=%lu timeouts=%lu rxbytes=%lu intfree=%u ms=%lu "
            "kbs=%lu baud=%lu chunk=%u pace=%lu mode=%u",
            static_cast<unsigned long>(r.ok), static_cast<unsigned long>(r.herr), static_cast<unsigned long>(r.perr),
            static_cast<unsigned long>(r.drops), static_cast<unsigned long>(r.mismatch),
            static_cast<unsigned long>(r.timeouts), static_cast<unsigned long>(r.rxbytes),
            static_cast<unsigned>(intFree), static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.kbs),
            static_cast<unsigned long>(baud), chunk, static_cast<unsigned long>(pace), mode);
      return;
    }
    LOG_I("POC",
          "SELFTEST WIRE ok=%lu herr=%lu perr=%lu drops=%lu mismatch=%lu timeouts=%lu rxbytes=%lu intfree=%u ms=%lu "
          "kbs=%lu",
          static_cast<unsigned long>(r.ok), static_cast<unsigned long>(r.herr), static_cast<unsigned long>(r.perr),
          static_cast<unsigned long>(r.drops), static_cast<unsigned long>(r.mismatch),
          static_cast<unsigned long>(r.timeouts), static_cast<unsigned long>(r.rxbytes), static_cast<unsigned>(intFree),
          static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.kbs));
  }

  // On-device WIRE matrix for the no-PC stage. QUICK (default): 3 baud x 3
  // chunk x 2 pace x mode 1 x 3 frames = 18 combos (230400 BANNED, §3).
  // FULL adds chunks 16/1024, pace 5000, mode 2 (90 combos), plus a mode-3
  // 1-frame single-point per baud (skipped
  // with a note when the camera is unavailable, SKIP_BIGFRAME when the frame
  // exceeds the 64KB verify slot — counted as neither ok nor bad), plus the HIGH tier
  // (1M-5M x {64,128,512} x {0,1000} x mode 1 + mode-3 point per HIGH baud).
  // Expect 3M+ to fail (beyond practical UART scope) — record, move on.
  void runSelfSweep_(bool full) {
    static const uint32_t kBauds[] = {115200, 460800, 921600};
    static const uint32_t kBaudsH[] = {1000000, 1500000, 2000000, 3000000, 4000000, 5000000};
    static const uint16_t kChunksQ[] = {64, 128, 512};
    static const uint16_t kChunksF[] = {16, 64, 128, 512, 1024};
    static const uint32_t kPacesQ[] = {0, 1000};
    static const uint32_t kPacesF[] = {0, 1000, 5000};
    static const uint8_t kModesQ[] = {1};
    static const uint8_t kModesF[] = {1, 2};  // Head sweeps JPEG-heavy {1,2}; S3 (no camera) sweeps TEXT-heavy {0,1}.
    const uint16_t* chunks = full ? kChunksF : kChunksQ;
    const size_t nChunks = full ? 5 : 3;
    const uint32_t* paces = full ? kPacesF : kPacesQ;
    const size_t nPaces = full ? 3 : 2;
    const uint8_t* modes = full ? kModesF : kModesQ;
    const size_t nModes = full ? 2 : 1;
    const size_t nBauds = sizeof(kBauds) / sizeof(kBauds[0]);
    uint32_t combos = 0;
    uint32_t bad = 0;
    for (size_t bi = 0; bi < nBauds; ++bi) {
      for (size_t ci = 0; ci < nChunks; ++ci) {
        for (size_t pi = 0; pi < nPaces; ++pi) {
          for (size_t mi = 0; mi < nModes; ++mi) {
            const SelfRes r = runSelfWireRes_(kBauds[bi], modes[mi], chunks[ci], paces[pi], 3);
            ++combos;
            if (r.camUnavail || r.ok != 3 || r.timeouts > 0 || r.mismatch > 0 || r.herr > 0 || r.perr > 0 ||
                r.drops > 0) {
              ++bad;
            }
            logWireRes_(r, kBauds[bi], chunks[ci], paces[pi], modes[mi], true);
            FaultManager::watchdogFeed();
          }
        }
      }
    }
    if (full) {
      const bool camAvail = ensureCamera();
      if (!camAvail) {
        LOG_I("POC", "SELFTEST SWEEP mode3 skipped CAMUNAVAIL");
      } else {
        for (size_t bi = 0; bi < nBauds; ++bi) {
          const SelfRes r = runSelfWireRes_(kBauds[bi], uartpoc::MODE_HW_CAM, 128, 1000, 1);
          ++combos;
          // SKIP_BIGFRAME counts as NEITHER ok NOR bad (no bad++): the frame
          // cannot fit the 64KB slot at this resolution, so there is nothing
          // to verify. The SKIP line above carries the size.
          if (!(r.skipped > 0 && r.ok == 0) && (r.camUnavail || r.ok != 1 || r.timeouts > 0 || r.mismatch > 0 ||
                                                r.herr > 0 || r.perr > 0 || r.drops > 0)) {
            ++bad;
          }
          logWireRes_(r, kBauds[bi], 128, 1000, uartpoc::MODE_HW_CAM, true);
          FaultManager::watchdogFeed();
        }
      }
      // HIGH tier: base-subset sweep per high baud + mode-3 point (camera already ensured).
      static const uint16_t kChunksH[] = {64, 128, 512};
      static const uint32_t kPacesH[] = {0, 1000};
      const size_t nBaudsH = sizeof(kBaudsH) / sizeof(kBaudsH[0]);
      for (size_t bi = 0; bi < nBaudsH; ++bi) {
        for (size_t ci = 0; ci < 3; ++ci) {
          for (size_t pi = 0; pi < 2; ++pi) {
            const SelfRes r = runSelfWireRes_(kBaudsH[bi], uartpoc::MODE_SYNTH_RAMP, kChunksH[ci], kPacesH[pi], 3);
            ++combos;
            if (r.camUnavail || r.ok != 3 || r.timeouts > 0 || r.mismatch > 0 || r.herr > 0 || r.perr > 0 ||
                r.drops > 0) {
              ++bad;
            }
            logWireRes_(r, kBaudsH[bi], kChunksH[ci], kPacesH[pi], uartpoc::MODE_SYNTH_RAMP, true);
            FaultManager::watchdogFeed();
          }
        }
        if (camAvail) {
          const SelfRes r = runSelfWireRes_(kBaudsH[bi], uartpoc::MODE_HW_CAM, 128, 1000, 1);
          ++combos;
          // Same SKIP rule as the base-tier mode-3 points: neither ok nor bad.
          if (!(r.skipped > 0 && r.ok == 0) && (r.camUnavail || r.ok != 1 || r.timeouts > 0 || r.mismatch > 0 ||
                                                r.herr > 0 || r.perr > 0 || r.drops > 0)) {
            ++bad;
          }
          logWireRes_(r, kBaudsH[bi], 128, 1000, uartpoc::MODE_HW_CAM, true);
          FaultManager::watchdogFeed();
        }
      }
    }
    LOG_I("POC", "SELFTEST SWEEP done combos=%lu bad=%lu", static_cast<unsigned long>(combos),
          static_cast<unsigned long>(bad));
  }

  void buildStats_(char* out, size_t cap) {
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // txp (parity chunks) is appended LAST: the existing field order never moves.
    snprintf(out, cap,
             "STATS txf=%lu txc=%lu txb=%lu rxcmd=%lu rxe=%lu mode=%u chunk=%u pace=%lu baud=%lu fps=%u intfree=%u "
             "txp=%lu",
             static_cast<unsigned long>(txFrames_), static_cast<unsigned long>(txChunks_),
             static_cast<unsigned long>(txBytes_), static_cast<unsigned long>(rxCmds_),
             static_cast<unsigned long>(rxInvalid_), cfg_.mode, cfg_.chunk_bytes,
             static_cast<unsigned long>(cfg_.pace_us), static_cast<unsigned long>(currentBaud_()), cfg_.fps,
             static_cast<unsigned>(intFree), static_cast<unsigned long>(txParity_));
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
  uartpoc::Reassembler verifyReasm_;  // Test-local state; attached once in begin(), reset per test.
  uint8_t* txBuf_;                    // INTERNAL encode/staging (kMaxFrameLen).
  uint8_t* chunkBuf_;                 // INTERNAL raw chunk staging (kMaxPayload).
  uint8_t* verifySlot_;               // INTERNAL verify reassembly slot (kVerifySlotCap).
  uint8_t* verifyChunk_;              // INTERNAL verify chunk staging (kMaxPayload).
  uint8_t* fecBuf_;  // INTERNAL FEC scratch: N*stride data staging + K*stride parity (kFecScratchBytes).
  const uint8_t* fecDataPtrs_[uartpoc::fec::kMaxData];  // Per-frame data pointers into fecBuf_/fb (no heap).
  uint8_t* fecParPtrs_[uartpoc::fec::kMaxParity];       // Per-frame parity pointers into the fecBuf_ parity area.
  size_t fecLens_[uartpoc::fec::kMaxData];              // Per-frame true data lengths (LAST may be short).
  bool streaming_;
  bool txHold_;      // Silent window across baud switches.
  bool testActive_;  // WIRE/SWEEP own Serial2 while set (pollLink hands off).
  uint16_t txFrameId_;
  uint32_t nextTickMs_;
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
  uint32_t txParity_;  // Parity CHUNKs only (txChunks_ stays data-only; txp is appended LAST in STATS).
  uint32_t txBytes_;
  uint32_t rxCmds_;
  uint32_t rxInvalid_;
  char usbBuf_[96];
  size_t usbLen_;
  char cmdBuf_[160];
  TaskHandle_t genTask_;
  SemaphoreHandle_t txMutex_;  // Serializes ALL Serial2 TX (generator task vs loop task).
};

#endif  // ARDUINO
