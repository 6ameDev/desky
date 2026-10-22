#pragma once
// Lightweight header-only logger: compile-time level stripping, pluggable sinks.
// Logger::begin() in setup() after Serial.begin(). No heap, no String, never call from ISR.

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

class LogSink {
 public:
  virtual ~LogSink() = default;
  virtual void write(const char* line) = 0;
};

class SerialSink : public LogSink {
 public:
  void write(const char* line) override { Serial.println(line); }
};

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
    // Fixed 10-col boot-millis: monotonic, script-friendly.
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
      SemaphoreHandle_t m = xSemaphoreCreateMutex();
      if (m != nullptr) {
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

// Throttle (respects compile-time level via inner macro).
#define _LOG_EVERY_N_CONCAT(a, b) a##b
#define _LOG_EVERY_N_VAR(line) _LOG_EVERY_N_CONCAT(_log_every_n_, line)

#define LOG_EVERY_N(n, tag, fmt, ...)                        \
  do {                                                       \
    static uint32_t _LOG_EVERY_N_VAR(__LINE__) = 0;          \
    if (++_LOG_EVERY_N_VAR(__LINE__) % (uint32_t)(n) == 0) { \
      LOG_D(tag, fmt, ##__VA_ARGS__);                        \
    }                                                        \
  } while (0)
