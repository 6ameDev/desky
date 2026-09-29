#pragma once
// Shared ESP-IDF log-output filter — MECHANISM only, never policy.
//
// Layer honesty: our Logger (common/logger.h) formats its own lines and
// writes them straight to its sinks — ESP-IDF internal logs (e.g.
// "E (...) gpio: ...") never pass through it, so Logger-side filtering
// CANNOT intercept them. This filter installs at the ESP-IDF vprintf layer
// (esp_log_set_vprintf), the only point that sees every system log line.
//
// Backend-registry pattern: this utility owns HOW (bounded table, matcher,
// shim); each firmware owns WHAT (registers its own rules at boot in
// setup(), before tasks start — same settle-before-use discipline as
// EventBus::subscribe; never register from a task). Core registers nothing
// today; head registers its camera quirk. Boot should log ruleCount() so no
// suppression is ever silent.
//
// Rules: no heap (fixed 8-slot static table of string-literal pointers),
// no locks (setup-time registration only, lock-free reads), never log from
// inside the shim.

#include <stdint.h>
#include <string.h>

namespace logfilter {

static const uint8_t kMaxRules = 8;

struct Registry {
  const char* contains[kMaxRules];
  uint8_t count;
};

// Single program-wide instance (function-local static in an inline
// function — one copy even across translation units, no .cpp needed).
inline Registry& registry() {
  static Registry r = {};
  return r;
}

// Setup/test only. Stores the pointer (callers pass string literals, which
// live forever). False on null/empty input or a full table.
inline bool addRule(const char* contains) {
  if (contains == nullptr || contains[0] == '\0') {
    return false;
  }
  Registry& r = registry();
  if (r.count >= kMaxRules) {
    return false;
  }
  r.contains[r.count++] = contains;
  return true;
}

// Setup/test only. Lets host tests isolate cases; never call after begin()
// in firmware (rules are boot policy, not runtime state).
inline void clear() { registry().count = 0; }

inline uint8_t ruleCount() { return registry().count; }

// Substring (not exact-line): survives ESP-IDF's timestamp/tag prefixes and
// minor SDK rewordings around the stable core phrase. Null-safe.
inline bool isSuppressed(const char* line) {
  if (line == nullptr) {
    return false;
  }
  const Registry& r = registry();
  for (uint8_t i = 0; i < r.count; ++i) {
    if (r.contains[i] != nullptr && strstr(line, r.contains[i]) != nullptr) {
      return true;
    }
  }
  return false;
}

}  // namespace logfilter

#ifdef ARDUINO
// ── Firmware: one-time vprintf shim ─────────────────────────────────────────

#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>

class LogFilter {
 public:
  // Installs the shim once; safe to call twice. Call in setup() AFTER
  // registering rules, BEFORE the first subsystem that logs noise.
  static void begin() {
    if (installed()) {
      return;
    }
    prev() = esp_log_set_vprintf(&LogFilter::shim);
    installed() = true;
  }

  static bool addRule(const char* contains) { return logfilter::addRule(contains); }
  static uint8_t ruleCount() { return logfilter::ruleCount(); }

 private:
  static bool& installed() {
    static bool b = false;
    return b;
  }

  static vprintf_like_t& prev() {
    static vprintf_like_t p = nullptr;
    return p;
  }

  // Formats into a bounded stack buffer, drops on match, forwards
  // byte-for-byte otherwise. No heap, no logging, no blocking — safe
  // anywhere the log path runs.
  static int shim(const char* format, va_list args) {
    char buf[256];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(buf, sizeof(buf), format, copy);
    va_end(copy);
    if (logfilter::isSuppressed(buf)) {
      return 0;
    }
    if (prev() != nullptr) {
      return prev()(format, args);
    }
    return vprintf(format, args);
  }
};

#endif  // ARDUINO
