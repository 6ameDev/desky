#pragma once
// desky-core UART POC receiver — header-only service (poc-comm-link spike).
//
// Owns the S3 side of the barebones bi-directional UART transport:
//  - Serial1 link (RX=MCU_LINK_UART_RX=18, TX=MCU_LINK_UART_TX=17). TX stays
//    TRISTATED (RX-only begin, TX pin unassigned) until the first valid Head
//    frame — the S3 must never drive the CAM's GPIO12 strapping pin during
//    CAM reset. First valid frame attaches TX.
//  - Reassembly in one INTERNAL frame slot (~64KB cap, else OVERSIZE drop
//    counted as `big`).
//    Out-of-order tolerated, duplicates ignored, missing-at-LAST drops.
//  - Counters + machine-parseable single-line STATS for the sweep script.
//  - USB CLI: STATS | RESET | SET k v | GET k | GET all | HEAD SET k v |
//    HEAD GET k | HELP. Baud changes always run the deferred ACK-then-switch
//    protocol (CMD at old baud, 2s ACK wait else abort, both switch after a
//    delay, USB reminder to switch the monitor).
//  - SELFTEST verbs (USB CLI, needs TX17-RX18 loopback jumper, CAM powered
//    off): WIRE (physical loopback through silicon: synth TEXT/RAMP/JPEG TX
//    -> RX -> regen+memcmp verify), SWEEP (WIRE matrix for the no-PC stage).
//    USB stays 115200 throughout; only Serial1 changes baud, always restored
//    RX-only tristated with the normal decoder reset.
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
        verifyChunk_(nullptr),
        parBuf_(nullptr),
        testActive_(false),
        txEnabled_(false),
        txFrameId_(0),
        framesOk_(0),
        chunksRx_(0),
        hdrErr_(0),
        payErr_(0),
        drops_(0),
        big_(0),
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
        usbLen_(0),
        recFec_(0),
        parRx_(0),
        parCount_(0),
        parHave_(false),
        parFid_(0),
        dActive_(false),
        dFid_(0),
        dStride_(0),
        dHaveLast_(false),
        dLastIdx_(0),
        dLastLen_(0),
        dPend_(false),
        dPendIdx_(0) {
    respBuf_[0] = '\0';
    parReset_();
    dReset_();
  }

  void begin() {
    rxBuf_ = static_cast<uint8_t*>(heap_caps_malloc(kRxBuf, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(rxBuf_ != nullptr);
    slot_ = static_cast<uint8_t*>(heap_caps_malloc(kSlotCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(slot_ != nullptr);
    txBuf_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxFrameLen, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(txBuf_ != nullptr);
    verifyChunk_ = static_cast<uint8_t*>(heap_caps_malloc(uartpoc::kMaxPayload, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(verifyChunk_ != nullptr);
    parBuf_ = static_cast<uint8_t*>(heap_caps_malloc(kParityBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    DESKY_ASSERT(parBuf_ != nullptr);
#if POC_HAVE_INTERNAL_CHECK
    DESKY_ASSERT(esp_ptr_internal(rxBuf_) && esp_ptr_internal(slot_) && esp_ptr_internal(txBuf_) &&
                 esp_ptr_internal(verifyChunk_) && esp_ptr_internal(parBuf_));
#endif
    parReset_();
    dReset_();
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
      parReset_();  // The partial's parity dies with it: no cross-frame leakage.
      dReset_();
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
    if (testActive_) {
      return;  // WIRE/SWEEP own Serial1: test bytes stay buffered for the test pump, never link traffic.
    }
    int avail = Serial1.available();
    if (avail >= static_cast<int>(kRxOvfWarn)) {
      ++ovf_;  // At most once per poll: ring is nearly full, overflow pressure.
    }
    int passes = 0;
    // Bounded: an undistinguishable drain (wedged UART, ever-failing read) would
    // otherwise pin loopTask until the task watchdog panics (observed in Stage 3).
    // 64 passes ≈ 128KB, far beyond any legitimate backlog; rest waits for next poll().
    while (avail > 0 && passes < 64) {
      ++passes;
      FaultManager::watchdogFeed();
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
    if (testActive_) {
      return;  // Belt-and-braces: test bytes are never mistaken for link traffic.
    }
    const uint32_t now = millis();
    lastRxMs_ = now;
    if (tFirstMs_ == 0) {
      tFirstMs_ = now;
    }
    if (!txEnabled_) {
      enableTx_();
    }
    if (fr.type == uartpoc::MSG_CHUNK) {
      // FEC parity intercept BEFORE reassembly: parity rides the same CHUNK
      // type with IS_PARITY set (never LAST_CHUNK — LAST stays on data N-1).
      // It stages into the small INTERNAL parity slots keyed by frameId and
      // NEVER enters place_/seen_/total_ accounting or OVERSIZE math.
      // Ignored parity is silent by design (no counter); accepted stores
      // count parRx_ in onParity_ below.
      if ((fr.flags & uartpoc::fec::kFlagParity) != 0) {
        onParity_(fr);
      } else {
        ++chunksRx_;
        lastChunkMs_ = now;
        size_t total = 0;
        uartpoc::Reassembler::Push res =
            reasm_.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, total);
        if (res == uartpoc::Reassembler::Push::STALE) {
          ++drops_;     // Previous partial abandoned; re-push starts the new frame.
          parReset_();  // That frame's parity dies with it: no cross-frame leakage.
          dReset_();    // Shadow re-adopts below via syncShadow_.
          res = reasm_.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, total);
        }
        syncShadow_(fr);
        if (res == uartpoc::Reassembler::Push::COMPLETE) {
          // Clean-only: recovery completions count recFec_ instead (see tryRecover_).
          ++framesOk_;
          bytesRx_ += total;
          oooFrame_ = 0xFFFF;  // Fresh frame resets the OOO heuristic.
          parReset_();         // Buffered parity for a done frame is stale; late parity is ignored.
        } else if (res == uartpoc::Reassembler::Push::OVERSIZE) {
          // 64KB-slot honesty: the frame can never fit, so it is dropped — but
          // flagged separately (big ⊆ drops) instead of vanishing silently.
          // Cap overflow is not an erasure: OVERSIZE frames never recover.
          ++drops_;
          ++big_;
          oooFrame_ = 0xFFFF;
          parReset_();
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
          noteAccepted_(fr);  // Shadow stride/haveLast/seen mirror (data only).
          tryRecover_();      // Recover-before-reset: missing<=good-parity completes via recFec_.
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

  // ── FEC-RX (S3 recover-before-reset) ──
  // Parity slots hold K<=4 full-stride blocks for exactly one frameId; the
  // data shadow mirrors Reassembler stride/haveLast/seen (which exposes no
  // accessors) so recovery can name the exact erasure set. Both are INTERNAL-
  // only, flushed on STALE/reset/COMPLETE/OVERSIZE — never across frames.

  void parReset_() {
    parHave_ = false;
    parFid_ = 0;
    parCount_ = 0;
    for (uint8_t i = 0; i < kParitySlots; ++i) {
      parIdx_[i] = 0;
      parLen_[i] = 0;
    }
  }

  void dReset_() {
    dActive_ = false;
    dFid_ = 0;
    dStride_ = 0;
    dHaveLast_ = false;
    dLastIdx_ = 0;
    dLastLen_ = 0;
    dPend_ = false;
    dPendIdx_ = 0;
    for (size_t i = 0; i < uartpoc::Reassembler::kBitmapWords; ++i) {
      dBits_[i] = 0;
    }
  }

  void dSet_(uint16_t idx) { dBits_[idx / 32] |= (1UL << (idx % 32)); }

  bool dSeen_(uint16_t idx) const { return (dBits_[idx / 32] & (1UL << (idx % 32))) != 0; }

  // Shadow sync after a data push: adopt silently (re)started frames, drop
  // dead ones. STALE/COMPLETE/OVERSIZE callers manage parity explicitly; here
  // only the quiet cases need work.
  void syncShadow_(const uartpoc::DecodedFrame& fr) {
    if (!reasm_.active()) {
      dActive_ = false;
      dReset_();
      return;
    }
    if (!dActive_ || fr.frameId != dFid_) {
      // Silent (re)start — reasm_ had nothing to abandon (no STALE fired).
      // Any other frame's parity is stale: drop it silently, no counter.
      if (parHave_ && parFid_ != fr.frameId) {
        parReset_();
      }
      dReset_();
      dActive_ = true;
      dFid_ = fr.frameId;
    }
  }

  // Shadow mirror of the Reassembler stride/stash bookkeeping, DATA chunks
  // only (parity never reaches here): first non-LAST length wins as stride; a
  // LAST arriving before any stride is stashed (pend), not placed.
  void noteAccepted_(const uartpoc::DecodedFrame& fr) {
    const bool last = (fr.flags & uartpoc::FLAG_LAST_CHUNK) != 0;
    if (!last && dStride_ == 0 && fr.payloadLen > 0) {
      dStride_ = fr.payloadLen;
      if (dPend_) {
        dSet_(dPendIdx_);
        dPend_ = false;
      }
    }
    if (last && fr.chunkIdx == 0 && dStride_ == 0) {
      dStride_ = (fr.payloadLen > 0) ? fr.payloadLen : 1;
    }
    if (last && dStride_ == 0 && fr.chunkIdx != 0) {
      dHaveLast_ = true;
      dLastIdx_ = fr.chunkIdx;
      dLastLen_ = fr.payloadLen;
      dPend_ = true;
      dPendIdx_ = fr.chunkIdx;
      return;
    }
    dSet_(fr.chunkIdx);
    if (last) {
      dHaveLast_ = true;
      dLastIdx_ = fr.chunkIdx;
      dLastLen_ = fr.payloadLen;
    }
  }

  // Parity intercept: buffer only for the live data frame. Parity for a
  // non-active/older frame, or arriving after its data COMPLETE/STALE
  // (slots already flushed), is ignored with no counter.
  void onParity_(const uartpoc::DecodedFrame& fr) {
    if (!dActive_ || fr.frameId != dFid_) {
      return;
    }
    if (fr.payloadLen == 0 || fr.payloadLen > uartpoc::kMaxPayload) {
      return;
    }
    if (fr.payloadLen > 0 && fr.payload == nullptr) {
      return;
    }
    if (parBuf_ == nullptr) {
      return;
    }
    if (!parHave_ || parFid_ != fr.frameId) {
      parReset_();
      parFid_ = fr.frameId;
      parHave_ = true;
    }
    if (parCount_ >= kParitySlots) {
      return;
    }
    for (uint8_t i = 0; i < parCount_; ++i) {
      if (parIdx_[i] == fr.chunkIdx) {
        return;  // Duplicate parity: silent.
      }
    }
    uint8_t* dst = parBuf_ + static_cast<size_t>(parCount_) * uartpoc::kMaxPayload;
    for (uint16_t i = 0; i < fr.payloadLen; ++i) {
      dst[i] = fr.payload[i];
    }
    parIdx_[parCount_] = fr.chunkIdx;
    parLen_[parCount_] = fr.payloadLen;
    ++parCount_;
    ++parRx_;       // Every accepted parity store counts (duplicates/foreign-frame
                    // stays silent above): task-4 separates wire-loss of parity
                    // (Head txp vs parRx_) from staged-but-unused parity.
    tryRecover_();  // Fresh parity may complete a waiting partial.
  }

  // Recover-before-reset: a data frame with LAST known, stride known, no
  // pending stash, exactly n_missing short with 1<=n_missing<=K_good (good
  // parity buffered for this frameId) is solved via fec::recover and each
  // reconstruction is fed through the NORMAL reasm_.push path, so COMPLETE
  // fires honestly with the right total. The frame then counts recFec_
  // (recovered), never framesOk_ (clean); bytesRx_ counts it identically.
  // Anything short of a solving COMPLETE falls through untouched: the live
  // partial ages out through the existing STALE/DROPPED paths with identical
  // counting. OVERSIZE frames never reach here (cap overflow is not erasure).
  void tryRecover_() {
    if (!dActive_ || !dHaveLast_ || dPend_ || dStride_ == 0) {
      return;
    }
    if (!parHave_ || parFid_ != dFid_ || parCount_ == 0) {
      return;
    }
    if (slot_ == nullptr) {
      return;
    }
    const uint32_t n = static_cast<uint32_t>(dLastIdx_) + 1;
    if (n > uartpoc::fec::kMaxData) {
      return;  // Beyond the erasure code: the normal drop path owns it.
    }
    if (dStride_ == 0 || dStride_ > uartpoc::kMaxPayload) {
      return;
    }
    if (n * dStride_ > kSlotCap) {
      return;  // Would OVERSIZE: let the normal path flag big.
    }
    if (dLastLen_ > dStride_) {
      return;
    }
    uint8_t missIdx[uartpoc::fec::kMaxData];
    uint8_t nMiss = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (!dSeen_(static_cast<uint16_t>(i))) {
        missIdx[nMiss++] = static_cast<uint8_t>(i);
      }
    }
    if (nMiss == 0) {
      return;  // Nothing to repair; live pushes COMPLETE clean.
    }
    const uint8_t* parPtr[uartpoc::fec::kMaxParity] = {nullptr, nullptr, nullptr, nullptr};
    bool parOk[uartpoc::fec::kMaxParity] = {false, false, false, false};
    uint8_t nGood = 0;
    for (uint8_t s = 0; s < parCount_; ++s) {
      if (parLen_[s] != dStride_) {
        continue;  // Stride mismatch: not usable for this frame.
      }
      if (static_cast<uint32_t>(parIdx_[s]) < n) {
        continue;  // Data-range idx, not parity N..N+K-1.
      }
      const uint32_t j = static_cast<uint32_t>(parIdx_[s]) - n;
      if (j >= uartpoc::fec::kMaxParity || parOk[j]) {
        continue;
      }
      parPtr[j] = parBuf_ + static_cast<size_t>(s) * uartpoc::kMaxPayload;
      parOk[j] = true;
      ++nGood;
    }
    if (nMiss > nGood) {
      return;  // NEED_MORE: wait for the remaining parity.
    }
    // Present blocks read straight from the reassembly slot at their placed
    // offsets; missing blocks are solved back full-stride into the same slot
    // (recover owns the stride padding — the caller truncates N-1 below).
    uint8_t* dataIo[uartpoc::fec::kMaxData];
    bool missing[uartpoc::fec::kMaxData];
    for (uint32_t i = 0; i < n; ++i) {
      dataIo[i] = slot_ + i * dStride_;
      missing[i] = !dSeen_(static_cast<uint16_t>(i));
    }
    // Stride-padding rule (see uart_fec.h): encode() zero-pads short tails
    // internally, so a PRESENT short LAST must carry zero padding past
    // last_len for the solve to verify. That slot region holds stale bytes
    // from older frames — normalize it now (outside the frame total, so a
    // failed recovery leaves reassembly untouched).
    for (size_t b = dLastLen_; b < dStride_; ++b) {
      dataIo[n - 1][b] = 0;
    }
    const uartpoc::fec::Recover rc =
        uartpoc::fec::recover(dataIo, missing, static_cast<uint8_t>(n), parPtr, parOk, uartpoc::fec::kMaxParity,
                              dStride_, dLastLen_, fecScratch_, sizeof(fecScratch_));
    if (rc != uartpoc::fec::Recover::OK) {
      return;  // UNRECOVERABLE (e.g. corrupt parity): never emit, drop path owns it.
    }
    for (uint8_t m = 0; m < nMiss; ++m) {
      const uint8_t idx = missIdx[m];
      const bool isLast = (idx == dLastIdx_);
      const uint16_t ln = isLast ? dLastLen_ : static_cast<uint16_t>(dStride_);
      const uint8_t fl = isLast ? uartpoc::FLAG_LAST_CHUNK : 0;
      size_t t2 = 0;
      const uartpoc::Reassembler::Push r2 =
          reasm_.push(dFid_, idx, fl, slot_ + static_cast<size_t>(idx) * dStride_, ln, t2);
      if (r2 == uartpoc::Reassembler::Push::COMPLETE) {
        ++recFec_;
        bytesRx_ += t2;
        oooFrame_ = 0xFFFF;
        parReset_();
        dActive_ = false;
        dReset_();
        return;
      }
      if (r2 == uartpoc::Reassembler::Push::ACCEPTED) {
        dSet_(idx);
        continue;
      }
      if (r2 == uartpoc::Reassembler::Push::DUPLICATE) {
        continue;  // Shadow over-marked a present chunk: harmless.
      }
      // Defensive: bounds pre-checked and same fid, so OVERSIZE/DROPPED/STALE
      // cannot fire here. Stop feeding; the live partial ages out normally.
      dActive_ = reasm_.active();
      if (!dActive_) {
        dReset_();
      }
      return;
    }
    // All reconstructions placed but COMPLETE did not fire (shadow
    // over-marked): leave the partial live; later chunks/parity still complete
    // it or age it out through the existing paths.
  }

  // Send a CMD frame and wait ≤2s per attempt for its RESP (up to 3 attempts).
  // Retry is load-bearing, not garnish: the S3 decoder is greedy — a RESP that
  // arrives on top of a partial streaming chunk is consumed as that chunk's
  // payload (the chunk's own header CRC stays valid), destroying the RESP with
  // exactly one perr and zero other symptoms (measured ~25% first-try loss with
  // streaming on, 0% stopped). CMDs are idempotent (SET/GET/STATS), so resend
  // is safe. Each attempt uses a fresh frameId + deadline; 50ms between
  // attempts lets in-flight bytes settle. True + respOut on first reply.
  bool sendCmdAndWait_(const char* cmd, char* respOut, size_t respCap) {
    if (!txEnabled_) {
      LOG_W("POC", "no link yet (tx tristated), bridge refused");
      return false;
    }
    size_t n = 0;
    while (cmd[n] != '\0') {
      ++n;
    }
    for (int att = 0; att < 3; ++att) {
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
        FaultManager::watchdogFeed();  // 2s+ waits must never starve the watchdog.
      }
      waitingResp_ = false;
      if (respGot_) {
        size_t i = 0;
        while (i + 1 < respCap && respBuf_[i] != '\0') {
          respOut[i] = respBuf_[i];
          ++i;
        }
        respOut[i] = '\0';
        return true;
      }
      delay(50);  // Let in-flight bytes settle before resending.
    }
    return false;
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
    // Drain + end/begin (NEVER updateBaudRate mid-stream: reconfiguring with a live
    // FIFO intermittently wedges UART TX silent with zero counters. end/begin has
    // 200+ clean runs across all loopback sweeps).
    Serial1.flush();  // Blocks until TX drains at the OLD rate.
    Serial1.end();
    Serial1.setRxBufferSize(kRxRing);
    Serial1.begin(target, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    linkDec_.reset();  // Old-rate bytes must not seed new-rate decodes.
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
      // Note: fec_k is accepted-but-inert on S3 (parseSet stores it, but S3
      // never emits parity — S3 is the FEC-RX side only; recovery consumes
      // Head parity regardless of this knob). exposure/jpeg_quality are likewise
      // accepted-but-inert here (camera lives on Head; set them via HEAD SET).
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
      char kv[128];  // GET-all line grew with the exposure/jpeg_quality tokens (~98 worst case).
      uartpoc::formatGet(cfg_, key, kv, sizeof(kv));
      LOG_I("POC", "%s", kv);
      return;
    }
    if (isWord_(line, "STATS")) {
      char msg[288];  // Worst-case all-counters-max line with rec+par needs ~267+NUL.
      buildStats_(msg, sizeof(msg));
      LOG_I("POC", "%s", msg);
      return;
    }
    if (isWord_(line, "RESET")) {
      framesOk_ = chunksRx_ = hdrErr_ = payErr_ = drops_ = big_ = ooo_ = dups_ = ovf_ = hbRx_ = bytesRx_ = recFec_ =
          parRx_ = 0;
      tFirstMs_ = 0;
      lastChunkMs_ = 0;
      oooFrame_ = 0xFFFF;
      parReset_();
      dReset_();
      reasm_.reset();
      linkDec_.reset();
      LOG_I("POC", "counters reset");
      return;
    }
    if (isWord_(line, "HELP")) {
      LOG_I("POC", "cmds: STATS | RESET | SET k v | GET k | GET all | HEAD SET k v | HEAD GET k | HELP");
      LOG_I("POC",
            "keys: chunk|chunk_bytes 16..1024 pace|pace_us 0..50000 baud <list incl 1M-5M> mode 0..3 fps 1..30 "
            "fec|fec_k 0..4");
      LOG_I("POC",
            "keys (Head-side via HEAD SET): framesize qvga|vga|svga|xga|sxga|uxga|qxga (mode-3 re-init) "
            "exposure 0..1200 (live) jpeg_quality 10..30 (re-init)");
      LOG_I("POC",
            "selftest: SELFTEST WIRE <baud> <mode 0-2> <chunk 16-1024> <pace 0-50000> <nframes 1-50> (needs "
            "TX17-RX18 jumper, CAM off)");
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

  // ── SELFTEST (S3 loopback verification, TX17-RX18 jumper, CAM powered off) ──
  // Mirrors the Head WIRE design: settle → flush AFTER settling (never before)
  // → throwaway-HB warmup through a junk decoder → counted frames, one
  // per-frame retry. The S3 has no generator task (loop is single-threaded),
  // so encode staging reuse would be race-free — but Head-style stack enc[1040]
  // is used anyway to keep the TX path obviously disjoint from txBuf_ (CMD/HB).
  // Verify reuses slot_ through a TEST-LOCAL Reassembler (reset per frame;
  // the normal reasm_ is untouched). testActive_ suspends normal link
  // processing so test bytes are never mistaken for link traffic; afterwards
  // Serial1 is restored RX-only tristated (strapping rule: S3 TX must end
  // tristated), flushed, with linkDec_ reset. USB (Serial) baud never changes.

  struct SelfRes {
    uint32_t ok;
    uint32_t herr;
    uint32_t perr;
    uint32_t drops;
    uint32_t mismatch;
    uint32_t timeouts;
    uint32_t rxbytes;
    uint32_t ms;
    uint32_t kbs;
  };

  void handleSelftest_(const char* args) {
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
      if (mode > uartpoc::MODE_SYNTH_JPEG) {
        LOG_W("POC", "NACK bad_value");  // S3 has no camera: modes 0-2 only.
        return;
      }
      if (nframes < 1 || nframes > 50) {
        LOG_W("POC", "NACK bad_value");
        return;
      }
      const SelfRes r =
          runSelfWireRes_(baud, static_cast<uint8_t>(mode),
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
  // truncated); returns the count, or -1 on trailing garbage / empty token.
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

  // One synthetic test frame through silicon: per-chunk encode -> TX -> drain
  // -> regen+memcmp verify, with a post-TX drain until deadline. Returns true
  // on COMPLETE with the expected total (else the caller counts drop/timeout).
  bool testFrameSynth_(uint8_t mode, uint16_t fid, uint16_t chunk, uint32_t pace, uartpoc::Decoder& dec,
                       uartpoc::Reassembler& verifyReasm, uint8_t* enc, SelfRes& r, uint32_t deadline) {
    verifyReasm.reset();
    const uint32_t total = pocself::synthTotal(mode);
    const uint8_t flags = pocself::synthFlags(mode);
    const uint32_t nChunks = (total + chunk - 1) / chunk;
    bool done = false;
    size_t outTotal = 0;
    for (uint32_t idx = 0; idx < nChunks && !done; ++idx) {
      if ((int32_t)(millis() - deadline) >= 0) {
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
      size_t encLen = 0;
      const bool encOk = uartpoc::encodeFrame(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), verifyChunk_, n,
                                              enc, uartpoc::kMaxFrameLen, encLen);
      if (!encOk) {
        ++r.drops;
        continue;
      }
      Serial1.write(enc, encLen);
      wireDrain_(dec, verifyReasm, mode, chunk, total, r, done, outTotal);
      if (pace > 0 && idx + 1 < nChunks) {
        delayMicroseconds(pace);
      }
      FaultManager::watchdogFeed();
    }
    if (!done) {
      while (!done && (int32_t)(deadline - millis()) > 0) {
        wireDrain_(dec, verifyReasm, mode, chunk, total, r, done, outTotal);
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

  // Handle one decoded test CHUNK: reassemble with Head-identical STALE
  // handling, then regenerate expected bytes and memcmp. Sets done/outTotal
  // on COMPLETE.
  void onTestChunk_(uint8_t mode, uint16_t stride, uint32_t total, uartpoc::Reassembler& verifyReasm,
                    const uartpoc::DecodedFrame& fr, SelfRes& r, bool& done, size_t& outTotal) {
    size_t tot = 0;
    uartpoc::Reassembler::Push pr = verifyReasm.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, tot);
    if (pr == uartpoc::Reassembler::Push::STALE) {
      ++r.drops;
      pr = verifyReasm.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, tot);
    }
    if (pr == uartpoc::Reassembler::Push::DROPPED || pr == uartpoc::Reassembler::Push::OVERSIZE) {
      ++r.drops;
      return;
    }
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
    if (pr == uartpoc::Reassembler::Push::COMPLETE) {
      outTotal = tot;
      done = true;
    }
  }

  // Drain all currently-available Serial1 bytes through the test decoder.
  void wireDrain_(uartpoc::Decoder& dec, uartpoc::Reassembler& verifyReasm, uint8_t mode, uint16_t stride,
                  uint32_t total, SelfRes& r, bool& done, size_t& outTotal) {
    FaultManager::watchdogFeed();
    while (Serial1.available() > 0 && !done) {
      const int c = Serial1.read();
      if (c < 0) {
        break;
      }
      ++r.rxbytes;
      const uint8_t b = static_cast<uint8_t>(c);
      size_t consumed = 0;
      uartpoc::DecodedFrame fr;
      const uartpoc::DecodeStatus st = dec.feed(&b, 1, consumed, fr);
      if (st == uartpoc::DecodeStatus::OK) {
        onTestChunk_(mode, stride, total, verifyReasm, fr, r, done, outTotal);
      } else if (st == uartpoc::DecodeStatus::ERR_HDR_CRC) {
        ++r.herr;
      } else if (st == uartpoc::DecodeStatus::ERR_PAY_CRC) {
        ++r.perr;
      }
    }
  }

  void flushSerial1_() {
    while (Serial1.available() > 0) {
      Serial1.read();
    }
  }

  // Throwaway HB round-trip on the live Serial1 rate: any residual settle glitch lands here,
  // decoded by a junk decoder, never touching test state. Bounded (~200ms), WDT-fed.
  void wireWarmup_() {
    uint8_t hb[uartpoc::kHeaderLen + uartpoc::kTailLen];
    size_t hbLen = 0;
    if (!uartpoc::encodeFrame(uartpoc::MSG_HB, 0, 0xFFFF, 0, nullptr, 0, hb, sizeof(hb), hbLen)) {
      return;
    }
    Serial1.write(hb, hbLen);
    uartpoc::Decoder junk;
    const uint32_t end = millis() + 200;
    while ((int32_t)(end - millis()) > 0) {
      while (Serial1.available() > 0) {
        const int c = Serial1.read();
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
    flushSerial1_();
  }

  // Physical loopback core: suspends normal link processing, owns Serial1 at
  // <baud> with an enlarged RX buffer (self-echo of a full-size chunk must not
  // overrun the 256B Arduino default), restores everything. USB (Serial) baud
  // never changes; cfg_ is untouched (only the live UART rate moves).
  SelfRes runSelfWireRes_(uint32_t baud, uint8_t mode, uint16_t chunk, uint32_t pace, uint32_t nframes) {
    SelfRes r = {};
    const uint32_t t0 = millis();
    testActive_ = true;  // pollLink/onFrame_ hand off: test bytes are never link traffic.
    Serial1.end();
    Serial1.setRxBufferSize(kRxRing);
    Serial1.begin(baud, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    delay(50);                         // Let the new baud settle: reconfig can spit a framing glitch that would
    flushSerial1_();                   // otherwise eat frame 0's magic (silent resync, fatal). Flush AFTER settling.
    uartpoc::Decoder dec;              // Test-local: the shared linkDec_ is never touched.
    uartpoc::Reassembler verifyReasm;  // Test-local state on the shared slot_; normal reasm_ untouched.
    verifyReasm.attach(slot_, kSlotCap);
    wireWarmup_();                       // Throwaway HB round-trip: proves the path clean before frame 0 counts.
    uint8_t enc[uartpoc::kMaxFrameLen];  // Stack staging: txBuf_ stays with the CMD/HB path.
    for (uint32_t f = 0; f < nframes; ++f) {
      const uint16_t fid = static_cast<uint16_t>(f);
      bool done = false;
      for (int att = 0; att < 2 && !done; ++att) {  // One retry: a settle-glitch casualty
        if (att > 0) {                              // must not doom the whole test.
          flushSerial1_();                          // (Retry-attempt wire errors still count: real events.)
          dec.reset();
        }
        const uint32_t deadline = millis() + 2000;
        done = testFrameSynth_(mode, fid, chunk, pace, dec, verifyReasm, enc, r, deadline);
      }
      if (done) {
        ++r.ok;
      } else {
        ++r.timeouts;  // Abort remaining frames on timeout.
        break;
      }
    }
    Serial1.end();
    Serial1.setRxBufferSize(kRxRing);
    Serial1.begin(cfg_.baud, SERIAL_8N1, MCU_LINK_UART_RX, -1);  // RX-only tristate: S3 TX ends detached.
    flushSerial1_();
    linkDec_.reset();
    testActive_ = false;
    r.ms = millis() - t0;
    r.kbs = (r.ms < 1000) ? 0 : static_cast<uint32_t>(r.rxbytes / r.ms);
    return r;
  }

  void logWireRes_(const SelfRes& r, uint32_t baud, uint16_t chunk, uint32_t pace, uint8_t mode, bool combo) {
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
  // FULL adds chunks 16/1024, pace 5000, mode 0 (90 base combos), plus the
  // HIGH tier (1M-5M x {64,128,512} x {0,1000} x mode 1 x 3 frames = 36 combos).
  // No mode-3 on the S3 (no camera). USB stays 115200 throughout.
  void runSelfSweep_(bool full) {
    static const uint32_t kBauds[] = {115200, 460800, 921600};
    static const uint32_t kBaudsH[] = {1000000, 1500000, 2000000, 3000000, 4000000, 5000000};
    static const uint16_t kChunksQ[] = {64, 128, 512};
    static const uint16_t kChunksF[] = {16, 64, 128, 512, 1024};
    static const uint32_t kPacesQ[] = {0, 1000};
    static const uint32_t kPacesF[] = {0, 1000, 5000};
    static const uint8_t kModesQ[] = {1};
    static const uint8_t kModesF[] = {0, 1};  // S3 has no camera: TEXT-heavy {0,1}; Head sweeps JPEG-heavy {1,2}.
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
            if (r.ok != 3 || r.timeouts > 0 || r.mismatch > 0 || r.herr > 0 || r.perr > 0 || r.drops > 0) {
              ++bad;
            }
            logWireRes_(r, kBauds[bi], chunks[ci], paces[pi], modes[mi], true);
            FaultManager::watchdogFeed();
          }
        }
      }
    }
    if (full) {
      // HIGH tier: base-subset sweep per high baud (mode 1 only, no mode-3 on S3).
      static const uint16_t kChunksH[] = {64, 128, 512};
      static const uint32_t kPacesH[] = {0, 1000};
      const size_t nBaudsH = sizeof(kBaudsH) / sizeof(kBaudsH[0]);
      for (size_t bi = 0; bi < nBaudsH; ++bi) {
        for (size_t ci = 0; ci < 3; ++ci) {
          for (size_t pi = 0; pi < 2; ++pi) {
            const SelfRes r = runSelfWireRes_(kBaudsH[bi], uartpoc::MODE_SYNTH_RAMP, kChunksH[ci], kPacesH[pi], 3);
            ++combos;
            if (r.ok != 3 || r.timeouts > 0 || r.mismatch > 0 || r.herr > 0 || r.perr > 0 || r.drops > 0) {
              ++bad;
            }
            logWireRes_(r, kBaudsH[bi], kChunksH[ci], kPacesH[pi], uartpoc::MODE_SYNTH_RAMP, true);
            FaultManager::watchdogFeed();
          }
        }
      }
    }
    LOG_I("POC", "SELFTEST SWEEP done combos=%lu bad=%lu", static_cast<unsigned long>(combos),
          static_cast<unsigned long>(bad));
  }

  void buildStats_(char* out, size_t cap) {
    const uint32_t now = millis();
    const uint32_t elapsed = (tFirstMs_ == 0 || now <= tFirstMs_) ? 0 : now - tFirstMs_;
    const unsigned long kbps = (elapsed < 1000) ? 0 : static_cast<unsigned long>(bytesRx_ * 8 / elapsed);
    const unsigned long kbs = (elapsed < 1000) ? 0 : static_cast<unsigned long>(bytesRx_ / elapsed);
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // Single machine-parseable line (sweep script scrapes k=v tokens).
    // kbps = kilobits/s, kbs = KB/s per spec (bytes/elapsed). rec counts
    // frames COMPLETED via FEC recovery (ok stays clean-only); par counts
    // accepted parity stores (wire-loss vs unused-parity split for task-4).
    // rec/par are appended last so existing field order is undisturbed.
    snprintf(out, cap,
             "STATS ok=%lu chunks=%lu herr=%lu perr=%lu drops=%lu big=%lu ooo=%lu dups=%lu ovf=%lu hb=%lu bytes=%lu "
             "kbps=%lu "
             "kbs=%lu intfree=%u up=%lu rec=%lu par=%lu",
             static_cast<unsigned long>(framesOk_), static_cast<unsigned long>(chunksRx_),
             static_cast<unsigned long>(hdrErr_), static_cast<unsigned long>(payErr_),
             static_cast<unsigned long>(drops_), static_cast<unsigned long>(big_), static_cast<unsigned long>(ooo_),
             static_cast<unsigned long>(dups_), static_cast<unsigned long>(ovf_), static_cast<unsigned long>(hbRx_),
             static_cast<unsigned long>(bytesRx_), kbps, kbs, static_cast<unsigned>(intFree),
             static_cast<unsigned long>(elapsed), static_cast<unsigned long>(recFec_),
             static_cast<unsigned long>(parRx_));
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
  uint8_t* rxBuf_;        // INTERNAL UART read staging (kRxBuf).
  uint8_t* slot_;         // INTERNAL one-frame reassembly slot (kSlotCap).
  uint8_t* txBuf_;        // INTERNAL CMD/HB encode staging (kMaxFrameLen).
  uint8_t* verifyChunk_;  // INTERNAL selftest chunk staging + regen scratch (kMaxPayload).
  uint8_t* parBuf_;       // INTERNAL parity staging (kParityBytes), keyed to one frameId at a time.
  bool testActive_;       // WIRE/SWEEP own Serial1 while set (pollLink hands off).
  bool txEnabled_;
  uint16_t txFrameId_;
  uint32_t framesOk_;
  uint32_t chunksRx_;
  uint32_t hdrErr_;
  uint32_t payErr_;
  uint32_t drops_;
  uint32_t big_;  // Oversize-frame drops (frame total > 64KB slot); subset of drops_.
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
  // ── FEC-RX state (S3 recover-before-reset) ──
  // Parity slots: K<=4 full-stride blocks for exactly one frameId (INTERNAL
  // kParityBytes, allocated once in begin()). Data shadow: stride/haveLast/
  // seen mirror over Reassembler (which exposes no accessors) so recovery can
  // name the exact erasure set. Parity never enters reassembly accounting;
  // OVERSIZE frames never recover. recFec_ counts recovery completions
  // (framesOk_ stays clean-only); parRx_ counts accepted parity stores
  // (foreign-frame/duplicate/invalid parity stays silent). RESET clears both;
  // STALE/COMPLETE/sync flushes must never clear them (cumulative counters,
  // not slot state — parReset_ leaves them alone).
  static constexpr size_t kParitySlots = uartpoc::fec::kMaxParity;
  static constexpr size_t kParityBytes = kParitySlots * uartpoc::kMaxPayload;
  uint32_t recFec_;
  uint32_t parRx_;
  uint8_t parCount_;
  bool parHave_;
  uint16_t parFid_;
  uint16_t parIdx_[uartpoc::fec::kMaxParity];
  uint16_t parLen_[uartpoc::fec::kMaxParity];
  uint8_t fecScratch_[uartpoc::fec::kRecoverScratchMin];
  bool dActive_;
  uint16_t dFid_;
  size_t dStride_;
  bool dHaveLast_;
  uint16_t dLastIdx_;
  uint16_t dLastLen_;
  bool dPend_;
  uint16_t dPendIdx_;
  uint32_t dBits_[uartpoc::Reassembler::kBitmapWords];
};

#endif  // ARDUINO
