#pragma once
// desky-head link stub — TRANSPORT layer, dumb pipe (PARKED, retained for
// switch-back). Moved via git mv from middleware/link_stub.h (Task 2 split).
//
// Top half (namespace linkstub) is Arduino-free line->verb mapping so host
// Unity tests cover it; the bottom half (class LinkStub) is firmware-only
// (#ifdef ARDUINO): UART2 @115200 on GPIO12/13 when wired, plain "AWAKE" +
// periodic "HB" heartbeat TX. No copper routed while parked. Business
// decisions arrive as an injected onCommand callback (StubHandlers, stored
// never owned, bound in main.cpp): no PowerManager include, no direct
// business calls, no EventBus publish here (the handler owns the bus + the
// ONE command table downstream).

#include "head_context.h"

// Injected business edge (stored, never owned). main.cpp binds PowerManager
// (+ the LINK_CMD publish) here; transport never includes those headers.
struct StubHandlers {
  void* ctx = nullptr;
  void (*onCommand)(void* ctx, const PowerCommand& cmd) = nullptr;
};

namespace linkstub {

// Tail of a line after a verb: only spaces/CR/LF/NUL may follow.
inline bool tailOk(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  return *p == '\0';
}

// "FACE <n>": numeric id (clamped downstream by apply/clampFace, so any
// non-negative int parses); "CAM ON|OFF"; "OLED ON|DIM|OFF".
inline bool mapLine(const char* line, PowerCommand& out) {
  if (line == nullptr) {
    return false;
  }
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  if (strncmp(line, "FACE ", 5) == 0) {
    const char* p = line + 5;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (*p < '0' || *p > '9') {
      return false;
    }
    int32_t id = 0;
    while (*p >= '0' && *p <= '9') {
      id = id * 10 + (*p - '0');
      ++p;
    }
    if (!tailOk(p)) {
      return false;
    }
    out.target = CmdTarget::FACE;
    out.arg = id;
    return true;
  }
  if (strncmp(line, "CAM ", 4) == 0) {
    const char* p = line + 4;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (strncmp(p, "ON", 2) == 0 && tailOk(p + 2)) {
      out.target = CmdTarget::CAM;
      out.arg = 1;
      return true;
    }
    if (strncmp(p, "OFF", 3) == 0 && tailOk(p + 3)) {
      out.target = CmdTarget::CAM;
      out.arg = 0;
      return true;
    }
    return false;
  }
  if (strncmp(line, "OLED ", 5) == 0) {
    const char* p = line + 5;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (strncmp(p, "ON", 2) == 0 && tailOk(p + 2)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_ON;
      return true;
    }
    if (strncmp(p, "DIM", 3) == 0 && tailOk(p + 3)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_DIM;
      return true;
    }
    if (strncmp(p, "OFF", 3) == 0 && tailOk(p + 3)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_OFF;
      return true;
    }
    return false;
  }
  return false;
}

}  // namespace linkstub

#ifdef ARDUINO
// ── Firmware: UART2 AWAKE/heartbeat TX + verb RX ───────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_LINK_BAUD
#define CFG_LINK_BAUD 115200
#endif
#ifndef CFG_LINK_HEARTBEAT_MS
#define CFG_LINK_HEARTBEAT_MS 1000
#endif

class LinkStub {
 public:
  LinkStub() : handlers_(), lastHb_(0), lineLen_(0) {}

  void begin() {
    Serial2.begin(CFG_LINK_BAUD, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    lastHb_ = millis();
    sendAwake();
    LOG_I("LINK", "uart2 up tx=%d rx=%d baud=%d", MCU_LINK_UART_TX, MCU_LINK_UART_RX, CFG_LINK_BAUD);
  }

  // Business-edge binder (stored, never owned). Call once in setup() after
  // begin(); main.cpp owns the handler target.
  void setHandlers(const StubHandlers& h) { handlers_ = h; }

  // Plain AWAKE after boot (the S3-waits-for-AWAKE rule is consumed by the
  // task-3 codec; this line is what it will hear).
  void sendAwake() { Serial2.println("AWAKE"); }

  void heartbeatTick() {
    const uint32_t now = millis();
    if (now - lastHb_ >= CFG_LINK_HEARTBEAT_MS) {
      lastHb_ = now;
      Serial2.println("HB");
    }
  }

  // RX stub: fixed-buffer line assembly; each newline maps into the ONE
  // command table via the injected onCommand edge (main owns the bus +
  // PowerManager there). Overlong/garbage lines are dropped with a WARN (the
  // local stand-in for task 3's drop-stale-frames rule).
  void poll() {
    while (Serial2.available() > 0) {
      const char c = static_cast<char>(Serial2.read());
      if (c == '\n') {
        lineBuf_[lineLen_] = '\0';
        PowerCommand cmd;
        if (linkstub::mapLine(lineBuf_, cmd)) {
          if (handlers_.onCommand != nullptr) {
            handlers_.onCommand(handlers_.ctx, cmd);
          }
        } else if (lineLen_ > 0) {
          LOG_W("LINK", "drop garbage line");
        }
        lineLen_ = 0;
      } else if (c != '\r') {
        if (lineLen_ + 1 < sizeof(lineBuf_)) {
          lineBuf_[lineLen_++] = c;
        } else {
          LOG_W("LINK", "drop overlong line");
          lineLen_ = 0;
        }
      }
    }
  }

 private:
  StubHandlers handlers_;  // injected business edge (main binds, never owned here)
  uint32_t lastHb_;
  char lineBuf_[64];
  size_t lineLen_;
};

#endif  // ARDUINO
