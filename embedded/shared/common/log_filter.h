#pragma once
// ESP-IDF log-output filter at the vprintf layer (the only point that sees every system log line).
// No heap (8-slot static table), no locks (boot-time registration only); never log from inside the shim.

#include <stdint.h>
#include <string.h>

namespace logfilter {

static const uint8_t kMaxRules = 8;

struct Registry {
  const char* contains[kMaxRules];
  uint8_t count;
};

inline Registry& registry() {
  static Registry r = {};
  return r;
}

// Setup/test only; stores the pointer (pass string literals).
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

// Test only; never call after begin() in firmware.
inline void clear() { registry().count = 0; }

inline uint8_t ruleCount() { return registry().count; }

// Substring match: survives IDF timestamp/tag prefixes and minor SDK rewordings.
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
#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>

class LogFilter {
 public:
  // Call in setup() after registering rules; safe to call twice.
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

  // Drop on match, forward byte-for-byte otherwise; no heap, no logging.
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
