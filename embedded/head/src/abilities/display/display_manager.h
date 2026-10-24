#pragma once
// Display overlay state machine (Arduino-free, host-testable). Single home of
// the overlay + deadline state: at most one live overlay at a time.

#include "abilities/display/display_codec.h"
#include "head_context.h"
#include "link/qr_codec.h"

#ifndef CFG_DISPLAY_QR_TIMEOUT_MS
#define CFG_DISPLAY_QR_TIMEOUT_MS 120000
#endif
#ifndef CFG_DISPLAY_OTA_TIMEOUT_MS
#define CFG_DISPLAY_OTA_TIMEOUT_MS 120000
#endif

// Which live overlays a transition cleared (so the binding logs + repaints).
struct DisplayTransition {
  bool clearedQr = false;
  bool clearedPair = false;
  bool clearedOta = false;
};

// Live paint mode for the render binding.
enum class DisplayLiveMode : uint8_t { kEyes, kQr, kOtaBar, kOtaAlert };

class DisplayManager {
 public:
  // Single gated expression entry: the core->head EXPR link command and the
  // local bench trigger (CLI `face`) converge here — one entry, two callers.
  // Pairing/OTA own the screen while held, so a held overlay rejects the
  // request with zero state change; eyes otherwise. Core owns app/behavior
  // requests; head keeps local preemption for its bench path via showEyes.
  bool requestEyes(uint8_t id, DisplayTransition& t, uint32_t nowMs) {
    if (pairActive_ || ota_.active) {
      t.clearedQr = false;
      t.clearedPair = false;
      t.clearedOta = false;
      return false;
    }
    return showEyes(id, t, nowMs);
  }

  // Stores (clamped) + clears every live overlay. Neutral-family ids always
  // release; higher priorities preempt, equal refresh, lower never displace.
  // True when the request took effect.
  bool showEyes(uint8_t id, DisplayTransition& t, uint32_t nowMs) {
    t.clearedQr = qrActive_;
    t.clearedPair = pairActive_;
    t.clearedOta = ota_.active;
    qrActive_ = false;
    pairActive_ = false;
    otadisplay::hide(ota_);
    const uint8_t c = clampFace(static_cast<int>(id));
    if (!exprcat::isNeutral(c) && exprcat::priorityFor(c) < exprcat::priorityFor(eyesId_)) {
      return false;
    }
    eyesId_ = c;
    exprSinceMs_ = nowMs;
    return true;
  }

  uint8_t eyesId() const { return eyesId_; }

  // Play-once expiry pump (wrap-safe): true once when an id fell back to neutral.
  bool exprTick(uint32_t nowMs) {
    if (!exprcat::isPlayOnce(eyesId_) || (nowMs - exprSinceMs_) < exprcat::holdMsFor(eyesId_)) {
      return false;
    }
    eyesId_ = FACE_BOOT;
    exprSinceMs_ = nowMs;
    return true;
  }

  // Stages the 63B bitmap until a FACE command or timeout. False on null.
  bool showQr(const uint8_t* qr63, uint32_t nowMs, DisplayTransition& t) {
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

  // Clears the QR show mode (also clears a live pairing show). False when idle.
  bool clearQr(DisplayTransition& t) {
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
  bool showPairing(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs, uint32_t nowMs, DisplayTransition& t) {
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

  // Clears the pairing show mode (also clears a live QR). False when idle.
  bool clearPairing(DisplayTransition& t) {
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
  void showOta(uint8_t pct, uint32_t nowMs, DisplayTransition& t) {
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

  DisplayLiveMode liveMode() const {
    if (ota_.active) {
      return ota_.failed ? DisplayLiveMode::kOtaAlert : DisplayLiveMode::kOtaBar;
    }
    if (qrActive_ || pairActive_) {
      return DisplayLiveMode::kQr;
    }
    return DisplayLiveMode::kEyes;
  }

  // Wrap-safe expiry predicates for the render task.
  bool qrExpired(uint32_t nowMs) const {
    return qrActive_ && (nowMs - qrShownMs_ >= static_cast<uint32_t>(CFG_DISPLAY_QR_TIMEOUT_MS));
  }
  bool pairExpired(uint32_t nowMs) const { return pairActive_ && pairdisplay::isExpired(nowMs, pairDeadlineMs_); }
  bool otaExpired(uint32_t nowMs) const {
    return ota_.active && (nowMs - otaMs_ >= static_cast<uint32_t>(CFG_DISPLAY_OTA_TIMEOUT_MS));
  }

  void setDim(bool dim) { dim_ = dim; }
  bool isDim() const { return dim_; }

  const uint8_t* qrBuf() const { return qrBuf_; }
  const uint8_t* pairPin() const { return pairPin_; }

 private:
  uint8_t eyesId_ = FACE_BOOT;
  uint32_t exprSinceMs_ = 0;
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
