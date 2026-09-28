#pragma once
// desky v2 lightweight logger — header-only, zero new deps.
//
// Good practice for firmware (vs raw Serial.printf everywhere):
// - Compile-time level stripping: disabled levels -> ((void)0), zero flash/CPU.
// - Pluggable sinks: Serial now, WS/RingBuffer later without touching call sites.
// - Backend-style QoL: tags, timestamps, core/task id, throttle, once, hexdump.
//
// Usage:
//   Logger::begin();                    // in setup(), after Serial.begin()
//   LOG_I("BOOT", "MCU=%s", MCU_NAME);  // + LOG_V/D/W/E
//   LOG_EVERY_N(100, "TOF", "dist=%d", mm);
//   LOG_ONCE("WIFI", "connected");
//   Logger::hexdump("I2C", buf, len);
//
// Rules: no heap in log path, no String, never call from ISR.

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

// ── Levels (macros so -D LOG_LEVEL works in #if) ─────────────
#define LOG_LEVEL_VERBOSE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARN 3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_NONE 5

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_DEBUG
#endif

// ── Sink interface ───────────────────────────────────────────
class LogSink {
 public:
  virtual ~LogSink() = default;
  virtual void write(const char* line) = 0;
};

class SerialSink : public LogSink {
 public:
  void write(const char* line) override { Serial.println(line); }
};

// ── Logger ───────────────────────────────────────────────────
class Logger {
 public:
  static constexpr int kMaxSinks = 4;
  static constexpr size_t kBufSize = 192;

  static void begin() {
    ensureMutex();
    if (s_sinkCount == 0) {
      addSink(defaultSink());
    }
  }

  static void addSink(LogSink* sink) {
    ensureMutex();
    if (sink == nullptr || s_sinkCount >= kMaxSinks) {
      return;
    }
    for (int i = 0; i < s_sinkCount; ++i) {
      if (s_sinks[i] == sink) {
        return;
      }
    }
    s_sinks[s_sinkCount++] = sink;
  }

  static void log(int level, const char* tag, const char* fmt, ...) {
    char msg[kBufSize];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit(level, tag, msg);
  }

  static void hexdump(const char* tag, const uint8_t* buf, size_t len) {
    // One line per 16 bytes, throttled by caller's level macro.
    char line[kBufSize];
    for (size_t off = 0; off < len; off += 16) {
      size_t n = (len - off > 16) ? 16 : (len - off);
      int pos = snprintf(line, sizeof(line), "+%04X: ", (unsigned)off);
      for (size_t i = 0; i < n && pos > 0 && pos < (int)sizeof(line) - 4; ++i) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[off + i]);
      }
      emit(LOG_LEVEL_DEBUG, tag, line);
    }
  }

 private:
  static void emit(int level, const char* tag, const char* msg) {
    const char* lvl = levelChar(level);
    unsigned long ms = millis();

    char task[13] = "setup";
#if defined(portTICK_PERIOD_MS)
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
      const char* name = pcTaskGetName(nullptr);
      if (name != nullptr) {
        strncpy(task, name, sizeof(task) - 1);
        task[sizeof(task) - 1] = '\0';
      }
    }
#endif
    int core = 0;
#if defined(ARDUINO_ARCH_ESP32)
    core = xPortGetCoreID();
#endif

    char line[kBufSize];
    // Padded boot-millis: fixed 10 cols, monotonic, script-friendly.
    // Wall-clock join later via one TIME sync-marker (epoch + boot_ms).
    snprintf(line, sizeof(line), "[%010lu][%s][%s][c%d:%s] %s", ms, lvl, tag, core, task, msg);

    lock();
    if (s_sinkCount == 0) {
      defaultSink()->write(line);
    } else {
      for (int i = 0; i < s_sinkCount; ++i) {
        s_sinks[i]->write(line);
      }
    }
    unlock();
  }

  static const char* levelChar(int level) {
    switch (level) {
      case LOG_LEVEL_VERBOSE:
        return "V";
      case LOG_LEVEL_DEBUG:
        return "D";
      case LOG_LEVEL_INFO:
        return "I";
      case LOG_LEVEL_WARN:
        return "W";
      case LOG_LEVEL_ERROR:
        return "E";
      default:
        return "?";
    }
  }

  static SerialSink* defaultSink() {
    static SerialSink s;
    return &s;
  }

  static void ensureMutex() {
    if (s_mutex == nullptr) {
#if defined(portTICK_PERIOD_MS)
      // Safe to call repeatedly; first writer wins.
      SemaphoreHandle_t m = xSemaphoreCreateMutex();
      if (m != nullptr) {
        // Avoid clobbering if two tasks race here.
        if (s_mutex == nullptr) {
          s_mutex = m;
        } else {
          vSemaphoreDelete(m);
        }
      }
#endif
    }
  }

  static void lock() {
    if (s_mutex != nullptr) {
      xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
  }

  static void unlock() {
    if (s_mutex != nullptr) {
      xSemaphoreGive(s_mutex);
    }
  }

  static LogSink* s_sinks[kMaxSinks];
  static int s_sinkCount;
  static SemaphoreHandle_t s_mutex;
};

inline LogSink* Logger::s_sinks[Logger::kMaxSinks] = {nullptr};
inline int Logger::s_sinkCount = 0;
inline SemaphoreHandle_t Logger::s_mutex = nullptr;

// ── Compile-time gated macros (zero-cost when stripped) ──────
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
#define LOG_V(tag, fmt, ...) Logger::log(LOG_LEVEL_VERBOSE, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_V(tag, fmt, ...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_D(tag, fmt, ...) Logger::log(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_D(tag, fmt, ...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_INFO
#define LOG_I(tag, fmt, ...) Logger::log(LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_I(tag, fmt, ...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_WARN
#define LOG_W(tag, fmt, ...) Logger::log(LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_W(tag, fmt, ...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_ERROR
#define LOG_E(tag, fmt, ...) Logger::log(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_E(tag, fmt, ...) ((void)0)
#endif

// ── QoL: throttle + once (respect compile-time level via inner macro) ──
#define _LOG_EVERY_N_CONCAT(a, b) a##b
#define _LOG_EVERY_N_VAR(line) _LOG_EVERY_N_CONCAT(_log_every_n_, line)

#define LOG_EVERY_N(n, tag, fmt, ...)                        \
  do {                                                       \
    static uint32_t _LOG_EVERY_N_VAR(__LINE__) = 0;          \
    if (++_LOG_EVERY_N_VAR(__LINE__) % (uint32_t)(n) == 0) { \
      LOG_D(tag, fmt, ##__VA_ARGS__);                        \
    }                                                        \
  } while (0)

#define _LOG_ONCE_CONCAT(a, b) a##b
#define _LOG_ONCE_VAR(line) _LOG_ONCE_CONCAT(_log_once_, line)

#define LOG_ONCE(tag, fmt, ...)                  \
  do {                                           \
    static bool _LOG_ONCE_VAR(__LINE__) = false; \
    if (!_LOG_ONCE_VAR(__LINE__)) {              \
      _LOG_ONCE_VAR(__LINE__) = true;            \
      LOG_I(tag, fmt, ##__VA_ARGS__);            \
    }                                            \
  } while (0)
