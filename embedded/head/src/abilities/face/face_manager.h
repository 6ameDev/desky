#pragma once
// desky-head face ability — overlay state machine (Arduino-free, host-testable).
//
// Ability layer: face_manager (state machine + business logic, zero socket
// deps, zero GFX). Extracted from FaceRenderer in middleware/face_renderer.h
// (Task 3 split): the single-overlay rivalry rules live here, driven by an
// explicit nowMs (never millis()), so host tests can pin every transition.
// Painting stays in face_task.h — the binding reads liveMode()/buffers and
// owns all panel gates, logs, and the render cadence.
//
// Rivalry (preserved exactly — one live overlay at a time):
// - showFace clears all three overlays (QR + pairing + OTA).
// - showQr clears OTA + pairing; showPairing clears OTA + demo-QR.
// - showOta clears QR + pairing.
// - clearQr clears both QR + pairing; clearPairing clears both pairing + QR.
// - repaint preserves the live overlay (OTA bar/alert, else QR/pairing,
//   else the stored face).

#include "abilities/face/face_codec.h"
#include "head_context.h"
#include "link/qr_codec.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_FACE_QR_TIMEOUT_MS
#define CFG_FACE_QR_TIMEOUT_MS 120000
#endif
#ifndef CFG_FACE_OTA_TIMEOUT_MS
#define CFG_FACE_OTA_TIMEOUT_MS 120000
#endif

// Single-overlay rivalry verdict: which live overlays a transition cleared,
// so the render binding can log + repaint exactly like the legacy renderer.
struct FaceTransition {
  bool clearedQr = false;
  bool clearedPair = false;
  bool clearedOta = false;
};

// Live paint mode for the render binding (repaint preserves the overlay:
// OTA bar/alert, else QR/pairing, else the stored face).
enum class FaceLiveMode : uint8_t { kFace, kQr, kOtaBar, kOtaAlert };

class FaceOverlayManager {
 public:
  // SHOW_FACE{id}: stores (clamped) + clears every live overlay (QR show
  // mode, pairing show mode, OTA progress mode — one live overlay at a time).
  void showFace(uint8_t id, FaceTransition& t) {
    t.clearedQr = qrActive_;
    t.clearedPair = pairActive_;
    t.clearedOta = ota_.active;
    qrActive_ = false;
    pairActive_ = false;
    otadisplay::hide(ota_);
    faceId_ = clampFace(static_cast<int>(id));
  }

  uint8_t faceId() const { return faceId_; }

  // QR show mode: stages the 63B bitmap until a FACE command arrives or
  // CFG_FACE_QR_TIMEOUT_MS elapses. One live overlay: QR wins over OTA and
  // over pairing here. False on null (no state change).
  bool showQr(const uint8_t* qr63, uint32_t nowMs, FaceTransition& t) {
    if (qr63 == nullptr) {
      return false;
    }
    for (size_t i = 0; i < qrdisplay::kQrLen; ++i) {
      qrBuf_[i] = qr63[i];
    }
    t.clearedOta = ota_.active;
    t.clearedPair = pairActive_;
    if (ota_.active) {
      otadisplay::hide(ota_);  // one live overlay: QR wins over OTA here
    }
    if (pairActive_) {
      pairActive_ = false;  // one live overlay: demo QR wins over pairing
    }
    qrActive_ = true;
    qrShownMs_ = nowMs;
    return true;
  }

  // Bench demo pattern (deterministic border + X, NOT a scannable QR):
  // exercises the QR show/clear path with no link. Same showQr rivalry.
  void showQrDemo(uint32_t nowMs, FaceTransition& t) {
    uint8_t demo[qrdisplay::kQrLen] = {};
    for (int y = 0; y < qrdisplay::kModules; ++y) {
      for (int x = 0; x < qrdisplay::kModules; ++x) {
        const bool edge = (x < 2 || y < 2 || x >= qrdisplay::kModules - 2 || y >= qrdisplay::kModules - 2);
        const bool cross = (x == y) || (x + y == qrdisplay::kModules - 2);
        if (edge || cross) {
          demo[static_cast<size_t>(y) * 3u + static_cast<size_t>(x >> 3)] |= static_cast<uint8_t>(1u << (7 - (x & 7)));
        }
      }
    }
    showQr(demo, nowMs, t);
  }

  // Clears the QR show mode (timeout/CLI path). Also clears a live pairing
  // show (single overlay rivalry — `qr clear` never leaves a stale PIN up).
  // False when neither is up (no-op, so only real transitions log).
  bool clearQr(FaceTransition& t) {
    t.clearedQr = qrActive_;
    t.clearedPair = pairActive_;
    if (!qrActive_ && !pairActive_) {
      return false;
    }
    qrActive_ = false;
    pairActive_ = false;
    return true;
  }

  bool qrActive() const { return qrActive_; }

  // Pairing show mode: renders the authoritative PIN — the QR bitmap is
  // re-derived LOCALLY via qr::encode6Digits into the existing qrBuf_
  // staging path (no bitmap rides the wire) — until PAIRDONE, a FACE
  // command, or the valid-seconds deadline elapses (deadline = show-time +
  // validSecs*1000, re-armed on every fresh show). Fail-closed: a null/bad
  // PIN changes no state. One live overlay: pairing wins over OTA and over
  // demo QR here.
  bool showPairing(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs, uint32_t nowMs, FaceTransition& t) {
    if (!pairdisplay::isValidPin(pin6)) {
      return false;
    }
    uint8_t modules[qr::kModules];
    if (!qr::encode6Digits(pin6, modules)) {
      return false;  // impossible for validated digits: fail closed, no state change
    }
    uint8_t packed[qr::kPackedLen];
    qr::packBitmap(modules, packed);
    for (size_t i = 0; i < qrdisplay::kQrLen; ++i) {
      qrBuf_[i] = packed[i];
    }
    for (size_t i = 0; i < pairdisplay::kPinLen; ++i) {
      pairPin_[i] = pin6[i];
    }
    t.clearedOta = ota_.active;
    t.clearedQr = qrActive_;
    if (ota_.active) {
      otadisplay::hide(ota_);  // one live overlay: pairing wins over OTA here
    }
    if (qrActive_) {
      qrActive_ = false;  // one live overlay: pairing wins over demo QR
    }
    pairActive_ = true;
    pairDeadlineMs_ = pairdisplay::deadlineFor(nowMs, validSecs);
    return true;
  }

  // Clears the pairing show mode (PAIRDONE/timeout/CLI path). Also clears a
  // live demo QR (single overlay — no stale bitmap survives pairing
  // completion). False when neither is up (no-op).
  bool clearPairing(FaceTransition& t) {
    t.clearedPair = pairActive_;
    t.clearedQr = qrActive_;
    if (!pairActive_ && !qrActive_) {
      return false;
    }
    pairActive_ = false;
    qrActive_ = false;
    return true;
  }

  bool pairActive() const { return pairActive_; }

  // OTA progress mode (carrier trigger): arms/refreshes the fill bar (FETCH:
  // fill proportion, VERIFY/COMMIT: full bar). One live overlay: OTA wins
  // over QR and over pairing here.
  void showOta(uint8_t pct, uint32_t nowMs, FaceTransition& t) {
    t.clearedQr = qrActive_;
    t.clearedPair = pairActive_;
    if (qrActive_) {
      qrActive_ = false;  // one live overlay: OTA wins over QR
    }
    if (pairActive_) {
      pairActive_ = false;  // one live overlay: OTA wins over pairing
    }
    otadisplay::show(ota_, pct);
    otaMs_ = nowMs;
  }

  // Updater success: clear to the stored face. False when no OTA is up.
  bool hideOta() {
    if (!ota_.active) {
      return false;
    }
    otadisplay::hide(ota_);
    return true;
  }

  // Updater abort/failure: alert face now, timeout back to the stored face.
  void failOta(uint32_t nowMs) {
    otadisplay::markFailed(ota_);
    otaMs_ = nowMs;
  }

  bool otaActive() const { return ota_.active; }
  bool otaFailed() const { return ota_.failed; }
  uint8_t otaPct() const { return ota_.pct; }

  // Clears the OTA mode (timeout path). False when no OTA is up (no-op).
  bool clearOta() {
    if (!ota_.active) {
      return false;
    }
    otadisplay::hide(ota_);
    return true;
  }

  // Live paint mode — the binding repaints the overlay (OTA bar/alert, else
  // QR/pairing, else the stored face) when power/DIM returns without changing
  // the face id, so an overlay survives ON/DIM while any FACE command still
  // clears it.
  FaceLiveMode liveMode() const {
    if (ota_.active) {
      return ota_.failed ? FaceLiveMode::kOtaAlert : FaceLiveMode::kOtaBar;
    }
    if (qrActive_ || pairActive_) {
      return FaceLiveMode::kQr;
    }
    return FaceLiveMode::kFace;
  }

  // Expiry predicates for the render task (wrap-safe elapsed/deadline gates).
  bool qrExpired(uint32_t nowMs) const {
    return qrActive_ && (nowMs - qrShownMs_ >= static_cast<uint32_t>(CFG_FACE_QR_TIMEOUT_MS));
  }
  bool pairExpired(uint32_t nowMs) const { return pairActive_ && pairdisplay::isExpired(nowMs, pairDeadlineMs_); }
  bool otaExpired(uint32_t nowMs) const {
    return ota_.active && (nowMs - otaMs_ >= static_cast<uint32_t>(CFG_FACE_OTA_TIMEOUT_MS));
  }

  // DIM matrix row: low refresh + low contrast (binding-owned cadence).
  void setDim(bool dim) { dim_ = dim; }
  bool isDim() const { return dim_; }

  // Staged paint buffers for the binding's drawQr (read-only views).
  const uint8_t* qrBuf() const { return qrBuf_; }
  const uint8_t* pairPin() const { return pairPin_; }

 private:
  uint8_t faceId_ = FACE_BOOT;
  bool qrActive_ = false;
  uint8_t qrBuf_[qrdisplay::kQrLen] = {};
  uint32_t qrShownMs_ = 0;
  bool pairActive_ = false;
  uint8_t pairPin_[pairdisplay::kPinLen] = {};
  uint32_t pairDeadlineMs_ = 0;
  otadisplay::Progress ota_;
  uint32_t otaMs_ = 0;
  bool dim_ = false;
};

static_assert(qr::kPackedLen == qrdisplay::kQrLen, "pairing re-derive must fill the qr staging buffer exactly");
