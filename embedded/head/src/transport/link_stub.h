#pragma once
// Link stub (UART2) — TRANSPORT layer, dumb pipe, PARKED.
// Line->verb mapping up top (host-tested); UART2 TX/RX below (firmware-only).
// Business arrives as one injected onCommand callback (stored, never owned).

#include "head_context.h"

// Injected business edge (stored, never owned).
struct StubHandlers {
  void* ctx = nullptr;
  void (*onCommand)(void* ctx, const PowerCommand& cmd) = nullptr;
};

namespace linkstub {

inline bool tailOk(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  return *p == '\0';
}

// "FACE <n>" (numeric, clamped downstream); "CAM ON|OFF"; "OLED ON|DIM|OFF".
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

  // Stored, never owned. Call once in setup() after begin().
  void setHandlers(const StubHandlers& h) { handlers_ = h; }

  void sendAwake() { Serial2.println("AWAKE"); }

  void heartbeatTick() {
    const uint32_t now = millis();
    if (now - lastHb_ >= CFG_LINK_HEARTBEAT_MS) {
      lastHb_ = now;
      Serial2.println("HB");
    }
  }

  // Fixed-buffer line assembly; overlong/garbage lines drop with a WARN.
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
  StubHandlers handlers_;
  uint32_t lastHb_;
  char lineBuf_[64];
  size_t lineLen_;
};

#endif  // ARDUINO
