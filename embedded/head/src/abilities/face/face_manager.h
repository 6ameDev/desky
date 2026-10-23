#pragma once
// Face overlay state machine (Arduino-free, host-testable). Single home of
// the overlay + deadline state: at most one live overlay at a time.

#include "abilities/face/face_codec.h"
#include "head_context.h"
#include "link/qr_codec.h"

#ifndef CFG_FACE_QR_TIMEOUT_MS
#define CFG_FACE_QR_TIMEOUT_MS 120000
#endif
#ifndef CFG_FACE_OTA_TIMEOUT_MS
#define CFG_FACE_OTA_TIMEOUT_MS 120000
#endif

// Which live overlays a transition cleared (so the binding logs + repaints).
struct FaceTransition {
  bool clearedQr = false;
  bool clearedPair = false;
  bool clearedOta = false;
};

// Live paint mode for the render binding.
enum class FaceLiveMode : uint8_t { kFace, kQr, kOtaBar, kOtaAlert };

class FaceOverlayManager {
 public:
  // Stores (clamped) + clears every live overlay.
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

  // Stages the 63B bitmap until a FACE command or timeout. False on null.
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
      otadisplay::hide(ota_);
    }
    if (pairActive_) {
      pairActive_ = false;
    }
    qrActive_ = true;
    qrShownMs_ = nowMs;
    return true;
  }

  // Demo pattern (border + X, not a scannable QR): exercises show/clear with no link.
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

  // Clears the QR show mode (also clears a live pairing show). False when idle.
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

  // Renders the authoritative PIN until PAIRDONE, a FACE command, or the
  // valid-seconds deadline. Fail-closed on null/bad PIN.
  bool showPairing(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs, uint32_t nowMs, FaceTransition& t) {
    if (!pairdisplay::isValidPin(pin6)) {
      return false;
    }
    uint8_t modules[qr::kModules];
    if (!qr::encode6Digits(pin6, modules)) {
      return false;
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
      otadisplay::hide(ota_);
    }
    if (qrActive_) {
      qrActive_ = false;
    }
    pairActive_ = true;
    pairDeadlineMs_ = pairdisplay::deadlineFor(nowMs, validSecs);
    return true;
  }

  // Clears the pairing show mode (also clears a live demo QR). False when idle.
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

  // Whole seconds left on the pairing show (0 when idle/expired).
  uint32_t remainingSecs(uint32_t nowMs) const {
    if (!pairActive_) {
      return 0;
    }
    return pairdisplay::remainingSecs(nowMs, pairDeadlineMs_);
  }

  static bool formatCountdown(uint32_t totalSecs, char* out, size_t outLen) {
    return pairdisplay::formatCountdown(totalSecs, out, outLen);
  }

  // 1Hz repaint gate: true once per displayed-second change (updates lastSec).
  bool pairSecondTicked(uint32_t nowMs, uint32_t& lastSec) {
    if (!pairActive_) {
      return false;
    }
    const uint32_t s = pairdisplay::remainingSecs(nowMs, pairDeadlineMs_);
    if (s == lastSec) {
      return false;
    }
    lastSec = s;
    return true;
  }

  // Arms/refreshes the OTA fill bar.
  void showOta(uint8_t pct, uint32_t nowMs, FaceTransition& t) {
    t.clearedQr = qrActive_;
    t.clearedPair = pairActive_;
    if (qrActive_) {
      qrActive_ = false;
    }
    if (pairActive_) {
      pairActive_ = false;
    }
    otadisplay::show(ota_, pct);
    otaMs_ = nowMs;
  }

  // False when no OTA is up.
  bool hideOta() {
    if (!ota_.active) {
      return false;
    }
    otadisplay::hide(ota_);
    return true;
  }

  void failOta(uint32_t nowMs) {
    otadisplay::markFailed(ota_);
    otaMs_ = nowMs;
  }

  bool otaActive() const { return ota_.active; }
  bool otaFailed() const { return ota_.failed; }
  uint8_t otaPct() const { return ota_.pct; }

  // False when no OTA is up.
  bool clearOta() {
    if (!ota_.active) {
      return false;
    }
    otadisplay::hide(ota_);
    return true;
  }

  FaceLiveMode liveMode() const {
    if (ota_.active) {
      return ota_.failed ? FaceLiveMode::kOtaAlert : FaceLiveMode::kOtaBar;
    }
    if (qrActive_ || pairActive_) {
      return FaceLiveMode::kQr;
    }
    return FaceLiveMode::kFace;
  }

  // Wrap-safe expiry predicates for the render task.
  bool qrExpired(uint32_t nowMs) const {
    return qrActive_ && (nowMs - qrShownMs_ >= static_cast<uint32_t>(CFG_FACE_QR_TIMEOUT_MS));
  }
  bool pairExpired(uint32_t nowMs) const { return pairActive_ && pairdisplay::isExpired(nowMs, pairDeadlineMs_); }
  bool otaExpired(uint32_t nowMs) const {
    return ota_.active && (nowMs - otaMs_ >= static_cast<uint32_t>(CFG_FACE_OTA_TIMEOUT_MS));
  }

  void setDim(bool dim) { dim_ = dim; }
  bool isDim() const { return dim_; }

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
