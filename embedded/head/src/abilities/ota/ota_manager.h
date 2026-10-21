#pragma once
// desky-head OTA ability — lifecycle policy (Arduino-free, host-testable).
//
// Ability layer: ota_manager (lifecycle policy, zero socket deps). Extracted
// from the OtaUpdater pump in middleware/ota_client.h (Task 5 split): the
// start-gate, header validation (status-200 / Content-Length-vs-slot /
// manifest header), the 10% progress-step rule, the VERIFY/COMMIT verdicts,
// and the torn/stall abort mapping live here as pure functions over explicit
// values (never millis(), never WiFiClient/Update/mbedTLS). The single
// ota::State still lives in ota_codec.h — this policy only SELECTS events;
// the task applies them via applyEvent (one ota::step driver, never a forked
// state machine). Socket/Update/sha work stays in ota_task.h — the binding
// owns all pumps, logs, and the camera-suspend contract (active()).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "abilities/ota/ota_codec.h"

namespace otapolicy {

// Content-Length verdict: the length is REQUIRED (chunked encoding refused —
// Update needs the size up front) and must fit the app slot. The slot bytes
// ride a parameter so host tests pin the policy, never the board.
enum class LengthVerdict : uint8_t { kMissing, kTooBig, kOk };

inline LengthVerdict checkLength(uint32_t length, uint32_t slotBytes) {
  if (length == 0) {
    return LengthVerdict::kMissing;
  }
  if (!ota::fitsSlot(static_cast<size_t>(length), slotBytes)) {
    return LengthVerdict::kTooBig;
  }
  return LengthVerdict::kOk;
}

// Manifest body length (fetchManifest path): tiny by contract — empty or
// over-cap bodies refuse (fail closed at the call site).
inline bool manifestLengthOk(uint32_t length) { return length != 0 && length < otamanifest::kManifestCap; }

// Start-gate: a download is already in flight unless the lifecycle is IDLE.
// (Mirrors OtaUpdater::active() = state != kIdle.)
inline bool canStart(ota::State s) { return s == ota::State::kIdle; }

// Status-line validation: the status line must be an HTTP success ("HTTP/" +
// " 200 "). Pure over the already-read line (the task owns the socket read).
inline bool statusOk(const char* line) {
  if (line == nullptr) {
    return false;
  }
  return strncmp(line, "HTTP/", 5) == 0 && strstr(line, " 200 ") != nullptr;
}

// Manifest header value: exactly 64 hex chars staged verbatim (fail closed —
// a malformed value is ignored, never truncated). The sha256-toolchain guard
// stays in the task; this grammar check is unconditional.
inline bool manifestValueOk(const char* v) { return ota::isHex64(v); }

// 10% progress-step rule: report only when the fill crosses the next 10%
// boundary (counters-only logs ride the report, never per-chunk spam).
// Returns true + sets reportPct (floored to the boundary) when a report is
// due; lastPct is updated in place.
inline bool progressDue(uint32_t received, uint32_t total, uint32_t& lastPct, uint32_t& reportPct) {
  if (total == 0) {
    return false;
  }
  const uint32_t pct = (received * 100u) / total;
  if (pct < lastPct + 10u) {
    return false;
  }
  lastPct = pct - (pct % 10u);
  reportPct = lastPct;
  return true;
}

// Torn-stream gate: the body ended (no bytes available, peer gone) before
// streamComplete — the partial slot write must abort (old image boots).
inline bool isTorn(bool available, bool connected, uint32_t received, uint32_t total) {
  return !available && !connected && !ota::streamComplete(received, total);
}

// Stall gate: no body byte for the timeout window (wrap-safe elapsed).
inline bool isStalled(uint32_t nowMs, uint32_t lastRxMs, uint32_t timeoutMs) { return (nowMs - lastRxMs) >= timeoutMs; }

// VERIFY verdict: the staged manifest (when one was sent) must match the
// computed digest hex; no manifest degrades to size+magic only (committable).
inline ota::Event verifyOutcome(bool manifestSet, const char* actualHex, const char* expectedHex) {
  if (manifestSet && !ota::manifestMatches(actualHex, expectedHex)) {
    return ota::Event::kVerifyFail;
  }
  return ota::Event::kVerifyOk;
}

// COMMIT verdict: Update.end() success commits, failure aborts.
inline ota::Event commitOutcome(bool endOk) { return endOk ? ota::Event::kCommitOk : ota::Event::kCommitFail; }

// Single-state driver: the lifecycle in ota_codec.h is the ONLY state
// machine — policy selects events, the task applies them here (never forked).
inline bool applyEvent(ota::State& s, ota::Event e) { return ota::step(s, e); }

}  // namespace otapolicy
