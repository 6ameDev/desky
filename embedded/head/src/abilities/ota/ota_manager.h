#pragma once
// OTA lifecycle policy (Arduino-free, host-testable): header validation,
// progress rule, VERIFY/COMMIT verdicts, torn/stall abort mapping.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "abilities/ota/ota_codec.h"

namespace otapolicy {

// Content-Length is required and must fit the app slot.
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

// Manifest bodies are tiny: empty or over-cap refuses (fail closed).
inline bool manifestLengthOk(uint32_t length) { return length != 0 && length < otamanifest::kManifestCap; }

// Start-gate: only from IDLE.
inline bool canStart(ota::State s) { return s == ota::State::kIdle; }

// The status line must be an HTTP success.
inline bool statusOk(const char* line) {
  if (line == nullptr) {
    return false;
  }
  return strncmp(line, "HTTP/", 5) == 0 && strstr(line, " 200 ") != nullptr;
}

// Manifest header value: exactly 64 hex chars, else ignored.
inline bool manifestValueOk(const char* v) { return ota::isHex64(v); }

// 10% progress-step rule: report only when the fill crosses the next boundary.
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

// Torn-stream gate: body ended before streamComplete.
inline bool isTorn(bool available, bool connected, uint32_t received, uint32_t total) {
  return !available && !connected && !ota::streamComplete(received, total);
}

// Stall gate: no body byte for the timeout window.
inline bool isStalled(uint32_t nowMs, uint32_t lastRxMs, uint32_t timeoutMs) { return (nowMs - lastRxMs) >= timeoutMs; }

// VERIFY verdict (no manifest degrades to size+magic only).
inline ota::Event verifyOutcome(bool manifestSet, const char* actualHex, const char* expectedHex) {
  if (manifestSet && !ota::manifestMatches(actualHex, expectedHex)) {
    return ota::Event::kVerifyFail;
  }
  return ota::Event::kVerifyOk;
}

inline ota::Event commitOutcome(bool endOk) { return endOk ? ota::Event::kCommitOk : ota::Event::kCommitFail; }

// Single-state driver: policy selects events, the task applies them.
inline bool applyEvent(ota::State& s, ota::Event e) { return ota::step(s, e); }

}  // namespace otapolicy
